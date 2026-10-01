/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * T6021 ANE firmware loader — installed-module port of the proven lab
 * staging unit (receipts 2026-09-19-h14-w13-boot-contract.md,
 * -w14-staging-proven.md; add-path proof
 * 2026-09-28-h14-fsm-secure-park-decode/first-inference.md).
 *
 * Implements, behind fw_load=1 (default on):
 *   1. request_firmware("apple/ane/t602x_ane0_fw_selene_rc4x.macho")
 *   2. Validation via ane_fw_validate.h (shared with the offline
 *      regression h14_fwload_regression.c, shipped in
 *      ane-linux-experiments/tools/): sha256 pin, strict
 *      exact-image assertions (7 load commands, 3 pinned segments,
 *      entry 0, bounded LC walk).
 *   3. dma_alloc_coherent on the ANE platform device: the buffer is
 *      DART-mapped through the device's iommu group (stream 0 of the
 *      three bound instances, H14DartAudit). Segment-wise copy
 *      (__TEXT fileoff 0x4000 -> vm 0, __DATA fileoff 0xc8000 -> vm
 *      0xc4000; vmsize tail zero from the coherent alloc). Coherent
 *      memory needs no explicit cache clean.
 *   4. Which copy runs. fw_alias_reserved=1 (default, T6021 only) maps
 *      the copy iBoot preloaded at SEG0/SEGi. Otherwise the staged copy
 *      runs: ane_fw_apply_boot_patches() first writes iBoot's runtime
 *      patches into it (DATA base, RTK_soc, revision, ASC addresses,
 *      stack guard, ASC tunables), and then it equals the preload byte
 *      for byte except the random guard (17 captures,
 *      receipts/2026-10-01-t602x-independent). That needs no reserved
 *      memory and no preload address, so it is the only mode on SoCs
 *      without a recorded placement (T6020, T6022).
 *   5. W16 entry alias: the staged fw pages are ALIASED at the latched
 *      RVBAR entry on the device's default DMA domain. Placement
 *      contract, receipt receipts/2026-09-20-t6021-entry-alias.md:
 *        - live RVBAR read64 = 0x10000000001 (bit0 latched, entry
 *          bits 0x10000000000 = dart-ane vm base); kext law
 *          (rvbar-lifecycle 6288b0b): bit0 set => the RVBAR write
 *          branch is skipped, so the ASC fetches AT the latched entry;
 *        - netconsole 2026-09-20T17:48:19: apple-dart 285800000.iommu
 *          "translation fault ... code:0x2 (NO PGD FOR IOVA) at
 *          0x100000dca10" from the pre-module userspace CPU release —
 *          the ASC fetched the entry region while nothing was mapped;
 *        - current-state table walk (kcore, 2026-09-20): entry
 *          pgd[16] = 0 while the fw leaves are valid -> the region the
 *          hardware fetches is unmapped and the fw surface is
 *          physically NON-contiguous (consecutive leaf PAs differ by
 *          -0x4000). The alias therefore copies each page's PA from
 *          the live fw translation (iommu_iova_to_phys), never from
 *          the kernel VA, and verifies every target page unmapped
 *          before mapping it.
 *      Driver-managed only: iommu_map() on the attached domain —
 *      apple_dart implements .map_pages + .iotlb_sync_map, so the
 *      driver flushes the DART TLB; no raw TTBR/TCR writes.
 *      Collision ceiling: the iovad allocates top-down from ~4 TiB
 *      and total mappable RAM is 94.9 GiB (/proc/iomem), so the
 *      allocator floor 4 TiB - 96 GiB = 0x3e800000000 is far above
 *      the alias ceiling 0x10005000000; the per-page zero-check
 *      additionally guards each map.
 *
 * NOT implemented: the boot step (CPU release + SCRATCH). The userspace
 * hybrid stage owns the W8 grant writes; with the alias in place a
 * release no longer faults the entry fetch.
 *
 * Error ownership: every failure path releases the firmware and, once
 * allocated, the coherent buffer, and clears ane->fw_buf — remove()
 * frees only what fw_buf still names. Load is non-fatal to probe.
 */
#include <crypto/sha2.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "ane_t6021.h"
#include "ane_t6021_boot.h"
#include "ane_fw_validate.h"

/* This object links into both ane_t6021.ko and ane_t6021_rtclient.ko;
 * per-object metadata keeps modpost happy for either composition. */
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 ANE firmware staging + entry alias");

static bool fw_load = true;
module_param(fw_load, bool, 0444);
MODULE_PARM_DESC(fw_load,
		 "Validate + DART-map the selene PRELOAD payload (default on: "
		 "the proven add-path configuration).");

static unsigned int fw_extra_ram = 0x200000;
module_param(fw_extra_ram, uint, 0444);
MODULE_PARM_DESC(fw_extra_ram,
		 "Page-aligned owned RAM after the 5 MiB firmware allocation "
		 "(default 0x200000, the proven add-path grant; maximum 16 MiB).");

/* Preloaded-placement alias: map the iBoot-reserved SEG0/SEGi phys at
 * the entry IOVAs (the preload, with iBoot's patches in place). Default
 * on: the proven add-path configuration. Only SoCs whose placement is
 * recorded honor it (ane_t602x_soc.preload_placement). */
static bool fw_alias_reserved = true;
module_param(fw_alias_reserved, bool, 0444);
MODULE_PARM_DESC(fw_alias_reserved,
		 "T6021: map reserved SEG0 0x10000848000+0xc4000 at entry and SEG1 "
		 "0x10001400000+0x438000 after it (default on); 0 = run the staged "
		 "copy with iBoot's patches replayed (own memory, the only mode on "
		 "T6020/T6022).");

static bool ane_t6021_fw_alias_is_reserved(const struct ane_t6021 *ane)
{
	const struct ane_t602x_soc *soc = of_device_get_match_data(ane->dev);

	return fw_alias_reserved && soc->preload_placement;
}

bool ane_t6021_fwload_requested(void)
{
	return fw_load;
}

/* DART page size on t6021 (apple_dart probe line: "pagesize 4000");
 * fw_size is a multiple. Shared with the probe-top predicate below. */
#define ANE_T6021_FW_ALIAS_PAGE	0x4000

bool ane_t6021_fwload_options_ok(void)
{
	/* BINDING probe-top predicate. MUST be called before
	 * devm_kzalloc / power / CPU release at every probe site.
	 * The alloc-time check below runs as defense in depth.
	 * The lab envelope rule: 16 KiB-aligned, <= 16 MiB. Both alias
	 * modes map the whole allocation, so the grant needs no mode. */
	return fw_extra_ram <= SZ_16M &&
	       IS_ALIGNED(fw_extra_ram, ANE_T6021_FW_ALIAS_PAGE);
}

#define ANE_FW_NAME "apple/ane/t602x_ane0_fw_selene_rc4x.macho"

/* ANE sub-block power registers (pmgr 0x28e080000 + 0x4000: ane_sys_mpm,
 * ane_td, ane_base, ane_set1..4). The firmware's power service programs
 * them through its DART at IOVA == PA once SET_SNE_PMU_BASE2 (0x29) sets
 * its base (fw 13.5 SetPMUBaseAddress 0x62694 stores 0x28e084008). macOS
 * maps the page first; without it the first access faults
 * (NO PMD FOR IOVA 0x28e084008, 2026-09-29) and the firmware halts. */
#define ANE_T6021_PMU_PA	0x28e084000ull

static int ane_t6021_pmu_map(struct ane_t6021 *ane, struct iommu_domain *dom)
{
	int prot = IOMMU_READ | IOMMU_WRITE;
	int ret;

	if (dev_is_dma_coherent(ane->dev))
		prot |= IOMMU_CACHE;
	ret = iommu_map(dom, ANE_T6021_PMU_PA, ANE_T6021_PMU_PA, ANE_T6021_FW_ALIAS_PAGE,
			prot, GFP_KERNEL);
	if (!ret && iommu_iova_to_phys(dom, ANE_T6021_PMU_PA) != ANE_T6021_PMU_PA)
		ret = -EIO;
	dev_info(ane->dev, "pmu: DART map %#llx (IOVA == PA, %#x bytes): %d\n",
		 ANE_T6021_PMU_PA, ANE_T6021_FW_ALIAS_PAGE, ret);
	return ret;
}

static int ane_t6021_fw_alias_map(struct ane_t6021 *ane, bool reserved)
{
	struct iommu_domain *dom = iommu_get_domain_for_dev(ane->dev);
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u64 rvbar = readq(eng + ANE_ASC_RVBAR);
	u64 entry = ane_t6021_rvbar_entry_bits(rvbar);
	phys_addr_t pa0 = 0;
	int prot = IOMMU_READ | IOMMU_WRITE;
	u64 off;
	int ret;

	if (!dom)
		return -ENODEV;
	if (!ane_t6021_rvbar_latched(rvbar) || !entry) {
		/* Unlatched branch: the boot path programs RVBAR to the
		 * fw DVA itself (ane_t6021_rvbar_compose), no alias. */
		dev_info(ane->dev,
			 "fwalias: rvbar %016llx not latched/entry 0 — skip (boot reprograms RVBAR)\n",
			 rvbar);
		return 0;
	}
	if (!ane_t6021_rvbar_entry_ok(entry) ||
	    entry & (ANE_T6021_FW_ALIAS_PAGE - 1)) {
		dev_err(ane->dev,
			"fwalias: entry %#llx not %#x-aligned in fold\n",
			entry, ANE_T6021_FW_ALIAS_PAGE);
		return -EINVAL;
	}
	if (entry + ane->fw_size - 1 > dom->geometry.aperture_end) {
		dev_err(ane->dev,
			"fwalias: entry %#llx+%#x outside aperture %#llx\n",
			entry, ane->fw_size,
			(unsigned long long)dom->geometry.aperture_end);
		return -ERANGE;
	}
	if (dev_is_dma_coherent(ane->dev))
		prot |= IOMMU_CACHE;

	if (reserved) {
		/* Preloaded placement: the two reserved windows mapped at
		 * the entry IOVAs. SEG0 0xc4000 is TEXT and SEG1 0x438000
		 * is DATA (the 13.5 layout, ane_fw_expected_segs).
		 * The windows are adjacent, so the mapping is one
		 * contiguous run of `mapped` bytes starting at entry —
		 * tracked exactly, because teardown must never unmap a
		 * page that was not mapped (dart_unmap_pages WARNs on
		 * holes and the 2026-09-26 state-report unwind hit it).
		 * SEG1 IOVA = entry + 0xc4000 = 0x100000c4000 per the
		 * pinned ADT segment-ranges record (0x10000000000 ->
		 * 0x10000848000 0xc4000; 0x100000c4000 -> 0x10001400000
		 * 0x438000). Commit e6612e9 shipped 0x1000000c4000 — one
		 * hex digit off, mapping DATA at 16 TiB instead: every
		 * fw_alias_reserved boot since 2026-09-24 left the fw
		 * DATA section unmapped past the SEG0 head. */
		struct { u64 iova, phys, len; } win[] = {
			{ 0x10000000000ull, 0x10000848000ull, 0xc4000ull },
			{ 0, 0x10001400000ull, 0x438000ull },
			{ entry + 0x4fc000, 0, fw_extra_ram ? ane->fw_size - 0x4fc000 : 0 },
		};
		unsigned int w, windows = fw_extra_ram ? ARRAY_SIZE(win) : 2;

		/* The remap is contiguous: SEG1 base = SEG0 base + SEG0
		 * len (segment-ranges order). Derived, never hand-written
		 * — the standalone literal 0x1000000c4000 that shipped in
		 * e6612e9 was one digit off and mapped DATA at 16 TiB.
		 * The remap base must also equal the latched entry, or
		 * the fetch head is not where we mapped. */
		win[1].iova = win[0].iova + win[0].len;
		if (win[0].iova != entry) {
			dev_err(ane->dev,
				"fwalias: remap base %#llx != latched entry %#llx\n",
				win[0].iova, entry);
			return -EINVAL;
		}

		for (w = 0; w < windows; w++) {
			u64 o;

			for (o = 0; o < win[w].len; o += ANE_T6021_FW_ALIAS_PAGE) {
				phys_addr_t pa = w < 2 ? win[w].phys + o :
					iommu_iova_to_phys(dom, ane->fw_iova + win[w].iova + o - entry);
				if (!pa || !IS_ALIGNED(pa, ANE_T6021_FW_ALIAS_PAGE)) {
					ret = -EFAULT;
					goto err_unmap_mapped;
				}
				if (iommu_iova_to_phys(dom, win[w].iova + o)) {
					dev_err(ane->dev,
						"fwalias: reserved entry +%#llx mapped — refusing\n",
						win[w].iova + o - entry);
					ret = -EEXIST;
					goto err_unmap_mapped;
				}
				ret = iommu_map(dom, win[w].iova + o,
						pa,
						ANE_T6021_FW_ALIAS_PAGE, prot,
						GFP_KERNEL);
				if (ret) {
					dev_err(ane->dev,
						"fwalias: reserved map +%#llx: %d\n",
						win[w].iova + o - entry, ret);
					goto err_unmap_mapped;
				}
				/* per-window successfully mapped bytes */
				ane->fw_alias_ext_len[w] = o + ANE_T6021_FW_ALIAS_PAGE;
				ane->fw_alias_ext_iova[w] = win[w].iova;
				if (iommu_iova_to_phys(dom, win[w].iova + o) != pa) {
					ret = -EIO;
					goto err_unmap_mapped;
				}
			}
		}
		ane->fw_alias_extn = windows;
		ane->fw_alias_iova = entry;
		dev_info(ane->dev,
			 "fwalias: reserved SEG0/SEGi at entry %#llx (%llx+%zx %llx+%zx, preloaded placement)\n",
			 entry,
			 ane->fw_alias_ext_iova[0], ane->fw_alias_ext_len[0],
			 ane->fw_alias_ext_iova[1], ane->fw_alias_ext_len[1]);
		if (fw_extra_ram)
			dev_info(ane->dev, "fwalias: owned heap [%#llx,%#llx) roundtrip verified\n",
				 win[2].iova, win[2].iova + win[2].len);
		return ane_t6021_pmu_map(ane, dom);

err_unmap_mapped:
		/* Cleanup exactly the per-window bytes we mapped; windows
		 * are not assumed adjacent (live trace: a hole between
		 * SEG0 and SEGi), and foreign collision mappings are
		 * never touched. */
		for (w = 0; w < windows; w++)
			if (ane->fw_alias_ext_len[w])
				iommu_unmap(dom, ane->fw_alias_ext_iova[w],
					    ane->fw_alias_ext_len[w]);
		memset(ane->fw_alias_ext_len, 0, sizeof(ane->fw_alias_ext_len));
		ane->fw_alias_extn = 0;
		return ret;
	}

	for (off = 0; off < ane->fw_size; off += ANE_T6021_FW_ALIAS_PAGE) {
		phys_addr_t pa = iommu_iova_to_phys(dom, ane->fw_iova + off);

		if (!pa || pa & (ANE_T6021_FW_ALIAS_PAGE - 1)) {
			dev_err(ane->dev,
				"fwalias: fw +%#llx untranslated (%pa)\n",
				off, &pa);
			ret = -EFAULT;
			goto err_unmap;
		}
		if (iommu_iova_to_phys(dom, entry + off)) {
			dev_err(ane->dev,
				"fwalias: entry +%#llx already mapped — refusing\n",
				off);
			ret = -EEXIST;
			goto err_unmap;
		}
		ret = iommu_map(dom, entry + off, pa,
				ANE_T6021_FW_ALIAS_PAGE, prot, GFP_KERNEL);
		if (ret) {
			dev_err(ane->dev, "fwalias: iommu_map +%#llx: %d\n",
				off, ret);
			goto err_unmap;
		}
		if (!off)
			pa0 = pa;
	}

	/* full per-page roundtrip: every alias page must resolve to the
	 * same PA as its fw source page (not just page 0) */
	for (off = 0; off < ane->fw_size; off += ANE_T6021_FW_ALIAS_PAGE) {
		if (iommu_iova_to_phys(dom, entry + off) !=
		    iommu_iova_to_phys(dom, ane->fw_iova + off)) {
			dev_err(ane->dev,
				"fwalias: roundtrip mismatch at +%#llx\n", off);
			ret = -EIO;
			/* Mapping finished: unwind every page, not only the
			 * prefix already checked by this verification loop. */
			off = ane->fw_size;
			goto err_unmap;
		}
	}

	ane->fw_alias_iova = entry;
	ane->fw_alias_ext_iova[0] = entry;
	ane->fw_alias_ext_len[0] = ane->fw_size;
	ane->fw_alias_extn = 1;
	dev_info(ane->dev,
		 "fwalias: entry %#llx <- %u dart pages aliased from fw %pad (first %pa, roundtrip OK)\n",
		 entry, ane->fw_size / ANE_T6021_FW_ALIAS_PAGE,
		 &ane->fw_iova, &pa0);
	/* The firmware's power service needs the pmgr sub-block mapped
	 * IOVA == PA in every vehicle (NO PMD FOR IOVA 0x28e084008,
	 * 2026-09-29); the staged-DMA alias branch skipped it and left
	 * a bisect run booting a halting fw. */
	return ane_t6021_pmu_map(ane, dom);

err_unmap:
	if (off)
		iommu_unmap(dom, entry, off);
	return ret;
}

/* ACTUAL iBoot-preloaded payload: macOS 13.5 (22G74) selene — root
 * preload capture 20260926T230146 byte-verified the reserved windows
 * against this exact archive (SHA-256
 * a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc).
 * The previous pin (9f7915c4…) was the other-generation blob: the
 * loader validated its bytes while the reserved-alias boot executed
 * 13.5. */
static const u8 ane_fw_sha256_expected[32] = {
	0xa9, 0xc4, 0xb7, 0x71, 0x29, 0x4a, 0x6b, 0x11,
	0x56, 0x24, 0xd9, 0x48, 0x0a, 0x62, 0x48, 0xd0,
	0x89, 0x9a, 0x16, 0x81, 0xa5, 0x75, 0xe8, 0x65,
	0x07, 0x0b, 0x87, 0xa3, 0x24, 0x84, 0x27, 0xbc,
};

/* Own memory (header item 4): iBoot's runtime patches, with values from
 * the running system: the latched entry (or the staged DVA that the boot
 * path programs when RVBAR is not latched), the DT engine window (probe
 * already refused a node without it), the compatible, a fresh guard. */
static int ane_t6021_fw_patch(struct ane_t6021 *ane, u8 *img)
{
	const struct ane_t602x_soc *soc = of_device_get_match_data(ane->dev);
	struct resource *res = platform_get_resource(to_platform_device(ane->dev),
						     IORESOURCE_MEM, 0);
	u64 rvbar = readq(ane->base[ANE_T6021_REG_ENGINE] + ANE_ASC_RVBAR);
	u64 entry = ane_t6021_rvbar_latched(rvbar) ?
		    ane_t6021_rvbar_entry_bits(rvbar) : 0;
	u64 guard = get_random_u64();
	struct ane_fw_boot_patch p = {
		.exec_base = entry ?: ane->fw_iova,
		/* iBoot's guards have one zero byte at a random position */
		.stack_guard = guard & ~(0xffull << (8 * (guard >> 61))),
		.soc = soc->soc,
		.soc_revision = ANE_T602X_SOC_REVISION,
		.cpu_pa = res->start + ANE_ASC_CPU_BASE,
		.wrapper_pa = res->start + ANE_ASC_WRAPPER_BASE,
	};
	const char *reason = NULL;

	if (ane_fw_apply_boot_patches(img, &p, &reason)) {
		dev_err(ane->dev, "fwload: own memory: %s\n", reason);
		return -EINVAL;
	}
	dev_info(ane->dev,
		 "fwload: own memory: iBoot patches replayed (soc %#x rev %#x DATA %#llx cpu %#llx wrapper %#llx)\n",
		 p.soc, p.soc_revision,
		 p.exec_base + ane_fw_expected_segs[1].vmaddr, p.cpu_pa,
		 p.wrapper_pa);
	return 0;
}

int ane_t6021_fwload_probe(struct ane_t6021 *ane)
{
	const struct firmware *fw = NULL;
	struct ane_fw_seg segs[ANE_FW_NSEGS];
	u64 entry = 0;
	u8 actual_sha[32];
	void *buf;
	dma_addr_t iova;
	unsigned int i;
	const char *reason = NULL;
	u32 alloc_size = ANE_FW_BUF_SIZE + fw_extra_ram;
	int ret;
	bool reserved = ane_t6021_fw_alias_is_reserved(ane);

	if (!fw_load)
		return 0;
	if (fw_extra_ram > SZ_16M || !IS_ALIGNED(fw_extra_ram, ANE_T6021_FW_ALIAS_PAGE))
		return -EINVAL;
	/* The 64-bit coherent mask is set once in ane_t6021_probe,
	 * BEFORE rtkit_init allocates the rings (W15 review). */

	ret = request_firmware(&fw, ANE_FW_NAME, ane->dev);
	if (ret) {
		dev_err(ane->dev,
			"fwload: request_firmware(%s): %d — stage the payload "
			"under /lib/firmware/apple/ane/\n", ANE_FW_NAME, ret);
		return ret;
	}

	sha256(fw->data, fw->size, actual_sha);
	ret = ane_fw_validate_blob(fw->data, fw->size,
				   ane_fw_sha256_expected, actual_sha,
				   segs, &entry, &reason);
	if (ret) {
		dev_err(ane->dev, "fwload: validation failed: %s\n",
			reason ? reason : "?");
		release_firmware(fw);
		return ret;
	}

	buf = dma_alloc_coherent(ane->dev, alloc_size, &iova, GFP_KERNEL);
	if (!buf) {
		dev_err(ane->dev, "fwload: coherent alloc %#x failed\n",
			alloc_size);
		release_firmware(fw);
		return -ENOMEM;
	}

	for (i = 0; i < ANE_FW_NSEGS; i++) {
		if (segs[i].filesize)
			memcpy(buf + segs[i].vmaddr,
			       fw->data + segs[i].fileoff, segs[i].filesize);
	}

	ane->fw_buf = buf;
	ane->fw_iova = iova;
	ane->fw_size = alloc_size;

	dev_info(ane->dev,
		 "fwload: selene PRELOAD validated + DART-mapped: 3 segs, "
		 "entry %#llx, iova %pad size %#x\n",
		 entry, &iova, alloc_size);

	ret = reserved ? 0 : ane_t6021_fw_patch(ane, buf);
	if (!ret)
		ret = ane_t6021_fw_alias_map(ane, reserved);
	if (ret) {
		ane_t6021_fwload_remove(ane);
		release_firmware(fw);
		return ret;
	}

	release_firmware(fw);
	return 0;
}

void ane_t6021_fwload_remove(struct ane_t6021 *ane)
{
	if (ane->fw_alias_extn) {
		struct iommu_domain *dom = iommu_get_domain_for_dev(ane->dev);
		int i;

		/* Unmap exactly the per-window recorded extents: the
		 * reserved-alias windows are not assumed adjacent (live
		 * trace hole between SEG0 and SEGi) and unmapping bytes
		 * that were never mapped trips dart_unmap_pages
		 * (io-pgtable-dart.c:319 WARN, 2026-09-26). */
		if (dom)
			for (i = 0; i < ane->fw_alias_extn; i++)
				iommu_unmap(dom, ane->fw_alias_ext_iova[i],
					    ane->fw_alias_ext_len[i]);
		ane->fw_alias_extn = 0;
	}
	if (!ane->fw_buf)
		return;
	dma_free_coherent(ane->dev, ane->fw_size, ane->fw_buf, ane->fw_iova);
	ane->fw_buf = NULL;
	ane->fw_size = 0;
}
