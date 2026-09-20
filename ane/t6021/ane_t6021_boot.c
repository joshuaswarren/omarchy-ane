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
 *   3. RVBAR lifecycle — RESOLVED (M2ResetLifecycle,
 *     rvbar-lifecycle-evidence.json, anchors 38/38
 *     check_rvbar_lifecycle_anchors.py): live bit0-set + entry 0 is
 *     the STALE state; the kext-lawful transition is power_off
 *     (ANE_deInit when dev+0x3FB, then PMGR ps 0x2e0 ← 0x00000000 via
 *     the dev+0x190 PMGR accessor) → power_on → re-init, NEVER an
 *     RVBAR write while bit0 is set. RVBAR (engine accessor,
 *     0x1050000, u64) is written only after a read64 with bit0 == 0;
 *     CPU_CONTROL write32 0 then 0x10 runs on BOTH paths; success =
 *     SCRATCH7 (dev+0x454 selector) read32 == 0x08042006. Open edge:
 *     whether ps-off clears bit0 — one live read-only read64 after a
 *     domain-off answers it. IMPLEMENTATION CAVEAT: the ps-word write
 *     is the class that froze this host from kernel context
 *     (2026-09-19 receipt) and the W3 gate demands act=0xf +
 *     AUTO_ENABLE clear afterwards — the reset vehicle (userspace
 *     stage vs in-kernel) is a Main decision, so the fold+start path
 *     stays behind boot_preflight_complete.
 *   4. Init publication: the fw consumes [0x08]..[0x68] (pass5) and
 *      the Linux sources of those fields are not pinned ([0x08] =
 *      *(dev+0x988+0x18), a Params-pattern DVA of a second surface
 *      whose identity is undecoded; [0x10]/[0x18] = config-size terms
 *      with the 0x10000000-config.size formula closed but config.size
 *      identity unconfirmed; [0x30] = dev+0x990 load-progress word) —
 *      the closed-field fill leaves them zero and publication cannot
 *      fire over them. Also open: which site owns the first-alive ack
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

/* Boot-write gates — the ENTIRE write sequence (preboot engine table,
 * RVBAR resolution, CPU start, SCRATCH publication) is gated on ONE
 * complete-preflight flag: it runs start-to-finish once EVERY named
 * prerequisite is closed by a cited commit, or not at all (Main
 * 2026-09-20: no partial mutating boot for diagnostics; writes behind
 * complete preflight). Flipping this flag is a Main-reviewed commit,
 * not a runtime knob. */
static const bool boot_preflight_complete = false;

int ane_t6021_boot_probe(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u64 rvbar;

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
	/* Acceptance predicate (shared with the regression): the staged
	 * DVA must lose NOTHING to the entry fold — bit 11 survives,
	 * bits 0-10/48/55 do not, so any of those set means the staging
	 * placement is wrong for the fold and we refuse instead of
	 * truncating. */
	if (!ane_t6021_rvbar_entry_ok(ane->fw_iova)) {
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
			 "boot: bit0 set, entry bits 0 — STALE state (M2ResetLifecycle rvbar-lifecycle-evidence 38/38: kext remedy is power_off (PMGR ps 0x2e0 <- 0 via dev+0x190) -> power_on -> re-init, NEVER an RVBAR write while bit0 is set; RVBAR write requires read64 bit0 == 0 first). Reset vehicle unresolved: the ps-word write froze this host from kernel context (2026-09-19); userspace stage vs in-kernel cycle is a Main decision. Open edge: whether ps-off clears bit0 — one live read-only read64 after domain-off answers it\n");

	/* HARD BLOCK before ANY MMIO write (Main 2026-09-20): the whole
	 * write sequence — preboot engine table (eng+0xb38/0xb98/0xbf8
	 * <- 0x01ff01ff), RVBAR resolution, CPU_CONTROL release, SCRATCH
	 * publication — sits behind boot_preflight_complete and runs
	 * start-to-finish or not at all. The pass5 "no writer of
	 * dev+0x784 bit0" claim is itself under review (alias/indirect
	 * absence not definitive), and pass5's init header table is
	 * being corrected (fields +0x50/+0x58/+0x60 missing). With
	 * fw_boot=1 this -ENODATA FAILS the probe before any write;
	 * with fw_boot=0 this function returned at the fence above. */
	dev_err(ane->dev,
		"boot: BLOCKED (probe fails while fw_boot=1; NO MMIO write performed) — preflight open on: (1) provider enableDeviceClock/enableDevicePower gate-ID arrays vs the genpd raise; (2) RVBAR mode fork (bit0 set, entry 0 — M2ResetLifecycle: ANE_CleanupForColdReboot_gated, island power-cycle semantics, dev+0x41f); (3) preboot table gate dev+0x784 writer proof (alias/indirect review) plus init publication fields [0x08]..[0x68] Linux sources ([0x08] = *(dev+0x988+0x18) second-surface DVA, [0x10]/[0x18] config-size terms, +0x50/+0x58/+0x60 pending pass5b) and the first-alive ack site (0x77c8 vs 0x86EC). cpu_started=%u fw_alive=%u booted=%u\n",
		ane->cpu_started, ane->fw_alive, ane->booted);
	return -ENODATA;
}

bool ane_t6021_boot_requested(void)
{
	return fw_boot;
}
