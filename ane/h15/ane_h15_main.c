/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h15 — OPT-IN EXPERIMENTAL bring-up module for the H15 (M3)
 * ANE family: T8122 (M3), T6030 (M3 Pro), T6031 (M3 Max), T6034
 * (M3 Max die-variant, j514m/j516m). T6034 uses t6031's ADT nodes
 * and is the fourth row in the same module; no t6034 overlay is
 * shipped in this change (assignment scope: t8122/t6030/t6031 only;
 * a t6034 owner copies the t6031 overlay and swaps the compatible).
 *
 * No silicon has run this module. It performs no inference, no
 * CSNE_CMD, and no DRM device; it is a bring-up tool that the
 * first M3 owner drives through the stages below.
 *
 * Stages (selected by the `stage` module parameter):
 *   0 dt     : parse the running DT and print what would be mapped;
 *              no ioremap, no rpm.
 *   1 status : enable ANE power domains, wait for every pmgr ANE
 *              state word to read ACTUAL=0xf, then log every SAFE
 *              word (only the pmgr words qualify for H15).
 *   2 wrapper: refuses on H15 unless confirm_read=1 (CPU_STATUS and
 *              RVBAR are MEASURED-address, INFERENCE-role). With
 *              confirm_read=1 it runs the read-only preflight of the b0
 *              plan: behind the ACTUAL=0xf gate it reads CPU_STATUS,
 *              RVBAR and the two mailbox CTRL words, each address logged
 *              before its read, inside the two ADT-published windows
 *              only. No write, no CPU release.
 *   3 boot   : the boot-time route Main GOed 2026-11-10 for T6030: apply_overlay=1 boot_overlay=1
 *              applies the boot blob (DART instances enabled, ane node an IOMMU client) at init
 *              BEFORE the driver probes, the probe powers the domains, and this stage then runs
 *              the same run path as stage 5 (run_common, stg=3) with map_fw=1 and the pinned
 *              owned image, so the core fetches firmware through all three DART instances.
 *              Requires fw_path, confirm_boot, confirm_read and confirm_run. The overlay is
 *              never removed in-session (w91 M6 lesson: removal oopses apple_psd_request; a
 *              second apply fails -EEXIST); a reboot reclaims it.
 *   4 dart   : confirm_read=1 required. Read only: the four ANE DART windows (engine+0x1800000..),
 *              version, lock state, and per stream TCR, TTBR and the enable bits. Says whether iBoot left a
 *              stream enabled and mapped. See the comment above ane_h15_stage_dart().
 *   5 run    : confirm_run=1 required. The only stage that writes: it starts
 *              the ASC core once (CPU_CONTROL RUN) behind a read-only
 *              preflight (core stopped, RUN clear, RVBAR exactly the value a
 *              stage 2 run measured on this SoC, mailbox words EMPTY), watches
 *              CPU_STATUS and pops at most four mailbox messages (raw words,
 *              no reply), then stops the core again. Refused on any SoC row
 *              without a measured rvbar_expect (today t6034 only), and without a live DART
 *              stream from the same scan stage 4 does (run_unmapped=1 overrides). scratch_boot=1 (off by
 *              default) starts the core in macOS's cold-boot order instead of the read-modify-write
 *              and reports s7_ready on the RESULT line. See the
 *              comment above ane_h15_stage_run().
 *
 *   6 rtkit  : confirm_run=1 and confirm_rtkit=1 required. Stage 5 plus an RTKit management session on the ASC
 *              mailbox: the first stage that writes the mailbox send registers. See ane_h15_rtk_* below.
 *
 *   7 chan   : confirm_run, confirm_rtkit and confirm_chan required. Stage 6 plus the macOS handshake after AP power ON:
 *              the firmware surfaces, SCRATCH0/1/7, the channel table, ring ownership, SCRATCH3 READY, optionally the
 *              first commands. See ane_h15_chan_run().
 *
 * No MODULE_DEVICE_TABLE: nothing autoloads. probe() returns
 * -EPERM unless `optin=<soc>` matches. Insmod line:
 *   insmod ane_h15.ko optin=t8122 stage=status
 *
 * After stage=boot releases the ASC CPU, remove() refuses and the
 * machine needs a reboot to park the ANE (ane_t6021_boot.c:494-514
 * semantics; corrected in agent/ane-h16-ps-fix and verified by
 * receipts/2026-10-03-ane-h16).
 */
#include <linux/bitfield.h>
#include <crypto/sha2.h>
#include <linux/bits.h>
#include <linux/cred.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iommu.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>
#include <crypto/sha2.h>
#include <linux/unaligned.h>

#include "ane_h15.h"
#include "ane_h15_overlay.h"
#include "ane_h15_domain_window.h"
#include "ane_h15_fw_layout.h"

static char *optin;
module_param(optin, charp, 0444);
MODULE_PARM_DESC(optin,
		 "Opt-in key: must equal the SoC row name (t8122, t6030, t6031, t6034) or probe returns -EPERM.");

static char *stage = "status";
module_param(stage, charp, 0444);
MODULE_PARM_DESC(stage,
		 "domain-window: stage2 guards and domain aperture classification only; requires fw_seg, no translation lookup. "
		 "0/1/2/3/4/5/6/7 or name (dt/status/wrapper/boot/dart/run/rtkit/chan). 1/status (default): power up and log ps ACTUAL. 0/dt: parse the DT only. 2/wrapper: REFUSED on H15 without confirm_read=1. 3/boot: refused without fw_path=. 4/dart: read-only DART liveness, needs confirm_read=1. 5/run: refused without confirm_run=1 and without a live DART stream; starts the core once behind a preflight. 6/rtkit: stage 5 plus an RTKit session over the ASC mailbox, needs confirm_rtkit=1 as well. 7/chan: stage 6 plus the SCRATCH handshake and the channel step, needs confirm_chan=1 as well.");

static unsigned int ps_wait_ms = 500;
module_param(ps_wait_ms, uint, 0444);
MODULE_PARM_DESC(ps_wait_ms,
		 "Per-word pmgr ACTUAL=0xf wait, ms. Default 500.");

static bool confirm_boot;
module_param(confirm_boot, bool, 0444);
MODULE_PARM_DESC(confirm_boot,
		 "Required for stage=boot. Off by default; stage=boot refuses without it.");

static bool confirm_read;
static bool pwr_req;
static unsigned int pwr_poll_ms = 1000;
module_param(confirm_read, bool, 0444);
MODULE_PARM_DESC(confirm_read,
		 "Stage=wrapper only. Off by default; stage=wrapper refuses without it. With it, stage=wrapper reads CPU_STATUS, RVBAR and the two mailbox CTRL words behind the pmgr ACTUAL=0xf gate. Reads only, nothing is written.");
module_param(pwr_req, bool, 0444);
MODULE_PARM_DESC(pwr_req,
		 "stage=run or boot: the M2-order ASC CPU power-up request: set EDPRCR bit0, then poll EDPRSR bounded (pwr_poll_ms) for the powered status before RUN. On poll timeout: FAIL, no RUN write. Off by default.");
module_param(pwr_poll_ms, uint, 0444);
MODULE_PARM_DESC(pwr_poll_ms,
		 "pwr_req poll bound in ms, 10 to 10000, default 1000.");

static bool run_unmapped;
module_param(run_unmapped, bool, 0444);
MODULE_PARM_DESC(run_unmapped,
		 "stage=run only: start the core even though stage=dart found no enabled DART stream (the core cannot fetch its firmware then; expect silence)");

static bool map_fw;
module_param(map_fw, bool, 0444);
MODULE_PARM_DESC(map_fw,
		 "stage=run only: before RUN attach the ANE to its DART through the iommu API and map the two preloaded firmware segments named by fw_seg. Needs iommus in the node. Default off.");

static bool wrad_patch = true;
module_param(wrad_patch, bool, 0444);
MODULE_PARM_DESC(wrad_patch,
		 "Owned firmware image (fw_path): write the provider's wrapper address into the image's wrapper field before the core starts, as the shipped driver does (default on). 0 leaves the payload's own word there, as the M6 start that reached its READY did. For testing why the core stays silent.");

static char *fw_seg;
module_param(fw_seg, charp, 0444);
MODULE_PARM_DESC(fw_seg,
		 "TEXT and DATA from this boot's live ADT as hex \"TEXTPA:SIZE,DATAPA:SIZE\". "
		 "Stage 2 compares the current domain's RVBAR entry with TEXT without mapping; "
		 "stage=run map_fw=1 maps TEXT read only and DATA read+write. "
		 "Both segments must be 16 KiB aligned and outside system RAM.");

static bool confirm_rtkit;
module_param(confirm_rtkit, bool, 0444);
MODULE_PARM_DESC(confirm_rtkit,
		 "stage=rtkit only: required. Stage 6 is stage 5 plus an RTKit management session on the ASC mailbox: it WRITES the mailbox send registers (HELLO reply, endpoint map, STARTEP, power states) and then runs the shutdown handshake. Nobody has run it on an H15 part.");

static bool hold_vote;
module_param(hold_vote, bool, 0444);
MODULE_PARM_DESC(hold_vote,
		 "Experiment: a passing probe keeps its runtime reference (no pm_runtime_put_sync), so the PMP vote and the ANE domains stay held until rmmod. Splits a PMP crash at vote time from one at release.");

static unsigned int rtkit_wait_ms = 2000;
module_param(rtkit_wait_ms, uint, 0444);
MODULE_PARM_DESC(rtkit_wait_ms,
		 "stage=rtkit: how long to wait for the session to reach AP power ON and the application endpoints (100 to 10000 ms, default 2000)");

static unsigned int rtkit_stop = 1;
module_param(rtkit_stop, uint, 0444);
MODULE_PARM_DESC(rtkit_stop,
		 "stage=rtkit: shutdown before RUN is cleared. 1 (default): AP QUIESCED then IOP SLEEP, restartable (apple_rtkit_shutdown). 2: OFF then OFF, the core cannot be restarted until a reboot (apple_rtkit_poweroff).");

static bool confirm_chan;
module_param(confirm_chan, bool, 0444);
MODULE_PARM_DESC(confirm_chan,
		 "stage=chan only: required. Stage 7 is stage 6 plus the macOS SCRATCH handshake and the channel step: it WRITES SCRATCH0, SCRATCH1, SCRATCH7 and SCRATCH3 of the engine block, writes ring memory the firmware reads and (chan_cmds=1) the doorbell. Nobody has run it on an H15 part.");

static bool chan_cmds;
module_param(chan_cmds, bool, 0444);
MODULE_PARM_DESC(chan_cmds,
		 "stage=chan: after the channel step send PRINT_ENABLE and CONFIG_GET on the IO ring (writes the doorbell at engine + 0x1844000). Default off.");

static unsigned int hs_wait_ms = 1000;
module_param(hs_wait_ms, uint, 0444);
MODULE_PARM_DESC(hs_wait_ms,
		 "stage=chan: how long each handshake wait lasts (the table address, SCRATCH3, a command answer), 10 to 5000 ms. Default 1000.");

static bool confirm_run;
module_param(confirm_run, bool, 0444);
MODULE_PARM_DESC(confirm_run,
		 "Required for stage=run. Off by default. Stage=run starts the ASC core once (CPU_CONTROL RUN) behind a read-only preflight, watches CPU_STATUS and the mailbox, and stops it again. Refused on SoCs without a measured RVBAR (rvbar_expect).");

static unsigned int run_wait_ms = 300;
module_param(run_wait_ms, uint, 0444);
MODULE_PARM_DESC(run_wait_ms,
		 "Stage=run: how long the core runs before RUN is cleared again, ms (10 to 2000). Default 300.");

static bool run_scratch;
module_param(run_scratch, bool, 0444);
MODULE_PARM_DESC(run_scratch,
		 "Stage=run (5) or stage=boot (3): read the eight SCRATCH words, read only, once before RUN and once after the wait, and log them. Off by default. The offsets come from macOS's T6030 case and have not been read on an M3.");

static bool run_pmgr;
module_param(run_pmgr, bool, 0444);
MODULE_PARM_DESC(run_pmgr,
		 "Stage=run (5) only: read the five pmgr ANE state words, read only, before RUN, just after the RUN write and after the wait, and log them. Off by default. They are the words stage 1 already reads.");

static bool scratch_boot;
module_param(scratch_boot, bool, 0444);
MODULE_PARM_DESC(scratch_boot,
		 "Stage=run (5) or boot (3): start the core in the recorded cold-boot order instead of the read-modify-write: write 0 to SCRATCH0..7, then SCRATCH6=1, SCRATCH7=0 (read back), then CPU_CONTROL 0 and 0x10, then poll SCRATCH7 for the ready word 0x08042006. Never RVBAR, the mailbox or the DART; SCRATCH5 only ever receives 0. Off by default. Recorded behaviour, never run on an M3. 2026-11-10: the boot route takes the same handshake, because the working T6021 sequence always runs it before CPU release (ane_t6021_boot.h P1+P3) and the T6030 stage=3 without it started nothing.");

static unsigned int sb_wait_ms = 1000;
module_param(sb_wait_ms, uint, 0444);
MODULE_PARM_DESC(sb_wait_ms,
		 "Stage=run scratch_boot=1: how long to poll SCRATCH7 for the ready word, 10 to 5000 ms. Default 1000.");

/* The twelve T6021 grant tunables (ane_t6021_boot.h P-1), written before
 * the SCRATCH handshake and CPU release. Recorded on working T6021 boots;
 * never measured on T6030 before 2026-11-10. */
static bool grant_tunables;
module_param(grant_tunables, bool, 0444);
MODULE_PARM_DESC(grant_tunables,
		 "Stage=run (5) or boot (3), before the scratch handshake: write the twelve T6021 grant-tunable words at eng+0x000..0x430 (ane_t6021_boot.h P-1). The working T6021 flow runs this unconditionally; the h15 sequences never had it. Off by default.");

/* The T6021 mailbox transport variant (ane_t6021_boot.h rtb_mode): SCRATCH6=0
 * in the handshake, a SCRATCH7 ACK timeout is nonfatal because aliveness comes
 * from the RTKit HELLO over the ASC mailbox, and the session runs after the
 * start. This is the w91 M2 path that reached its READY. */
static bool mailbox_mode;
module_param(mailbox_mode, bool, 0444);
MODULE_PARM_DESC(mailbox_mode,
		 "Stage=run (5) or boot (3), with scratch_boot=1: select the mailbox transport (SCRATCH6=0), tolerate a SCRATCH7 ACK timeout, and run the RTKit session after the start; the RESULT carries rtkit-hello. The w91 M2 READY path. Off by default.");

/* The names these two parameters had before the rename. Accepted, not listed (permission 0), so the commands already
 * posted to volunteers still run. Remove both lines once those runs are done. */
module_param_named(kext_boot, scratch_boot, bool, 0);
module_param_named(kb_wait_ms, sb_wait_ms, uint, 0);

static bool apply_overlay;
module_param(apply_overlay, bool, 0444);
MODULE_PARM_DESC(apply_overlay,
		 "Off by default. For a boot whose device tree has no ANE node (a maclab lab boot cannot carry one): apply the compiled overlay for optin=t8122, t6030 or t6031 to the live device tree with the kernel's overlay call, then register the driver, so the normal probe runs. T8122 keeps DART nodes disabled and has no IOMMU client. Refuses, touching nothing, unless optin names a row with an overlay, the machine matches it and every node the overlay refers to exists; does nothing when the tree already has the ANE node. Unloading removes it again. Never touches a boot image.");

/* The boot blob (DART instances enabled, ane node an IOMMU client) instead
 * of the runtime blob: stage=3 applies it BEFORE the driver probes, so the
 * DARTs bind while the tree is cold. Never removed in-session; a reboot
 * reclaims it (w91 M6 lesson: removal oopses apple_psd_request, a second
 * apply fails -EEXIST). */
static bool boot_overlay;
module_param(boot_overlay, bool, 0444);
MODULE_PARM_DESC(boot_overlay,
		 "Off by default; stage=3 refuses without apply_overlay=1 boot_overlay=1. Applies the boot-time variant of the overlay: the three ANE DART instances enabled and the ane node an IOMMU client of all of them. Requires the apple-dart driver present so the instances bind; a stage 3 boot then maps firmware through the kernel's IOMMU API. The overlay is NOT removed at unload.");

static unsigned int pwgate_t6031;
module_param(pwgate_t6031, uint, 0444);
MODULE_PARM_DESC(pwgate_t6031,
		 "Stage=run (5) or stage=boot (3), rows with a PWGATE window (T8122, T6030, T6031): the PWGATE step before the first CPU_CONTROL write. 0 off (default, byte-identical). 1: read the row's word (T8122 pmgr_ps+0x272c, T6030 pmgr_ps+0x2b6c, T6031 +0x199c), log it; if bits 1:0 are nonzero write 0 once and poll up to 5000 times (10 us each) until they read 0; if already 0 write nothing. 2: like 1 but always write 0 first. Other values refuse in probe. The name dates from the T6031 row, which had the step first. 2026-11-10: the stage 3 boot route measured the T6030 word 0x3 (closed) after two RUNs that left RUNNING clear, so the boot route now runs the shipped clear. Never run on an M3.");


static bool stop_after_pwgate;
module_param(stop_after_pwgate, bool, 0444);
MODULE_PARM_DESC(stop_after_pwgate,
		 "Stage=run (5) with pwgate_t6031 only, without scratch_boot: stop right after the PWGATE step, before any CPU_CONTROL write. The RESULT reason carries the pwgate state and stop-after-pwgate. Splits a crash between the gate write and the core start. Off by default. Refuses, touching nothing, without pwgate_t6031=1 or with scratch_boot=1 (pwgate_t6031 itself enforces stage 5).");

static bool rpm_hold_sweep;
module_param(rpm_hold_sweep, bool, 0444);
MODULE_PARM_DESC(rpm_hold_sweep,
		 "Stage=run (5): take a second runtime-PM reference immediately before the M6-lesson DART sweep and release it after, so the hold is explicit from the sweep's first read to its last. The probe's own get already spans the sweep; this one exists so a sweep crash can be measured with the hold made explicit rather than inherited. Default off, byte-identical.");

static bool sweep_watch;
module_param(sweep_watch, bool, 0444);
MODULE_PARM_DESC(sweep_watch,
		 "Stage=run (5): time every DART sweep read; when a read takes over sweep_watch_us microseconds, immediately read and steplog the five ANE power-state words plus the slow read's own value. Diagnostic for the intermittent sweep crash: the steplog shows whether the whole ANE MMIO stalls or only the DART window, and captures the power-state bits at the stall. Pairs with steplog. Default off.");

static unsigned int sweep_watch_us = 1000;
module_param(sweep_watch_us, uint, 0444);
MODULE_PARM_DESC(sweep_watch_us,
		 "Slow-read threshold in microseconds for sweep_watch. Default 1000.");

static bool ps_assert;
module_param(ps_assert, bool, 0444);
MODULE_PARM_DESC(ps_assert,
		 "Stage=run (5) with pwgate_t6031: before the DART sweep, write 0xf to the four ANE power-state words in the pmgr window (ANE_MPM 0xc000, ANE_CPU 0xc008, ANE_TD 0xc010, ANE_BASE 0xc018) and read them back. This is the power-state re-assert of the shipped boot: the four B-class deaths landed 19 to 23 ms after the first power-state query, wherever the DART sweep happened to be, which fits the window's power domain gating off mid-sweep. The re-assert holds the domain across the sweep. Off by default, byte-identical. Refuses with pwgate_t6031=0 or off stage 5, before any MMIO.");

static unsigned int dart_scan_mask = 0xf;
module_param(dart_scan_mask, uint, 0444);
MODULE_PARM_DESC(dart_scan_mask,
		 "Bitmask of DART instances the scan reads, bit 0 = instance 0. Default 0xf (all four, byte-identical). A clear bit skips that instance's sweep reads entirely and logs one skip line; the firmware segment map is untouched. Exists because the 2026-11-03 B run died on a dart1 TTBR read and the A run survived the same read: the sweep is an intermittent crash source, so a run can now leave the suspect instance unread. Refuses 0 and anything above 0xf.");

static char *steplog;
module_param(steplog, charp, 0444);
MODULE_PARM_DESC(steplog,
		 "Path: append one line, fsynced, before every MMIO access from the first stage access on: PWGATE reads and write, CPU_CONTROL reads and writes, SCRATCH reads and writes, the engine word reads, the pmgr ps reads, the DART reads, the doorbell and IPI writes, and the first and last pass of each poll. The file survives a reset that eats the kmsg ring, so a crashed run names its last access. Every line hits the disk before the access runs. The open happens after the power-domain bring-up, so those ps reads are not in the file. Off by default. A failed open refuses the run before the stage's MMIO.");
static unsigned int pwgate_read;
module_param(pwgate_read, uint, 0444);
MODULE_PARM_DESC(pwgate_read,
		 "Stage=wrapper (2) only, rows with a PWGATE window (T6030, T8122, T6031), with confirm_read=1, without fw_seg: after the preflight reads, read the one PWGATE word (T6030 pmgr_ps+0x2b6c, T8122 pmgr_ps+0x272c, T6031 +0x199c) once and log it with bits 1:0 closed or open. Writes nothing. 0 off (default, the stage 2 run is byte-identical). Other values refuse in probe.");

static char *fw_path;
module_param(fw_path, charp, 0444);
MODULE_PARM_DESC(fw_path,
		 "Stage=boot only. Firmware API name of an exact pinned 26A428 H15 payload. Requires confirm_boot=1 and confirm_read=1. Prepares coherent owned memory, then retains the existing startup refusal; no CPU release. "
		 "With map_fw=1 and no fw_seg, the run stages take the same owned image as their firmware source and map its pages at the RVBAR entry instead of the iBoot segments.");

/* apple-pmgr-pwrstate word: TARGET bits 3:0, ACTUAL bits 7:4;
 * 0xf is fully on (ane_t6021.h ANE_PS_*; docs/t6021-ane-bringup-
 * findings.md §4 gate). */
#define ANE_H15_PS_ACTUAL	GENMASK(7, 4)
#define ANE_H15_PS_ON		0xfu

static const char * const ane_h15_ps_names[] = {
	"ANE_SYS", "ANE_MPM", "ANE_CPU", "ANE_TD", "ANE_BASE"
};

struct ane_h15_hs_buf {
	void *va;
	dma_addr_t iova;
	size_t size;
};

struct ane_h15 {
	struct device *dev;
	const struct ane_h15_soc *soc;
	void __iomem *engine;
	void __iomem *pmgr;
	struct ane_h15_seg segs[2];
	bool fw_touched;
	bool fw_started;
	struct iommu_domain *domain;	/* map_fw: the DART domain, NULL when none */
	u64 map_pa[2], map_iova[2], map_size[2];
	unsigned int map_n;	/* segments mapped so far */
	bool dom_own;		/* the domain was attached by this module (stage 7 maps into the default DMA domain instead) */
	/* fw_path with map_fw: the Linux-owned image replaces the iBoot segments. owned_pa holds the physical page behind each 16 KiB
	 * of owned_fw, read from the default DMA domain before the attach replaces it. */
	u64 owned_pa[ANE_H15_FW_IMAGE_SIZE / SZ_16K];
	bool owned_map;
	bool map_retained;
	bool chan;		/* stage 7 */
	unsigned int ipi_acks;	/* stage 7: acknowledges written this run, capped at ANE_H15_IPI_ACK_MAX */
	u32 ipi_last;		/* stage 7: last IPI pending value seen, for the change log */
	struct ane_h15_hs_buf hs_pool, hs_ipc, hs_heap, hs_cmd;
	struct ane_h15_hs_buf owned_fw;
	/* The ANE node carries several power-domains (sys, mpm, cpu, td,
	 * base). The kernel's automatic attach handles only a single
	 * domain, so for more than one we attach each by id and link it
	 * for runtime PM, as the in-tree ane driver does. */
#define ANE_H15_MAX_PD 12
	struct device *pd_dev[ANE_H15_MAX_PD];
	struct device_link *pd_link[ANE_H15_MAX_PD];
	int pd_count;
	bool vote_held;			/* hold_vote: the probe kept its runtime reference */
	struct device *vote_dev;	/* the PMP vote domain, taken in runtime_resume; NULL when the node has none */
};

/* Attach every power-domain the node lists and link it so a single
 * pm_runtime_get on this device powers them all. Returns 0 when the
 * node has 0 or 1 domains (the core's automatic attach covers those).
 */
static int ane_h15_attach_pds(struct ane_h15 *ane)
{
	struct device *dev = ane->dev;
	int count, i;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count <= 1)
		return 0;
	if (count > ANE_H15_MAX_PD) {
		dev_err(dev, "too many power-domains: %d\n", count);
		return -EINVAL;
	}
	for (i = 0; i < count; i++) {
		ane->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR_OR_NULL(ane->pd_dev[i])) {
			int err = ane->pd_dev[i] ? PTR_ERR(ane->pd_dev[i]) : -ENODEV;

			ane->pd_dev[i] = NULL;
			dev_err(dev, "attach power-domain %d: %d\n", i, err);
			return err;
		}
		/* Count it before adding the link: if device_link_add fails,
		 * ane_h15_detach_pds must still detach this attached domain. */
		ane->pd_count = i + 1;
		struct of_phandle_args args;
		u32 flags = DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE;

		/* The PMP vote is found by what it is, not where it sits in the list. */
		if (!of_parse_phandle_with_args(dev->of_node, "power-domains",
						"#power-domain-cells", i, &args)) {
			if (of_device_is_compatible(args.np, "apple,t6000-pmp-v2-report-entry")) {
				flags = DL_FLAG_STATELESS;
				ane->vote_dev = ane->pd_dev[i];
			}
			of_node_put(args.np);
		}
		ane->pd_link[i] = device_link_add(dev, ane->pd_dev[i], flags);
		/* device_link_add() returns NULL on failure, never an
		 * ERR_PTR, so a plain !link test is the right check. */
		if (!ane->pd_link[i]) {
			dev_err(dev, "link power-domain %d\n", i);
			return -EINVAL;
		}
	}
	dev_info(dev, "attached %d ANE power-domains\n", ane->pd_count);
	return 0;
}

static void ane_h15_detach_pds(struct ane_h15 *ane)
{
	int i;

	ane->vote_dev = NULL;

	for (i = ane->pd_count - 1; i >= 0; i--) {
		if (ane->pd_link[i]) {
			device_link_del(ane->pd_link[i]);
			ane->pd_link[i] = NULL;
		}
		if (ane->pd_dev[i]) {
			dev_pm_domain_detach(ane->pd_dev[i], true);
			ane->pd_dev[i] = NULL;
		}
	}
	ane->pd_count = 0;
}

/* ---- RESULT line, emitted at every stage ---- */
static void ane_h15_result(struct ane_h15 *ane, unsigned int stage_idx,
			   const char *verdict, const char *reason)
{
	if (ane && ane->dev)
		dev_crit(ane->dev,
			 "ane_h15 RESULT stage=%u soc=%s verdict=%s reason=%s\n",
			 stage_idx, ane->soc->name, verdict, reason);
	else
		pr_crit("ane_h15 RESULT stage=%u soc=%s verdict=%s reason=%s\n",
			stage_idx, "?", verdict, reason);
}

static int ane_h15_stage_idx(const char *name, unsigned int *out)
{
	if (!strcmp(name, "dt") || !strcmp(name, "0")) { *out = 0; return 0; }
	if (!strcmp(name, "status") || !strcmp(name, "1")) { *out = 1; return 0; }
	if (!strcmp(name, "wrapper") || !strcmp(name, "2") || !strcmp(name, "domain-window")) { *out = 2; return 0; }
	if (!strcmp(name, "boot") || !strcmp(name, "3")) { *out = 3; return 0; }
	if (!strcmp(name, "dart") || !strcmp(name, "4")) { *out = 4; return 0; }
	if (!strcmp(name, "run") || !strcmp(name, "5")) { *out = 5; return 0; }
	if (!strcmp(name, "rtkit") || !strcmp(name, "6")) { *out = 6; return 0; }
	if (!strcmp(name, "chan") || !strcmp(name, "7")) { *out = 7; return 0; }
	return -EINVAL;
}

/* ---- pmgr ACTUAL gate (always runs before any engine access) ---- */

static void ane_h15_step(const char *fmt, ...);
static u64 ane_first_ps_ns;

static bool ane_h15_ps_on(struct ane_h15 *ane, unsigned int i, u32 *v)
{
	if (!ane_first_ps_ns)
		ane_first_ps_ns = ktime_get_boottime_ns();
	ane_h15_step("read pmgr.%s pa=%#llx\n", ane_h15_ps_names[i],
		     ane->soc->pmgr_pa + ane->soc->ps_off[i]);
	*v = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
	return FIELD_GET(ANE_H15_PS_ACTUAL, *v) == ANE_H15_PS_ON;
}

static int ane_h15_ps_wait(struct ane_h15 *ane)
{

	unsigned int i;
	u32 v;

	for (i = 0; i < 5; i++) {
		unsigned long deadline = jiffies + msecs_to_jiffies(ps_wait_ms);

		while (!ane_h15_ps_on(ane, i, &v)) {
			if (time_after(jiffies, deadline)) {
				dev_err(ane->dev,
					"pmgr %s @+%#x stuck at %#x (ACTUAL=%#lx); refusing to touch the engine window\n",
					ane_h15_ps_names[i], ane->soc->ps_off[i], v,
					FIELD_GET(ANE_H15_PS_ACTUAL, v));
				return -ETIMEDOUT;
			}
			usleep_range(100, 200);
		}
	}
	for (i = 0; i < 5; i++) {
		if (!ane_h15_ps_on(ane, i, &v)) {
			dev_err(ane->dev,
				"pmgr %s @+%#x dropped to %#x after the wait\n",
				ane_h15_ps_names[i], ane->soc->ps_off[i], v);
			return -EIO;
		}
		dev_info(ane->dev, "pmgr %s @+%#x = %#x (ACTUAL 0xf)\n",
			 ane_h15_ps_names[i], ane->soc->ps_off[i], v);
	}
	return 0;
}

/* ---- stage 0: dt-only parse, no MMIO ---- */
static int ane_h15_stage_dt(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	unsigned int i;

	dev_info(ane->dev, "stage=dt: soc=%s ane-type=%u", s->name, s->ane_type);
	dev_info(ane->dev, "engine  pa=%#llx size=%#x", s->engine_pa, s->engine_size);
	dev_info(ane->dev, "pmgr    pa=%#llx size=%#x", s->pmgr_pa, s->pmgr_size);
	for (i = 0; i < 5; i++)
		dev_info(ane->dev, "pmgr.%s offset=%#x", ane_h15_ps_names[i],
			 s->ps_off[i]);
	for (i = 0; i < 5; i++) {
		u64 pa = (s->words[i].where == ANE15_WHERE_ENGINE) ? s->engine_pa : s->pmgr_pa;

		dev_info(ane->dev, "word[%u] %-13s where=%s off=%#x size=%u tier=%d",
			 i, s->words[i].name,
			 s->words[i].where == ANE15_WHERE_ENGINE ? "engine" : "pmgr",
			 s->words[i].off, s->words[i].size, s->words[i].tier);
		if (s->words[i].tier == ANE15_TIER_MEASURED_GUARD)
			dev_info(ane->dev, "  pa=%#llx", pa + s->words[i].off);
	}
	if (!s->fw) {
		dev_info(ane->dev, "fw pin: none (stage=boot refuses)");
	} else {
		dev_info(ane->dev, "fw pin: name=%s size=%u manifest=%u (S3 hash is over size)",
			 s->fw->name, s->fw->size, s->fw->manifest_size);
	}
	ane_h15_result(ane, 0, "PASS", "dt-parse-only");
	return 0;
}

/* ---- stage 1: pm up, log the SAFE pmgr words ---- */
static int ane_h15_stage_status(struct ane_h15 *ane)
{

	unsigned int i;
	u32 v;
	int ret;

	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, 1, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	/* Re-read every ps word behind the guard; each read logged
	 * BEFORE the access so a hang leaves the last attempted
	 * address in the log. */
	for (i = 0; i < 5; i++) {
		dev_crit(ane->dev, "ane_h15 read pmgr.%s pa=%#llx off=%#x",
			 ane_h15_ps_names[i], ane->soc->pmgr_pa + ane->soc->ps_off[i],
			 ane->soc->ps_off[i]);
		ane_h15_step("read pmgr.%s pa=%#llx\n", ane_h15_ps_names[i],
			     ane->soc->pmgr_pa + ane->soc->ps_off[i]);
		v = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
		dev_info(ane->dev,
			 "ane_h15 word=%s pa=%#llx value=%#x actual=%#lx pass=true",
			 ane_h15_ps_names[i], ane->soc->pmgr_pa + ane->soc->ps_off[i],
			 v, FIELD_GET(ANE_H15_PS_ACTUAL, v));
	}
	ane_h15_result(ane, 1, "PASS", "ps-guard+reads");
	return 0;
}

/* ---- stage 2: wrapper preflight ----
 * Without confirm_read=1 the stage refuses before any engine read and
 * lists the ADDR_MEASURED words (their roles are INFERENCE on H15).
 * With confirm_read=1 it runs the read-only preflight of the b0 plan
 * (receipts/2026-10-03-ane-h15, "b1 plan", step 2) once every pmgr word
 * reads ACTUAL=0xf: CPU_STATUS, RVBAR (two 32-bit reads) and the two
 * mailbox CTRL words. Every address is logged BEFORE its read, so a
 * hang leaves the last attempted address in the log. Only offsets
 * inside the two windows the ADT publishes for the ANE node are read:
 * the wrapper (engine+0x1400000, 0x6c000) and the RVBAR block
 * (engine+0x1050000, 0x4000). CPU_CONTROL, the CoreSight line and the
 * T6021 fatal range are refused by ane_h15_read_ok(), whatever the
 * soc row says. Nothing is written.
 */
/* Sources of the numbers below, by exact commit.
 * - Wrapper (CPU_CONTROL +0x44, CPU_STATUS +0x48), RVBAR +0x1050000,
 *   mailbox CTRL words +0x1408110 and +0x1408114, and the fatal range
 *   0x1854000..0x1c04000: ane/t6021/ane_t6021.h at 16cfa8720b80677dbcdf8fd1d04077a8b72768bf
 *   (lines 104-109, 142-143 and 160-163). These are T6021 values. Their
 *   H15 roles are INFERENCE (see the b0 receipt).
 * - The two ADT windows (wrapper 0x6c000 bytes at engine + 0x1400000,
 *   RVBAR block 0x4000 bytes at engine + 0x1050000): the shipped system's 27.0 ADT
 *   summary adt-27.0.txt, sha256
 *   04fe38b8ee009700f02e2c01e83bf608afab5e843f5878a83f162973a1ffdb84,
 *   as tabulated (A1, A3) in receipts/2026-10-03-ane-h15/README.md at
 *   71ed021859a3f7e55c04756a1c6ab456fa48fc9f.
 * - CoreSight line engine + 0x1010000: docs/t6021-ane-bringup-findings.md
 *   section 7 at 16cfa8720b80677dbcdf8fd1d04077a8b72768bf.
 */
#define ANE_H15_WRAP_OFF	0x1400000u
#define ANE_H15_WRAP_SIZE	0x6c000u
#define ANE_H15_RVBAR_WIN_OFF	0x1050000u
#define ANE_H15_RVBAR_WIN_SIZE	0x4000u
#define ANE_H15_CORESIGHT	0x1010000u
#define ANE_H15_FATAL_LO	0x1854000u
#define ANE_H15_FATAL_HI	0x1c04000u

static bool ane_h15_read_ok(const struct ane_h15_soc *s, u32 off, u32 size)
{
	u64 end = (u64)off + size;
	bool in_wrap = off >= ANE_H15_WRAP_OFF &&
		       end <= (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE;
	bool in_rvbar = off >= ANE_H15_RVBAR_WIN_OFF &&
			end <= (u64)ANE_H15_RVBAR_WIN_OFF + ANE_H15_RVBAR_WIN_SIZE;

	/* EDPRCR (the ASC CPU power/reset request, t6021 phase1 S2
	 * whitelist) sits outside both windows; it is read once by the
	 * wrapper preflight: a single named address, no writes. */
	if ((off == 0x1010310 || off == 0x1010314) && size == 4)
		return true;
	if (!in_wrap && !in_rvbar)
		return false;
	if (end > s->engine_size)
		return false;
	/* CPU_CONTROL is never read (hostile on T6001), neither the
	 * CoreSight line nor the T6021 fatal range. */
	if (off < s->cpu_control + 4 && end > s->cpu_control)
		return false;
	if (off < ANE_H15_CORESIGHT + 4 && end > ANE_H15_CORESIGHT)
		return false;
	if (off < ANE_H15_FATAL_HI && end > ANE_H15_FATAL_LO)
		return false;
	return true;
}

static int ane_h15_read_word(struct ane_h15 *ane, const char *name, u32 off,
			     u32 *v)
{
	if (!ane_h15_read_ok(ane->soc, off, 4)) {
		dev_err(ane->dev, "ane_h15 refusing read %s engine+%#x\n", name, off);
		return -EPERM;
	}
	dev_crit(ane->dev, "ane_h15 read %s pa=%#llx off=%#x\n", name,
		 ane->soc->engine_pa + off, off);
	ane_h15_step("read %s pa=%#llx\n", name, ane->soc->engine_pa + off);
	*v = readl_relaxed(ane->engine + off);
	dev_info(ane->dev, "ane_h15 word=%s pa=%#llx value=%#x\n", name,
		 ane->soc->engine_pa + off, *v);
	return 0;
}

/* The v4 mailbox receive registers are 64-bit and pop on read (ane_t6021.h:138-147): one readq per register,
 * never two 32-bit reads, which would take the message apart (and eat it). */
static int ane_h15_read_word64(struct ane_h15 *ane, const char *name, u32 off, u64 *v)
{
	if ((off & 7) || !ane_h15_read_ok(ane->soc, off, 8)) {
		dev_err(ane->dev, "ane_h15 refusing 64-bit read %s engine+%#x\n", name, off);
		return -EPERM;
	}
	dev_crit(ane->dev, "ane_h15 read64 %s pa=%#llx off=%#x\n", name,
		 ane->soc->engine_pa + off, off);
	*v = readq_relaxed(ane->engine + off);
	dev_info(ane->dev, "ane_h15 word64=%s pa=%#llx value=%#llx\n", name,
		 ane->soc->engine_pa + off, *v);
	return 0;
}

/* The static offsets hold only for the windows the SoC row describes.
 * Compare a live reg resource, the one of_iomap() mapped, with the row. */
static bool ane_h15_window_ok(struct ane_h15 *ane, int idx, const char *what,
			      u64 pa, u32 size)
{
	struct resource res;
	int ret = of_address_to_resource(ane->dev->of_node, idx, &res);

	if (!ret && res.start == pa && resource_size(&res) == size)
		return true;
	dev_err(ane->dev,
		"ane_h15 %s window mismatch: reg[%d] start=%#llx size=%#llx, SoC row wants start=%#llx size=%#x\n",
		what, idx, ret ? 0ULL : (u64)res.start,
		ret ? 0ULL : (u64)resource_size(&res), pa, size);
	return false;
}

static int ane_h15_translation(struct ane_h15 *ane, u64 rvbar);
static int ane_h15_pwgate_read(struct ane_h15 *ane, char *suffix, size_t sz);

static int ane_h15_stage_wrapper(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 status = 0, rvbar_lo = 0, rvbar_hi = 0, a2i = 0, i2a = 0, edprcr = 0, edpsr = 0;
	unsigned int i, refused = 0;
	char why[128];
	int ret;

	if (!confirm_read) {
		for (i = 0; i < 5; i++) {
			const struct ane_h15_word *w = &s->words[i + 5];

			if (w->tier != ANE15_TIER_ADDR_MEASURED)
				continue;
			dev_info(ane->dev,
				 "word %s tier=ADDR_MEASURED not cleared; src=%s",
				 w->name, w->src);
			refused++;
		}
		snprintf(why, sizeof(why),
			 "%u ADDR_MEASURED word(s) lack a macOS capture; role INFERENCE; confirm_read=1 runs the read-only preflight",
			 refused);
		ane_h15_result(ane, 2, "REFUSED", why);
		return -EPERM;
	}

	/* Both windows are checked before any bus access: the pmgr gate
	 * below reads through reg[1], the preflight through reg[0]. */
	if (!ane_h15_window_ok(ane, 0, "engine", s->engine_pa, s->engine_size) ||
	    !ane_h15_window_ok(ane, 1, "pmgr", s->pmgr_pa, s->pmgr_size)) {
		ane_h15_result(ane, 2, "FAIL", "window-mismatch");
		return -EINVAL;
	}
	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, 2, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &status);
	if (!ret)
		ret = ane_h15_read_word(ane, "EDPRCR", 0x1010310, &edprcr);
	if (!ret)
		ret = ane_h15_read_word(ane, "EDPRSR", 0x1010314, &edpsr);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_LO", s->rvbar, &rvbar_lo);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_HI", s->rvbar + 4, &rvbar_hi);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_A2I_CTRL", s->mbox + 0x110, &a2i);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	if (ret) {
		ane_h15_result(ane, 2, "FAIL", "read-refused");
		return ret;
	}
	/* The T6021 capture of a stopped core reads CPU_STATUS 0x2a and
	 * RVBAR 0x1 (ane/t6021/ane_t6021.h). The H15 meaning of the
	 * bits is not measured; the line only compares. */
	dev_info(ane->dev,
		 "ane_h15 preflight cpu_status=%#x rvbar=%#llx a2i_ctrl=%#x i2a_ctrl=%#x t6021_stopped_status=%s\n",
		 status, ((u64)rvbar_hi << 32) | rvbar_lo, a2i, i2a,
		 status == 0x2a ? "equal" : "differs");
	if (fw_seg || !strcmp(stage, "domain-window"))
		return ane_h15_translation(ane, ((u64)rvbar_hi << 32) | rvbar_lo);
	if (pwgate_read) {
		char pw[48];

		ret = ane_h15_pwgate_read(ane, pw, sizeof(pw));
		if (ret) {
			ane_h15_result(ane, 2, "FAIL", "pwgate-window-refused");
			return ret;
		}
		snprintf(why, sizeof(why), "preflight-read-only %s edprcr=0x%x edpsr=0x%x", pw, edprcr, edpsr);
		ane_h15_result(ane, 2, "PASS", why);
		return 0;
	}
	snprintf(why, sizeof(why), "preflight-read-only edprcr=0x%x edpsr=0x%x", edprcr, edpsr);
	ane_h15_result(ane, 2, "PASS", why);
	return 0;
}

/* ---- stage 3: see header. The refusal is generated from the
 * family-wide fact table (ane_h15_facts, plan section 1.5): one line
 * per still-unmeasured fact, then the RESULT line. Even when every
 * row is filled, THIS build has no boot path: it refuses with a
 * distinct reason and releases no CPU.
 * ----
 */
static void ane_h15_fact_emit(void *ctx, const struct ane_h15_fact *f)
{
	dev_crit((struct device *)ctx, "ane_h15 hole=%s %s (fill: %s)\n",
		 f->id, f->what, f->fill);
}

static int ane_h15_prepare_owned_firmware(struct ane_h15 *ane);
static int ane_h15_run_common(struct ane_h15 *ane, unsigned int stg);

static int ane_h15_stage_boot(struct ane_h15 *ane)
{
	int unfilled, ret;

	if (!confirm_boot) {
		ane_h15_result(ane, 3, "REFUSED",
			       "confirm_boot=1 required");
		return -EPERM;
	}
	if (!fw_path) {
		ane_h15_result(ane, 3, "REFUSED",
			       "fw_path=<kernel firmware name> required for pinned owned-image preparation");
		return -ENOENT;
	}
	ret = ane_h15_prepare_owned_firmware(ane);
	if (ret) {
		ane_h15_result(ane, 3, "REFUSED", "owned-firmware-prepare-failed");
		return ret;
	}
	unfilled = ane_h15_facts_report_stage(ane_h15_facts, ane_h15_n_facts,
					      ane_h15_fact_emit, ane->dev,
					      0x08);
	if (unfilled) {
		ane_h15_result(ane, 3, "REFUSED", "unfilled-facts");
		return -ENOENT;
	}
	/* Every fact row filled: the boot route runs the stage 5 machinery
	 * (run_common, stg=3) behind the boot-time overlay. The overlay
	 * applied at init carries the enabled DART instances and the ane
	 * node's iommus, so map_fw maps through all of them (w91 M6 lesson:
	 * one instance means DMA never completes). Power ran at probe via
	 * the domain attach; run_common re-gates ACTUAL=0xf before any
	 * engine access. */
	if (!apply_overlay || !boot_overlay) {
		ane_h15_result(ane, 3, "REFUSED",
			       "stage 3 needs apply_overlay=1 boot_overlay=1 (the boot blob: DARTs enabled, ane node an IOMMU client)");
		return -EINVAL;
	}
	if (!map_fw) {
		ane_h15_result(ane, 3, "REFUSED",
			       "stage 3 needs map_fw=1 (the core fetches firmware only through the mapped DART)");
		return -EINVAL;
	}
	if (!device_iommu_mapped(ane->dev)) {
		ane_h15_result(ane, 3, "REFUSED",
			       "the boot overlay's DARTs did not bind (no IOMMU client)");
		return -ENODEV;
	}
	return ane_h15_run_common(ane, 3);
}

/* ---- stage 4: is any ANE DART stream enabled? read only ----
 *
 * Sources: ane/h15/run-stage-sources.md (apple-dart.c t8110 at omarchy-linux 94fb2334, data/ane-soc/t6030.json).
 *
 * The M6 (T8152) lesson (an earlier run on a Mac mini M6, boot 235c8ff4, 2026-10-23): RUN alone gave
 * a core that left STOPPED and posted nothing; a later board read of the ANE DART (boot 5f6aa530, PR 183 and 185) found every
 * ENABLE_STREAMS word 0, so iBoot left no stream enabled on the M6. macOS maps TEXT and DATA through the ANE DART before it
 * releases the core. This stage reads, and only reads, what the T6030 DART says, so a tester's first boot answers
 * it instead of a core start.
 *
 * T6030 differs from the M6 here: its ANE DART is `dart,t8110` (ADT S1, data/ane-soc/t6030.json), the register
 * file drivers/iommu/apple-dart.c already drives (PARAMS3 +0x08, PARAMS4 +0x0c, PROTECT +0x200, PROTECT_LOCK
 * +0x208, ENABLE_STREAMS +0xc00, TCR +0x1000+4*sid, TTBR +0x1400+4*sid, one TTBR per stream). The ADT lists four
 * 0x4000 windows; which of them is the main block is not stated (the M6 has a DAPF block among its five). So
 * only a window whose PARAMS3 version equals window 0's is scanned; the others are logged and left alone.
 *
 * Reads: PARAMS3 of each window, then for each scanned window PARAMS4, PROTECT, PROTECT_LOCK, ENABLE_STREAMS
 * word 0 and 1, TCR for streams 0..63 and TTBR where TCR is non-zero. All inside engine+0x1800000..+0x1824000, below the
 * T6021 fatal range, behind the same ACTUAL=0xf gate, each group logged before it is read. A stream is "live"
 * when TCR translate is set, TTBR is valid and its ENABLE bit is set. The stage writes nothing.
 */
#define ANE_H15_DART_WIN	0x4000u
#define ANE_H15_DART_PARAMS3	0x08u
#define ANE_H15_DART_PARAMS4	0x0cu
#define ANE_H15_DART_PROTECT	0x200u
#define ANE_H15_DART_LOCK	0x208u
#define ANE_H15_DART_ENABLE	0xc00u
#define ANE_H15_DART_TCR	0x1000u
#define ANE_H15_DART_TTBR	0x1400u
#define ANE_H15_DART_ERROR	0x100u	/* t8110: FLAG bit31, STREAM 27:20, CODE 14:0 */
#define ANE_H15_DART_ERROR_ADDR_LO	0x170u
#define ANE_H15_DART_ERROR_ADDR_HI	0x174u
#define ANE_H15_DART_SIDS	64u

static const u32 ane_h15_dart_rel[4] = { 0, 0x10000, 0x20000, 0x4000 };

static bool ane_h15_dart_off_ok(u32 off)
{
	return off == ANE_H15_DART_PARAMS3 || off == ANE_H15_DART_PARAMS4 ||
	       off == ANE_H15_DART_PROTECT || off == ANE_H15_DART_LOCK ||
	       off == ANE_H15_DART_ENABLE || off == ANE_H15_DART_ENABLE + 4 ||
	       off == ANE_H15_DART_ERROR || off == ANE_H15_DART_ERROR_ADDR_LO ||
	       off == ANE_H15_DART_ERROR_ADDR_HI ||
	       (off >= ANE_H15_DART_TCR && off < ANE_H15_DART_TCR + 4 * ANE_H15_DART_SIDS && !(off & 3)) ||
	       (off >= ANE_H15_DART_TTBR && off < ANE_H15_DART_TTBR + 4 * ANE_H15_DART_SIDS && !(off & 3));
}

static int ane_h15_dart_rd(struct ane_h15 *ane, unsigned int inst, u32 off, u32 *v)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 woff;
	u64 dt;

	if (!s->dart_off || inst > 3 || !ane_h15_dart_off_ok(off))
		return -EPERM;
	woff = s->dart_off + ane_h15_dart_rel[inst] + off;
	if ((u64)woff + 4 > s->engine_size || woff + 4 > ANE_H15_FATAL_LO)
		return -EPERM;
	ane_h15_step("read dart%u+%#x pa=%#llx\n", inst, off,
		     s->engine_pa + woff);
	if (sweep_watch) {
		u64 t0 = ktime_get_boottime_ns();
		*v = readl_relaxed(ane->engine + woff);
		dt = ktime_get_boottime_ns() - t0;
		{
			unsigned int k;

			ane_h15_step("dart read wall=%lluus\n",
				     dt / 1000ULL);
			if (dt > (u64)sweep_watch_us * 1000ULL) {
				dev_crit(ane->dev,
					 "ane_h15 sweep stall: dart%u+%#x took %lluus\n",
					 inst, off, dt / 1000ULL);
				for (k = 0; k < 5; k++) {
					u32 pv;
					ane_h15_step("sweep stall pmgr.%s pa=%#llx\n",
						     ane_h15_ps_names[k],
						     ane->soc->pmgr_pa + ane->soc->ps_off[k]);
					pv = readl_relaxed(ane->pmgr + ane->soc->ps_off[k]);
					ane_h15_step("sweep stall pmgr.%s reads %#x\n",
						     ane_h15_ps_names[k], pv);
				}
				ane_h15_step("sweep stall dart%u+%#x reads %#x\n",
					     inst, off, *v);
			}
		}
	} else {
		*v = readl_relaxed(ane->engine + woff);
	}
	return 0;
}

static void ane_h15_dart_note(struct ane_h15 *ane, unsigned int inst, const char *what)
{
	const struct ane_h15_soc *s = ane->soc;

	dev_crit(ane->dev, "ane_h15 read DART inst=%u %s pa=%#llx\n", inst, what,
		 s->engine_pa + s->dart_off + ane_h15_dart_rel[inst]);
}

/* *live = number of live streams over the scanned windows; *locked = windows reporting PROTECT_LOCK bit 0. */
static int ane_h15_dart_scan(struct ane_h15 *ane, unsigned int *live, unsigned int *locked)
{
	u32 p3[4] = { 0 }, p4, prot, lock, en[2], tcr, ttbr;
	unsigned int inst, sid, nsid;
	int ref = -1, ret;

	*live = *locked = 0;
	for (inst = 0; inst < 4; inst++) {
		if (!(dart_scan_mask & (1u << inst))) {
			dev_info(ane->dev, "ane_h15 dart scan: instance %u skipped by dart_scan_mask=%#x\n",
				 inst, dart_scan_mask);
			continue;
		}
		ane_h15_dart_note(ane, inst, "PARAMS3");
		ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_PARAMS3, &p3[inst]);
		if (ret)
			return ret;
		dev_info(ane->dev, "ane_h15 dart inst=%u params3=%#x\n", inst, p3[inst]);
		/* the first scanned instance is the version reference, whatever its number */
		if (ref < 0) {
			if (!p3[inst] || p3[inst] == 0xffffffffu || !(p3[inst] & 0xff00)) {
				dev_err(ane->dev, "ane_h15 dart inst=%u PARAMS3 %#x is not a DART version word\n",
					inst, p3[inst]);
				return -ENODEV;
			}
			ref = (int)inst;
		}
		if ((int)inst != ref && (p3[inst] & 0xffff) != (p3[ref] & 0xffff)) {
			dev_info(ane->dev, "ane_h15 dart inst=%u version differs from inst %d: not scanned\n",
				 inst, ref);
			continue;
		}
		ane_h15_dart_note(ane, inst, "PARAMS4 PROTECT PROTECT_LOCK ENABLE_STREAMS[0..1]");
		ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_PARAMS4, &p4);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_PROTECT, &prot);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_LOCK, &lock);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_ENABLE, &en[0]);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_ENABLE + 4, &en[1]);
		if (ret)
			return ret;
		nsid = FIELD_GET(GENMASK(8, 0), p4);
		if (nsid > ANE_H15_DART_SIDS)
			nsid = ANE_H15_DART_SIDS;
		dev_info(ane->dev, "ane_h15 dart inst=%u params4=%#x streams=%u protect=%#x lock=%#x enable0=%#x enable1=%#x\n",
			 inst, p4, nsid, prot, lock, en[0], en[1]);
		if (lock & 1)
			(*locked)++;
		ane_h15_dart_note(ane, inst, "TCR[0..] and TTBR[] where TCR is set");
		for (sid = 0; sid < nsid; sid++) {
			bool on;

			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_TCR + 4 * sid, &tcr);
			if (ret)
				return ret;
			ttbr = 0;
			if (tcr) {
				ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_TTBR + 4 * sid, &ttbr);
				if (ret)
					return ret;
			}
			on = (en[sid / 32] >> (sid % 32)) & 1;
			if (tcr || ttbr || on)
				dev_info(ane->dev, "ane_h15 dart inst=%u sid=%u tcr=%#x ttbr=%#x enabled=%u\n",
					 inst, sid, tcr, ttbr, on);
			if ((tcr & 1) && (ttbr & 1) && on)
				(*live)++;
		}
	}
	return 0;
}

static int ane_h15_stage_dart(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	unsigned int live = 0, locked = 0;
	int ret;

	if (!confirm_read) {
		ane_h15_result(ane, 4, "REFUSED", "confirm_read=1 required");
		return -EPERM;
	}
	if (!s->dart_off) {
		ane_h15_result(ane, 4, "REFUSED", "no ADT DART window for this SoC row");
		return -ENODEV;
	}
	if (!ane_h15_window_ok(ane, 0, "engine", s->engine_pa, s->engine_size) ||
	    !ane_h15_window_ok(ane, 1, "pmgr", s->pmgr_pa, s->pmgr_size)) {
		ane_h15_result(ane, 4, "FAIL", "window-mismatch");
		return -EINVAL;
	}
	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, 4, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	ret = ane_h15_dart_scan(ane, &live, &locked);
	if (ret) {
		ane_h15_result(ane, 4, "FAIL", "dart-read-refused-or-not-a-dart");
		return ret;
	}
	dev_info(ane->dev, "ane_h15 dart summary live_streams=%u locked_windows=%u\n", live, locked);
	ane_h15_result(ane, 4, "PASS", live ? "dart-live-stream-found" : "dart-no-stream-enabled");
	return 0;
}

/* ---- stage 5 map_fw=1: give the core a DART mapping before RUN ----
 *
 * The pattern of the M6 module (ane/h18, PR 185): attach the ANE to its DART through the iommu API, so the DART
 * driver programs tunables, TTBR, TCR and the stream enable with the domains powered, then iommu_map() the two segments
 * the boot loader left in DRAM at the addresses the firmware is linked at. A later M6 board report, relayed without a
 * merged receipt: with the map in place the firmware sent HELLO and EPMAP and accepted replies. None of it has run on
 * an H15 part.
 *
 * What this module cannot know and the tester must give: the physical addresses, which iBoot picks at boot and which the
 * Linux device tree does not carry for the ANE (m1n1 writes asc-firmware reserved regions only for dcp, dcpext and ave).
 * They come from the live ADT segment-ranges of the same boot, as `fw_seg=TEXTPA:SIZE,DATAPA:SIZE`. A wrong address would
 * hand the core a window onto kernel memory, so each segment must be 16 KiB aligned, at most 64 MiB, disjoint from the
 * other and from system RAM (carved-out firmware memory is not in the RAM map). The IOVAs are not asked for: TEXT goes
 * to the RVBAR entry address (rvbar_expect without its valid bit, 0x10000000000 on the T6030) and DATA directly behind it,
 * which is how the M6 ADT lays out its two segments (remap = vm-base + cumulative offset; INFERRED the same on H15).
 * TEXT is read only, DATA read+write (the DART format has no execute bit).
 */
#define ANE_H15_MAP_MAX 0x4000000ull

static int ane_h15_parse_segs(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	unsigned long long pa[2], size[2];
	unsigned int i;

	if (!fw_seg || sscanf(fw_seg, "%llx:%llx,%llx:%llx", &pa[0], &size[0], &pa[1], &size[1]) != 4) {
		dev_err(ane->dev, "ane_h15 map_fw: fw_seg=TEXTPA:SIZE,DATAPA:SIZE (hex) is required\n");
		return -EINVAL;
	}
	for (i = 0; i < 2; i++) {
		if (!size[i] || size[i] > ANE_H15_MAP_MAX || ((pa[i] | size[i]) & (SZ_16K - 1)) || pa[i] + size[i] < pa[i]) {
			dev_err(ane->dev, "ane_h15 map_fw: segment %u pa=%#llx size=%#llx is empty, over 64 MiB, not 16 KiB aligned or wraps\n",
				i, pa[i], size[i]);
			return -EINVAL;
		}
		if (region_intersects(pa[i], size[i], IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) != REGION_DISJOINT) {
			dev_err(ane->dev, "ane_h15 map_fw: segment %u pa=%#llx size=%#llx touches system RAM: refusing to map it into the ANE\n",
				i, pa[i], size[i]);
			return -EBUSY;
		}
	}
	if (pa[0] < pa[1] + size[1] && pa[1] < pa[0] + size[0]) {
		dev_err(ane->dev, "ane_h15 map_fw: the two segments overlap\n");
		return -EINVAL;
	}
	ane->map_pa[0] = pa[0];
	ane->map_pa[1] = pa[1];
	ane->map_size[0] = size[0];
	ane->map_size[1] = size[1];
	ane->map_iova[0] = s->rvbar_expect & ~1ull;
	ane->map_iova[1] = ane->map_iova[0] + size[0];
	return 0;
}

static int ane_h15_translation(struct ane_h15 *ane, u64 rvbar)
{
	struct iommu_domain *domain;
	u64 pa;
	int ret;

	if (!ane->soc->rvbar_expect) {
		ane_h15_result(ane, 2, "REFUSED", "no-measured-rvbar");
		return -ENODEV;
	}
	if (rvbar != ane->soc->rvbar_expect) {
		ane_h15_result(ane, 2, "REFUSED", "rvbar-entry-changed");
		return -EINVAL;
	}
	ret = ane_h15_parse_segs(ane);
	if (ret) {
		ane_h15_result(ane, 2, "REFUSED", "fw_seg missing or unsafe");
		return ret;
	}
	domain = iommu_get_domain_for_dev(ane->dev);
	if (!strcmp(stage, "domain-window")) {
		static const char *const names[] = {
			"domain-window-missing", "domain-window-unbounded",
			"domain-window-outside", "domain-window-inside",
		};
		enum ane_h15_window overall = ANE_WINDOW_INSIDE;
		unsigned int i;

		for (i = 0; i < 2; i++) {
			enum ane_h15_window state = ane_h15_classify_window(domain,
				ane->map_iova[i], ane->map_size[i]);

			dev_info(ane->dev,
				 "ane_h15 domain-window segment=%u iova=%#llx size=%#llx type=%u first=%#llx last=%#llx force=%u state=%s\n",
				 i, ane->map_iova[i], ane->map_size[i],
				 domain ? domain->type : 0,
				 domain ? domain->geometry.aperture_start : 0,
				 domain ? domain->geometry.aperture_end : 0,
				 domain ? domain->geometry.force_aperture : 0, names[state]);
			if (state < overall)
				overall = state;
		}
		ane_h15_result(ane, 2, overall == ANE_WINDOW_INSIDE ? "PASS" : "REFUSED",
			       names[overall]);
		return overall == ANE_WINDOW_INSIDE ? 0 : -ERANGE;
	}
	if (!domain) {
		ane_h15_result(ane, 2, "REFUSED", "translation-domain-unavailable");
		return -ENODEV;
	}
	pa = iommu_iova_to_phys(domain, ane->map_iova[0]);
	dev_info(ane->dev, "ane_h15 translation rvbar_iova=%#llx expected_pa=%#llx resolved_pa=%#llx\n",
		 ane->map_iova[0], ane->map_pa[0], pa);
	if (!pa) {
		ane_h15_result(ane, 2, "REFUSED", "translation-zero");
		return -ENOENT;
	}
	if (pa != ane->map_pa[0]) {
		ane_h15_result(ane, 2, "FAIL", "rvbar-text-entry-mismatch");
		return -EIO;
	}
	ane_h15_result(ane, 2, "PASS", "rvbar-text-entry-match");
	return 0;
}

static bool ane_h15_unmap_range(struct ane_h15 *ane, u64 iova, u64 size)
{
	size_t unmapped;

	if (ane->map_retained)
		return false;
	unmapped = iommu_unmap(ane->domain, iova, size);
	if (unmapped != size) {
		ane->map_retained = true;
		dev_emerg(ane->dev,
			  "ane_h15 incomplete firmware unmap iova=%#llx size=%#llx unmapped=%#zx: retaining domain and buffers; reboot required\n",
			  iova, size, unmapped);
		return false;
	}
	return true;
}

/* Undo ane_h15_map_firmware(). NULL-safe. Never called while the core may be running. */
static void ane_h15_unmap_firmware(struct ane_h15 *ane)
{
	unsigned int i;

	if (!ane->domain || ane->map_retained)
		return;
	for (i = 0; i < ane->map_n; i++)
		if (!ane_h15_unmap_range(ane, ane->map_iova[i], ane->map_size[i]))
			return;
	ane->map_n = 0;
	if (ane->dom_own) {
		iommu_detach_device(ane->domain, ane->dev);
		iommu_domain_free(ane->domain);
	}
	ane->domain = NULL;
	ane->dom_own = false;
}

/* The Linux-owned image as the firmware source. TEXT goes to the RVBAR entry address and DATA directly behind it, exactly as for the
 * iBoot segments; the firmware's own entry code finds DATA at its link address, ANE_H15_FW_TEXT_SIZE behind TEXT. Nothing is written to RVBAR:
 * the entry iBoot left names an IOVA, and this makes that IOVA resolve to the owned pages. Read each 16 KiB page's physical address from
 * the default DMA domain now, because the attach replaces it and the pages are not necessarily contiguous. */
static int ane_h15_plan_owned_map(struct ane_h15 *ane)
{
	struct iommu_domain *dom;
	u64 off;
	/* stage=3 already prepared the image behind its own gates (fwload.h); prepare() refuses a second call with -EBUSY. */
	int ret = ane->owned_fw.va ? 0 : ane_h15_prepare_owned_firmware(ane);

	if (ret)
		return ret;
	dom = iommu_get_domain_for_dev(ane->dev);
	for (off = 0; off < ANE_H15_FW_IMAGE_SIZE; off += SZ_16K) {
		u64 pa = iommu_iova_to_phys(dom, ane->owned_fw.iova + off);

		if (!pa || (pa & (SZ_16K - 1)))
			return -EIO;
		ane->owned_pa[off / SZ_16K] = pa;
	}
	ane->map_pa[0] = ane->owned_pa[0];
	ane->map_pa[1] = ane->owned_pa[ANE_H15_FW_TEXT_SIZE / SZ_16K];
	ane->map_size[0] = ANE_H15_FW_TEXT_SIZE;
	ane->map_size[1] = ANE_H15_FW_IMAGE_SIZE - ANE_H15_FW_TEXT_SIZE;
	ane->map_iova[0] = ane->soc->rvbar_expect & ~1ull;
	ane->map_iova[1] = ane->map_iova[0] + ANE_H15_FW_TEXT_SIZE;
	return 0;
}

/* Map one segment of the owned image in physically contiguous runs. A failure unmaps every run of this segment mapped so far. */
static int ane_h15_map_owned_segment(struct ane_h15 *ane, struct iommu_domain *dom, unsigned int seg)
{
	const u64 first = seg ? ANE_H15_FW_TEXT_SIZE : 0;
	const u64 end = first + ane->map_size[seg];
	const int prot = IOMMU_READ | (seg ? IOMMU_WRITE : 0);
	u64 off = first;
	int ret = 0;

	while (off < end) {
		u64 pa = ane->owned_pa[off / SZ_16K], len = SZ_16K, iova = ane->map_iova[seg] + off - first;

		while (off + len < end && ane->owned_pa[(off + len) / SZ_16K] == pa + len)
			len += SZ_16K;
		ret = iommu_map(dom, iova, pa, len, prot, GFP_KERNEL);
		if (ret)
			break;
		off += len;
		if (iommu_iova_to_phys(dom, iova) != pa) {
			ret = -EIO;
			break;
		}
	}
	if (ret && off > first)
		ane_h15_unmap_range(ane, ane->map_iova[seg], off - first);
	return ret;
}

static int ane_h15_map_firmware(struct ane_h15 *ane)
{
	struct iommu_domain *dom;
	unsigned int i;
	int ret;

	if (ane->chan) {
		/* The DMA API (the handshake surfaces) allocates in the default domain, so the segments go into that one and nothing is attached. */
		dom = iommu_get_domain_for_dev(ane->dev);
		if (!dom || !(dom->type & __IOMMU_DOMAIN_DMA_API)) {
			dev_err(ane->dev, "ane_h15 map_fw: the ANE has no default DMA domain\n");
			return -ENODEV;
		}
		dev_crit(ane->dev, "ane_h15 map the firmware into the default DMA domain of the ANE (no attach)\n");
	} else {
		dom = iommu_paging_domain_alloc(ane->dev);
		if (IS_ERR(dom)) {
			dev_err(ane->dev, "ane_h15 map_fw: no iommu domain: %ld\n", PTR_ERR(dom));
			return PTR_ERR(dom);
		}
		dev_crit(ane->dev, "ane_h15 attach the ANE to a new DART domain (the DART driver programs the DART now)\n");
		ret = iommu_attach_device(dom, ane->dev);
		if (ret) {
			dev_err(ane->dev, "ane_h15 map_fw: attach failed: %d\n", ret);
			iommu_domain_free(dom);
			return ret;
		}
	}
	ane->dom_own = !ane->chan;
	ane->domain = dom;
	for (i = 0; i < 2; i++) {
		dev_crit(ane->dev, "ane_h15 iommu_map iova=%#llx pa=%#llx size=%#llx %s\n", ane->map_iova[i], ane->map_pa[i],
			 ane->map_size[i], i ? "read+write" : "read");
		if (ane->owned_map) {
			ret = ane_h15_map_owned_segment(ane, dom, i);
			if (!ret)
				ane->map_n = i + 1;
		} else {
			ret = iommu_map(dom, ane->map_iova[i], ane->map_pa[i], ane->map_size[i],
					IOMMU_READ | (i ? IOMMU_WRITE : 0), GFP_KERNEL);
			if (!ret) {
				ane->map_n = i + 1;
				if (iommu_iova_to_phys(dom, ane->map_iova[i]) != ane->map_pa[i])
					ret = -EIO;
			}
		}
		if (ret) {
			dev_err(ane->dev, "ane_h15 map_fw: iommu_map of segment %u failed: %d (or reads back wrong)\n", i, ret);
			ane_h15_unmap_firmware(ane);
			return ret;
		}
	}
	return 0;
}

/* ---- stage 5: start the ASC core once, watch, stop it again ----
 *
 * Sources, commit-pinned, with H15 observations told apart from cross-generation inference:
 * ane/h15/run-stage-sources.md (m1n1 asc.py 97820568, macOS _runCPU decode sha d383f674, ane_t6021.h
 * 16cfa8720b80677dbcdf8fd1d04077a8b72768bf, omarchy-linux mailbox.c 57f8f6de, the M6 boot 235c8ff4).
 *
 * Why this is the next step. The T6030 stage 2 run (PR 175, J516S) read RVBAR 0x0000010000000001 and CPU_STATUS
 * 0x0030002a: the boot loader already preloaded the firmware and wrote the entry address, and the core is stopped (m1n1
 * R_CPU_STATUS: STOPPED bit 1 and IDLE bit 5 set, RUNNING bit 0 clear; bits 20 and 21 unknown on H15). The T8152
 * (M6) ANE0 is in the same state (CPU_STATUS 0xa, IORVBAR 0x10000000001). On the M6 the same RUN write took
 * CPU_STATUS from 0xa to 0x8 in under 11 ms: STOPPED clears, RUNNING (bit 0) does not read 1. So "started" is
 * tested as STOPPED cleared here, not as RUNNING set.
 *
 * What it does, in order (every access address is logged BEFORE it):
 *   1. refuses without confirm_run=1, with run_wait_ms outside 10..2000, and on a SoC row without a measured
 *      rvbar_expect (nothing is touched);
 *   2. checks both live reg windows against the SoC row, waits for the five pmgr words to read ACTUAL=0xf,
 *      then runs the stage 4 DART scan and refuses if no stream is live (run_unmapped=1 overrides);
 *   3. preflight, read only: CPU_STATUS (STOPPED set, RUNNING clear), CPU_CONTROL (RUN clear), RVBAR (exactly
 *      rvbar_expect) and both mailbox CTRL words (EMPTY, bit 17, set: nothing stale). Anything else refuses;
 *   4. CPU_CONTROL |= RUN (bit 4), the macOS sequence (read, OR 0x10, write);
 *      with scratch_boot=1 instead the whole macOS cold-boot order runs first: 0 to SCRATCH0..7, SCRATCH6=1,
 *      SCRATCH7=0 (read back), CPU_CONTROL 0 then 0x10, then a 1 ms poll of SCRATCH7 for the ready word
 *      0x08042006 (ane_h15_scratch_boot below);
 *      with pwgate_t6031 (rows with a PWGATE window) macOS's PWGATE gate clear runs last, after the
 *      macOS SCRATCH sequence (if any) and before the first CPU_CONTROL write (ane_h15_pwgate_open below);
 *   5. for run_wait_ms every 10 ms: CPU_STATUS and I2A_CTRL; if I2A is not EMPTY it pops the message
 *      (I2A_RECV0 at wrapper+0x8830, I2A_RECV1 at +0x8838, the Linux apple-mailbox v4 layout the other T6030 ASCs
 *      use) and logs the raw words, at most 4 messages. It sends NOTHING and never replies to a HELLO;
 *   6. stops the core the macOS way (_runCPU(false)): clear bit 4, write; re-read, clear bit 5, write; waits for
 *      STOPPED. If the core does not stop the power domains stay on, the windows stay mapped and remove()
 *      refuses: reboot.
 * It writes only CPU_CONTROL (bits 4 and 5), never RVBAR, never the mailbox, never the DART; with
 * scratch_boot=1 it also writes the eight SCRATCH words, each only as 0 except SCRATCH6=1 (macOS's
 * helper zeroes all eight before it sets the wake bit, and its SCRATCH5 config value is unknown, so
 * SCRATCH5 receives 0 only); with pwgate_t6031 on a row with a PWGATE window it also writes the one PWGATE word of that row
 * (0x199c on a T6031; nothing else in that window). The order is the one the shipped system uses, never run on an M3. Nothing here
 * programs the DART: a core that cannot fetch its firmware may simply sit started. That is a result, not a failure of this stage.
 */
#define ANE_H15_ST_RUNNING	BIT(0)
#define ANE_H15_ST_STOPPED	BIT(1)
#define ANE_H15_CPU_RUN		BIT(4)
#define ANE_H15_CPU_BIT5	BIT(5)
#define ANE_H15_MBOX_EMPTY	BIT(17)
#define ANE_H15_MBOX_I2A_RECV0	0x830u
#define ANE_H15_MBOX_I2A_RECV1	0x838u
#define ANE_H15_RUN_MAX_MSGS	4u

/* steplog: one line per MMIO access, on the disk before the access runs. A run that
 * hangs the machine leaves its last access in the file; the kmsg ring dies with RAM. */
static struct file *ane_stepf;

static void ane_h15_step(const char *fmt, ...);

static int ane_h15_steplog_open(struct device *dev, unsigned int stg)
{
	if (!steplog)
		return 0;
	ane_stepf = filp_open(steplog, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (IS_ERR(ane_stepf)) {
		dev_err(dev, "ane_h15 steplog: open %s failed: %ld\n", steplog, PTR_ERR(ane_stepf));
		ane_stepf = NULL;
		return -EIO;
	}
	/* the init line: a hung or crashed machine that never reached MMIO still
	 * leaves proof that this module ran, and the header dates the open */
	ane_h15_step("module init stage=%u\n", stg);
	return 0;
}

static void ane_h15_steplog_close(void)
{
	if (ane_stepf) {
		filp_close(ane_stepf, NULL);
		ane_stepf = NULL;
	}
}

static void ane_h15_step(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	loff_t pos = 0;
	u64 ns = ktime_get_boottime_ns();
	int n;

	if (!ane_stepf)
		return;
	n = scnprintf(buf, sizeof(buf), "%llu.%06llu ", ns / 1000000000ULL,
		      (ns % 1000000000ULL) / 1000ULL);
	if (ane_first_ps_ns)
		n += scnprintf(buf + n, sizeof(buf) - n, "q=%llu.%03llums ",
			       (ns - ane_first_ps_ns) / 1000000ULL,
			       ((ns - ane_first_ps_ns) % 1000000ULL) / 1000ULL);
	else
		n += scnprintf(buf + n, sizeof(buf) - n, "q=pre ");
	va_start(ap, fmt);
	n += vscnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	kernel_write(ane_stepf, buf, n, &pos);
	vfs_fsync(ane_stepf, 0);
}

static int ane_h15_read_ctrl(struct ane_h15 *ane, u32 *v)
{
	const struct ane_h15_soc *s = ane->soc;

	if (s->cpu_control < ANE_H15_WRAP_OFF ||
	    (u64)s->cpu_control + 4 > (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE ||
	    (u64)s->cpu_control + 4 > s->engine_size)
		return -EPERM;
	ane_h15_step("read CPU_CONTROL pa=%#llx\n", s->engine_pa + s->cpu_control);
	dev_crit(ane->dev, "ane_h15 read CPU_CONTROL pa=%#llx off=%#x\n",
		 s->engine_pa + s->cpu_control, s->cpu_control);
	*v = readl_relaxed(ane->engine + s->cpu_control);
	dev_info(ane->dev, "ane_h15 word=CPU_CONTROL pa=%#llx value=%#x\n",
		 s->engine_pa + s->cpu_control, *v);
	return 0;
}

/* The single sanctioned debug-block write: set EDPRCR bit0, the ASC CPU
 * power-up request (the M2 order step). Never clears bits, never writes
 * any other debug-block address. */
static int ane_h15_write_edprcr(struct ane_h15 *ane, u32 old, u32 val)
{
	if (!(val & 1u) || (old ^ val) & ~1u) {
		dev_err(ane->dev, "ane_h15 refusing EDPRCR write %#x over %#x: only bit0 may be set\n", val, old);
		return -EPERM;
	}
	ane_h15_step("write EDPRCR=%#x pa=%#llx\n", val, ane->soc->engine_pa + 0x1010310);
	dev_crit(ane->dev, "ane_h15 write EDPRCR=%#x pa=%#llx (core power-up request)\n", val,
		 ane->soc->engine_pa + 0x1010310);
	writel(val, ane->engine + 0x1010310);
	return 0;
}

/* The only engine write this module has: CPU_CONTROL, and only bits 4 (RUN) and 5. */
static int ane_h15_write_ctrl(struct ane_h15 *ane, u32 old, u32 val, const char *why)
{
	const struct ane_h15_soc *s = ane->soc;

	if ((old ^ val) & ~(ANE_H15_CPU_RUN | ANE_H15_CPU_BIT5)) {
		dev_err(ane->dev, "ane_h15 refusing CPU_CONTROL write %#x over %#x: bits other than 4 and 5 differ\n", val, old);
		return -EPERM;
	}
	if (s->cpu_control < ANE_H15_WRAP_OFF ||
	    (u64)s->cpu_control + 4 > (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE ||
	    (u64)s->cpu_control + 4 > s->engine_size)
		return -EPERM;
	ane_h15_step("write CPU_CONTROL=%#x pa=%#llx (%s)\n", val, s->engine_pa + s->cpu_control, why);
	dev_crit(ane->dev, "ane_h15 write CPU_CONTROL=%#x pa=%#llx (%s)\n", val,
		 s->engine_pa + s->cpu_control, why);
	writel(val, ane->engine + s->cpu_control);
	return 0;
}

/* Stopped = STOPPED set and RUNNING clear (m1n1 R_CPU_STATUS; the M6 RUN write cleared STOPPED, RUNNING never read 1). */
static bool ane_h15_core_stopped(u32 st)
{
	return !(st & ANE_H15_ST_RUNNING) && (st & ANE_H15_ST_STOPPED);
}

#include "ane_h15_fwload.h"

/* ---- stage 6: an RTKit management session over the ASC mailbox ----
 *
 * The M6 module (ane/h18, PR 186 and 190) runs this session on the M6's Postoffice: the firmware sent HELLO (version
 * 12), an endpoint map (endpoints 0, 1 and 0x20), and acknowledged the power states; the host answered HELLO, the
 * map, started the system endpoints, answered the IOP power ON acknowledgement with AP power ON, started the
 * application endpoint, and shut the session down with AP QUIESCED then IOP SLEEP (restartable) before it cleared RUN.
 * That is the Linux apple-rtkit message sequence (drivers/soc/apple/rtkit.c, management endpoint 0): message type in
 * bits 59:52 of msg0, the endpoint in the low byte of msg1.
 *
 * What differs here: the transport. The M6 ANE (ascwrap-v8) uses a Postoffice window. The H15 ANE is
 * `iop,ascwrap-v6`, the ASC wrapper has its own mailbox at wrapper + 0x8000, and its two CTRL words read 0x20001 (empty)
 * on a T6030 (PR 175). The send and receive registers used here are the apple-mailbox v4 layout the other T6030 ASCs
 * use: A2I_SEND0 +0x800, A2I_SEND1 +0x808 (64-bit, SEND0 first), I2A_RECV0 +0x830, I2A_RECV1 +0x838 (64-bit, pop on
 * read). INFERRED for the ANE, never written on an H15 part. Whether the H15 ANE firmware talks RTKit on this mailbox
 * at all is what stage 5's raw message log would show: run stage 6 only after a stage 5 result with a message in it.
 *
 * Writes of this stage beyond stage 5: ONLY the two send registers, 16 messages at most, never when A2I_CTRL reads
 * FULL, never anything but the management endpoint (msg1 = 0). It answers no buffer request (the firmware's crash log
 * request on endpoint 1 stays unanswered, as on the M6) and nothing it does not recognise. Sources:
 * ane/h15/run-stage-sources.md and the h18 module.
 */
#define ANE_H15_MBOX_FULL	BIT(16)
#define ANE_H15_MBOX_A2I_SEND0	0x800u
#define ANE_H15_MBOX_A2I_SEND1	0x808u
#define ANE_H15_RTK_TYPE_SHIFT	52
#define ANE_H15_RTK_HELLO	1
#define ANE_H15_RTK_HELLO_REPLY	2
#define ANE_H15_RTK_STARTEP	5
#define ANE_H15_RTK_SET_IOP_PWR	6
#define ANE_H15_RTK_IOP_PWR_ACK	7
#define ANE_H15_RTK_EPMAP	8
#define ANE_H15_RTK_AP_PWR	0xb
#define ANE_H15_RTK_IOP_IDLE	0xc
#define ANE_H15_RTK_PWR_OFF	0x00
#define ANE_H15_RTK_PWR_SLEEP	0x01
#define ANE_H15_RTK_PWR_QUIESCED 0x10
#define ANE_H15_RTK_PWR_ON	0x20
#define ANE_H15_RTK_APP_EP	0x20
#define ANE_H15_RTK_VERSION	12
#define ANE_H15_RTK_MAXSEND	16
#define ANE_H15_RTK_MAXRX_TICK	4
#define ANE_H15_RTK_SHUTDOWN_MS	600

struct ane_h15_rtk {
	bool hello, epmap_last, ap_sent, ap_on, app_started, shutdown, ap_acked, iop_acked, iop_idle;
	u32 eps[8];
	u32 ap_state, iop_state;
	unsigned int sent, rx;
	int hs_ret;
	bool hs_done;
};

static int ane_h15_rtk_send(struct ane_h15 *ane, struct ane_h15_rtk *k, u64 m0)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 a2i = 0;
	unsigned int tries;
	int ret = 0;

	if (k->sent >= ANE_H15_RTK_MAXSEND) {
		dev_warn(ane->dev, "ane_h15 rtkit: send cap reached, not sending %#llx\n", m0);
		return -EBUSY;
	}
	if ((u64)s->mbox + ANE_H15_MBOX_A2I_SEND1 + 8 > s->engine_size || s->mbox + ANE_H15_MBOX_A2I_SEND0 < ANE_H15_WRAP_OFF ||
	    (u64)s->mbox + ANE_H15_MBOX_A2I_SEND1 + 8 > (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE)
		return -EPERM;
	for (tries = 0; tries < 10; tries++) {
		ret = ane_h15_read_word(ane, "MBOX_A2I_CTRL", s->mbox + 0x110, &a2i);
		if (ret)
			return ret;
		if (!(a2i & ANE_H15_MBOX_FULL))
			break;
		usleep_range(1000, 2000);
	}
	if (a2i & ANE_H15_MBOX_FULL) {
		dev_warn(ane->dev, "ane_h15 rtkit: A2I mailbox stays FULL, not sending %#llx\n", m0);
		return -EBUSY;
	}
	dev_crit(ane->dev, "ane_h15 write MBOX_A2I_SEND0=%#llx pa=%#llx then SEND1=0 (management endpoint)\n", m0,
		 s->engine_pa + s->mbox + ANE_H15_MBOX_A2I_SEND0);
	writeq_relaxed(m0, ane->engine + s->mbox + ANE_H15_MBOX_A2I_SEND0);
	writeq_relaxed(0, ane->engine + s->mbox + ANE_H15_MBOX_A2I_SEND1);
	k->sent++;
	return 0;
}

static bool ane_h15_rtk_system_ep(unsigned int ep)
{
	return ep == 1 || ep == 2 || ep == 3 || ep == 4 || ep == 8 || ep == 0xa;
}

/* One received message: reply to HELLO and the endpoint map, start the endpoints, walk the power states. */
static void ane_h15_rtk_handle(struct ane_h15 *ane, struct ane_h15_rtk *k, u64 m0, u64 m1)
{
	unsigned int type = (m0 >> ANE_H15_RTK_TYPE_SHIFT) & 0xff, i;

	dev_info(ane->dev, "ane_h15 rtkit rx type=%u ep=%#llx msg0=%#llx\n", type, m1 & 0xff, m0);
	if (m1 & 0xff)
		return;			/* not the management endpoint: logged, never answered */
	if (type == ANE_H15_RTK_HELLO) {
		unsigned int minv = m0 & 0xffff, maxv = (m0 >> 16) & 0xffff;

		k->hello = true;
		if (minv > ANE_H15_RTK_VERSION || maxv < ANE_H15_RTK_VERSION) {
			dev_warn(ane->dev, "ane_h15 rtkit: firmware versions %u..%u do not include %u, no reply\n", minv, maxv, ANE_H15_RTK_VERSION);
			return;
		}
		ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_HELLO_REPLY << ANE_H15_RTK_TYPE_SHIFT) |
				 ((u64)ANE_H15_RTK_VERSION << 16) | ANE_H15_RTK_VERSION);
	} else if (type == ANE_H15_RTK_EPMAP) {
		unsigned int base = (m0 >> 32) & 7;
		bool last = m0 & (1ULL << 51);

		for (i = 0; i < 32; i++)
			if (m0 & (1ULL << i)) {
				dev_info(ane->dev, "ane_h15 rtkit endpoint %#x discovered\n", base * 32 + i);
				k->eps[base] |= 1u << i;
			}
		if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_EPMAP << ANE_H15_RTK_TYPE_SHIFT) | (m0 & (7ULL << 32)) |
				     (last ? (1ULL << 51) : 1ULL)))
			return;
		if (last) {
			k->epmap_last = true;
			for (i = 0; i < 256; i++) {
				if (!(k->eps[i / 32] & (1u << (i % 32))) || !ane_h15_rtk_system_ep(i))
					continue;
				if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_STARTEP << ANE_H15_RTK_TYPE_SHIFT) | ((u64)i << 32) | (1ULL << 1)))
					break;
			}
		}
	} else if (type == ANE_H15_RTK_IOP_PWR_ACK) {
		dev_info(ane->dev, "ane_h15 rtkit: IOP power state acknowledged: %#llx\n", m0 & 0xffff);
		if (k->shutdown && (m0 & 0xffff) == k->iop_state)
			k->iop_acked = true;
		else if (!k->shutdown && !k->ap_sent && (m0 & 0xff) == ANE_H15_RTK_PWR_ON &&
			 !ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_AP_PWR << ANE_H15_RTK_TYPE_SHIFT) | ANE_H15_RTK_PWR_ON))
			k->ap_sent = true;
	} else if (type == ANE_H15_RTK_AP_PWR) {
		dev_info(ane->dev, "ane_h15 rtkit: AP power state acknowledged: %#llx\n", m0 & 0xffff);
		if (k->shutdown && (m0 & 0xffff) == k->ap_state) {
			k->ap_acked = true;
			ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_SET_IOP_PWR << ANE_H15_RTK_TYPE_SHIFT) | k->iop_state);
		} else if (!k->shutdown && (m0 & 0xff) == ANE_H15_RTK_PWR_ON && !k->ap_on) {
			k->ap_on = true;
			if (!k->epmap_last)
				return;
			for (i = ANE_H15_RTK_APP_EP; i < 256; i++) {
				if (!(k->eps[i / 32] & (1u << (i % 32))))
					continue;
				if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_STARTEP << ANE_H15_RTK_TYPE_SHIFT) | ((u64)i << 32) | (1ULL << 1)))
					return;
			}
			k->app_started = true;
		}
	} else if (type == ANE_H15_RTK_IOP_IDLE) {
		dev_info(ane->dev, "ane_h15 rtkit: the firmware reports IOP idle\n");
		if (k->shutdown)
			k->iop_idle = true;
	}
}

/* Drain at most ANE_H15_RTK_MAXRX_TICK messages: I2A_CTRL, then RECV0 and RECV1 as 64-bit reads. */
static int ane_h15_rtk_poll_once(struct ane_h15 *ane, struct ane_h15_rtk *k)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 i2a = 0;
	u64 m0, m1;
	unsigned int n = 0;
	int ret;

	ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	while (!ret && !(i2a & ANE_H15_MBOX_EMPTY) && n < ANE_H15_RTK_MAXRX_TICK) {
		m0 = m1 = 0;
		ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV0", s->mbox + ANE_H15_MBOX_I2A_RECV0, &m0);
		if (!ret)
			ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV1", s->mbox + ANE_H15_MBOX_I2A_RECV1, &m1);
		if (ret)
			break;
		dev_info(ane->dev, "ane_h15 msg%u raw0=%#llx raw1=%#llx\n", k->rx++, m0, m1);
		ane_h15_rtk_handle(ane, k, m0, m1);
		n++;
		ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	}
	return ret;
}

/* ---- stage 7: the macOS SCRATCH handshake and the channel step ----
 *
 * The M6 module (ane/h18, PRs 195, 196, 205) found the wall this stage is for: the firmware ran RTKit, but the
 * restartable stop got no acknowledgement and the core did not stop until the channels existed. macOS, after AP
 * power ON, allocates surfaces, publishes a 0x174 byte init block in SCRATCH0/1, writes a wake word to SCRATCH7, reads the
 * firmware's channel table back, makes the host the owner of every host-to-firmware ring slot, writes READY to SCRATCH3
 * and waits for the firmware to clear it. This stage does that, and with chan_cmds=1 sends PRINT_ENABLE and CONFIG_GET.
 *
 * Register offsets (the T6030 case, ane-type 0xc0, h15g): SCRATCH0..7 = engine + 0x1840050 .. + 0x184006c,
 * doorbell = engine + 0x1844000 (the shipped system writes (1 << bit) there), config size 0x380000. The interrupt pair is:
 * pending at engine + 0x184c000, acknowledge at engine + 0x1850000. The shipped system reads the pending word (0xffff when it
 * polls every channel instead), writes the value back to the acknowledge register, then dispatches per channel on
 * the bits it read. The M6 module services the same pair at engine + 0x18ac000 / + 0x18b0000; without the write
 * the firmware can wait for an acknowledge that never comes. Never read or written on an H15 part. The block
 * layout and the table layout are the M6/T6021 ones. The M6 numbers are different (SCRATCH at
 * engine + 0x18a0024, doorbell 0x18a4000): do not mix them up.
 *
 * Writes of this stage beyond stage 6: SCRATCH0, 1, 7 and 3 only (anything else refused), DMA memory it allocated,
 * the doorbell only for a command (chan_cmds=1), and the acknowledge register only with the value just read from the
 * pending word, at most ANE_H15_IPI_ACK_MAX per run. Nothing is written before the whole table checked out.
 */
#define ANE_H15_SCRATCH_OFF	0x1840050u
#define ANE_H15_SCRATCH_N	8
#define ANE_H15_DOORBELL_OFF	0x1844000u
#define ANE_H15_IPI_PENDING_OFF	0x184c000u
#define ANE_H15_IPI_ACK_OFF	0x1850000u
#define ANE_H15_IPI_ACK_MAX	64u
#define ANE_H15_HS_WAKE		0xf7fbdff9u
#define ANE_H15_HS_READY	0x08042006u
#define ANE_H15_HS_REQ_LIMIT	0x21
#define ANE_H15_HS_POOL_SIZE	0x40000
#define ANE_H15_HS_IPC_MAX	0x100000
#define ANE_H15_HS_HEAP_MAX	0x4000000
#define ANE_H15_HS_CFG_SIZE	0x380000
#define ANE_H15_CH_COUNT	8
#define ANE_H15_CH_ENTRY	0x100
#define ANE_H15_CH_NAME_LEN	0x40
#define ANE_H15_CH_SLOT		0x40
#define ANE_H15_CH_MAX_SLOTS	0x400
#define ANE_H15_CMD_BLOCK	0x4000
#define ANE_H15_CSNE_CONFIG_GET	0x03
#define ANE_H15_CSNE_PRINT_ENABLE 0x04

struct ane_h15_ring {
	u64 *base;
	u64 iova;
	u32 slots, bit, type;
	unsigned int cursor;
};

static int ane_h15_scr_rd(struct ane_h15 *ane, unsigned int i, u32 *v)
{
	u32 off = ANE_H15_SCRATCH_OFF + 4 * i;

	if (i >= ANE_H15_SCRATCH_N || (u64)off + 4 > ane->soc->engine_size)
		return -EPERM;
	dev_crit(ane->dev, "ane_h15 read SCRATCH%u pa=%#llx\n", i, ane->soc->engine_pa + off);
	ane_h15_step("read SCRATCH%u pa=%#llx\n", i, ane->soc->engine_pa + off);
	*v = readl_relaxed(ane->engine + off);
	return 0;
}

/* a polled read: the address was logged by the first read of this word */
static u32 ane_h15_scr_peek(struct ane_h15 *ane, unsigned int i)
{
	return readl_relaxed(ane->engine + ANE_H15_SCRATCH_OFF + 4 * i);
}

/* Stage 7 only: one poll of the firmware's IPI pending word, as macOS's interrupt path does: read engine +
 * 0x184c000, and when a bit is set write the same value back to engine + 0x1850000. The pending register is never
 * written; a zero pending writes nothing; the acknowledges are capped per run. The window check uses the larger of
 * the two offsets. */
static void ane_h15_ipi_poll(struct ane_h15 *ane)
{
	u32 pend;

	if ((u64)ANE_H15_IPI_ACK_OFF + 4 > ane->soc->engine_size)
		return;
	ane_h15_step("read IPI_PENDING pa=%#llx\n", ane->soc->engine_pa + ANE_H15_IPI_PENDING_OFF);
	pend = readl_relaxed(ane->engine + ANE_H15_IPI_PENDING_OFF);
	if (pend != ane->ipi_last) {
		dev_info(ane->dev, "ane_h15 read IPI_PENDING=%#x pa=%#llx\n", pend, ane->soc->engine_pa + ANE_H15_IPI_PENDING_OFF);
		ane->ipi_last = pend;
	}
	if (pend && ane->ipi_acks < ANE_H15_IPI_ACK_MAX) {
		ane->ipi_acks++;
		dev_crit(ane->dev, "ane_h15 write IPI_ACK=%#x pa=%#llx\n", pend, ane->soc->engine_pa + ANE_H15_IPI_ACK_OFF);
		ane_h15_step("write IPI_ACK=%#x pa=%#llx\n", pend, ane->soc->engine_pa + ANE_H15_IPI_ACK_OFF);
		writel(pend, ane->engine + ANE_H15_IPI_ACK_OFF);
	}
}

/* Stage 5, ps_assert=1: the power-state re-assert of the shipped boot. Writes
 * 0xf to the four ANE power-state words in the pmgr window, reads them back,
 * every access stepped. Holds the domain across the DART sweep. */
static int ane_h15_ps_assert(struct ane_h15 *ane)
{
	unsigned int i;
	u32 v;

	/* ps_off[1..4]: the per-chip layout, never a hardcoded one. T6031 and
	 * T6034 carry their ANE power-state words at 0x5a0/0x5a8/0x5b8/0x5c0;
	 * writing the T8122 layout there would hit foreign pmgr words. */
	for (i = 1; i < 5; i++) {
		ane_h15_step("ps assert write pmgr.%s pa=%#llx val=0xf\n",
			     ane_h15_ps_names[i],
			     ane->soc->pmgr_pa + ane->soc->ps_off[i]);
		dev_crit(ane->dev, "ane_h15 ps assert: write pmgr.%s = 0xf\n",
			 ane_h15_ps_names[i]);
		writel_relaxed(0xf, ane->pmgr + ane->soc->ps_off[i]);
	}
	for (i = 1; i < 5; i++) {
		ane_h15_step("ps assert readback pmgr.%s\n", ane_h15_ps_names[i]);
		v = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
		dev_info(ane->dev, "ane_h15 ps assert: %s reads %#x\n",
			 ane_h15_ps_names[i], v);
	}
	return 0;
}

/* Stage 5 only, once after the wait and before RUN is cleared: the final CPU_STATUS and both mailbox CTRL words on one
 * line. Read only, all inside the ADT-published wrapper window; each address is logged by ane_h15_read_word before its
 * read. The interrupt-pending word (+0x184c000) is deliberately NOT read here: it lies outside both published windows
 * and stays refused by ane_h15_read_ok(); stage 7 reads it only behind its table check. With no mailbox message this
 * line still says whether the core left STOPPED and whether either mailbox side filled. */
static void ane_h15_post_wait_status(struct ane_h15 *ane)
{
	u32 st = 0, a2i = 0, i2a = 0;

	if (ane_h15_read_word(ane, "CPU_STATUS", ane->soc->cpu_status, &st) ||
	    ane_h15_read_word(ane, "MBOX_A2I_CTRL", ane->soc->mbox + 0x110, &a2i) ||
	    ane_h15_read_word(ane, "MBOX_I2A_CTRL", ane->soc->mbox + 0x114, &i2a))
		return;
	dev_info(ane->dev, "ane_h15 post-wait cpu_status=%#x a2i_ctrl=%#x i2a_ctrl=%#x\n", st, a2i, i2a);
}

/* Stage 5, run_scratch=1: the eight SCRATCH words on one line, read only. */
static void ane_h15_scr_dump(struct ane_h15 *ane, const char *when)
{
	u32 v[ANE_H15_SCRATCH_N];
	unsigned int i;

	for (i = 0; i < ANE_H15_SCRATCH_N; i++)
		if (ane_h15_scr_rd(ane, i, &v[i]))
			return;
	dev_info(ane->dev, "ane_h15 scratch %s: S0=%#x S1=%#x S2=%#x S3=%#x S4=%#x S5=%#x S6=%#x S7=%#x\n", when,
		 v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}

/* Stage 5, run_pmgr=1: the five pmgr ANE words on one line, read only. */
static void ane_h15_pmgr_dump(struct ane_h15 *ane, const char *when)
{
	u32 v[5];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(v); i++) {
		ane_h15_step("read pmgr.%s pa=%#llx\n", ane_h15_ps_names[i],
			     ane->soc->pmgr_pa + ane->soc->ps_off[i]);
		v[i] = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
	}
	dev_info(ane->dev, "ane_h15 pmgr %s: ANE_SYS=%#x ANE_MPM=%#x ANE_CPU=%#x ANE_TD=%#x ANE_BASE=%#x\n", when,
		 v[0], v[1], v[2], v[3], v[4]);
}

static int ane_h15_scr_wr(struct ane_h15 *ane, unsigned int i, u32 v)
{
	u32 off = ANE_H15_SCRATCH_OFF + 4 * i;

	if ((i != 0 && i != 1 && i != 3 && i != 7) || (u64)off + 4 > ane->soc->engine_size)
		return -EPERM;
	ane_h15_step("write SCRATCH%u=%#x pa=%#llx\n", i, v, ane->soc->engine_pa + off);
	dev_crit(ane->dev, "ane_h15 write SCRATCH%u=%#x pa=%#llx\n", i, v, ane->soc->engine_pa + off);
	writel(v, ane->engine + off);
	return 0;
}

/* scratch_boot=1 writes only: any SCRATCH word as 0 (macOS zeroes all eight first), and the
 * wake word 1 in SCRATCH6. SCRATCH5 never receives anything but 0: macOS's config word
 * value there is unknown. */
static int ane_h15_scr_wr_kboot(struct ane_h15 *ane, unsigned int i, u32 v)
{
	u32 off = ANE_H15_SCRATCH_OFF + 4 * i;

	if (i >= ANE_H15_SCRATCH_N || (v && i != 6) || (u64)off + 4 > ane->soc->engine_size)
		return -EPERM;
	ane_h15_step("write SCRATCH%u=%#x pa=%#llx\n", i, v, ane->soc->engine_pa + off);
	dev_crit(ane->dev, "ane_h15 write SCRATCH%u=%#x pa=%#llx\n", i, v, ane->soc->engine_pa + off);
	writel(v, ane->engine + off);
	return 0;
}

/* P-1 of the working T6021 boot (ane_t6021_boot.h): twelve grant-tunable
 * words before the scratch handshake and CPU release. Values recorded on
 * T6021; the point of the 2026-11-10 run is whether T6030 needs them. */
static const struct { u32 off; u32 val; } ane_h15_grant_tunables_tab[] = {
	{ 0x000, 0x00000010 }, { 0x038, 0x00050020 },
	{ 0x03c, 0x000a0030 }, { 0x400, 0x40010001 },
	{ 0x600, 0x01ffffff }, { 0x738, 0x00200020 },
	{ 0x798, 0x00100030 }, { 0x7f8, 0x0100000a },
	{ 0x900, 0x00000101 }, { 0x410, 0x00001100 },
	{ 0x420, 0x00001100 }, { 0x430, 0x00001100 },
};

/* Post-start DART error dump (Main-sanctioned 2026-11-10): read the t8110
 * ERROR/ERROR_ADDR and ENABLE_STREAMS of all four windows while the module
 * still holds the DARTs live, then the faulting stream's TCR/TTBR if the
 * flag is set. Reads only; the raw words go to the log for the receipt.
 * /dev/mem reads after the probe showed a different view (PARAMS3
 * 0x2a2a0200 vs the module's in-window 0x125cc034), so the dump must run
 * inside this access window. */
static void ane_h15_dart_error_dump(struct ane_h15 *ane)
{
	unsigned int inst, sid;
	u32 err, alo, ahi, en0, en1;

	for (inst = 0; inst < 4; inst++) {
		if (ane_h15_dart_rd(ane, inst, ANE_H15_DART_ERROR, &err))
			continue;
		if (ane_h15_dart_rd(ane, inst, ANE_H15_DART_ERROR_ADDR_LO, &alo))
			continue;
		if (ane_h15_dart_rd(ane, inst, ANE_H15_DART_ERROR_ADDR_HI, &ahi))
			continue;
		if (ane_h15_dart_rd(ane, inst, ANE_H15_DART_ENABLE, &en0))
			continue;
		if (ane_h15_dart_rd(ane, inst, ANE_H15_DART_ENABLE + 4, &en1))
			continue;
		dev_crit(ane->dev,
			 "ane_h15 dart%u error=%#x addr=%#llx%08x en=%#08x:%#x\n",
			 inst, err, (unsigned long long)ahi, alo, en0, en1);
		if (!(err & 0x80000000u))
			continue;
		sid = (err & 0x0ff00000u) >> 20;
		if (sid < ANE_H15_DART_SIDS) {
			u32 tcr = 0, ttbr = 0;

			if (!ane_h15_dart_rd(ane, inst, ANE_H15_DART_TCR + 4 * sid, &tcr) &&
			    !ane_h15_dart_rd(ane, inst, ANE_H15_DART_TTBR + 4 * sid, &ttbr))
				dev_crit(ane->dev,
					 "ane_h15 dart%u fault sid=%u code=%#lx tcr=%#x ttbr=%#x\n",
					 inst, sid,
					 (unsigned long)(err & 0x7fffu), tcr, ttbr);
		}
	}
}

static int ane_h15_grant_tunables_wr(struct ane_h15 *ane)
{
	unsigned int ti;

	for (ti = 0; ti < ARRAY_SIZE(ane_h15_grant_tunables_tab); ti++) {
		u32 off = ane_h15_grant_tunables_tab[ti].off;
		u32 v = ane_h15_grant_tunables_tab[ti].val;

		if ((u64)off + 4 > ane->soc->engine_size)
			return -EPERM;
		ane_h15_step("write GRANT off=%#x=%#x pa=%#llx\n", off, v, ane->soc->engine_pa + off);
		dev_crit(ane->dev, "ane_h15 write GRANT %#x=%#x pa=%#llx\n", off, v, ane->soc->engine_pa + off);
		writel(v, ane->engine + off);
	}
	return 0;
}

static void ane_h15_hs_free(struct ane_h15 *ane)
{
	struct ane_h15_hs_buf *b[] = { &ane->owned_fw, &ane->hs_cmd, &ane->hs_heap, &ane->hs_ipc, &ane->hs_pool };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(b); i++)
		if (b[i]->va) {
			dma_free_coherent(ane->dev, b[i]->size, b[i]->va, b[i]->iova);
			b[i]->va = NULL;
		}
}

/* One zeroed surface from the DMA API, in the default domain the firmware segments are mapped in. It must not overlap a firmware segment. */
static int ane_h15_hs_alloc(struct ane_h15 *ane, struct ane_h15_hs_buf *b, size_t size, const char *name)
{
	unsigned int i;

	b->va = dma_alloc_coherent(ane->dev, size, &b->iova, GFP_KERNEL);
	if (!b->va) {
		dev_err(ane->dev, "ane_h15 handshake: allocation of %s (%#zx bytes) failed\n", name, size);
		return -ENOMEM;
	}
	b->size = size;
	dev_info(ane->dev, "ane_h15 hs surface %s size=%#zx iova=%#llx\n", name, size, (u64)b->iova);
	for (i = 0; i < 2; i++)
		if (b->iova < ane->map_iova[i] + ane->map_size[i] && b->iova + size > ane->map_iova[i]) {
			dev_err(ane->dev, "ane_h15 handshake: %s overlaps firmware segment %u\n", name, i);
			return -EBUSY;
		}
	return 0;
}

static void ane_h15_hs_put(struct ane_h15 *ane, u8 *blk, unsigned int off, unsigned int width, u64 v)
{
	dev_info(ane->dev, "ane_h15 hs block +%#x (%u bytes) = %#llx\n", off, width, v);
	memcpy(blk + off, &v, width);	/* little endian host */
}

static const struct ane_h15_hs_buf *ane_h15_hs_find(const struct ane_h15 *ane, u64 a)
{
	const struct ane_h15_hs_buf *b[3] = { &ane->hs_pool, &ane->hs_ipc, &ane->hs_heap };
	unsigned int i;

	for (i = 0; i < 3; i++)
		if (b[i]->va && a >= b[i]->iova && a < b[i]->iova + b[i]->size)
			return b[i];
	return NULL;
}

/* An entry of the published table: name[0x40], type u32 +0x40 (0 host to firmware, 1 firmware to host, 2 terminal), bit u32 +0x44, slots u64 +0x48,
 * ring u64 +0x50. It has to be sane: a name, type 0 to 2, bit below 32, 1 to CH_MAX_SLOTS slots, a ring inside the surface that holds the table,
 * aligned to a slot. */
static int ane_h15_chan_get(struct ane_h15 *ane, const struct ane_h15_hs_buf *tb, const u8 *tp, unsigned int i, struct ane_h15_ring *r, char *name)
{
	const u8 *e = tp + i * ANE_H15_CH_ENTRY;
	u32 type, bit;
	u64 size, ring;

	memcpy(name, e, ANE_H15_CH_NAME_LEN);
	name[ANE_H15_CH_NAME_LEN - 1] = 0;
	memcpy(&type, e + 0x40, 4);
	memcpy(&bit, e + 0x44, 4);
	memcpy(&size, e + 0x48, 8);
	memcpy(&ring, e + 0x50, 8);
	dev_info(ane->dev, "ane_h15 hs chan %u name=%s type=%u bit=%u slots=%#llx ring=%#llx\n", i, name, type, bit, size, ring);
	if (!name[0] || type > 2 || bit >= 32 || !size || size > ANE_H15_CH_MAX_SLOTS || ring < tb->iova || ring % ANE_H15_CH_SLOT ||
	    ring - tb->iova >= tb->size || size > (tb->size - (ring - tb->iova)) / ANE_H15_CH_SLOT)
		return -EPROTO;
	r->base = (u64 *)((u8 *)tb->va + (ring - tb->iova));
	r->iova = ring;
	r->slots = size;
	r->bit = bit;
	r->type = type;
	r->cursor = 0;
	return 0;
}

/* One command on the IO ring: the command block (id at +4, optional u32 argument at +8), slot header = its DMA address, doorbell BIT(bit), then wait for
 * the firmware to give the slot back (header | 1) with a zero acknowledgement at +6. */
static int ane_h15_cmd(struct ane_h15 *ane, struct ane_h15_ring *io, u16 op, u32 len, bool arg_set, u32 arg, u8 **blk)
{
	u64 *slot = io->base + io->cursor * 8, hdr, dma = ane->hs_cmd.iova;
	u8 *cmd = ane->hs_cmd.va;
	unsigned int waited;
	u16 id, ack;

	if (!chan_cmds || (u64)ANE_H15_DOORBELL_OFF + 4 > ane->soc->engine_size)
		return -EPERM;
	if (!(READ_ONCE(slot[0]) & 1)) {
		dev_err(ane->dev, "ane_h15 cmd %#x: IO slot %u is not host owned (%#llx)\n", op, io->cursor, READ_ONCE(slot[0]));
		return -EBUSY;
	}
	memset(cmd, 0, ANE_H15_CMD_BLOCK);
	memcpy(cmd + 4, &op, 2);
	if (arg_set)
		memcpy(cmd + 8, &arg, 4);
	WRITE_ONCE(slot[1], len);
	WRITE_ONCE(slot[2], len);
	dma_wmb();
	WRITE_ONCE(slot[0], dma);
	dma_wmb();
	dev_crit(ane->dev, "ane_h15 write doorbell BIT(%u) pa=%#llx for command %#x slot %u block iova %#llx\n", io->bit,
		 ane->soc->engine_pa + ANE_H15_DOORBELL_OFF, op, io->cursor, (u64)dma);
	ane_h15_step("write doorbell BIT(%u) pa=%#llx\n", io->bit, ane->soc->engine_pa + ANE_H15_DOORBELL_OFF);
	writel(BIT(io->bit), ane->engine + ANE_H15_DOORBELL_OFF);
	for (waited = 0; waited < hs_wait_ms; waited++) {
		ane_h15_ipi_poll(ane);
		hdr = READ_ONCE(slot[0]);
		if (hdr & 1) {
			dma_rmb();
			memcpy(&id, cmd + 4, 2);
			memcpy(&ack, cmd + 6, 2);
			dev_info(ane->dev, "ane_h15 cmd %#x answered after %ums: header=%#llx words=%#llx,%#llx id=%#x ack=%#x\n", op, waited, hdr,
				 READ_ONCE(slot[1]), READ_ONCE(slot[2]), id, ack);
			io->cursor = (io->cursor + 1) % io->slots;
			*blk = cmd;
			return (hdr == (dma | 1) && READ_ONCE(slot[1]) == len && READ_ONCE(slot[2]) == 0 && id == op && !ack) ? 0 : -EPROTO;
		}
		usleep_range(500, 1000);
	}
	dev_err(ane->dev, "ane_h15 cmd %#x: no answer within %ums (header %#llx)\n", op, hs_wait_ms, READ_ONCE(slot[0]));
	return -ETIMEDOUT;
}

/* macOS's step after it created its channels: the host owns every H2T slot, writes READY to SCRATCH3 and waits until the firmware clears it. With
 * chan_cmds then PRINT_ENABLE and CONFIG_GET. Nothing is written before the whole table checked out. Returns 0 only when every requested step worked. */
static int ane_h15_chan_step(struct ane_h15 *ane, const struct ane_h15_hs_buf *tb, u64 table)
{
	const u8 *tp = (u8 *)tb->va + (table - tb->iova);
	struct ane_h15_ring io = { 0 }, rings[ANE_H15_CH_COUNT];
	unsigned int i, j, waited;
	bool have_io = false;
	char name[ANE_H15_CH_NAME_LEN];
	u32 v = 0, s3 = ~0u;
	u8 *blk;
	int ret;

	if (tb->iova + tb->size - table < (u64)ANE_H15_CH_COUNT * ANE_H15_CH_ENTRY) {
		dev_err(ane->dev, "ane_h15 chan: the table does not fit in its surface\n");
		return -EPROTO;
	}
	for (i = 0; i < ANE_H15_CH_COUNT; i++) {
		ret = ane_h15_chan_get(ane, tb, tp, i, &rings[i], name);
		if (ret) {
			dev_err(ane->dev, "ane_h15 chan: entry %u is not a ring of the table surface, nothing written\n", i);
			return ret;
		}
		if (!strcmp(name, "IO") && rings[i].type == 0) {
			io = rings[i];
			have_io = true;
		}
	}
	/* the rings must not overlap the table entries or each other: the ownership loop below writes into every H2T ring */
	for (i = 0; i < ANE_H15_CH_COUNT; i++) {
		u64 lo = rings[i].iova, hi = lo + (u64)rings[i].slots * ANE_H15_CH_SLOT;

		if (lo < table + (u64)ANE_H15_CH_COUNT * ANE_H15_CH_ENTRY && hi > table) {
			dev_err(ane->dev, "ane_h15 chan: ring %u overlaps the table, nothing written\n", i);
			return -EPROTO;
		}
		for (j = 0; j < i; j++)
			if (lo < rings[j].iova + (u64)rings[j].slots * ANE_H15_CH_SLOT && hi > rings[j].iova) {
				dev_err(ane->dev, "ane_h15 chan: ring %u overlaps ring %u, nothing written\n", i, j);
				return -EPROTO;
			}
	}
	if (chan_cmds && !have_io) {
		dev_err(ane->dev, "ane_h15 chan: no IO ring of type 0 in the table, nothing written\n");
		return -EPROTO;
	}
	for (i = 0; i < ANE_H15_CH_COUNT; i++) {
		if (rings[i].type)
			continue;
		for (j = 0; j < rings[i].slots; j++) {
			u8 *e = (u8 *)rings[i].base + j * ANE_H15_CH_SLOT;

			memset(e, 0, 24);
			e[0] = 1;
		}
	}
	wmb();
	ret = ane_h15_scr_wr(ane, 3, ANE_H15_HS_READY);
	if (ret)
		return ret;
	for (waited = 0; waited < hs_wait_ms; waited++) {
		usleep_range(1000, 1100);
		s3 = ane_h15_scr_peek(ane, 3);
		ane_h15_ipi_poll(ane);
		if (!s3)
			break;
	}
	dev_info(ane->dev, "ane_h15 chan: SCRATCH3 reads %#x after %ums\n", s3, waited);
	if (s3)
		return -EIO;
	if (!chan_cmds)
		return 0;
	ret = ane_h15_hs_alloc(ane, &ane->hs_cmd, ANE_H15_CMD_BLOCK, "CMD");
	if (ret)
		return ret;
	ret = ane_h15_cmd(ane, &io, ANE_H15_CSNE_PRINT_ENABLE, 12, true, 0, &blk);
	if (ret)
		return ret == -EPROTO || ret == -ETIMEDOUT || ret == -EBUSY ? -EIO : ret;
	ret = ane_h15_cmd(ane, &io, ANE_H15_CSNE_CONFIG_GET, 16, false, 0, &blk);
	if (ret)
		return ret == -EPROTO || ret == -ETIMEDOUT || ret == -EBUSY ? -EIO : ret;
	memcpy(&v, blk + 8, 4);
	dev_info(ane->dev, "ane_h15 chan: CONFIG_GET reply u32 at +8 = %#x (macOS reads it as the tick rate)\n", v);
	return 0;
}

/* The shipped-system handshake after READY: surfaces IPC (max(page, SCRATCH1 + 1) bytes) and HEAP (SCRATCH3 bytes), the 0x174 byte init block taken from a pool
 * surface, then SCRATCH0 (low), SCRATCH1 (high), the wake word in SCRATCH7. Needs the core running with AP power ON acknowledged. Returns 0 only when the
 * firmware published a table address and the channel step worked. */
static int ane_h15_chan_run(struct ane_h15 *ane, struct ane_h15_rtk *k)
{
	u32 w[ANE_H15_SCRATCH_N], cur[3], last[3] = { 0xffffffffu, 0xffffffffu, 0xffffffffu };
	unsigned int i, waited;
	const struct ane_h15_hs_buf *tb;
	u64 ipc_size, heap_size, pub, table = 0;
	bool answered = false;
	u8 *blk;
	int ret;

	for (i = 0; i < ANE_H15_SCRATCH_N; i++) {
		ret = ane_h15_scr_rd(ane, i, &w[i]);
		if (ret)
			return ret;
		dev_info(ane->dev, "ane_h15 hs SCRATCH%u=%#x\n", i, w[i]);
	}
	if (w[7] != ANE_H15_HS_READY) {
		dev_err(ane->dev, "ane_h15 handshake: SCRATCH7=%#x, not the READY value %#x yet, the requests in SCRATCH1 and SCRATCH3 are not valid\n", w[7], ANE_H15_HS_READY);
		return -EAGAIN;
	}
	if (w[0] >= ANE_H15_HS_REQ_LIMIT) {
		dev_err(ane->dev, "ane_h15 handshake: SCRATCH0=%#x is not below %#x, not going on\n", w[0], ANE_H15_HS_REQ_LIMIT);
		return -EOPNOTSUPP;
	}
	ipc_size = ALIGN((u64)w[1] + 1 > PAGE_SIZE ? (u64)w[1] + 1 : PAGE_SIZE, PAGE_SIZE);
	heap_size = w[3] ? ALIGN(w[3] > PAGE_SIZE ? (u64)w[3] : PAGE_SIZE, PAGE_SIZE) : 0;
	if (ipc_size > ANE_H15_HS_IPC_MAX || heap_size > ANE_H15_HS_HEAP_MAX) {
		dev_err(ane->dev, "ane_h15 handshake: firmware asks for IPC %#llx and HEAP %#llx bytes, above the limits %#x and %#x\n", ipc_size, heap_size,
			ANE_H15_HS_IPC_MAX, ANE_H15_HS_HEAP_MAX);
		return -E2BIG;
	}
	ret = dma_set_mask_and_coherent(ane->dev, DMA_BIT_MASK(42));
	if (ret) {
		dev_err(ane->dev, "ane_h15 handshake: no 42 bit DMA mask: %d\n", ret);
		return ret;
	}
	ret = ane_h15_hs_alloc(ane, &ane->hs_pool, ANE_H15_HS_POOL_SIZE, "POOL");
	if (!ret)
		ret = ane_h15_hs_alloc(ane, &ane->hs_ipc, ipc_size, "IPC");
	if (!ret && heap_size)
		ret = ane_h15_hs_alloc(ane, &ane->hs_heap, heap_size, "HEAP");
	if (ret)
		return ret;			/* freed after the core stopped */
	blk = ane->hs_pool.va;
	/* The block layout of ane_t6021_boot.h, which macOS shares on the T8152 and so does this one. Everything not listed stays zero. */
	ane_h15_hs_put(ane, blk, 0x00, 8, ane->map_iova[0]);		/* firmware surface */
	ane_h15_hs_put(ane, blk, 0x08, 8, ane->hs_ipc.iova);
	ane_h15_hs_put(ane, blk, 0x10, 8, ANE_H15_HS_CFG_SIZE);
	ane_h15_hs_put(ane, blk, 0x18, 8, 0x10000000ULL - ANE_H15_HS_CFG_SIZE);
	ane_h15_hs_put(ane, blk, 0x20, 8, heap_size ? ane->hs_heap.iova : 0);
	ane_h15_hs_put(ane, blk, 0x28, 8, w[3]);
	ane_h15_hs_put(ane, blk, 0x30, 4, 0);				/* previous firmware length, first boot */
	ane_h15_hs_put(ane, blk, 0x50, 8, (u64)w[1] + 1);		/* ordinal */
	ane_h15_hs_put(ane, blk, 0x58, 8, ane->hs_pool.iova);
	ane_h15_hs_put(ane, blk, 0x60, 8, ANE_H15_HS_POOL_SIZE);	/* first qword of the pool object, as T6021 */
	ane_h15_hs_put(ane, blk, 0x68, 4, 0x40);
	ane_h15_hs_put(ane, blk, 0x6c + 0xc0, 4, 4);			/* template word +0xc0, the template is otherwise zero */
	pub = ane->hs_pool.iova;
	dev_info(ane->dev, "ane_h15 handshake: init block at iova %#llx (IPC %#llx bytes, HEAP %#llx bytes)\n", pub, ipc_size, heap_size);
	wmb();
	ret = ane_h15_scr_wr(ane, 0, (u32)pub);
	if (!ret)
		ret = ane_h15_scr_wr(ane, 1, (u32)(pub >> 32));
	if (!ret)
		ret = ane_h15_scr_wr(ane, 7, ANE_H15_HS_WAKE);
	if (ret)
		return ret;
	ane->ipi_acks = 0;
	ane->ipi_last = 0;
	for (waited = 0; waited < hs_wait_ms; waited++) {
		usleep_range(1000, 1100);
		cur[0] = ane_h15_scr_peek(ane, 0);
		cur[1] = ane_h15_scr_peek(ane, 1);
		cur[2] = ane_h15_scr_peek(ane, 7);
		if (cur[0] != last[0] || cur[1] != last[1] || cur[2] != last[2]) {
			dev_info(ane->dev, "ane_h15 handshake t+%ums SCRATCH0=%#x SCRATCH1=%#x SCRATCH7=%#x\n", waited + 1, cur[0], cur[1], cur[2]);
			memcpy(last, cur, sizeof(last));
		}
		ane_h15_rtk_poll_once(ane, k);			/* log whatever the firmware sends now */
		ane_h15_ipi_poll(ane);
		table = ((u64)cur[1] << 32) | cur[0];
		if (table != pub && cur[2] == ANE_H15_HS_READY) {
			answered = true;
			break;
		}
	}
	if (!answered) {
		dev_err(ane->dev, "ane_h15 handshake: no table address within %ums\n", hs_wait_ms);
		return -ETIMEDOUT;
	}
	tb = ane_h15_hs_find(ane, table);
	if (!tb) {
		dev_err(ane->dev, "ane_h15 handshake: the table address %#llx is outside the surfaces allocated here\n", table);
		return -EIO;
	}
	dev_info(ane->dev, "ane_h15 handshake: table address %#llx is in a surface of %#zx bytes at %#llx\n", table, tb->size, (u64)tb->iova);
	return ane_h15_chan_step(ane, tb, table);
}

/* Start phase until AP power ON is acknowledged and the application endpoints are started (plus 100 ms to log what
 * follows), then the shutdown phase. The caller clears RUN afterwards. */
static void ane_h15_rtk_session(struct ane_h15 *ane, struct ane_h15_rtk *k)
{
	unsigned int waited, settle = 0;

	for (waited = 0; waited < rtkit_wait_ms && settle < 100; waited += 10) {
		usleep_range(10000, 11000);
		if (ane_h15_rtk_poll_once(ane, k))
			return;
		if (k->app_started)
			settle += 10;
	}
	dev_info(ane->dev, "ane_h15 rtkit session: hello=%d epmap_last=%d ap_on=%d app_started=%d sent=%u after %ums\n",
		 k->hello, k->epmap_last, k->ap_on, k->app_started, k->sent, waited);
	if (k->app_started && ane->chan) {
		k->hs_ret = ane_h15_chan_run(ane, k);
		k->hs_done = true;
		dev_info(ane->dev, "ane_h15 handshake result %d\n", k->hs_ret);
	}
	if (!k->ap_on)
		return;			/* no session to shut down */
	k->shutdown = true;
	k->ap_state = rtkit_stop == 2 ? ANE_H15_RTK_PWR_OFF : ANE_H15_RTK_PWR_QUIESCED;
	k->iop_state = rtkit_stop == 2 ? ANE_H15_RTK_PWR_OFF : ANE_H15_RTK_PWR_SLEEP;
	k->sent = 0;			/* the shutdown has its own send budget */
	if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_AP_PWR << ANE_H15_RTK_TYPE_SHIFT) | k->ap_state))
		return;
	for (waited = 0; waited < ANE_H15_RTK_SHUTDOWN_MS && !k->iop_acked && !k->iop_idle; waited += 10) {
		usleep_range(10000, 11000);
		if (ane_h15_rtk_poll_once(ane, k))
			return;
	}
	dev_info(ane->dev, "ane_h15 rtkit shutdown: ap_acked=%d iop_acked=%d iop_idle=%d after %ums\n", k->ap_acked, k->iop_acked,
		 k->iop_idle, waited);
}

/* ---- stage 5, pwgate_t6031: the PWGATE gate clear ----
 *
 * On a T6031 (ane-type 0xe0) the shipped system writes 0 to the 32 bit register at offset 0x199c of the
 * PWGATE window, then polls it until bits 1:0 read 0, up to 0x1388 polls of 10 microseconds
 * each. The PWGATE window is the fifth reg entry of the ADT ane0 node in the macOS 27.0
 * device tree of the J516C: 0x292290000, 0x4000, next to the pmgr window at 0x292280000.
 * The decoded CPU-power step of the T6030 and T8122 has the same sequence at the word of their
 * row (+0x2b6c, +0x272c) in the third reg entry of their ANE node. The T6031 offset comes from the SoC data. The
 * hypothesis under test (inferred): a gate still closed there is why the stage 5 runs on these chips ended with
 * CPU_STATUS 0x30002a to 0x300028 and no mailbox message. A stage 2 read (pwgate_read) found the
 * T8122 word closed on a 13-inch M3 MacBook Air. No stage 5 run with this step has happened.
 *
 * pwgate_t6031=1 reads the word first and writes 0 only when bits 1:0
 * are set; pwgate_t6031=2 always writes 0 first (the unconditional
 * order). The poll runs at most 5000 times, 10 microseconds
 * apart; a timeout returns before the caller touches CPU_CONTROL. One
 * 32 bit write, to the row's PWGATE word only, and the window is mapped and
 * unmapped here. The caller owns the RESULT line.
 */
#define ANE_H15_PWGATE_BITS	3u
#define ANE_H15_PWGATE_POLLS	5000u

static int ane_h15_pwgate_open(struct ane_h15 *ane, char *suffix, size_t sz)
{
	const struct ane_h15_soc *s = ane->soc;
	const u64 word_pa = s->pwgate_pa + s->pwgate_word;
	void __iomem *pw;
	unsigned int i;
	u32 v;

	if ((u64)s->pwgate_word + 4 > s->pwgate_size) {
		dev_err(ane->dev, "ane_h15 pwgate: offset %#x+4 is outside the %#x byte window\n",
			s->pwgate_word, s->pwgate_size);
		return -EINVAL;
	}
	pw = ioremap(s->pwgate_pa, s->pwgate_size);
	if (!pw) {
		dev_err(ane->dev, "ane_h15 pwgate: ioremap of %#llx failed\n", s->pwgate_pa);
		return -ENOMEM;
	}
	ane_h15_step("read PWGATE pa=%#llx\n", word_pa);
	dev_crit(ane->dev, "ane_h15 read PWGATE pa=%#llx\n", word_pa);
	v = readl_relaxed(pw + s->pwgate_word);
	dev_info(ane->dev, "ane_h15 word=PWGATE pa=%#llx value=%#x\n", word_pa, v);
	if (!(v & ANE_H15_PWGATE_BITS) && pwgate_t6031 == 1) {
		dev_info(ane->dev, "ane_h15 pwgate already open\n");
		snprintf(suffix, sz, "pwgate=already-open");
		iounmap(pw);
		return 0;
	}
	ane_h15_step("write PWGATE=0x0 pa=%#llx\n", word_pa);
	dev_crit(ane->dev, "ane_h15 write PWGATE=0x0 pa=%#llx%s\n", word_pa,
		 pwgate_t6031 == 2 ? " (forced, the unconditional macOS order)" : "");
	writel(0, pw + s->pwgate_word);
	for (i = 0; i < ANE_H15_PWGATE_POLLS; i++) {
		usleep_range(10, 10);
		if (i == 0)
			ane_h15_step("poll PWGATE begin pa=%#llx\n", word_pa);
		dev_crit(ane->dev, "ane_h15 read PWGATE pa=%#llx\n", word_pa);
		v = readl_relaxed(pw + s->pwgate_word);
		if (!(v & ANE_H15_PWGATE_BITS)) {
			ane_h15_step("poll PWGATE end %u passes word=%#x\n", i + 1, v);
			dev_info(ane->dev, "ane_h15 pwgate open after %u poll(s), final word %#x\n",
				 i + 1, v);
			snprintf(suffix, sz, "pwgate=%s word=0x%x",
				 pwgate_t6031 == 2 ? "forced" : "opened", v);
			iounmap(pw);
			return 0;
		}
	}
	ane_h15_step("poll PWGATE end timeout %u passes word=%#x\n",
		     (u32)ANE_H15_PWGATE_POLLS, v);
	dev_err(ane->dev, "ane_h15 pwgate: bits 1:0 still set after %u polls, final word %#x; not starting the core\n",
		(u32)ANE_H15_PWGATE_POLLS, v);
	iounmap(pw);
	return -ETIMEDOUT;
}

/* ---- stage 2, pwgate_read=1: the PWGATE word, read only ----
 *
 * One 32 bit read of the word the shipped CPU-power step clears (offset from the soc row: T6030 0x2b6c,
 * T8122 0x272c, T6031 0x199c) and one log line with its value. Bits 1:0 nonzero mean the gate reads
 * closed. Nothing is written. The address is logged before the read, so a hang names it. The window is
 * the ANE node's pmgr_ps register entry; it is a power-state window, not a performance or DVFS window.
 * The caller owns the RESULT line.
 */
static int ane_h15_pwgate_read(struct ane_h15 *ane, char *suffix, size_t sz)
{
	const struct ane_h15_soc *s = ane->soc;
	const u64 word_pa = s->pwgate_pa + s->pwgate_word;
	void __iomem *pw;
	u32 v;

	if ((u64)s->pwgate_word + 4 > s->pwgate_size) {
		dev_err(ane->dev, "ane_h15 pwgate: offset %#x+4 is outside the %#x byte window\n",
			s->pwgate_word, s->pwgate_size);
		return -EINVAL;
	}
	pw = ioremap(s->pwgate_pa, s->pwgate_size);
	if (!pw) {
		dev_err(ane->dev, "ane_h15 pwgate: ioremap of %#llx failed\n", s->pwgate_pa);
		return -ENOMEM;
	}
	ane_h15_step("read PWGATE pa=%#llx\n", word_pa);
	dev_crit(ane->dev, "ane_h15 read PWGATE pa=%#llx\n", word_pa);
	v = readl_relaxed(pw + s->pwgate_word);
	dev_info(ane->dev, "ane_h15 word=PWGATE pa=%#llx value=%#x gate=%s\n", word_pa, v,
		 (v & ANE_H15_PWGATE_BITS) ? "closed" : "open");
	snprintf(suffix, sz, "pwgate=%s word=0x%x", (v & ANE_H15_PWGATE_BITS) ? "closed" : "open", v);
	iounmap(pw);
	return 0;
}

/* Read-only fetch evidence for the owned image (map_fw + fw_path): sha256 of TEXT, of the initialised DATA and of the zero tail (BSS), the
 * count of non-zero 64-bit words in the tail, and CPU_STATUS, logged before RUN and after the wait. A core that executed the image writes
 * DATA/BSS (stack, globals); a tail that is still all zero and a DATA digest equal to the one before RUN mean it never ran our code.
 * The buffer is dma_alloc_coherent, so the CPU reads what the core wrote. Nothing is written. */
static u64 ane_h15_be64(const u8 *p)
{
	u64 v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = v << 8 | p[i];
	return v;
}

static void ane_h15_owned_digest(struct ane_h15 *ane, const char *when, u8 **snap)
{
	const u8 *base = ane->owned_fw.va;
	const u64 *tail;
	u8 d_text[SHA256_DIGEST_SIZE], d_data[SHA256_DIGEST_SIZE], d_bss[SHA256_DIGEST_SIZE];
	size_t bss_off = ANE_H15_FW_TEXT_SIZE + ANE_H15_FW_DATA_FILE_SIZE, i, nz = 0;
	u32 st = 0;

	if (!ane->owned_map || !base || ane->owned_fw.size < ANE_H15_FW_IMAGE_SIZE)
		return;
	sha256(base, ANE_H15_FW_TEXT_SIZE, d_text);
	sha256(base + ANE_H15_FW_TEXT_SIZE, ANE_H15_FW_DATA_FILE_SIZE, d_data);
	sha256(base + bss_off, ANE_H15_FW_IMAGE_SIZE - bss_off, d_bss);
	tail = (const u64 *)(base + bss_off);
	for (i = 0; i < (ANE_H15_FW_IMAGE_SIZE - bss_off) / sizeof(u64); i++)
		nz += tail[i] != 0;
	if (ane_h15_read_word(ane, "CPU_STATUS", ane->soc->cpu_status, &st))
		st = 0xffffffffu;
	/* snap: before RUN keep a copy of DATA; after the wait log each changed 64-bit word (offset in DATA, old, new), at most 64, then free it. */
	if (snap && !*snap) {
		*snap = kvmalloc(ANE_H15_FW_DATA_FILE_SIZE, GFP_KERNEL);
		if (*snap)
			memcpy(*snap, base + ANE_H15_FW_TEXT_SIZE, ANE_H15_FW_DATA_FILE_SIZE);
	} else if (snap && *snap) {
		const u64 *o = (const u64 *)*snap, *n = (const u64 *)(base + ANE_H15_FW_TEXT_SIZE);
		size_t changed = 0, shown = 0;

		for (i = 0; i < ANE_H15_FW_DATA_FILE_SIZE / sizeof(u64); i++) {
			if (o[i] == n[i])
				continue;
			changed++;
			if (shown < 64) {
				dev_info(ane->dev, "ane_h15 data diff +%#zx old=%#llx new=%#llx\n", i * sizeof(u64), o[i], n[i]);
				shown++;
			}
		}
		dev_info(ane->dev, "ane_h15 data diff: %zu changed 64-bit words, %zu shown\n", changed, shown);
		kvfree(*snap);
		*snap = NULL;
	}
	dev_info(ane->dev, "ane_h15 owned image %s: sha256[0..15] text=%016llx%016llx data=%016llx%016llx bss=%016llx%016llx bss_nonzero_words=%zu cpu_status=%#x\n",
		 when, ane_h15_be64(d_text), ane_h15_be64(d_text + 8), ane_h15_be64(d_data), ane_h15_be64(d_data + 8),
		 ane_h15_be64(d_bss), ane_h15_be64(d_bss + 8), nz, st);
}

/* Stage 5, scratch_boot=1: macOS's cold-boot order, never run on an M3. Every write address is logged before the write, so a hang names the
 * last address. Order: 0 to SCRATCH0..7 (macOS's helper clears all eight first), SCRATCH6=1,
 * SCRATCH7=0 with a read back that must read 0 (anything else stops the run before CPU_CONTROL:
 * the register ignored the write and the stale word would be mistaken for the firmware's ready
 * signal in the poll), CPU_CONTROL 0 then 0x10 (RUN, plain writes, not the
 * read-modify-write), then poll SCRATCH7 every 1 ms for the ready word 0x08042006 that the shipped system
 * waits for (it logs "No wake signal" on timeout). Never RVBAR, never the mailbox, never the
 * DART; SCRATCH5 receives 0 only. The stale-ready-word hypothesis: a SCRATCH7 that still holds
 * 0x08042006 from the boot loader may send the firmware down the warm-boot path, where it idles at
 * CPU_STATUS 0x300028. Zeroing it first is what this option tests. With pwgate_t6031 on a
 * T6031 macOS's PWGATE gate clear runs after the SCRATCH sequence and before the first
 * CPU_CONTROL write (pwgate_t6031 above); a poll timeout returns before RUN and run_common
 * reports pwgate-not-open. */
static int ane_h15_scratch_boot(struct ane_h15 *ane, u32 ctrl, bool *s7_ready,
			     char *pwres, size_t pwres_sz)
{
	unsigned int i, waited;
	u32 v;
	int ret;

	for (i = 0; i < ANE_H15_SCRATCH_N; i++) {
		ret = ane_h15_scr_wr_kboot(ane, i, 0);
		if (ret)
			return ret;
	}
	ret = ane_h15_scr_wr_kboot(ane, 6, mailbox_mode ? 0 : 1);
	if (ret)
		return ret;
	ret = ane_h15_scr_wr_kboot(ane, 7, 0);
	if (ret)
		return ret;
	if (ane_h15_scr_rd(ane, 7, &v))
		return -EPERM;
	dev_info(ane->dev, "ane_h15 scratch_boot SCRATCH7 reads %#x after the cold-boot write\n", v);
	/* The clear must take: a non-zero readback means the register ignored the write and the
	 * stale word would be mistaken for the firmware's ready signal in the poll. Stop here,
	 * before the first CPU_CONTROL write; run_common reports clear-failed. */
	if (v) {
		dev_err(ane->dev, "ane_h15 scratch_boot SCRATCH7 still reads %#x after the clear, not writing CPU_CONTROL\n", v);
		return -EIO;
	}
	/* pwgate_t6031: macOS's PWGATE gate clear, after the SCRATCH
	 * sequence and before the first CPU_CONTROL write. A poll timeout
	 * stops the run here, before RUN; run_common reports
	 * pwgate-not-open and cleans up like the clear-failed path. */
	if (pwgate_t6031 && ane->soc->pwgate_pa) {
		ret = ane_h15_pwgate_open(ane, pwres, pwres_sz);
		if (ret)
			return ret;
	}
	/* Plain writes of 0 then RUN, not the read-modify-write. write_ctrl still checks that
	 * only bits 4 and 5 differ: a CPU_CONTROL with any other bit set refuses before the write. */
	ret = ane_h15_write_ctrl(ane, ctrl, 0, "scratch boot: CPU_CONTROL to 0");
	if (ret)
		return ret;
	ret = ane_h15_write_ctrl(ane, 0, ANE_H15_CPU_RUN, "scratch boot: RUN");
	if (ret)
		return ret;
	for (waited = 0; waited < sb_wait_ms; waited++) {
		usleep_range(1000, 1000);
		if (waited == 0)
			ane_h15_step("poll SCRATCH7 first pass pa=%#llx\n",
				     ane->soc->engine_pa + ANE_H15_SCRATCH_OFF + 4 * 7);
		if (ane_h15_scr_peek(ane, 7) == ANE_H15_HS_READY) {
			*s7_ready = true;
			ane_h15_step("poll SCRATCH7 end ready after %u passes\n", waited + 1);
			dev_info(ane->dev, "ane_h15 scratch_boot SCRATCH7 ready word %#x after %ums\n",
				 ANE_H15_HS_READY, waited + 1);
			return 0;
		}
	}
	ane_h15_step("poll SCRATCH7 end timeout %u passes\n", sb_wait_ms);
	dev_err(ane->dev, "ane_h15 scratch_boot SCRATCH7 ready word not seen within %ums\n", sb_wait_ms);
	return 0;
}

static int ane_h15_run_common(struct ane_h15 *ane, unsigned int stg)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 st = 0, ctrl = 0, rv_lo = 0, rv_hi = 0, a2i = 0, i2a = 0, last = 0xffffffffu;
	u64 m0, m1;
	unsigned int waited, i, msgs = 0, started_seen = 0, posted = 0, live = 0, locked = 0;
	bool stopped = false;
	bool kboot = (stg == 5 || stg == 3) && scratch_boot;
	bool s7_ready = false;
	char kres[128], pwres[64] = "";
	struct ane_h15_rtk rtk = { 0 };
	u8 *data_snap = NULL;
	int ret;

	if (!confirm_run) {
		ane_h15_result(ane, stg, "REFUSED", "confirm_run=1 required");
		return -EPERM;
	}
	if (run_wait_ms < 10 || run_wait_ms > 2000) {
		ane_h15_result(ane, stg, "REFUSED", "run_wait_ms 10 to 2000");
		return -EINVAL;
	}
	if (stg == 5 && scratch_boot && (sb_wait_ms < 10 || sb_wait_ms > 5000)) {
		ane_h15_result(ane, stg, "REFUSED", "sb_wait_ms 10 to 5000");
		return -EINVAL;
	}
	if (stg >= 6 && !confirm_rtkit) {
		ane_h15_result(ane, stg, "REFUSED", "confirm_rtkit=1 required");
		return -EPERM;
	}
	if (stg == 7 && !confirm_chan) {
		ane_h15_result(ane, stg, "REFUSED", "confirm_chan=1 required");
		return -EPERM;
	}
	if (stg == 7 && (!map_fw || hs_wait_ms < 10 || hs_wait_ms > 5000)) {
		ane_h15_result(ane, stg, "REFUSED", "stage 7 needs map_fw=1 and hs_wait_ms 10 to 5000");
		return -EINVAL;
	}
	if (stg >= 6 && (rtkit_wait_ms < 100 || rtkit_wait_ms > 10000 || rtkit_stop < 1 || rtkit_stop > 2)) {
		ane_h15_result(ane, stg, "REFUSED", "rtkit_wait_ms 100 to 10000, rtkit_stop 1 or 2");
		return -EINVAL;
	}
	if (!s->rvbar_expect) {
		ane_h15_result(ane, stg, "REFUSED", "no measured RVBAR for this SoC row: run stage 2 and report it");
		return -ENODEV;
	}
	ane->chan = stg == 7;
	ane->owned_map = map_fw && fw_path;
	if (map_fw) {
		if (!device_iommu_mapped(ane->dev)) {
			dev_err(ane->dev, "ane_h15 map_fw: the node has no iommus (no DART client): use the DART overlay\n");
			ane_h15_result(ane, stg, "REFUSED", "map_fw needs iommus in the node");
			return -ENODEV;
		}
		if (ane->owned_map) {
			if (!confirm_read) {
				ane_h15_result(ane, stg, "REFUSED", "owned firmware needs confirm_read=1");
				return -EPERM;
			}
			if (fw_seg) {
				ane_h15_result(ane, stg, "REFUSED", "fw_path and fw_seg name two firmware sources");
				return -EINVAL;
			}
		} else {
			ret = ane_h15_parse_segs(ane);
			if (ret) {
				ane_h15_result(ane, stg, "REFUSED", "fw_seg missing or unsafe");
				return ret;
			}
		}
	}
	if (!ane_h15_window_ok(ane, 0, "engine", s->engine_pa, s->engine_size) ||
	    !ane_h15_window_ok(ane, 1, "pmgr", s->pmgr_pa, s->pmgr_size)) {
		ane_h15_result(ane, stg, "FAIL", "window-mismatch");
		return -EINVAL;
	}
	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, stg, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	if (map_fw) {
		/* Attaching replaces the DART state a running core is fetching through: look first, and only on a stopped core. */
		ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
		if (ret) {
			ane_h15_result(ane, stg, "FAIL", "read-refused");
			return ret;
		}
		if (!ane_h15_core_stopped(st)) {
			dev_err(ane->dev, "ane_h15 map_fw: cpu_status=%#x, the core is not stopped: not touching the DART\n", st);
			ane_h15_result(ane, stg, "FAIL", "core-not-stopped-before-map");
			return -EBUSY;
		}
	}
	/* Preflight, read only. A map_fw run is about to attach the DART, so it checks that RUN is clear first. Any other run reads CPU_CONTROL
	 * only after the DART gate below: with no live stream it refuses without reading it. */
	ctrl = 0;
	ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
	if (!ret && map_fw)
		ret = ane_h15_read_ctrl(ane, &ctrl);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_LO", s->rvbar, &rv_lo);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_HI", s->rvbar + 4, &rv_hi);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_A2I_CTRL", s->mbox + 0x110, &a2i);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	if (ret) {
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "read-refused");
		return ret;
	}
	if (!ane_h15_core_stopped(st) || (ctrl & ANE_H15_CPU_RUN) ||
	    (((u64)rv_hi << 32) | rv_lo) != s->rvbar_expect ||
	    !(a2i & ANE_H15_MBOX_EMPTY) || !(i2a & ANE_H15_MBOX_EMPTY)) {
		dev_err(ane->dev,
			"ane_h15 preflight: cpu_status=%#x cpu_control=%#x%s rvbar=%#llx a2i_ctrl=%#x i2a_ctrl=%#x (want stopped, RUN clear, rvbar %#llx, both mailbox words EMPTY)\n",
			st, ctrl, map_fw ? "" : " (not read yet)", ((u64)rv_hi << 32) | rv_lo, a2i, i2a, s->rvbar_expect);
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "preflight-not-as-iboot-left-it");
		return -EBUSY;
	}
	if (ane->owned_map) {
		ret = ane_h15_plan_owned_map(ane);
		if (ret) {
			ane_h15_result(ane, stg, "FAIL", "owned-firmware-prepare-failed");
			return ret;
		}
	}
	if (map_fw) {
		ret = ane_h15_map_firmware(ane);
		if (ret) {
			ane_h15_result(ane, stg, "FAIL", "fw-map-failed");
			return ret;
		}
	}
	/* ps_assert: the power-state re-assert goes here, between the firmware map
	 * and the sweep, so the domain hold spans the sweep reads (the four B-class
	 * deaths landed 19 to 23 ms after the first ps query, mid-sweep). */
	if (stg == 5 && ps_assert && s->pwgate_pa) {
		int pret = ane_h15_ps_assert(ane);
		if (pret) {
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL", "ps-assert-failed");
			return pret;
		}
	}
	/* The M6 lesson: RUN alone was silent and iBoot left no stream enabled. Without one the core cannot fetch. */
	if (rpm_hold_sweep) {
		ane_h15_step("rpm hold: get before the DART sweep\n");
		ret = pm_runtime_get_sync(ane->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(ane->dev);
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL", "rpm-hold-failed");
			return ret;
		}
	}
	ret = ane_h15_dart_scan(ane, &live, &locked);
	if (rpm_hold_sweep)
		pm_runtime_put_sync(ane->dev);
	if (ret) {
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "dart-read-refused-or-not-a-dart");
		return ret;
	}
	dev_info(ane->dev, "ane_h15 dart summary live_streams=%u locked_windows=%u\n", live, locked);
	if (!live && !run_unmapped) {
		dev_err(ane->dev, "ane_h15 no ANE DART stream is enabled and mapped: the core could not fetch its firmware; nothing written (run_unmapped=1 overrides)\n");
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "no-dart-stream-enabled");
		return -ENODEV;
	}
	if (!map_fw) {
		ret = ane_h15_read_ctrl(ane, &ctrl);
		if (ret) {
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL", "read-refused");
			return ret;
		}
		if (ctrl & ANE_H15_CPU_RUN) {
			dev_err(ane->dev, "ane_h15 preflight: cpu_control=%#x (want RUN clear)\n", ctrl);
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL", "preflight-not-as-iboot-left-it");
			return -EBUSY;
		}
	}

	if ((stg == 5 || stg == 3) && (run_scratch || kboot))
		ane_h15_scr_dump(ane, "before RUN");
	if (stg == 5 && run_pmgr)
		ane_h15_pmgr_dump(ane, "before RUN");

	/* pwgate_t6031 (rows with a PWGATE window): macOS's PWGATE gate clear, the
	 * last action before the core is released. On a poll timeout the run
	 * stops here: nothing written to the core, nothing pinned, claims
	 * released (the probe failure path unmaps and detaches). */
	if ((stg == 5 || stg == 3) && !kboot && pwgate_t6031 && s->pwgate_pa) {
		ret = ane_h15_pwgate_open(ane, pwres, sizeof(pwres));
		if (ret) {
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL",
				       ret == -ETIMEDOUT ? "pwgate-not-open" : "pwgate-window-refused");
			return ret;
		}
		/* stop_after_pwgate: the split run. The gate step is the whole run:
		 * no SCRATCH write, no CPU_CONTROL write, the core stays as it was,
		 * nothing pinned. A crash in this run names the gate step alone. */
		if (stop_after_pwgate) {
			strscpy(pwres + strlen(pwres), " stop-after-pwgate",
				sizeof(pwres) - strlen(pwres));
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "PASS", pwres);
			dev_info(ane->dev, "ane_h15 stop_after_pwgate: no CPU_CONTROL write\n");
			return 0;
		}
	}

	/* pwr_req (the M2 order, Main-sanctioned): set the ASC CPU power-up
	 * request (EDPRCR bit0) and poll EDPRSR bounded before RUN. On a
	 * poll timeout the run stops here: no RUN write, nothing started,
	 * nothing pinned, claims released via the unmap below. */
	if (pwr_req) {
		u32 pr = 0, psr = 0, psr_first = 0;
		unsigned int pol, polls = pwr_poll_ms * 10;

		if (pwr_poll_ms < 10 || pwr_poll_ms > 10000) {
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "REFUSED", "pwr_poll_ms 10 to 10000");
			return -EINVAL;
		}
		ret = ane_h15_read_word(ane, "EDPRCR", 0x1010310, &pr);
		if (!ret)
			ret = ane_h15_read_word(ane, "EDPRSR", 0x1010314, &psr_first);
		if (!ret)
			ret = ane_h15_write_edprcr(ane, pr, pr | 1u);
		if (ret) {
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL", "power-request-refused");
			return ret;
		}
		/* the poll reads through the quiet reader: read_word logs
		 * every access and 10000 pairs overflow the kmsg ring,
		 * burying the evidence lines ahead of them */
		for (pol = 0; pol < polls; pol++) {
			psr = readl_relaxed(ane->engine + 0x1010314);
			if (psr & 1u)
				break;
			udelay(100);
		}
		dev_crit(ane->dev, "ane_h15 power-request: edprcr_read=0x%x edpsr first=0x%x last=0x%x polls=%u powered=%d\n",
			 pr, psr_first, psr, pol, (int)(psr & 1u));
		if (!(psr & 1u)) {
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL", "power-poll-timeout");
			return -ETIMEDOUT;
		}
	}

	/* grant_tunables: the T6021 P-1 words, strictly before the scratch
	 * handshake and the first CPU_CONTROL write. On any refusal the run
	 * stops here: no SCRATCH write, no RUN, nothing started. */
	ane_h15_dart_error_dump(ane);	/* pre-RUN baseline: sticky flags from before this boot's release */
	if (grant_tunables && (stg == 5 || stg == 3)) {
		ret = ane_h15_grant_tunables_wr(ane);
		if (ret) {
			ane_h15_unmap_firmware(ane);
			ane_h15_result(ane, stg, "FAIL", "grant-tunables-refused");
			return ret;
		}
	}

	ane_h15_owned_digest(ane, "before RUN", &data_snap);
	/* Start the core. From here a reboot may be needed. */
	ane->fw_started = true;
	ane->fw_touched = true;
	if (kboot)
		ret = ane_h15_scratch_boot(ane, ctrl, &s7_ready, pwres, sizeof(pwres));
	else
		ret = ane_h15_write_ctrl(ane, ctrl, ctrl | ANE_H15_CPU_RUN, "RUN");
	if (ret) {
		kvfree(data_snap);
		ane->fw_started = false;
		ane->fw_touched = false;
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", kboot ? (ret == -EIO ? "scratch_boot=1 s7_ready=no clear-failed" :
							   ret == -ETIMEDOUT ? "pwgate-not-open" :
							   "scratch-boot-write-refused") : "write-refused");
		return ret;
	}
	if (stg == 5 && run_pmgr)
		ane_h15_pmgr_dump(ane, "after RUN");
	if (stg >= 6 || mailbox_mode) {
		ane_h15_dart_error_dump(ane);
		ane_h15_rtk_session(ane, &rtk);
		ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
		if (!ret && !(st & ANE_H15_ST_STOPPED))
			started_seen = 1;
	}
	for (waited = 0; stg == 5 && waited < run_wait_ms; waited += 10) {
		usleep_range(10000, 11000);
		ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
		if (!ret)
			ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
		if (ret)
			break;
		if (!(st & ANE_H15_ST_STOPPED))
			started_seen = 1;
		if (st != last) {
			dev_info(ane->dev, "ane_h15 t+%ums cpu_status=%#x i2a_ctrl=%#x\n", waited + 10, st, i2a);
			last = st;
		}
		if (!(i2a & ANE_H15_MBOX_EMPTY) && msgs < ANE_H15_RUN_MAX_MSGS) {
			posted = 1;
			m0 = m1 = 0;
			ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV0", s->mbox + ANE_H15_MBOX_I2A_RECV0, &m0);
			if (!ret)
				ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV1", s->mbox + ANE_H15_MBOX_I2A_RECV1, &m1);
			if (ret)
				break;
			dev_info(ane->dev, "ane_h15 msg%u raw0=%#llx raw1=%#llx\n", msgs, m0, m1);
			msgs++;
		}
	}
	if (stg == 5)
		ane_h15_post_wait_status(ane);
	if ((stg == 5 || stg == 3) && (run_scratch || kboot))
		ane_h15_scr_dump(ane, "after the wait");
	if (stg == 5 && run_pmgr)
		ane_h15_pmgr_dump(ane, "after the wait");
	ane_h15_owned_digest(ane, "after the wait", &data_snap);
	kvfree(data_snap);
	data_snap = NULL;
	/* Stop the core the macOS way: clear RUN, then bit 5, each from a fresh read. */
	if (ane_h15_read_ctrl(ane, &ctrl) == 0 &&
	    ane_h15_write_ctrl(ane, ctrl, ctrl & ~ANE_H15_CPU_RUN, "clear RUN") == 0) {
		if (ane_h15_read_ctrl(ane, &ctrl) == 0 && (ctrl & ANE_H15_CPU_BIT5))
			ane_h15_write_ctrl(ane, ctrl, ctrl & ~ANE_H15_CPU_BIT5, "clear bit 5");
	}
	for (i = 0; i < 20; i++) {
		usleep_range(5000, 6000);
		if (ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st))
			break;
		if (ane_h15_core_stopped(st)) {
			stopped = true;
			break;
		}
	}
	dev_info(ane->dev, "ane_h15 run started_seen=%u i2a_posted=%u messages=%u final cpu_status=%#x i2a_ctrl=%#x stopped=%d\n",
		 started_seen, posted, msgs, st, i2a, stopped);
	if (!stopped) {
		dev_emerg(ane->dev,
			  "ane_h15 the core did not report STOPPED after RUN was cleared: power domains stay on, do NOT unload, reboot required\n");
		if (kboot) {
			snprintf(kres, sizeof(kres), "scratch_boot=1 s7_ready=%s core-not-stopped",
				 s7_ready ? "yes" : "no");
			if (mailbox_mode)
				snprintf(kres + strlen(kres), sizeof(kres) - strlen(kres),
					 " rtkit-hello=%u", rtk.hello);
			ane_h15_result(ane, stg, "FAIL", kres);
		} else {
			ane_h15_result(ane, stg, "FAIL", "core-not-stopped");
		}
		/* Pin the module: ane_h15_remove() is void and cannot refuse an rmmod, and unloading would free the code that
		 * owns the power claim, the windows and (map_fw) the DART domain under a core that may still be running. The
		 * reference is never dropped: reboot to park the ANE. */
		__module_get(THIS_MODULE);
		/* 0, not an error: a failed probe would release the domains and the windows under a running core */
		return 0;
	}
	ane->fw_started = false;
	ane->fw_touched = false;
	ane_h15_unmap_firmware(ane);	/* first: with the owned image this drops the DART map of the same pages hs_free returns to the DMA API */
	if (ane->map_retained) {
		ane_h15_result(ane, stg, "FAIL", "firmware-unmap-incomplete");
		return -EIO;
	}
	ane_h15_hs_free(ane);		/* only after the core stopped: a running core may still write into these surfaces */
	if (stg >= 6) {
		if (!rtk.hello) {
			ane_h15_result(ane, stg, "FAIL", "rtkit-no-hello");
			return -ETIMEDOUT;
		}
		if (!(rtk.epmap_last && rtk.ap_on && rtk.app_started)) {
			ane_h15_result(ane, stg, "FAIL", "rtkit-session-incomplete");
			return -EIO;
		}
		if (!(rtk.ap_acked && rtk.iop_acked)) {
			ane_h15_result(ane, stg, "FAIL", "rtkit-shutdown-incomplete");
			return -EIO;
		}
		if (stg == 7 && rtk.hs_ret) {
			ane_h15_result(ane, stg, "FAIL", rtk.hs_ret == -ETIMEDOUT ? "handshake-no-table-address" : rtk.hs_ret == -EAGAIN ? "handshake-not-ready" : "handshake-not-done");
			return rtk.hs_ret;
		}
		ane_h15_result(ane, stg, "PASS", stg == 7 ? "rtkit-channels-and-shutdown" : "rtkit-session-and-shutdown");
		return 0;
	}
	if (kboot) {
		snprintf(kres, sizeof(kres), "scratch_boot=1 s7_ready=%s %s", s7_ready ? "yes" : "no",
			 started_seen ? "core-started-and-stopped" : "core-never-left-stopped");
		if (mailbox_mode)
			snprintf(kres + strlen(kres), sizeof(kres) - strlen(kres),
				 " rtkit-hello=%u app=%u", rtk.hello, rtk.app_started);
		if (pwres[0])
			snprintf(kres + strlen(kres), sizeof(kres) - strlen(kres), " %s", pwres);
		ane_h15_result(ane, stg, "PASS", kres);
		return 0;
	}
	if (pwres[0]) {
		snprintf(kres, sizeof(kres), "%s %s",
			 started_seen ? "core-started-and-stopped" : "core-never-left-stopped", pwres);
		ane_h15_result(ane, stg, "PASS", kres);
		return 0;
	}
	ane_h15_result(ane, stg, "PASS", started_seen ? "core-started-and-stopped" : "core-never-left-stopped");
	return 0;
}

static int ane_h15_stage_run(struct ane_h15 *ane)
{
	return ane_h15_run_common(ane, 5);
}

static int ane_h15_stage_rtkit(struct ane_h15 *ane)
{
	return ane_h15_run_common(ane, 6);
}

static int ane_h15_stage_chan(struct ane_h15 *ane)
{
	return ane_h15_run_common(ane, 7);
}

/* ---- driver ---- */
static void ane_h15_unmap_windows(struct ane_h15 *ane)
{
	if (ane->pmgr)
		iounmap(ane->pmgr);
	if (ane->engine)
		iounmap(ane->engine);
}

static int ane_h15_probe(struct platform_device *pdev)
{
	struct ane_h15 *ane;
	const struct ane_h15_soc *soc = of_device_get_match_data(&pdev->dev);
	u32 ane_type;
	unsigned int stage_idx;
	int ret;

	/* refused probes leave no device data behind: remove() runs only via the
	 * driver core on real hardware, but the sim harness calls it directly */
	platform_set_drvdata(pdev, NULL);

	if (!optin || strcmp(optin, soc->name)) {
		dev_err(&pdev->dev,
			"not probing: optin key must be \"%s\" (optin=%s). This module is EXPERIMENTAL; no silicon has run it.\n",
			soc->name, optin ? optin : "(unset)");
		return -EPERM;
	}
	if (of_property_read_u32(pdev->dev.of_node, "apple,ane-type", &ane_type) ||
	    ane_type != soc->ane_type) {
		dev_err(&pdev->dev,
			"device apple,ane-type=%u (want %u); refusing\n",
			ane_type, soc->ane_type);
		return -EINVAL;
	}
	ret = ane_h15_stage_idx(stage, &stage_idx);
	if (ret) {
		dev_err(&pdev->dev, "unknown stage \"%s\"\n", stage);
		return ret;
	}
	if (mailbox_mode && (stage_idx != 5 && stage_idx != 3)) {
		dev_err(&pdev->dev, "mailbox_mode=1 is stage=run (5) or boot (3) only: refusing\n");
		return -EINVAL;
	}
	if (mailbox_mode && !scratch_boot) {
		dev_err(&pdev->dev, "mailbox_mode=1 needs scratch_boot=1 (it changes the handshake transport): refusing\n");
		return -EINVAL;
	}
	if (scratch_boot && stage_idx != 5 && stage_idx != 3) {
		dev_err(&pdev->dev, "scratch_boot=1 is stage=run (5) or boot (3) only: refusing\n");
		return -EINVAL;
	}
	if (grant_tunables && stage_idx != 5 && stage_idx != 3) {
		dev_err(&pdev->dev, "grant_tunables=1 is stage=run (5) or boot (3) only: refusing\n");
		return -EINVAL;
	}
	/* pwgate_t6031 refusals, all before any MMIO and before any mapping,
	 * exactly as scratch_boot is refused on other stages. */
	if (pwgate_t6031 > 2) {
		dev_err(&pdev->dev, "REFUSED reason=pwgate_t6031 0 to 2 (got %u)\n", pwgate_t6031);
		return -EINVAL;
	}
	if (pwgate_t6031 && stage_idx != 5 && stage_idx != 3) {
		dev_err(&pdev->dev, "REFUSED reason=pwgate_t6031: stage=run (5) or stage=boot (3) only\n");
		return -EINVAL;
	}
	if (ps_assert && (!pwgate_t6031 || stage_idx != 5)) {
		dev_err(&pdev->dev,
			"REFUSED reason=ps_assert: needs pwgate_t6031=1 and stage=run (5)\n");
		return -EINVAL;
	}
	if (pwgate_t6031 && !soc->pwgate_pa) {
		dev_err(&pdev->dev,
			"REFUSED reason=pwgate_t6031: the %s row has no PWGATE window\n",
			soc->name);
		return -EINVAL;
	}
	/* dart_scan_mask refusals, before any MMIO: 0 scans nothing, the mask has 4 bits. */
	if (!dart_scan_mask || dart_scan_mask > 0xf) {
		dev_err(&pdev->dev, "REFUSED reason=dart_scan_mask: a 4 bit mask with at least one instance, got %#x\n",
			dart_scan_mask);
		return -EINVAL;
	}
	/* the steplog opens at probe entry: the init line lands before bring-up,
	 * so even a death in the power-domain attach leaves the file behind */
	ret = ane_h15_steplog_open(&pdev->dev, stage_idx);
	if (ret) {
		ane_h15_result(NULL, stage_idx, "REFUSED", "steplog-open-failed");
		return ret;
	}
	/* stop_after_pwgate refusals: the split needs the PWGATE step alone, without
	 * the SCRATCH sequence of scratch_boot. */
	if (stop_after_pwgate && (!pwgate_t6031 || scratch_boot)) {
		ane_h15_steplog_close();
		dev_err(&pdev->dev,
			"REFUSED reason=stop_after_pwgate: needs pwgate_t6031=1 without scratch_boot\n");
		return -EINVAL;
	}
	/* pwgate_read refusals: same rule, before any MMIO and any mapping. */
	if (pwgate_read > 1) {
		ane_h15_steplog_close();
		dev_err(&pdev->dev, "REFUSED reason=pwgate_read 0 or 1 (got %u)\n", pwgate_read);
		return -EINVAL;
	}
	if (pwgate_read && (stage_idx != 2 || fw_seg || !strcmp(stage, "domain-window"))) {
		ane_h15_steplog_close();
		dev_err(&pdev->dev,
			"REFUSED reason=pwgate_read: stage=wrapper (2) only, without fw_seg\n");
		return -EINVAL;
	}
	if (pwgate_read && !soc->pwgate_pa) {
		ane_h15_steplog_close();
		dev_err(&pdev->dev, "REFUSED reason=pwgate_read: the %s row has no PWGATE window\n",
			soc->name);
		return -EINVAL;
	}

	ane = devm_kzalloc(&pdev->dev, sizeof(*ane), GFP_KERNEL);
	if (!ane) {
		ane_h15_steplog_close();
		return -ENOMEM;
	}
	ane->dev = &pdev->dev;
	ane->soc = soc;
	platform_set_drvdata(pdev, ane);

	if (stage_idx == 0) {
		ret = ane_h15_stage_dt(ane);
		ane_h15_steplog_close();
		return ret;
	}

	/* Stages 1, 2, 3 map engine and pmgr via of_iomap (non-posted
	 * when the parent bus advertises nonposted-mmio). */
	ane->engine = of_iomap(pdev->dev.of_node, 0);
	ane->pmgr   = of_iomap(pdev->dev.of_node, 1);
	/* of_iomap returns NULL (not ERR_PTR) when the reg property is
	 * missing or bad; a NULL + offset read would oops. Refuse. */
	if (!ane->engine || !ane->pmgr) {
		dev_err(&pdev->dev,
			"of_iomap failed (engine=%s, pmgr=%s): node reg property missing?\n",
			ane->engine ? "ok" : "NULL", ane->pmgr ? "ok" : "NULL");
		ane_h15_steplog_close();
		ane_h15_unmap_windows(ane);
		return -ENXIO;
	}

	ret = ane_h15_attach_pds(ane);
	if (ret) {
		ane_h15_steplog_close();
		ane_h15_detach_pds(ane);
		ane_h15_unmap_windows(ane);
		return ret;
	}

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		/*
		 * With several power-domains attached by id, the RPM_ACTIVE
		 * device links already resumed every domain. On T6030 the main
		 * device's get_sync then returned -EACCES (observed at stage 1;
		 * the cause is not established). Only that code, and only with
		 * domains attached, is treated as powered;
		 * the ACTUAL=0xf gate in the stage is the real check. Any other
		 * error fails the probe.
		 */
		if (ane->pd_count > 0 && ret == -EACCES) {
			dev_info(&pdev->dev,
				 "engine powered via %d domain links (main get_sync %d ignored)\n",
				 ane->pd_count, ret);
		} else {
			dev_err(&pdev->dev, "power-domains bring-up failed: %d\n", ret);
			/* a failed get_sync still took a usage count */
			ane_h15_steplog_close();
			pm_runtime_put_noidle(&pdev->dev);
			pm_runtime_disable(&pdev->dev);
			ane_h15_detach_pds(ane);
			ane_h15_unmap_windows(ane);
			return ret;
		}
	}

	ane_h15_step("bring-up complete\n");
	if (stage_idx == 1)       ret = ane_h15_stage_status(ane);
	else if (stage_idx == 2)  ret = ane_h15_stage_wrapper(ane);
	else if (stage_idx == 4)  ret = ane_h15_stage_dart(ane);
	else if (stage_idx == 5)  ret = ane_h15_stage_run(ane);
	else if (stage_idx == 6)  ret = ane_h15_stage_rtkit(ane);
	else if (stage_idx == 7)  ret = ane_h15_stage_chan(ane);
	else                      ret = ane_h15_stage_boot(ane);
	ane_h15_steplog_close();

	if (ane->map_retained) {
		__module_get(THIS_MODULE);
		dev_emerg(&pdev->dev, "firmware mappings remain: do NOT unload; reboot required\n");
		return 0;
	}
	/* A started core that did not stop keeps its power claim (and its windows, see remove()). */
	if (!ane->fw_started && hold_vote && !ret) {
		ane->vote_held = true;
		dev_info(&pdev->dev, "hold_vote: runtime reference kept; rmmod releases it\n");
	} else if (!ane->fw_started) {
		pm_runtime_put_sync(&pdev->dev);
	}
	if (ane->fw_started)
		dev_emerg(&pdev->dev,
			  "firmware started: do NOT unload; reboot to park the ANE\n");
	/* a failed probe never reaches remove(); release everything */
	if (ret) {
		ane_h15_hs_free(ane);
		pm_runtime_disable(&pdev->dev);
		ane_h15_detach_pds(ane);
		ane_h15_unmap_windows(ane);
	}
	return ret;
}

static void ane_h15_remove(struct platform_device *pdev)
{
	struct ane_h15 *ane = platform_get_drvdata(pdev);

	if (ane->fw_touched || ane->map_retained) {
		dev_emerg(&pdev->dev,
			  "remove with the firmware released: reboot required\n");
		return;
	}
	if (ane->vote_held) {
		ane->vote_held = false;
		dev_info(&pdev->dev, "hold_vote: releasing the held runtime reference\n");
		pm_runtime_put_sync(&pdev->dev);
	}
	ane_h15_hs_free(ane);
	pm_runtime_disable(&pdev->dev);
	ane_h15_detach_pds(ane);
	ane_h15_unmap_windows(ane);
}

static const struct of_device_id ane_h15_of_match[] = {
	{ .compatible = "apple,t8122-ane", .data = &ane_t8122_soc },
	{ .compatible = "apple,t6030-ane", .data = &ane_t6030_soc },
	{ .compatible = "apple,t6031-ane", .data = &ane_t6031_soc },
	{ .compatible = "apple,t6034-ane", .data = &ane_t6034_soc },
	{ }
};

/* The PMP vote (the report entry in the power-domains list) is not a runtime-PM
 * link supplier: the core suspends link suppliers in attach order, after the
 * consumer's own callback, so a vote attached last was cleared after ane_sys
 * and the other pmgr domains were already off, and the PMP crashed. The vote
 * is taken in runtime_resume (suppliers already on) and dropped in
 * runtime_suspend (suppliers still on), on every path that suspends the device. */
static int ane_h15_runtime_resume(struct device *dev)
{
	struct ane_h15 *ane = dev_get_drvdata(dev);

	return ane && ane->vote_dev ? pm_runtime_resume_and_get(ane->vote_dev) : 0;
}

static int ane_h15_runtime_suspend(struct device *dev)
{
	struct ane_h15 *ane = dev_get_drvdata(dev);
	int ret;

	if (!ane || !ane->vote_dev)
		return 0;
	ret = pm_runtime_put_sync(ane->vote_dev);
	return ret < 0 ? ret : 0;
}

static const struct dev_pm_ops ane_h15_pm_ops = {
	SET_RUNTIME_PM_OPS(ane_h15_runtime_suspend, ane_h15_runtime_resume, NULL)
};

static struct platform_driver ane_h15_driver = {
	.driver = {
		.name = "ane_h15",
		.suppress_bind_attrs = true,
		.of_match_table = ane_h15_of_match,
		.pm = &ane_h15_pm_ops,
	},
	.probe = ane_h15_probe,
	.remove = ane_h15_remove,
};

static int __init ane_h15_init(void)
{
	return ane_h15_register(&ane_h15_driver, optin, apply_overlay, boot_overlay);
}

static void __exit ane_h15_exit(void)
{
	ane_h15_unregister(&ane_h15_driver);
}

module_init(ane_h15_init);
module_exit(ane_h15_exit);

MODULE_AUTHOR("Joshua Warren");
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H15-family ANE bring-up module (dt/status/wrapper/boot/dart/run/rtkit/chan; no inference)");
MODULE_LICENSE("Dual MIT/GPL");
