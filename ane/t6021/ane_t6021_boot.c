// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_t6021_boot.c — T6021 boot state reporter (W15).
 *
 * READ-ONLY. Main review 2026-09-20: the unresolved pre-CPU power and
 * config-table prerequisites BLOCK every boot write, so this unit
 * performs no MMIO write at all — it resolves the boot-input state,
 * reports it with per-read whitelist provenance, and names the exact
 * missing lifecycle datum. The write path (RVBAR entry fold + CPU RUN
 * release + first-alive ack) lands only after the prerequisites below
 * are pinned; its pure contract units (entry fold, latch/entry decode,
 * SCRATCH u64 split, init-structure fill) live in ane_t6021_boot.h and
 * are regression-tested host-side (tools/h14_boot_regression.c).
 *
 * Blocked boot writes, with the blocking prerequisite each one waits on:
 *   - RVBAR entry fold (ENTRY_BASE | fw DVA & ADDR_MASK; single u64
 *     store, width_proof 0x95e9860→0x9622b0c read / 0x95e9988→0x9622c94
 *     write): semantics of the observed RVBAR read 0x1 are unresolved
 *     (see the fork in ane_t6021_boot_probe) AND the pre-CPU block
 *     below applies regardless.
 *   - CPU_CONTROL write32 0 then 0x10 (rvbar-width local order): waits
 *     on the same pre-CPU block; releasing RUN with an unresolved
 *     entry state is the exact blind start the lane forbids.
 *   - SCRATCH0/1 publication + SCRATCH7 wake: init-structure fields
 *     [0x08] and [0x10,0x38) have no pinned selene fn 0x71A4 consumer
 *     read-set; zero-filling unknown fields and calling the structure
 *     valid is forbidden (Main 2026-09-20).
 *
 * Pre-CPU prerequisites, updated for pass4 (receipts 2026-09-20
 * mapper-callchain pass4 + tools/check_power_anchors.py 33/33, commit
 * 04630ef):
 *   RESOLVED (pass4): the CPU-power branch selector — H14g SoCConfig
 *     zeroes dev+0x3EA0 (0x9613e14, the block that also sets
 *     dev+0x4A0=0x01400044), so EnableCPUClocksAndPower takes the
 *     PLAIN branch: write32(PMGR+0x2e0, 0x0000000f), validate 0xff —
 *     AUTO_ENABLE bit28 never set. On Linux this END STATE is exactly
 *     what the first_resume stage-2/3 gate verifies (act=0xf,
 *     AUTO_ENABLE clear) before any of this code runs — the write is
 *     therefore not replicated (ps-word writes are forbidden by the
 *     standing rule and froze the box 2026-09-19); the gate IS the
 *     equivalence check.
 *   STILL OPEN (each blocks its write below):
 *   1. CLOSED (pass5 c364f24): the pre-CPU engine table writels
 *      eng+0xb38/0xb98/0xbf8 <- 0x01ff01ff are REQUIRED every
 *      EnableANEClocksAndPower — dev+0x784 bit0 has no writer, so the
 *      always-open gate runs the loop unconditionally; the
 *      order-vs-start question is moot. Receiver PROVEN pass4
 *      (dev+0x188 accessor = engine reg[0]); count = config+0x150 =
 *      3. Armed below; the first live firing must ride netconsole
 *      behind the seam logs.
 *   2. Provider first-enable enableDeviceClock(1,dev+0x8F0)/
 *      enableDevicePower(1,&out,dev+0x8F0) (0x95d1d48/0x95d1d90):
 *      internal gate-ID arrays are runtime-populated (open). Linux
 *      substitutes the genpd raise + supplier links (islands verified
 *      ACTUAL=0xf by the gate); the equivalence datum is the open
 *      half.
 *   3. RVBAR lifecycle fork (bit0 set + entry 0): audit lane owns
 *      ANE_CleanupForColdReboot_gated, island power-cycle RVBAR
 *      semantics, dev+0x41f provenance.
 *   4. Init publication: the fw consumes [0x08]..[0x68] (pass5) and
 *      the producing objects (dev+0x988 identity, config.size,
 *      sp136+0x24, dev+0x990 semantics) are not decoded — the
 *      closed-field fill leaves them zero and publication cannot fire
 *      over them. Also open: which site owns the first-alive ack
 *      (fn 0x71A4 acks 0x08042006 at 0x77c8, but 0x86EC also writes
 *      the value via idx0 — SCRATCH0 vs SCRATCH7 depends on the
 *      accessor base at that execution point, not dumped).
 *
 * Read whitelist — every MMIO read below, individually:
 *   - RVBAR eng+0x01050000: read64 width-proven (rvbar-width
 *     width_proof); W8 and W10 live Linux reads, value 0x1 each.
 *   - CPU_STATUS eng+0x1400048: phase-1 S2 whitelist; kext poll site
 *     0x…95ecfb4 (config field dev+0x49c = 0x1400048); W10 live read
 *     0x2a.
 *   - ASC mailbox controls eng+0x1408110/0x1408114: W10 live reads
 *     (both 0x00020001; 600 polls / 30 s, zero changes, zero aborts).
 *     The FATAL mailbox-class reads were the +0x1608xxx h16g analog —
 *     a different address never touched here; the 0x1854000..0x1c04000
 *     fabric-fatal window is likewise never touched.
 *   - SCRATCH eng+0x1840048..0x1840064: phase-1 S2 whitelist
 *     (all-zero pre-attach), W5 pre-read log clean, first_resume
 *     re-reads all eight on every load.
 */

#include <linux/array_size.h>
#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/moduleparam.h>

#include "ane_t6021.h"
#include "ane_t6021_boot.h"

static bool fw_boot;
module_param(fw_boot, bool, 0444);
MODULE_PARM_DESC(fw_boot,
		 "OPT-IN: boot state resolution + report (W15, read-only until the preboot/RVBAR prerequisites land — see ane_t6021_boot.c header).");

/* Boot-write gates — flip ONLY in a commit that cites the receipt
 * closing the named datum (Main review 2026-09-20: no unguarded
 * start; no runtime knobs, a knob would bypass review). */
static const bool preboot_table_ready = true;	/* pass5 c364f24: dev+0x784
						 * bit0 has NO writer
						 * (ctor-zero at 0x959b268;
						 * mutation 0x9600748-54
						 * unreachable) — the table
						 * gate is always open and
						 * the three writels are
						 * REQUIRED preboot; the
						 * order-vs-start question
						 * is moot */

/* Kext pre-CPU engine table (pass4-proven receiver, pass5-proven
 * gate): three writels to the ENGINE aperture, eng+0xb38/0xb98/0xbf8
 * <- 0x01ff01ff (table vm 0xcb6c188, count config+0x150 = 3; loop
 * consumes u32 offset+0 and u32 value+8, skipping value 0xffffffff —
 * none skipped here). REQUIRED every EnableANEClocksAndPower: the
 * dev+0x784 bit0 gate has no writer (ctor-zero 0x959b268; the
 * mutation sites 0x9600748-54 are unreachable), so the kext loop runs
 * unconditionally and so does this. Runs only behind fw_boot=1, the
 * power gate, and the staging/iommu gates; each write logs a
 * netconsole seam line first. */
static int ane_t6021_preboot_table(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	static const struct {
		u32 off;
		u32 val;
	} tbl[] = {
		{ 0xb38, 0x01ff01ff },
		{ 0xb98, 0x01ff01ff },
		{ 0xbf8, 0x01ff01ff },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(tbl); i++) {
		dev_info(ane->dev,
			 "preboot table seam %u/%u: eng+%#05x <- %#010x next\n",
			 i + 1, (unsigned int)ARRAY_SIZE(tbl),
			 tbl[i].off, tbl[i].val);
		writel(tbl[i].val, eng + tbl[i].off);
	}
	return 0;
}

int ane_t6021_boot_probe(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u64 rvbar;
	int err;

	if (!fw_boot) {
		dev_info(ane->dev,
			 "boot: fenced (fw_boot=0); no boot reads or writes\n");
		return 0;
	}

	/* Gates — all read-only; the W3 gate already ran in
	 * first_resume and probe unwound on its failure. */
	if (!ane->power_gated) {
		dev_err(ane->dev,
			"boot: REFUSED — eight-island gate/whitelist not passed\n");
		return -EIO;
	}
	if (!ane->fw_buf) {
		dev_err(ane->dev,
			"boot: REFUSED — no staged firmware (fw_load=1 must succeed first)\n");
		return -EINVAL;
	}
	/* A boot-published address must be a dart-ane0 translation DVA
	 * (live iommu group 6, three apple,t6020-dart streams, DMA
	 * domain — DMA-topology receipt). */
	if (!device_iommu_mapped(ane->dev)) {
		dev_err(ane->dev,
			"boot: REFUSED — device not IOMMU-mapped; a boot-published fw DVA would be untranslated\n");
		return -EINVAL;
	}
	/* Exact fold-loss check: the entry mask clears bits 0..11, 48
	 * and 55. ANY set bit outside the mask would be silently dropped
	 * by the fold — refuse rather than truncate. */
	if (ane->fw_iova & (u64)~ANE_T6021_RVBAR_ADDR_MASK) {
		dev_err(ane->dev,
			"boot: REFUSED — fw iova %pad sets bits the entry fold drops (%016llx outside %016llx); staging placement must be re-examined\n",
			&ane->fw_iova,
			ane->fw_iova & (u64)~ANE_T6021_RVBAR_ADDR_MASK,
			ANE_T6021_RVBAR_ADDR_MASK);
		return -EINVAL;
	}

	/* State report. Reads only — each register is on the whitelist
	 * in the header comment. */
	rvbar = readq(eng + ANE_ASC_RVBAR);
	dev_info(ane->dev,
		 "boot state: rvbar=%016llx (entry bits %0llx, bit0=%u) cpu_status=%08x a2i_ctrl=%08x i2a_ctrl=%08x\n",
		 rvbar, ane_t6021_rvbar_entry_bits(rvbar),
		 ane_t6021_rvbar_latched(rvbar) ? 1u : 0u,
		 readl(eng + ANE_ASC_CPU_STATUS),
		 readl(eng + ANE_ASC_MBOX_A2I_CTRL),
		 readl(eng + ANE_ASC_MBOX_I2A_CTRL));

	if (!ane_t6021_rvbar_latched(rvbar))
		dev_info(ane->dev,
			 "boot: bit0 clear — unprogrammed; the entry fold would be %016llx from fw_iova=%pad\n",
			 ane_t6021_rvbar_compose(ane->fw_iova),
			 &ane->fw_iova);
	else if (ane_t6021_rvbar_entry_bits(rvbar))
		dev_info(ane->dev,
			 "boot: bit0 set WITH entry bits %0llx — a boot entry is already programmed; re-booting over it is the move the kext skip prevents (tbnz w0,#0, ANE_Init 0x95e9878)\n",
			 ane_t6021_rvbar_entry_bits(rvbar));
	else
		dev_info(ane->dev,
			 "boot: bit0 set, entry bits 0 — the value every owned Linux boot read (W8/W10: 0x1). Which boot mode this names is UNRESOLVED: (a) entry 0 names the ASC boot ROM (the kext local order continues CPU_CONTROL 0->0x10 with RVBAR untouched), or (b) a programmed/latched state only a reset lifecycle reaches. No owned disassembly decides between (a) and (b); the audit lane owns ANE_CleanupForColdReboot_gated, island power-cycle RVBAR semantics, and dev+0x41f provenance\n");

	/* Preboot write sequence — each write behind its own named
	 * datum (header). Nothing has fired yet on this host. */
	err = ane_t6021_preboot_table(ane);
	if (err && err != -ENODATA)
		return err;

	/* HARD BLOCK (Main review 2026-09-20): remaining prerequisites
	 * hold ALL further boot writes — not RVBAR, not CPU_CONTROL.
	 * With fw_boot=1 this -ENODATA FAILS the probe (an explicit
	 * boot request does not bind half-armed); with fw_boot=0 this
	 * function returned at the fence above. */
	dev_err(ane->dev,
		"boot: BLOCKED (probe fails while fw_boot=1) — boot writes held on: (1) provider enableDeviceClock/enableDevicePower gate-ID arrays vs the genpd raise; (2) RVBAR mode fork (bit0 set, entry 0 — audit lane: ANE_CleanupForColdReboot_gated, island power-cycle semantics, dev+0x41f); (3) init publication: fw-consumed fields [0x08]..[0x68] have undecoded producers (dev+0x988 identity, config.size, sp136+0x24, dev+0x990) and the first-alive ack site is unattributed (0x77c8 vs 0x86EC). cpu_started=%u fw_alive=%u booted=%u\n",
		ane->cpu_started, ane->fw_alive, ane->booted);
	return -ENODATA;
}

bool ane_t6021_boot_requested(void)
{
	return fw_boot;
}
