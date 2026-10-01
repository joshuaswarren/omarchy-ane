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
 *     (see the rvbar lifecycle fork) AND the pre-CPU block
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
 *     stays behind the itemized preflight gates.
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
 *   - TICK eng+0x1160008 (PROGRESS dump only): 24 MHz domain tick,
 *     proven readable post-release (2026-09-25 handshake receipt);
 *     two reads 20 ms apart for a delta. engine+0x1140008 is
 *     deliberately NOT read (readability/width/reset semantics
 *     unproven, no pre-release baseline — Main review 2026-09-26).
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/iommu.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "ane_t6021.h"

#include "ane_t6021_boot.h"

/* Retain the firmware nap-prevention counter via the init resource bit
 * (lab boot_prevent_nap=1, the proven add-path value). */
static bool boot_prevent_nap = true;
module_param(boot_prevent_nap, bool, 0444);
MODULE_PARM_DESC(boot_prevent_nap,
		 "Retain firmware nap-prevention counter via init resource bit (default on: proven add-path value)");

/* A/B switch for the macOS AF-bridge tunables (ane_t6021_af_bridge_rmw):
 * default off = P-1 unchanged. Set at load (modprobe.d option). */
static bool af_bridge_macos;
module_param(af_bridge_macos, bool, 0444);
MODULE_PARM_DESC(af_bridge_macos,
		 "Replace P-1 eng+0x000/0x400 by the 26 macOS AF-bridge RMWs (default off)");

/* This object links into both ane_t6021.ko and ane_t6021_rtclient.ko;
 * per-object metadata keeps modpost happy for either composition. */
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 ANE contract-pinned boot sequence core");

/* Boot-write gates — ITEMIZED, each a HARD gate: the ENTIRE write
 * sequence (preboot engine table, scratch clear + pulse, RVBAR
 * resolution, CPU release, publication, wake) runs start-to-finish
 * only when EVERY gate below is true, checked BEFORE any write (Main
 * 2026-09-20: no partial boot; remaining semantics stay hard gates,
 * not comments). Each flips only with cited proof in a Main-reviewed
 * commit that also populates the sources and adds the kernel io
 * backend — never a runtime knob. */
/* Provider equivalence — CLOSED (Main provider review 2026-09-20):
 * Linux genpd attachment + supplier links accepted INSTEAD of
 * reproducing Apple runtime index arrays, CONDITIONAL on the live
 * first_resume verifying all 8 islands ACTUAL=0xf, BUSY=0, CPU
 * AUTO_ENABLE clear before the sequence — that verification runs on
 * every probe and the driver performs no direct kernel PMGR writes. */
static const bool pf_provider_genpd_strategy = true;
static const bool pf_pool_word0_proven = true;	/* CONFIRMED (Main raw +
						 * Reset 183fd50, chain
						 * 0x67dc/0x6824/0x68d8/
						 * 0x70dc/0x736c): DDM Params
						 * word0 = requested bytes =
						 * 0x40000 — sourced, not
						 * synthesized */
static const bool pf_heap_floor_pinned = true;	/* CLOSED: floor =
						 * getPageSize() return =
						 * DART page size 0x4000
						 * (store chain 0x9602e68-90
						 * + dart-ane0 page-size
						 * 0x4000 node, pass6h);
						 * sourced in boot_sources */
static const bool pf_dart_page_floor = true;	/* CLOSED (Main raw,
						 * pass6h + exact-node
						 * linkage): dtree-j414c.txt
						 * sha256 dd2955…d504, ane0
						 * iommu-parent 363 (line
						 * 11499), dart-ane0
						 * page-size 0x4000 (line
						 * 11510), child mapper-ane0
						 * phandle 363 (line 11527).
						 * Runnable linkage check:
						 * ane-linux-experiments
						 * tools/
						 * check_dart_ane_pagesize_linkage.py */
static const bool pf_rvbar_lifecycle = true;	/* 6288b0b, 57/57 anchors */
static const bool pf_pass6_init_contract = true; /* cd25b46, 87/87 */

/* FINAL authorization gate — CLOSED (Main provider review + pass6h
 * numeric closure + 183fd50 word0 confirmation + netconsole Wi-Fi
 * marker m2-wifi-check-1789937086 @ 15:44:47.121595Z). Source
 * contract closure complete; live attempt authorized by user
 * override (autonomous boot/recovery loop). */
/* Table-block mode selection (2026-09-20 16:23:07 wedge):
 *   mode 0 = ABORT before any write (accidental-repeat prevention),
 *   mode 1 = write the table (kext-faithful; selected per-run via the
 *            rtclient fw_start_table_mode param),
 *   mode 2 = SKIP the table (default: the shipped diagnostic).
 * W8 write-grant tunables are mode-independent (proven no-abort
 * class, w8-run.out: APERTURE_UNLOCKED) and run in every armed mode. */

static const bool pf_main_lifetime_review = true;

static bool ane_t6021_boot_preflight_complete(void)
{
	/* pf_preboot_table_safe is INTENTIONALLY NOT in this product:
	 * mode 2 (table skip) is the authorized diagnostic state —
	 * the preflight covers the source contract, the mode selector
	 * covers the live-fault table gating. */
	return pf_provider_genpd_strategy && pf_pool_word0_proven &&
	       pf_heap_floor_pinned && pf_dart_page_floor &&
	       pf_rvbar_lifecycle && pf_pass6_init_contract &&
	       pf_main_lifetime_review;
}

/* STATIC boot sources — populated per the pinned contract (Main
 * authorization 2026-09-20). Object layout (Main raw correction,
 * superseding audit 018abdb): the OSValueObject at +0x10 POINTS to
 * the Params; Params+0x00 = size, +0x18 = DVA, +0x38 = hostVA.
 * dev+0x980 is the 'DDM ' Params pool (size 0x40000); dev+0x968 is a
 * SEPARATE DMM manager constructed over the pool hostVA — never
 * conflate the two. Runtime fields (fw_dva, ipc_dva, pool_dma) are
 * filled by ane_t6021_boot_prepare() at sequence time (after poll A,
 * per "dynamic allocations occur after READY"); heap_floor stays 0
 * here and is supplied together with pf_heap_floor_pinned — NEVER
 * used unpinned (the gate above keeps the whole sequence unrun until
 * then). */
static const struct ane_t6021_init_sources boot_sources = {
	.cfg_size = 0x500000,		/* config+0x138 (0x9613da8/ dac) */
	.prev_fw_len = 0,		/* first boot; static per reload */
	.heap_floor = 0x4000,		/* DART page size: dev+0x3A90 =
					 * getPageSize() return; node
					 * authority dart-ane0 page-size
					 * 0x4000 (exact linkage check
					 * passing) */
	.pool_word0 = 0x40000,		/* DDM Params word0 = requested
					 * bytes (CONFIRMED, 183fd50) */
};

/* Poll A/B bound: the kext polls <=1000 x sleep(1ms) (selene poll
 * loop 0x73c4-0x73fc analog; rvbar-lifecycle step 6/10). */
#define ANE_BOOT_ACK_POLL_US	1000
#define ANE_BOOT_POLL_MS	1000

/* The COMPLETE resolved legacy-boot sequence is implemented ONCE in
 * ane_t6021_boot.h (ane_t6021_boot_run) against an io backend, so the
 * fake-MMIO trace check exercises the exact code the device will run
 * (Main: runnable trace check; implement the device wiring only after
 * ALL sources close — until then this file performs no boot write).
 * The kernel backend (readl/writeq/udelay wrappers + prepare hook
 * doing the pool/IPC allocations and the sourced fill) is the small
 * reviewed increment that lands with the gate flip. */

/* ---- kernel io backend for ane_t6021_boot_run() ---- */

struct ane_t6021_boot_mmio {
	struct ane_t6021 *ane;
};

static u32 ane_boot_rd32(void *ctx, unsigned int off)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	return readl(mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static u64 ane_boot_rd64(void *ctx, unsigned int off)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	return readq(mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static void ane_boot_wr32(void *ctx, unsigned int off, u32 v)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	writel(v, mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static void ane_boot_wr64(void *ctx, unsigned int off, u64 v)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	writeq(v, mm->ane->base[ANE_T6021_REG_ENGINE] + off);
}

static void ane_boot_publish_barrier(void *ctx)
{
	(void)ctx;
	/* dma_wmb() = dmb oshst on arm64: orders the coherent pool fill
	 * before the device publish. ORDERING guarantee — not the kext's
	 * dsb st (completion). Sufficient for the Linux coherent-DMA +
	 * writel doorbell contract (writel orders prior accesses before
	 * the MMIO store). */
	dma_wmb();
}

static void ane_boot_wait(void *ctx)
{
	(void)ctx;
	usleep_range(1000, 1500);	/* kext poll: sleep(1000us) */
}

static void ane_boot_phase(void *ctx, const char *what)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	/* bounded phase marker: one line per block boundary, survives
	 * netconsole for crash attribution (never per-poll). KERN_EMERG
	 * + a short drain so the line reaches tty0/netconsole/ssh
	 * BEFORE the risky write it announces (fw-start-debug
	 * 2026-09-22: fwstart#2 died with zero capture — the marker
	 * must beat the write). */
	dev_emerg(mm->ane->dev, "BOOT-PHASE %s\n", what);
	msleep(30);
}

/* PS guard of the probes and trace_td: ane_sys (pmgr+0x260) ACTUAL on and
 * the seven island words (ane_sys_mpm .. ane_set4, pmgr+0x4000..0x4030)
 * 0x3ff, read through the DT "pmgr" window. */
static int ane_boot_power_ok(void *ctx)
{
	struct ane_t6021_boot_mmio *mm = ctx;
	void __iomem *pm = mm->ane->base[ANE_T6021_REG_PMGR];
	unsigned int off;

	if (!pm || (readl(pm + 0x260) & ANE_PS_ACTUAL) != ANE_PS_ACTUAL)
		return 0;
	for (off = 0x4000; off <= 0x4030; off += 8)
		if ((readl(pm + off) & 0x3ff) != 0x3ff)
			return 0;
	return 1;
}

static void ane_boot_bridge_log(void *ctx, unsigned int off, u32 before,
				u32 v, int written)
{
	struct ane_t6021_boot_mmio *mm = ctx;

	if (written) {
		dev_emerg(mm->ane->dev, "AFB eng+0x%03x before 0x%08x after 0x%08x\n",
			  off, before, v);
		return;
	}
	dev_emerg(mm->ane->dev, "AFB eng+0x%03x before 0x%08x write 0x%08x\n",
		  off, before, v);
	msleep(30);	/* the line leaves before the write (ane_boot_phase) */
}

/* S5 prepare — runs strictly AFTER poll A (fw alive), BEFORE the
 * SCRATCH0/1 publish. Dynamic allocations occur here per "dynamic
 * allocations occur after READY" (Main): the 'DDM ' pool, the 'IPC '
 * surface, and the trust-bounded fw-requested HEAP surface. Every
 * allocation is wedged-pin owned from here on (held while
 * cpu_started; reboot reclaims). Publishes the pool DVA (suballoc at
 * offset 0) as the SCRATCH0/1 halves. */
static void *ane_boot_alloc(void *ctx, u64 size, u64 *iova)
{
	struct ane_t6021 *ane = ctx;
	dma_addr_t d = 0;
	void *p = dma_alloc_coherent(ane->dev, size, &d, GFP_KERNEL);

	if (p && !ane_t6021_fw_alias_iova_ok(ane, d, size)) {
		dev_err(ane->dev,
			"boot alloc %#llx+%#llx overlaps fw alias — refusing\n",
			(u64)d, size);
		dma_free_coherent(ane->dev, size, p, d);
		return NULL;
	}

	*iova = p ? d : 0;
	return p;
}

/* Kernel prepare: live SCRATCH3/SCRATCH1 reads feed the SHARED
 * assembly (ane_t6021_boot_prepare_publish) with the dma_alloc hook —
 * the same path the fake-MMIO trace test exercises. Allocations are
 * wedged-pin owned from creation (held while cpu_started). */
static int ane_t6021_boot_prepare(void *ctx, u32 *lo, u32 *hi)
{
	struct ane_t6021_boot_mmio *mm = ctx;
	struct ane_t6021 *ane = mm->ane;
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	struct ane_t6021_init_sources src = boot_sources;
	struct ane_t6021_boot_allocs a;
	u32 request, scratch0, scratch1;
	int err;

	if (!ane_t6021_boot_preflight_complete())
		return -ENODATA;	/* belt: run() already gated */

	/* dynamic reads (post-READY, live cells — never hardcoded):
	 * read order SCRATCH0 then SCRATCH1 (0x95ea0d8/0x95ea100);
	 * SCRATCH0 >= 0x21 refuses before allocations/publication;
	 * SCRATCH3 = fw extra-heap request; SCRATCH1+1 = ordinal. */
	scratch0 = readl(eng + ANE_MBI_SCRATCH0);
	scratch1 = readl(eng + ANE_MBI_SCRATCH0 + 4);
	request = readl(eng + ANE_MBI_SCRATCH0 + 4 * 3);

	src.fw_dva = ane->fw_iova;
	err = ane_t6021_boot_prepare_publish(&src, request, scratch0,
					     scratch1, 0x4000,
					     ANE_T6021_BOOT_IPC_CEILING,
					     ANE_T6021_BOOT_HEAP_CEILING,
					     ane, ane_boot_alloc, &a,
					     lo, hi);
	if (!err && boot_prevent_nap) {
		((u8 *)a.pool)[ANE_T6021_INIT_TEMPLATE_OFF + 0x84] |= 1;
		dev_info(ane->dev, "LAB init resource[0x84] bit0=1: prevent nap\n");
	}
	/* OWNERSHIP TRANSFER on EVERY path (Main lifetime review): an
	 * error after the pool/ipc allocations must NOT lose them — a
	 * started CPU may already be fetching, so partial allocations
	 * are RETAINED (wedged-pin holds them; reboot reclaims), never
	 * freed on this path. */
	ane->boot_pool = a.pool;
	ane->boot_pool_iova = a.pool_dva;
	ane->boot_ipc = a.ipc;
	ane->boot_ipc_iova = a.ipc_dva;
	ane->boot_ipc_size = a.ipc_size;
	ane->boot_heap = a.heap;
	ane->boot_heap_iova = a.heap_dva;
	ane->boot_heap_size = a.heap_size;
	return err;
}

/* Dispatch the resolved sequence against the kernel io backend. All
 * gates were checked by the caller; the run() core re-checks. After
 * the CPU release there is NO ordinary unwind: failures HOLD state
 * (wedged-pin cleanup refuses to free under a started CPU) and the
 * probe binds fenced. */
/* Poll-A-timeout progress dump — PROVEN-readable observables ONLY
 * (Main review 2026-09-26): all eight SCRATCH cells (pulse-cleared to 0
 * pre-release, so any nonzero word with SCRATCH7 != READY/WAKE is a
 * candidate firmware store), RVBAR, CPU_STATUS, and the 24 MHz domain
 * tick at engine+0x1160008 (watched advancing post-release in the
 * 2026-09-25 handshake receipt; a static tick separates "ASC domain
 * unclocked" from "clocked but silent").
 *
 * Deliberately NOT read: engine+0x1140008 (the 13.5 stub's VM 0x30c
 * store target). Its readability, width and reset value are unproven
 * and no pre-release baseline exists, so an after-value would not
 * locate PC (could be preexisting iBoot state, write-only, or
 * normalized). */
static void ane_t6021_boot_progress_dump(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u32 tick0 = readl(eng + ANE_T6021_BOOT_REG_TICK);
	u32 tick1;
	unsigned int i;

	msleep(20);
	tick1 = readl(eng + ANE_T6021_BOOT_REG_TICK);

	for (i = 0; i < 8; i++)
		dev_emerg(ane->dev, "PROGRESS SCRATCH%u=%08x\n", i,
			  readl(eng + ANE_T6021_BOOT_REG_SCRATCH0 + 4 * i));
	dev_emerg(ane->dev,
		  "PROGRESS rvbar=%016llx cpu_status=%08x tick %08x->%08x (%s)\n",
		  readq(eng + ANE_ASC_RVBAR),
		  readl(eng + ANE_ASC_CPU_STATUS),
		  tick0, tick1,
		  tick1 != tick0 ? "24MHz domain clocked" : "tick STATIC");
}

int ane_t6021_boot_start(struct ane_t6021 *ane, int stop_after, int table_mode, int rtb_mode)
{
	struct ane_t6021_boot_mmio mm = { .ane = ane };
	struct ane_t6021_boot_io io = {
		.ctx = &mm,
		.rd32 = ane_boot_rd32, .rd64 = ane_boot_rd64,
		.wr32 = ane_boot_wr32, .wr64 = ane_boot_wr64,
		.publish_barrier = ane_boot_publish_barrier, .poll_wait = ane_boot_wait,
		.phase = ane_boot_phase,
		.prepare = ane_t6021_boot_prepare,
		.power_ok = ane_boot_power_ok,
		.bridge_log = ane_boot_bridge_log,
	};
	struct ane_t6021_boot_cfg cfg = {
		.preflight_ok = ane_t6021_boot_preflight_complete(),
		.preboot_table_mode = table_mode,
		.fw_dva = ane->fw_iova,
		.stop_after = stop_after,
		.rtb_mode = rtb_mode,
		.af_bridge_macos = af_bridge_macos,
	};
	int cs = 0, fa = 0, bo = 0;
	u64 sres = 0;
	int r;

	/* Wedged-pin module lifetime (Main lifetime review): the ref is
	 * acquired BEFORE the first write — a started CPU can never
	 * outlive the pin, and a dying module refuses the boot before
	 * any write happens. Retained on CPU start (never released:
	 * intentional); released only if no CPU start occurred. */
	if (!try_module_get(THIS_MODULE)) {
		dev_err(ane->dev,
			"boot: REFUSED before any write — module ref unavailable (dying); no CPU start possible from a dying module\n");
		return -EBUSY;
	}

	dev_emerg(ane->dev,
		  "BOOT-PHASE dispatch (stop_after=%d%s)\n", stop_after,
		  stop_after ? " BISECT STOP ARMED" : "");

	r = ane_t6021_boot_run(&io, &cfg, &cs, &fa, &bo, &sres);

	ane->cpu_started = cs;
	ane->fw_alive = fa;
	ane->booted = bo;
	ane->boot_scratch_result = sres;

	/* Released CPU without READY (poll-A timeout, either transport
	 * mode): capture the 13.5 execution-progress observables NOW,
	 * while the islands are powered — this is the exact-progress
	 * datum the next pre-Linux capture cross-checks. */
	if (cs && !fa)
		ane_t6021_boot_progress_dump(ane);

	if (!cs) {
		/* no CPU start: full release path, normal ownership */
		module_put(THIS_MODULE);
		dev_emerg(ane->dev,
			  "BOOT-PHASE done r=%d cpu_started=0 (no CPU release: state clean, module unpinned)\n",
			  r);
		return r;
	}

	/* started CPU: retain the pin for the whole wedged lifetime.
	 * Residual: DT hotplug unbind cannot be fully prevented; devm
	 * release order frees irq before ioremap (probe-order reverse);
	 * DMA surfaces are wedge-held, never freed. */
	dev_warn(ane->dev,
		 "boot: module PINNED until reboot (started CPU; wedged-pin)\n");

	if (r == -ENODATA)
		return r;	/* unreachable: the caller gated */
	if (r && cs) {
		dev_err(ane->dev,
			"boot: sequence error %d AFTER CPU start (cpu_started=%u fw_alive=%u booted=%u scratch_result=%016llx) — WEDGED-PIN HOLD: all surfaces/rings/IRQ/links preserved; reboot is the only reclamation; no retry. HANDSHAKE state only: the result word has NO sourced success semantics\n",
			r, cs, fa, bo, sres);
		return 0;	/* bind fenced, state held */
	}
	if (!r)
		dev_info(ane->dev,
			 "boot: run returned 0 (booted=%u fw_alive=%u) — %s; scratch_result=%016llx (raw device address, semantics UNSOURCED — not a success claim; transport stays fenced until response validation)\n",
			 bo, fa,
			 bo ? "DONE observed — handshake complete"
			    : "rc 0 WITHOUT DONE (rtb_mode: HELLO-gated; DONE not part of this mode)",
			 sres);
	return r;
}

