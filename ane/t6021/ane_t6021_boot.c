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
 *   3. Ack model — TWO-PHASE, SINGLE REGISTER, TRANSITION-BASED
 *      (rvbar-lifecycle receipt 6288b0b, 57/57 anchors; supersedes the
 *      pass6 SCRATCH0-beacon reading): pre-CPU, SCRATCH cells are
 *      cleared and SCRATCH7 is PULSED 1 -> 0 (stale READY/wake
 *      cleared); after CPU_CONTROL 0 -> 0x10, poll A demands a FRESH
 *      SCRATCH7 READY 0x08042006 (fw alive; fw_alive). Publication
 *      (init suballoc DVA -> SCRATCH0/1, dsb st) happens ONLY after
 *      poll A; then wake 0xf7fbdff9 -> SCRATCH7 releases the fw; poll
 *      B awaits SCRATCH7 DONE 0x08042006 (booted) and reads back
 *      SCRATCH0/1. Because of the pulse, a post-CPU READY is
 *      unambiguous — no stale-ack hazard. The SCRATCH0 marker (fn
 *      0x86EC) is not a gate.
 *      Selene raw anchors: READY write idx7 @0x7360-0x7388 (movz
 *      0x2006 @0x7364, mov w1,#7 @0x7368, movk 0x804 @0x736c, slot
 *      byte48 @0x7380, salt 0x5bdd @0x7384, blraa @0x7388); poll loop
 *      READ32 idx7 @0x73c4-0x73e4, cmp wake 0xf7fbdff9 @0x73e8,
 *      sleep(1000) @0x73f8-0x73fc; DONE ack @0x77cc-0x77f0.
 *   4. Linux allocation map (legacy branch sizes): FWIM surface =
 *      config+0x138 byte-count = 0x500000 (Main, audit 751caa4);
 *      Linux fw_buf is ANE_FW_BUF_SIZE = 0x500000 — the semantic is
 *      honored directly, covering the image vmsize 0x36c000. 'IPC '
 *      surface = min(config+4, dev+0x3A70
 *      cap) — the numeric cap needs the h14g config blob field map
 *      (open prerequisite). Init suballoc = 0x174 from a Linux-owned
 *      pool standing in for the kext dev+0x968/dev+0x980 pool (pool
 *      total size open). RTBuddy FW_INIT sizes (64K…) are the OTHER
 *      branch — never used here.
 *   5. Init publication: the fw consumes [0x08]..[0x68] (pass5) and
 *      the Linux sources of those fields are not pinned ([0x08] =
 *      *(dev+0x988+0x18) = the 'IPC ' surface DVA — the surface
 *      itself is CLOSED (pass5c/5d), its Linux allocation size open
 *      per item 4; [0x10]/[0x18] = config-size terms with
 *      config+0x138 = image byte-count closed; [0x30] = dev+0x990
 *      load-progress word) — publication cannot fire until every
 *      fw-read field has a pinned Linux source.
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
#include <linux/iopoll.h>
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

/* Poll A/B bound: the kext polls <=1000 x sleep(1ms) (selene poll
 * loop 0x73c4-0x73fc analog; rvbar-lifecycle step 6/10). */
#define ANE_BOOT_ACK_POLL_US	1000
#define ANE_BOOT_POLL_MS	1000

/* The COMPLETE resolved legacy-boot sequence (rvbar-lifecycle receipt
 * 6288b0b, linux_first_boot_order 1-10, 57/57 anchors; selene raw
 * anchors per the ack-model comment). UNREACHABLE until
 * boot_preflight_complete flips in a Main-reviewed commit. Steps:
 *
 *   S1  SCRATCH6 <- 1; pulse SCRATCH7 1 -> 0 (stale READY/wake
 *       cleared BEFORE CPU start; informational reads logged).
 *   S2  RVBAR read64: bit0 clear -> write64 entry fold; bit0 set ->
 *       lawful skip (no reset before first attempt).
 *   S3  CPU_CONTROL write32 0 then 0x10 (both paths); cpu_started.
 *   S4  Poll A: SCRATCH7 == ACK, 1000 x 1ms -> FRESH READY = fw alive
 *       (the pulse made it unambiguous). Timeout: HOLD — no in-kernel
 *       power cycle (PMGR ps writes are the 2026-09-19 freeze class);
 *       recovery is a Main-decided operation on a live state.
 *   S5  Publication — STILL FENCED on the init-structure opens
 *       ([0x20]/[0x28]/[0x30]/[0x50] producers, IPC cap numeric, pool
 *       total size): fill + dsb st + SCRATCH0/1 publish cannot run.
 *   S6  Wake: SCRATCH7 <- 0xf7fbdff9.
 *   S7  Poll B: SCRATCH7 DONE -> booted; read back SCRATCH0/1 u64. */
static int ane_t6021_boot_start(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u64 rvbar;
	u32 v;
	int err;

	/* S1: scratch clear + stale-ack pulse (pre-CPU). */
	writel(1, eng + ANE_MBI_SCRATCH6);
	writel(1, eng + ANE_MBI_SCRATCH7);
	writel(0, eng + ANE_MBI_SCRATCH7);
	dev_info(ane->dev,
		 "boot S1: SCRATCH6 <- 1, SCRATCH7 pulsed 1->0 (stale ack cleared); post-pulse reads scratch6=%08x scratch7=%08x\n",
		 readl(eng + ANE_MBI_SCRATCH6),
		 readl(eng + ANE_MBI_SCRATCH7));

	/* S2: RVBAR skip-or-fold. */
	rvbar = readq(eng + ANE_ASC_RVBAR);
	if (!ane_t6021_rvbar_latched(rvbar)) {
		u64 entry = ane_t6021_rvbar_compose(ane->fw_iova);

		writeq(entry, eng + ANE_ASC_RVBAR);
		rvbar = readq(eng + ANE_ASC_RVBAR);
		dev_info(ane->dev,
			 "boot S2: RVBAR <- %016llx readback %016llx\n",
			 entry, rvbar);
	} else {
		dev_info(ane->dev,
			 "boot S2: bit0 set — lawful skip (entry bits %0llx); no latch override\n",
			 ane_t6021_rvbar_entry_bits(rvbar));
	}

	/* S3: CPU release, both paths, strictly 0 then 0x10. */
	writel(0, eng + ANE_ASC_CPU_CONTROL);
	writel(ANE_T6021_CPU_RUN_RELEASE, eng + ANE_ASC_CPU_CONTROL);
	ane->cpu_started = true;
	dev_info(ane->dev,
		 "boot S3: CPU_CONTROL <- 0x10 (RUN released); cpu_status=%08x\n",
		 readl(eng + ANE_ASC_CPU_STATUS));

	/* S4: poll A — FRESH READY. Past this write there is no safe
	 * teardown: timeout HOLDS state (no power cycle in-kernel). */
	err = readl_poll_timeout(eng + ANE_MBI_SCRATCH7, v,
				 v == ANE_T6021_BOOT_ACK,
				 ANE_BOOT_ACK_POLL_US,
				 ANE_BOOT_POLL_MS * 1000);
	if (err) {
		dev_err(ane->dev,
			"boot S4: READY poll TIMEOUT (%ums, SCRATCH7=%08x) — CPU START UNCONFIRMED; HOLDING state (no teardown, no in-kernel power cycle — recovery vehicle is a Main decision); sessions stay fenced\n",
			ANE_BOOT_POLL_MS, v);
		return 0;
	}
	ane->fw_alive = true;
	dev_info(ane->dev,
		 "boot S4: fresh READY SCRATCH7=%08x SCRATCH6=%08x — fw alive\n",
		 v, readl(eng + ANE_MBI_SCRATCH6));

	/* S5: publication — fenced on the init-structure opens. */
	dev_err(ane->dev,
		"boot S5: publication FENCED — init fields without pinned Linux sources: [0x20] (obj2 +0x18), [0x28] (w22), [0x30] (fw-load progress semantics), [0x50] ([x23+4] object), IPC size cap (dev+0x3A70), pool total size; zero-gap contract requires the sources first\n");
	return -ENODATA;
}

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
		/* Lawful normal branch (rvbar-lifecycle 6288b0b): bit0 set
		 * means SKIP the RVBAR write — no latch override, no reset
		 * before the first attempt. The sequence still converges on
		 * CPU_CONTROL 0 -> 0x10 and a FRESH SCRATCH7 READY demand
		 * (the pre-CPU pulse cleared any stale ack). Reset recovery
		 * (domain power cycle) is a retry-only path the kext runs
		 * after a poll-A timeout — never a first-attempt step, and
		 * the PMGR writes belong to a Main-decided vehicle. */
		dev_info(ane->dev,
			 "boot: bit0 set, entry bits %0llx — lawful skip branch: no RVBAR write, CPU_CONTROL 0->0x10 and fresh-READY poll next when the preflight opens\n",
			 ane_t6021_rvbar_entry_bits(rvbar));

	/* Preflight gate (Main 2026-09-20): boot writes — INCLUDING the
	 * S1 pulse — fire only when EVERY prerequisite below is closed
	 * by a cited commit, and then the sequence runs start-to-finish.
	 * With fw_boot=1 this -ENODATA FAILS the probe before any
	 * write; with fw_boot=0 this function returned at the fence
	 * above. When the preflight closes, this gate dispatches to the
	 * full S1-S7 sequence in ane_t6021_boot_start(). */
	if (!boot_preflight_complete) {
		dev_err(ane->dev,
		"boot: BLOCKED (probe fails while fw_boot=1; NO MMIO write performed) — preflight open on: (1) provider enableDeviceClock/enableDevicePower gate-ID arrays vs the genpd raise; (2) init-structure opens: [0x20]/[0x28]/[0x30]/[0x50] producers, IPC size cap dev+0x3A70 numeric, pool total size; RVBAR lifecycle RESOLVED (bit0-set = lawful skip branch, pulse clears stale ack, no reset before first attempt; domain power cycle = poll-A-timeout retry only, never in-kernel). cpu_started=%u fw_alive=%u booted=%u\n",
		ane->cpu_started, ane->fw_alive, ane->booted);
		return -ENODATA;
	}

	return ane_t6021_boot_start(ane);
}

bool ane_t6021_boot_requested(void)
{
	return fw_boot;
}
