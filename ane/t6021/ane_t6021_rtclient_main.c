// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_t6021_rtclient.c — T6021 (H14 / M2) ANE RTKit CLIENT.
 *
 * This is the t6021 bring-up path that uses mainline RTKit instead of
 * the H13 host-MMIO model: genpd power-up -> devm_apple_rtkit_init on
 * the ANE ASC mailbox (drivers/soc/apple/rtkit.c) -> apple_rtkit_boot
 * (HELLO / EPMAP / STARTEP / IOP power state) -> first CSNE_CMD.
 *
 * Division of labor (receipts/2026-09-22-t6021-rtkit-port):
 *  - CPU start belongs to a quiesce context (m1n1/iBoot). This box's
 *    RVBAR latch is sticky with mode bits 55/48 missing
 *    (2026-09-22-t6021-power-dart-fwload, s23/s24); kernel-context
 *    RVBAR and ps@2e0 writes are fatal. By default this driver
 *    therefore never programs RVBAR or CPU_CONTROL: it refuses to bind
 *    unless the firmware is already alive (CPU_STATUS RUNNING);
 *    fw_start=1 is the fenced exception below.
 *  - genpd/pmgr: Runtime PM + the DT power-domains binding owns the
 *    raise; probe verifies ACTUAL on ane_cpu (pmgr window) before the
 *    first engine read (gate G1). fw_start then puts the islands in the
 *    form macOS runs the ANE in: ane_sys_mpm off, ane_sys/ane_cpu on
 *    with AUTO_ENABLE, td/base/set1-4 on (fw_start_mpm_off).
 *  - Mailbox: apple,asc-mailbox-v4 child node at engine+0x1408000
 *    (a2i/i2a controls 0x285408110/0x285408114 live-read clean, W10).
 *    One AIC line only (ADT ane0 interrupts len 4: raw 0x374) ->
 *    recv-not-empty = that line; TX polls (mailbox.c poll_tx).
 *  - non-posted MMIO everywhere in the ANE aperture (posted writel
 *    froze the box, same receipt): the DT nodes carry
 *    "nonposted-mmio", which of_mmio_is_nonposted turns into
 *    IORESOURCE_MEM_NONPOSTED -> ioremap_np.
 *
 * Post-HELLO protocol (static, selene t602x_ane0_fw_selene_rc4x +
 * kext 26A428; ane-linux-experiments
 * receipts/2026-09-24-m2-post-hello-protocol):
 *  - RTKit protocol v12 only: fw HELLO = min 12 / max 12 (@0x971e0);
 *    mainline rtkit.c accepts 11..12.
 *  - fw RTKit endpoint table (vm 0xed0a0): 0 management, 1 crashlog,
 *    2 syslog, 0x20 "user1". EP1 is the RTKit crashlog endpoint, not
 *    an ANE command channel. The command endpoint is the app endpoint
 *    (>= 0x20) that EPMAP announces; this driver never assumes it.
 *  - fw buffer word (builder 0x982b4, decoder 0x98fc8): addr[43:0] |
 *    size_code[51:44] | unit[53:52], unit 1 = 4 KiB, 2 = 1 MiB,
 *    3 = 2 MiB. App-endpoint word: offset[23:0] | len[47:24] (fw
 *    0x6330/0x64b4, kext rtbuddyEndpointSendMessage).
 *  - The ANE data channels are ChMan rings that the fw lays out in the
 *    host 'IPC ' surface before DONE (fw 0x5348): a table of
 *    0x100-byte descriptors at IPC+0, rings after it.
 *  - CSNE header {u32 rsvd, u16 id, u8 flags, u8 rsvd}; ids from the
 *    selene id->name table (PING 0x11, BUILDINFO 0x06).
 */

#include <linux/completion.h>
#include <linux/dev_printk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/workqueue.h>

#include "ane_t6021.h"

/* pmgr ane_cpu ACTUAL word (ane0 reg1 window, pmgr+0x2e0) */
#define ANE_RTCLIENT_PS_CPU_ACTUAL_OFF	0x2e0

/* CPU_STATUS bits (m1n1 ASCRegs shape) */
#define ANE_ASC_CPU_STATUS_RUNNING	BIT(0)
#define ANE_ASC_CPU_STATUS_STOPPED	BIT(1)

/* rtkit.c routes endpoints below this to its own handlers
 * (rtkit-internal.h APPLE_RTKIT_APP_ENDPOINT_START) */
#define ANE_RTKIT_APP_EP_START		0x20

#define ANE_RTCLIENT_RING_SIZE		SZ_64K	/* csne_ping host ring */

struct ane_rtclient {
	struct device *dev;
	void __iomem *engine;
	void __iomem *pmgr;
	struct apple_rtkit *rtk;
	/* ane_cpu reset (DT resets = <&ane_cpu>, ps RESET bit 31). */
	struct reset_control *cpu_rst;

	/* fw_start=1: view over this device for the shared boot/fwload
	 * contract units (ane_t6021_boot.c/ane_t6021_fwload.c). */
	struct ane_t6021 *fw;
	/* A CPU we released is (or may be) running: state is HELD —
	 * surfaces/rings/IRQ/power links preserved, no unwind, reboot
	 * is the only reclamation (wedged-pin rule). */
	bool held;

	struct delayed_work poll_work;

	bool boot_done;

	/* Lowest app endpoint (>= 0x20) the fw announced in EPMAP and we
	 * STARTEPed; 0 = none. The ANE command endpoint candidate. */
	u8 cmd_ep;
	/* ChMan descriptor table validated in the 'IPC ' surface. */
	bool chman_ok;

	dma_addr_t ring_iova;
	void *ring;

	bool csne_setup_done;
};

/* Each rtkit.c boot-handshake wait (EPMAP, IOP power ack, AP power ack)
 * is 1 s, and the fw sends HELLO only after its ChMan DONE, so the
 * client retries -ETIME waits up to this bound. */
static unsigned int hello_wait_ms = 10000;
module_param(hello_wait_ms, uint, 0444);
MODULE_PARM_DESC(hello_wait_ms,
		 "Upper bound for the RTKit boot handshake (HELLO/EPMAP/power acks), retried in 1 s rtkit.c waits (default 10000)");

static bool start_app_eps = true;
module_param(start_app_eps, bool, 0444);
MODULE_PARM_DESC(start_app_eps,
		 "After the handshake, STARTEP every fw-announced app endpoint (>= 0x20; fw mgmt type 5, flag bit 1). Default on");

static bool scratch3_ack = true;
module_param(scratch3_ack, bool, 0444);
MODULE_PARM_DESC(scratch3_ack,
		 "fw_start=1 only: after DONE, write the host ack SCRATCH3 = 0x08042006 the fw spins on before starting RTKit (selene 0x7edc; kext 0x95eaee4). Default on; 0 withholds it to bisect");

static bool csne_ping;
module_param(csne_ping, bool, 0444);
MODULE_PARM_DESC(csne_ping,
		 "GATED [INFERENCE]: after the handshake, announce a 64 KiB host ring on the command endpoint with the fw buffer word, then send CSNE_CMD_PING (0x11) and BUILDINFO (0x06) as offset|len words. Default off");

static bool fw_start;
module_param(fw_start, bool, 0444);
MODULE_PARM_DESC(fw_start,
		 "OPT-IN: fenced Linux-context firmware start — stage selene (fw_load=1 required), alias it at the latched RVBAR entry, then run the contract-pinned scratch/RUN/READY/publish/wake sequence and continue into the RTKit handshake. Default off = refuse at the CPU gate (evidence-backed: the proven-safe starter is a quiesce-context write-arm; kernel-context start is the open discriminator). Bounded: <=1 s polls, no retry, no ps@2e0 writes.");

/*
 * fw-start-debug (2026-09-22): fwstart#2 hard-crashed with no capture.
 * Two knobs make every attempt observable and bounded:
 *   fw_start_state_report=1 — stage fwload + alias (IOMMU only, no
 *     engine writes), dump the whitelisted ASC state via dev_emerg,
 *     then unwind CLEANLY. Zero-risk observability proof.
 *   fw_start_stop_after=N — run the boot sequence only through step N
 *     (1 grant tunables, 2 scratch clear+pulse, 3 rvbar decision,
 *     4 cpu release + poll A), then stop with -ECANCELED. While no
 *     CPU started the stop unwinds cleanly (rmmod-able, no reboot
 *     needed); step 4 leaves the HELD wedged-pin state. 0 = full run.
 * Smallest crashing N localizes the fatal write; per-write dev_emerg
 * phases (30 ms drain) beat the write to every console.
 */
static int fw_start_stop_after;
module_param(fw_start_stop_after, int, 0444);
MODULE_PARM_DESC(fw_start_stop_after,
		 "fw-start-debug bisect: 0 = full sequence; 1..4 = stop after that step (-ECANCELED). Steps: 1 tunables, 2 scratch, 3 rvbar, 4 RUN+pollA");

static bool fw_start_state_report;
module_param(fw_start_state_report, bool, 0444);
static bool fw_start_skip_genpd;
module_param(fw_start_skip_genpd, bool, 0444);
MODULE_PARM_DESC(fw_start_skip_genpd,
		 "fw-start-debug: skip pm_runtime_resume_and_get in probe and run the static sequence directly. Use only when the islands already read on (recorded 'available' devlinks); the wedged-bind evidence shows the raise hangs the writer on this box.");
MODULE_PARM_DESC(fw_start_state_report,
		 "fw-start-debug: stage firmware + alias, dump ASC state (reads only), then clean unwind — no boot writes");

/*
 * fw_start_table_mode: the pre-CPU engine table block
 * (eng+0xB38/0xB98/0xBF8 <- 0x01FF01FF, kext-sourced, run EVERY
 * EnableANEClocksAndPower per pass4/pass5). 0 = abort before any
 * write, 1 = write the table, 2 = skip it (the 2026-09-20-era
 * diagnostic; the B3 run proved RUN itself is wedge-free with the
 * repaired dtb, so the table is the next discriminator, not a
 * live-fault gate). Default 2 keeps the shipped behavior.
 */
static int fw_start_table_mode = 2;
module_param(fw_start_table_mode, int, 0444);
MODULE_PARM_DESC(fw_start_table_mode,
		 "pre-CPU table block: 0 abort, 1 write (kext-faithful), 2 skip (default)");

static bool fw_start_rtb_mode;
module_param(fw_start_rtb_mode, bool, 0444);
MODULE_PARM_DESC(fw_start_rtb_mode,
		 "fw-start-debug: S1 writes SCRATCH6=0 (RTBuddy/RTKit-app-endpoint select) instead of 1 (legacy). In RTBuddy mode listen for HELLO, do not gate on READY.");
static bool fw_start_venc_gates = true;
module_param(fw_start_venc_gates, bool, 0444);
MODULE_PARM_DESC(fw_start_venc_gates,
		 "Raise the VENC rails the ANE clock-ids need (VENC_SYS 0x2902803e0, then PIPE4/PIPE5/ME0 at 0x290288008/10/18), kext order, parents first. The ANE complex sits behind VENC rails on T6021; Linux claims none of them. Disable only to bisect.");

static bool fw_start_mpm_off = true;
module_param(fw_start_mpm_off, bool, 0444);
MODULE_PARM_DESC(fw_start_mpm_off,
		 "fw_start=1: before the boot sequence's first engine write, power ane_sys_mpm@4000 down to TARGET 0 (the macOS working state) and refuse the sequence unless ane_sys/ane_cpu read the macOS AUTO_ENABLE on-form and td/base/set1-4 read ACTUAL 0xf. 0 keeps the boot-raised ane_sys_mpm on, to bisect.");

/* 0 = off. Nonzero is the CNTFRQ value written to the patchbay before
 * CPU_CONTROL release. The module refuses the write unless the live
 * 36 bytes still match the pinned pattern. */
static u32 patch_timer_freq;
module_param(patch_timer_freq, uint, 0444);
MODULE_PARM_DESC(patch_timer_freq,
		 "fw_start=1: before CPU release, write this to patchbay armv8_timer_frequency at PA 0x10001406880. 0 = off. Refuses unless islands read ACTUAL=0xf, fw_alias_reserved=1, and the 36 bytes at PA 0x10001406870 match (value 0, next tag LRSD).");

static bool fw_start_mbox_ctrl_bit19;
module_param(fw_start_mbox_ctrl_bit19, bool, 0444);
MODULE_PARM_DESC(fw_start_mbox_ctrl_bit19,
		 "fw_start=1: write 0x000a0001 (macOS working-state value, bit 19 set) to A2I_CTRL (0x1408110) and I2A_CTRL (0x1408114) before CPU release, logging before/after reads. Default 0 (off).");

static bool fw_start_dart_single_stream;
module_param(fw_start_dart_single_stream, bool, 0444);
MODULE_PARM_DESC(fw_start_dart_single_stream,
		 "fw_start=1: configure all three ANE DARTs to macOS working-state single-stream form (stream 0 only via DISABLE_STREAMS 0xc20, dart0 PROTECT 0x6) before CPU release, logging before/after reads. Default 0 (off).");

static bool fw_start_core1_run;
module_param(fw_start_core1_run, bool, 0444);
MODULE_PARM_DESC(fw_start_core1_run,
		 "fw_start=1: write 0x10 (RUN) to secondary core control engine+0x1400444 before CPU release, logging before/after reads. Default 0 (off).");

static bool fw_start_wrapper_b80_unmask;
module_param(fw_start_wrapper_b80_unmask, bool, 0444);
MODULE_PARM_DESC(fw_start_wrapper_b80_unmask,
		 "fw_start=1: write 0xffffffff to KIC interrupt registers engine+0x1400b80..b94 and +0x1400bfc before CPU release, logging before/after reads. Default 0 (off).");

static bool fw_start_dapf;
module_param(fw_start_dapf, bool, 0444);
MODULE_PARM_DESC(fw_start_dapf,
		 "fw_start=1: before CPU release, program the dart-ane0 DAPF (PA 0x285804000, ADT reg[3] DAPFLLT) with the five J414c ADT dapf-instance-0 windows, as XNU does, logging before/after reads. Default 0 (off).");

/*
 * Raise the VENC rails the ADT wires as ane0 clock-ids, kext order,
 * parents first. Plain TARGET write + low-byte-0xff poll, exactly the
 * validatePSReg semantics: write 0xf, wait until (val & 0xff) == 0xff
 * (TARGET nibble 0xf AND ACTUAL nibble 0xf). NOTE: no BIT(28)/BIT(31)
 * munging — the earlier code added AUTO_ENABLE and cleared bit 31,
 * which the kext never does, and which can put 0xf0003ff-class values
 * on words whose AUTO-enable semantics are unowned here. Never a
 * TARGET=0 write.
 */
static int ane_rtclient_venc_gates(struct device *dev)
{
	static const struct {
		u32 off;
		u32 id;
		const char *name;
	} gates[4] = {
		{ 0x008, 318, "VENC_PIPE4" },
		{ 0x010, 319, "VENC_PIPE5" },
		{ 0x018, 320, "VENC_ME0" },
		{ 0x020, 321, "VENC_ME1" },
	};
	void __iomem *base;
	void __iomem *root;
	unsigned int i;
	int ret_all = 0;

	/* VENC_SYS (299), map11 = window2+0x300 = 0x2902803e0: the
	 * ps-regs[15] block ROOT (M2Research B6 decode). ps power-up
	 * needs the parent rail first — B5/B5b proved the leaves latch
	 * TARGET without ACTUAL while this is off. Raise it BEFORE the
	 * leaf window. AVEMSR-V (519) above it is VIRTUAL: no write. */
	root = ioremap_np(0x290280000ull, 0x1000);
	if (!root) {
		dev_emerg(dev, "VENC-ROOT: ioremap FAILED\n");
		return -ENOMEM;
	}
	{
		u32 before = readl(root + 0x3e0);
		u32 after;
		int ret;

		dev_emerg(dev, "VENC-ROOT VENC_SYS @+3e0 before=%08x\n",
			  before);
		if ((before & 0xff) != 0xff) {
			writel(before | 0xf, root + 0x3e0);
			ret = readl_poll_timeout(root + 0x3e0, after,
						 ((after & 0xff) == 0xff),
						 10, 50 * 1000);
			after = readl(root + 0x3e0);
			dev_emerg(dev,
				  "VENC-ROOT after=%08x ret=%pe\n",
				  after, ERR_PTR(ret));
			if (ret) {
				iounmap(root);
				return -ETIMEDOUT;
			}
		} else {
			dev_emerg(dev, "VENC-ROOT already on\n");
		}
	}
	iounmap(root);

	base = ioremap_np(0x290288000ull, 0x40);
	if (!base) {
		dev_emerg(dev, "VENC-GATES: ioremap FAILED\n");
		return -ENOMEM;
	}

	/* Read-scan the whole ps-regs[15] block first (reads are safe):
	 * which words are on (ACTUAL[7:4]==0xf, ON signature 0x3ff) vs
	 * idle (0x300)? B5: 0x008 read 0x300, TARGET took, ACTUAL stuck
	 * at 0 -> a parent rail in this block (or above) is down. */
	for (i = 0; i < 8; i++)
		dev_emerg(dev, "VENC-SCAN +%03x = %08x\n", i * 8,
			  readl(base + i * 8));

	/* Only the REAL ps words the kext touches: VENC_DMA at +000 must
	 * already read 0x3ff (VENC_SYS granted it), and the three leaves
	 * PIPE4/PIPE5/ME0 at +008/+010/+018. Never a TARGET=0 write. */
	for (i = 1; i <= 3; i++) {
		void __iomem *reg = base + i * 8;
		u32 before = readl(reg);
		u32 after;
		int ret;

		if ((before & 0xff) == 0xff)
			continue;
		dev_emerg(dev, "VENC-GATES +%03x before=%08x\n", i * 8,
			  before);
		writel(before | 0xf, reg);
		ret = readl_poll_timeout(reg, after,
					 ((after & 0xff) == 0xff),
					 10, 50 * 1000);
		after = readl(reg);
		dev_emerg(dev, "VENC-GATES +%03x after=%08x ret=%pe\n",
			  i * 8, after, ERR_PTR(ret));
		if (ret)
			ret_all = ret;
	}

	for (i = 0; i < 3; i++) {
		u32 v = readl(base + gates[i].off);

		dev_emerg(dev, "VENC-GATES %u %s final=%08x\n",
			  gates[i].id, gates[i].name, v);
		if ((v & 0xff) != 0xff)
			ret_all = -ETIMEDOUT;
	}

	iounmap(base);
	return ret_all;
}

static bool poll_rx;
module_param(poll_rx, bool, 0444);
MODULE_PARM_DESC(poll_rx,
		 "Drive RX by apple_rtkit_poll from a workqueue even though a recv IRQ exists (fallback if raw 0x374 is not the recv line)");

/* ---- RTKit callbacks ---- */

static void ane_rtclient_recv(void *cookie, u8 ep, u64 message)
{
	struct ane_rtclient *ane = cookie;

	/* App endpoints only reach here (rtkit.c owns 0..0x1f). Both
	 * wire shapes the fw uses are decoded side by side; which one a
	 * message is gets pinned on sight. */
	dev_info(ane->dev,
		 "rtkit app msg: ep=%#x msg=%016llx | as offset|len: off=%#llx len=%#llx | as buffer word: addr=%#llx size=%#x unit=%llu\n",
		 ep, message,
		 (u64)FIELD_GET(ANE_MBI_MSG48_OFF, message),
		 (u64)FIELD_GET(ANE_MBI_MSG48_LEN, message),
		 message & ANE_EP_DOORBELL_OFFSET,
		 ane_ep_doorbell_size(message),
		 (u64)FIELD_GET(ANE_EP_DOORBELL_UNIT, message));
}

static void ane_rtclient_crashed(void *cookie, const void *crashlog,
				 size_t size)
{
	struct ane_rtclient *ane = cookie;

	dev_err(ane->dev, "rtkit: coprocessor crashed (crashlog %zu bytes)\n",
		size);
	print_hex_dump(KERN_ERR, "ANE crashlog: ", DUMP_PREFIX_OFFSET, 16, 1,
		       crashlog, min_t(size_t, size, 256), false);
}

/* System-endpoint buffer grants (crashlog EP1, syslog EP2): rtkit.c
 * decodes the fw request (unit-1 buffer word, addr 0 = host allocates)
 * and replies with the same word carrying our IOVA. The fw asks for
 * the crashlog buffer with addr 0 (0x98e44) and for the syslog buffer
 * with addr 0 unless its own-buffer flag is set (0x98128), in which
 * case it does not wait for a reply — so a fw-provided address is
 * refused (mainline behavior) and only logged. The host allocation
 * must honor the fw entry-alias invariant (W16). */
static int ane_rtclient_shmem_setup(void *cookie,
				    struct apple_rtkit_shmem *bfr)
{
	struct ane_rtclient *ane = cookie;

	if (bfr->iova) {
		dev_warn(ane->dev,
			 "rtkit: fw-provided shmem iova=%pad size=%#zx — refused (unmapped by design)\n",
			 &bfr->iova, bfr->size);
		return -EINVAL;
	}

	bfr->buffer = dma_alloc_coherent(ane->dev, bfr->size, &bfr->iova,
					 GFP_KERNEL);
	if (!bfr->buffer)
		return -ENOMEM;
	if (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, bfr->iova,
						    bfr->size)) {
		dev_err(ane->dev,
			"rtkit: shmem grant %pad+%#zx overlaps the fw alias — refusing\n",
			&bfr->iova, bfr->size);
		dma_free_coherent(ane->dev, bfr->size, bfr->buffer, bfr->iova);
		bfr->buffer = NULL;
		return -EBUSY;
	}
	dev_info(ane->dev, "rtkit: shmem grant iova=%pad size=%#zx\n",
		 &bfr->iova, bfr->size);
	return 0;
}

static void ane_rtclient_shmem_destroy(void *cookie,
				       struct apple_rtkit_shmem *bfr)
{
	struct ane_rtclient *ane = cookie;

	if (!bfr->buffer)
		return;
	if (ane->held) {
		/* wedged-pin: a running ASC may still write here */
		dev_warn(ane->dev,
			 "rtkit: shmem %pad HELD (CPU started) — not freed\n",
			 &bfr->iova);
		return;
	}
	dma_free_coherent(ane->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops ane_rtclient_rtkit_ops = {
	.crashed = ane_rtclient_crashed,
	.recv_message = ane_rtclient_recv,
	.shmem_setup = ane_rtclient_shmem_setup,
	.shmem_destroy = ane_rtclient_shmem_destroy,
};

/* ---- poll worker: RX fallback while the recv line is unproven ---- */

static void ane_rtclient_post_boot(struct work_struct *w)
{
	struct ane_rtclient *ane =
		container_of(to_delayed_work(w), struct ane_rtclient,
			     poll_work);

	apple_rtkit_poll(ane->rtk);

	if (!ane->boot_done) {
		schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));
		return;
	}

	if (poll_rx)
		schedule_delayed_work(&ane->poll_work, HZ);
}

/* ---- ChMan descriptor table: the control-command validation step ----
 * Contract and static layout: ane_t6021_boot.h (ane_t6021_chman_*),
 * checked offline by tools/h14_boot_regression.c. A mismatch is
 * logged, never acted on. */
static_assert(sizeof(struct ane_t6021_chman_desc) == ANE_T6021_CHMAN_ENTRY_SIZE);

static void ane_rtclient_validate_chman(struct ane_rtclient *ane)
{
	struct ane_t6021 *a = ane->fw;
	const struct ane_t6021_chman_desc *t;
	unsigned int i, bad;

	if (!a || !a->boot_ipc) {
		dev_info(ane->dev,
			 "chman: no host IPC surface (firmware not started by this driver) — table not validated\n");
		return;
	}
	if (a->boot_ipc_size < ANE_T6021_CHMAN_TOTAL) {
		dev_warn(ane->dev,
			 "chman: IPC surface %#llx bytes < fw layout %#x — table not validated\n",
			 a->boot_ipc_size, ANE_T6021_CHMAN_TOTAL);
		return;
	}

	dma_rmb();
	t = a->boot_ipc;
	dev_info(ane->dev,
		 "chman: IPC dva=%pad size=%#llx booted=%u scratch_result=%016llx (low32 = fw VA of its IPC mapping, 0x77a4)\n",
		 &a->boot_ipc_iova, a->boot_ipc_size, a->booted,
		 a->boot_scratch_result);

	bad = ane_t6021_chman_check(t, a->boot_ipc_iova);
	for (i = 0; i < ANE_T6021_CHMAN_COUNT; i++) {
		const struct ane_t6021_chman_desc *d = &t[i];
		const struct ane_t6021_chman_static *s = &ane_t6021_chman_layout[i];

		dev_info(ane->dev,
			 "chman[%u]: name=\"%.*s\" type=%u bit=%u size=%#llx ring=%#llx %s (static: %s/%u/%u/%#llx/ipc+%#x)\n",
			 i, ANE_T6021_CHMAN_NAME_LEN, d->name, d->type, d->bit,
			 d->size, d->ring, (bad & BIT(i)) ? "MISMATCH" : "OK",
			 s->name, s->type, s->bit, s->size, s->off);
		/* Ring head (IOP ring header: version/wrptr/rdptr/size
		 * per the fw asserts at cstring 0xa296e) for the receipt. */
		if (d->ring >= a->boot_ipc_iova &&
		    d->ring + 0x20 <= a->boot_ipc_iova + a->boot_ipc_size)
			print_hex_dump(KERN_INFO, "chman ring head: ",
				       DUMP_PREFIX_OFFSET, 16, 4,
				       a->boot_ipc + (d->ring - a->boot_ipc_iova),
				       0x20, false);
	}

	ane->chman_ok = !bad;
	dev_info(ane->dev, "chman: table %s (mismatch mask %#x)\n",
		 bad ? "NOT VALIDATED" : "VALIDATED", bad);
}

/* ---- app endpoints ---- */

/* STARTEP every announced app endpoint. Mainline apple_rtkit_start_ep
 * sends mgmt type 5 with flag bit 1; the fw mgmt dispatcher (0x97364
 * case 5 -> 0x973b4) reads ep from [47:32] and starts it on flag == 2
 * (0x974bc). Same call every Asahi RTKit client makes (SMC on 0x20). */
static void ane_rtclient_start_app_eps(struct ane_rtclient *ane)
{
	int ep;

	for (ep = ANE_RTKIT_APP_EP_START; ep < 0x100; ep++) {
		int ret;

		if (!apple_rtkit_has_endpoint(ane->rtk, ep))
			continue;
		ret = apple_rtkit_start_ep(ane->rtk, ep);
		dev_info(ane->dev, "rtkit: STARTEP app ep %#x -> %pe\n", ep,
			 ERR_PTR(ret));
		if (!ret && !ane->cmd_ep)
			ane->cmd_ep = ep;
	}
	if (!ane->cmd_ep)
		dev_warn(ane->dev,
			 "rtkit: fw announced no app endpoint (static table predicts 0x20 \"user1\")\n");
}

/* GATED [INFERENCE] first CSNE control command. What is proven: the
 * fw's buffer word format (0x98fc8), the app-endpoint offset|len word
 * (0x64b4), the CSNE header. What is not: that the app endpoint takes a
 * buffer word to set its ring base ([obj+0x28]) before offset|len
 * words. Everything sent is logged; a wrong guess can crash the fw
 * (crashlog is captured), never the host. */
static void ane_rtclient_csne_cmd(struct ane_rtclient *ane, u16 id,
				  u32 cursor)
{
	struct ane_csne_hdr hdr;
	u64 msg;
	int ret;

	ane_csne_hdr_init(&hdr, id);
	memcpy(ane->ring + cursor, &hdr, sizeof(hdr));
	dma_wmb();

	msg = ane_mbi_msg48_encode(cursor, sizeof(hdr));
	ret = apple_rtkit_send_message(ane->rtk, ane->cmd_ep, msg, NULL,
				       false);
	dev_info(ane->dev,
		 "csne: CSNE_CMD_%#x submit ep=%#x cursor=%u len=%zu word=%016llx -> %pe\n",
		 id, ane->cmd_ep, cursor, sizeof(hdr), msg, ERR_PTR(ret));
}

static void ane_rtclient_csne_ping(struct ane_rtclient *ane)
{
	u64 word;
	int ret;

	if (!ane->cmd_ep) {
		dev_info(ane->dev, "csne: no command endpoint; no ping\n");
		return;
	}

	if (!ane->csne_setup_done) {
		/* Never devm/dmam: under the wedged pin a started ASC may
		 * still read this ring after unbind; remove() frees it
		 * only when not held. */
		ane->ring = dma_alloc_coherent(ane->dev, ANE_RTCLIENT_RING_SIZE,
					       &ane->ring_iova, GFP_KERNEL);
		if (!ane->ring)
			return;
		if (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, ane->ring_iova,
							    ANE_RTCLIENT_RING_SIZE)) {
			dev_err(ane->dev, "csne: ring overlaps the fw alias — no ping\n");
			return;
		}
		word = ane_ep_doorbell_encode(ane->ring_iova,
					      ANE_RTCLIENT_RING_SIZE);
		ret = apple_rtkit_send_message(ane->rtk, ane->cmd_ep, word,
					       NULL, false);
		dev_info(ane->dev,
			 "csne: ring announce ep=%#x iova=%pad size=%#x word=%016llx -> %pe [INFERENCE]\n",
			 ane->cmd_ep, &ane->ring_iova, ANE_RTCLIENT_RING_SIZE,
			 word, ERR_PTR(ret));
		if (ret)
			return;
		ane->csne_setup_done = true;
	}

	ane_rtclient_csne_cmd(ane, CSNE_CMD_PING, 0);
	ane_rtclient_csne_cmd(ane, CSNE_CMD_BUILDINFO, sizeof(struct ane_csne_hdr));
}

/* ---- probe ---- */
/* Pinned live pattern at PA 0x10001406870, 9 words. Word 4 is the
 * frequency value (must be 0); word 5 is the LRSD tag. Confirmed by
 * read on the M2, 2026-09-25. */
static const u32 ane_patchbay_expect[9] = {
	0x00000000, 0x00000000, 0x76384671, 0x00000004, 0x00000000,
	0x4453524c, 0x00000001, 0x53565300, 0x00000844,
};

static int ane_rtclient_patch_timer_freq(struct ane_rtclient *ane, u32 freq)
{
	static const unsigned int islands[] = {
		0x2e0, 0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x4030
	};
	struct device *dev = ane->dev;
	void __iomem *win;
	unsigned int i;
	u32 before, after;

	for (i = 0; i < ARRAY_SIZE(islands); i++) {
		u32 v = readl(ane->pmgr + islands[i]);

		if (FIELD_GET(ANE_PS_ACTUAL, v) != ANE_PS_ON) {
			dev_emerg(dev,
				  "timer-freq: pmgr+%#x=%08x ACTUAL != 0xf — refusing\n",
				  islands[i], v);
			return -EIO;
		}
	}
	if (!ane_t6021_fw_alias_is_reserved()) {
		dev_emerg(dev,
			  "timer-freq: fw_alias_reserved=0 — core would not fetch this PA; refusing\n");
		return -EINVAL;
	}
	/* Same non-posted map the observer used to confirm these bytes.
	 * Not memremap: a second memremap of this DRAM EXEC-faults. */
	win = ioremap_np(0x10001406870ull, 36);
	if (!win)
		return -ENOMEM;
	for (i = 0; i < 9; i++) {
		u32 got = readl(win + 4 * i);

		if (got != ane_patchbay_expect[i]) {
			dev_emerg(dev,
				  "timer-freq: word[%u]=%08x want %08x — not writing\n",
				  i, got, ane_patchbay_expect[i]);
			iounmap(win);
			return -EIO;
		}
	}
	before = readl(win + 16);
	writel(freq, win + 16);
	after = readl(win + 16);
	iounmap(win);
	dev_emerg(dev,
		  "timer-freq: PA 0x10001406880 before=%08x wrote=%08x readback=%08x\n",
		  before, freq, after);
	return after == freq ? 0 : -EIO;
}

/*
 * macOS runs the ANE with ane_sys_mpm@4000 off: it reads 0x300 in all 14
 * samples of ane-linux-experiments receipt 2026-09-25-macos-ane-pstable,
 * including those at 4.8 W encoder load. In the same samples ane_sys@260 and
 * ane_cpu@2e0 read 0x1f0003ff (AUTO_ENABLE, ACTUAL and TARGET 0xf) and the
 * six compute islands read 0x3ff. Linux raises ane_sys_mpm at boot because
 * the stock DTB marks it apple,always-on, and genpd never lowers an
 * always-on domain. Power it down with the write apple_pmgr_ps_set()
 * issues for PWRGATE, then confirm the rest of the macOS form.
 */
static int ane_rtclient_ps_macos_form(struct ane_rtclient *ane)
{
	static const struct {
		u32 off;
		bool auto_enable;
		const char *name;
	} on[] = {
		{ 0x260, true, "ane_sys" },	{ 0x2e0, true, "ane_cpu" },
		{ 0x4008, false, "ane_td" },	{ 0x4010, false, "ane_base" },
		{ 0x4018, false, "ane_set1" },	{ 0x4020, false, "ane_set2" },
		{ 0x4028, false, "ane_set3" },	{ 0x4030, false, "ane_set4" },
	};
	struct device *dev = ane->dev;
	void __iomem *mpm = ane->pmgr + 0x4000;
	unsigned int i;
	int ret;
	bool ok;
	u32 v;

	v = readl(mpm);
	dev_emerg(dev, "PS-FORM ane_sys_mpm@4000 before=%08x\n", v);
	if (v & (ANE_PS_TARGET | ANE_PS_ACTUAL)) {
		writel(v & ~(ANE_PS_AUTO_ENABLE | ANE_PS_WAS_GATED |
			     ANE_PS_TARGET), mpm);
		ret = readl_poll_timeout(mpm, v,
					 !FIELD_GET(ANE_PS_ACTUAL, v),
					 10, 100 * 1000);
		dev_emerg(dev, "PS-FORM ane_sys_mpm@4000 after=%08x ret=%pe\n",
			  v, ERR_PTR(ret));
		if (ret)
			return ret;
	}

	ret = 0;
	for (i = 0; i < ARRAY_SIZE(on); i++) {
		v = readl(ane->pmgr + on[i].off);
		ok = FIELD_GET(ANE_PS_ACTUAL, v) == ANE_PS_ON &&
		     FIELD_GET(ANE_PS_TARGET, v) == ANE_PS_ON &&
		     (!on[i].auto_enable || (v & ANE_PS_AUTO_ENABLE));
		dev_emerg(dev, "PS-FORM %s@%#x=%08x %s\n", on[i].name,
			  on[i].off, v, ok ? "macOS form" : "NOT macOS form");
		if (!ok)
			ret = -EIO;
	}
	return ret;
}

static void ane_rtclient_apply_mbox_ctrl_bit19(struct ane_rtclient *ane)
{
	u32 a2i_before, a2i_after;
	u32 i2a_before, i2a_after;

	a2i_before = readl(ane->engine + ANE_ASC_MBOX_A2I_CTRL);
	i2a_before = readl(ane->engine + ANE_ASC_MBOX_I2A_CTRL);

	/* macOS working-state value: 0x000a0001 (bit 19 + bit 17 EMPTY + bit 0 ENABLE) */
	writel(0x000a0001, ane->engine + ANE_ASC_MBOX_A2I_CTRL);
	writel(0x000a0001, ane->engine + ANE_ASC_MBOX_I2A_CTRL);

	mb();
	a2i_after = readl(ane->engine + ANE_ASC_MBOX_A2I_CTRL);
	i2a_after = readl(ane->engine + ANE_ASC_MBOX_I2A_CTRL);

	dev_emerg(ane->dev,
		  "BOOT-PHASE mbox-ctrl-bit19: A2I_CTRL %08x -> %08x (wrote 000a0001), I2A_CTRL %08x -> %08x (wrote 000a0001)\n",
		  a2i_before, a2i_after, i2a_before, i2a_after);
}

static void ane_rtclient_apply_dart_single_stream(struct ane_rtclient *ane)
{
	static const struct {
		u64 base;
		const char *name;
	} darts[3] = {
		{ 0x285800000ull, "inst0-LLT" },
		{ 0x285810000ull, "inst1-BRD" },
		{ 0x285820000ull, "inst2-BWR" },
	};
	unsigned int di;

	for (di = 0; di < 3; di++) {
		void __iomem *d = ioremap_np(darts[di].base, 0x2000);
		u32 en_before, prot_before;
		u32 en_after, prot_after, tcr_after, ttbr_after;
		int w;

		if (!d) {
			dev_emerg(ane->dev,
				  "BOOT-PHASE dart-single-stream %s: ioremap FAILED\n",
				  darts[di].name);
			continue;
		}

		en_before = readl(d + 0xc00);
		prot_before = readl(d + 0x200);

		/* Disable streams 1..255 via DISABLE_STREAMS (0xc20..0xc3c):
		 * stream 0 kept enabled, streams 1..31 disabled by ~1U,
		 * streams 32..255 disabled by U32_MAX in words 1..7. */
		writel(~1U, d + 0xc20);
		for (w = 1; w < 8; w++)
			writel(U32_MAX, d + 0xc20 + 4 * w);

		/* Ensure stream 0 is enabled in ENABLE_STREAMS (0xc00) */
		writel(1U, d + 0xc00);

		/* On dart0 (inst0-LLT), macOS working state reads PROTECT = 0x6
		 * (LOCK_REG_4xx | _BIT2; TCR/TTBR unlocked). dart1 and dart2
		 * have PROTECT = 0. */
		if (di == 0)
			writel(0x6U, d + 0x200);

		mb();
		en_after = readl(d + 0xc00);
		prot_after = readl(d + 0x200);
		tcr_after = readl(d + 0x1000);
		ttbr_after = readl(d + 0x1400);

		dev_emerg(ane->dev,
			  "BOOT-PHASE dart-single-stream %s: ENABLE %08x -> %08x, PROTECT %08x -> %08x, TCR0=%08x, TTBR0=%08x\n",
			  darts[di].name,
			  en_before, en_after,
			  prot_before, prot_after,
			  tcr_after, ttbr_after);

		iounmap(d);
	}
}

/*
 * dart-ane0 DAPF: the filter on the ANE's physical (bypass-stream) MMIO
 * accesses. XNU programs it from the ADT property dapf-instance-0; m1n1
 * programs DAPF only for aop/mtp/pmp/isp and ANE tunables only on T8103,
 * so on T6021 nothing opens these windows for the ANE firmware. The first
 * window is the ANE pmgr ps block (ane_sys_mpm..set4). Values are the
 * J414cAP ADT entries (52-byte t8110 form), written in m1n1
 * dapf_init_t8110a register order: r4, start, end, r0 = r0h << 4 | r0l,
 * r20.
 */
static void ane_rtclient_apply_dapf(struct ane_rtclient *ane)
{
	static const struct {
		u64 start;
		u64 end;
	} win[] = {
		{ 0x28e084000ull, 0x28e084033ull },
		{ 0x28e080260ull, 0x28e080263ull },
		{ 0x38545c000ull, 0x38545c003ull },
		{ 0x406468000ull, 0x406468003ull },
		{ 0x228545c000ull, 0x228545c003ull },
	};
	void __iomem *d = ioremap_np(0x285804000ull, 0x4000);
	unsigned int i;

	if (!d) {
		dev_emerg(ane->dev, "BOOT-PHASE dapf: ioremap FAILED\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(win); i++) {
		void __iomem *e = d + i * 0x40;
		u32 r0 = readl(e), r4 = readl(e + 0x04), r20 = readl(e + 0x20);
		u64 start = readq(e + 0x08), end = readq(e + 0x10);

		writel(0, e + 0x04);
		writeq(win[i].start, e + 0x08);
		writeq(win[i].end, e + 0x10);
		writel(0x31, e + 0x00);
		writel(0x1, e + 0x20);
		mb();
		dev_emerg(ane->dev,
			  "BOOT-PHASE dapf[%u]: r0 %08x -> %08x, r4 %08x -> %08x, start %llx -> %llx, end %llx -> %llx, r20 %08x -> %08x\n",
			  i, r0, readl(e), r4, readl(e + 0x04),
			  start, readq(e + 0x08), end, readq(e + 0x10),
			  r20, readl(e + 0x20));
	}
	dev_emerg(ane->dev, "BOOT-PHASE dapf[%zu] (unused): r0 %08x start %llx end %llx\n",
		  ARRAY_SIZE(win), readl(d + ARRAY_SIZE(win) * 0x40),
		  readq(d + ARRAY_SIZE(win) * 0x40 + 0x08),
		  readq(d + ARRAY_SIZE(win) * 0x40 + 0x10));
	iounmap(d);
}

static void ane_rtclient_apply_core1_run(struct ane_rtclient *ane)
{
	u32 before, after;

	before = readl(ane->engine + 0x1400444);
	writel(0x10, ane->engine + 0x1400444);
	mb();
	after = readl(ane->engine + 0x1400444);
	dev_emerg(ane->dev,
		  "BOOT-PHASE core1-run: +0x1400444 %08x -> %08x (wrote 0x10)\n",
		  before, after);
}

static void ane_rtclient_apply_wrapper_b80_unmask(struct ane_rtclient *ane)
{
	static const u32 offs[] = {
		0x1400b80, 0x1400b84, 0x1400b88, 0x1400b8c,
		0x1400b90, 0x1400b94, 0x1400bfc
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(offs); i++) {
		u32 before = readl(ane->engine + offs[i]);
		writel(U32_MAX, ane->engine + offs[i]);
		mb();
		dev_emerg(ane->dev,
			  "BOOT-PHASE wrapper-b80 +%x: %08x -> %08x (wrote ffffffff)\n",
			  offs[i], before, readl(ane->engine + offs[i]));
	}
}

/*
 * Read-only snapshot of the engine words that differ between macOS's
 * working state and anything Linux writes (ane-linux-experiments receipt
 * 2026-09-25-macos-ane-engine-dump, wrapper map). Logged before and after
 * the CPU release so one run yields the Linux side of that diff. The list
 * holds no pop-on-read word (+0x1400818/81c/820, +0x1408810/818/830/838)
 * and nothing in CoreSight.
 */
static void ane_rtclient_log_wrapper(struct ane_rtclient *ane, const char *tag)
{
	static const u32 words[] = {
		0x1400000, 0x1400008, 0x1400040, 0x1400044, 0x1400048,
		0x1400444,
		0x1400a00, 0x1400a04, 0x1400a08, 0x1400a0c, 0x1400a10, 0x1400a14,
		0x1400b80, 0x1400b84, 0x1400b88, 0x1400b8c, 0x1400b90, 0x1400b94,
		0x1400bfc, 0x1401008,
		0x1404110, 0x1404114, 0x1408110, 0x1408114, 0x140c110, 0x1410110,
		0x1840048, 0x184004c, 0x1840050, 0x1840054, 0x1840058, 0x184005c,
		0x1840060, 0x1840064, 0x1840068, 0x184006c,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(words); i++)
		dev_emerg(ane->dev, "WRAPPER %s +%#09x = %08x\n", tag, words[i],
			  readl(ane->engine + words[i]));
}


/*
 * Fenced Linux-context firmware start (fw_start=1). Evidence chain:
 *  - iBoot latches ANE RVBAR = entry | 1 (live reads 0x10000000001:
 *    bit0 valid, entry bits = dart-ane0 vm-base 0x10000000000; ADT
 *    "pre-loaded" = 1). The kext law (ANE_Init, tbnz-skip) and this
 *    latch mean the driver NEVER writes RVBAR on this box.
 *  - Starting the CPU = CPU_CONTROL 0 -> 0x10 (RUN) — exactly what
 *    m1n1 does (ASC.boot(): RUN=1, no RVBAR anywhere) and what the
 *    kext does after its RVBAR skip. The open discriminator is
 *    whether the ASC fetch at the latched entry translates through
 *    dart-ane0 (mode bits 55/48 absent from the iBoot latch); with
 *    the selene alias mapped at the entry (fwload) a READY on
 *    SCRATCH7 proves translation; a dart translation fault names the
 *    stream; silence parks the core. Every outcome is bounded.
 * The sequence itself is the contract-pinned core in
 * ane_t6021_boot.h (ane_t6021_boot_run), exercised here through the
 * kernel io backend in ane_t6021_boot.c: W8 grant tunables, scratch
 * clear + stale-pulse, RVBAR skip-or-fold, RUN, poll A (READY
 * 0x08042006, <=1000 x 1 ms), publish (pinned pool sources) + wake,
 * poll B (DONE). Table block skipped (mode 2, authorized
 * diagnostic). No ps@2e0 write anywhere; no power_off/power_on
 * retry on timeout.
 */
static int ane_rtclient_fw_start(struct ane_rtclient *ane)
{
	struct device *dev = ane->dev;
	struct ane_t6021 *a;
	u32 cpu_status;
	int ret;

	if (!ane_t6021_fwload_requested()) {
		dev_err(dev, "fw_start: requires fw_load=1 (no staged firmware)\n");
		return -EINVAL;
	}
	if (!device_iommu_mapped(dev)) {
		dev_err(dev,
			"fw_start: device not IOMMU-mapped — a staged DVA/entry alias would be untranslated; refusing\n");
		return -EINVAL;
	}

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	a->base[ANE_T6021_REG_ENGINE] = ane->engine;
	a->irq = -1;
	a->power_gated = true;	/* the eight-island G1 gate passed above */
	ane->fw = a;

	/* Stage selene + alias it at the latched RVBAR entry
	 * (request_firmware + sha-pin + exact-image validation +
	 * per-page iommu_map with roundtrip verification). */
	dev_emerg(dev, "BOOT-PHASE fwload stage+alias begin\n");
	ret = ane_t6021_fwload_probe(a);
	if (ret) {
		dev_err_probe(dev, ret, "fw_start: staging failed\n");
		ane->fw = NULL;
		return ret;
	}
	dev_emerg(dev, "BOOT-PHASE fwload stage+alias done (fw_iova=%pad)\n",
		  &a->fw_iova);

	if (!ane_t6021_rvbar_entry_ok(a->fw_iova)) {
		dev_err(dev,
			"fw_start: staged iova %pad sets bits the entry fold drops — refusing\n",
			&a->fw_iova);
		ane_t6021_fwload_remove(a);
		ane->fw = NULL;
		return -EINVAL;
	}

	if (fw_start_state_report) {
		/* Zero-write observability run: reads only, then a CLEAN
		 * unwind (fwload removed, module unpinned, insmod fails
		 * with -ECANCELED). */
		u64 rv = readq(ane->engine + ANE_ASC_RVBAR);
		int s;

		/* power-dart-fwload preflight item 2/§2.2: dart-ane0
		 * instances 0/1/2 state — translate on, no bypass, TTBR
		 * valid, stream-0 enabled. Must run HERE: dart1/2 sit on
		 * the ane_cpu domain, only powered while genpd holds. */
		{
			static const struct {
				u64 base;
				const char *name;
			} darts[3] = {
				{ 0x285800000ull, "inst0-LLT" },
				{ 0x285810000ull, "inst1-BRD" },
				{ 0x285820000ull, "inst2-BWR" },
			};
			unsigned int di;

			for (di = 0; di < 3; di++) {
				void __iomem *d = ioremap_np(darts[di].base,
							     0x2000);

				if (!d) {
					dev_emerg(dev,
						  "DART %s: ioremap FAILED\n",
						  darts[di].name);
					continue;
				}
				dev_emerg(dev,
					  "DART %s: TCR=%08x TTBR=%08x ENABLE=%08x PROTECT=%08x %s%s\n",
					  darts[di].name,
					  readl(d + 0x1000),
					  readl(d + 0x1400),
					  readl(d + 0xc00),
					  readl(d + 0x200),
					  (readl(d + 0x1000) & BIT(1)) ?
						"BYPASS-DART!" : "translate",
					  (readl(d + 0x1400) & BIT(0)) ?
						"" : " TTBR-INVALID!");
				iounmap(d);
			}
		}

		dev_emerg(dev,
			  "BOOT-REPORT rvbar=%016llx cpu_status=%08x scratch=%08x %08x %08x %08x %08x %08x %08x %08x a2i=%08x i2a=%08x\n",
			  rv, readl(ane->engine + ANE_ASC_CPU_STATUS),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 0),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 1),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 2),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 3),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 4),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 5),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 6),
			  readl(ane->engine + ANE_MBI_SCRATCH0 + 4 * 7),
			  readl(ane->engine + ANE_ASC_MBOX_A2I_CTRL),
			  readl(ane->engine + ANE_ASC_MBOX_I2A_CTRL));
		ane_rtclient_log_wrapper(ane, "state-report");
		for (s = 0; s < 30; s++)
			msleep(10);	/* let the report hit every sink */
		ane_t6021_fwload_remove(a);
		ane->fw = NULL;
		return -ECANCELED;
	}

	if (fw_start_venc_gates) {
		/* Zero-engine-write precondition: VENC rails before the
		 * sequence (kext provider order). On failure: abort
		 * BEFORE any engine write, clean unwind. */
		int vg = ane_rtclient_venc_gates(dev);

		if (vg) {
			dev_emerg(dev,
				  "BOOT-PHASE venc-gates FAILED (%pe) — refusing sequence\n",
				  ERR_PTR(vg));
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			return vg;
		}
		dev_emerg(dev, "BOOT-PHASE venc-gates raised\n");
	}
	if (patch_timer_freq) {
		int pr = ane_rtclient_patch_timer_freq(ane, patch_timer_freq);

		if (pr) {
			dev_emerg(dev,
				  "BOOT-PHASE timer-freq FAILED (%pe) — refusing CPU release\n",
				  ERR_PTR(pr));
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			return pr;
		}
	}

	if (fw_start_mpm_off) {
		int pf = ane_rtclient_ps_macos_form(ane);

		if (pf) {
			dev_emerg(dev,
				  "BOOT-PHASE ps-form FAILED (%pe) — refusing sequence\n",
				  ERR_PTR(pf));
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			return pf;
		}
		dev_emerg(dev, "BOOT-PHASE ps-form: ane_sys_mpm off, macOS form\n");
	}

	if (fw_start_dart_single_stream)
		ane_rtclient_apply_dart_single_stream(ane);

	if (fw_start_mbox_ctrl_bit19)
		ane_rtclient_apply_mbox_ctrl_bit19(ane);

	if (fw_start_core1_run)
		ane_rtclient_apply_core1_run(ane);

	if (fw_start_wrapper_b80_unmask)
		ane_rtclient_apply_wrapper_b80_unmask(ane);

	if (fw_start_dapf)
		ane_rtclient_apply_dapf(ane);

	ane_rtclient_log_wrapper(ane, "pre-release");

	ret = ane_t6021_boot_start(a, fw_start_stop_after, fw_start_table_mode,
				 fw_start_rtb_mode);
	if (ret == -ENODATA || ret == -EAGAIN || ret == -EBUSY ||
	    ret == -ECANCELED) {
		/* Refused/stopped before any CPU start: normal unwind is
		 * safe (-ECANCELED = bisect stop, state clean). */
		ane_t6021_fwload_remove(a);
		ane->fw = NULL;
		return ret;
	}

	/* From here a CPU may be running: HELD. No unwind, ever. */
	ane->held = true;
	cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
	dev_emerg(dev,
		  "BOOT-PHASE sequence returned %pe (cpu_started=%u fw_alive=%u booted=%u) CPU_STATUS=0x%x\n",
		  ERR_PTR(ret), a->cpu_started, a->fw_alive, a->booted,
		  cpu_status);
	ane_rtclient_log_wrapper(ane, "post-release");

	/* M2Research split discriminator: the fw page-table region
	 * (VM 0x104000-0x110000) ships all-zero; nonzero descriptors
	 * after a timeout mean the fw reached the table builder
	 * (~0x4e4) and parks post-MMU-on; all zero means the park is
	 * at the ROM jump / entry fetch itself. Host-side read of the
	 * coherent staging buffer — no extra hardware access. */
	if (a->fw_buf) {
		const u64 *tt = a->fw_buf + 0x104000;
		unsigned int n, nonzero = 0, count = 0xC000 / 8;

		for (n = 0; n < count; n++)
			if (tt[n])
				nonzero++;
		dev_emerg(dev,
			  "FW-TT region 0x104000: first=%016llx second=%016llx nonzero=%u/%u\n",
			  tt[0], tt[1], nonzero, count);
	}

	if (fw_start_stop_after) {
		/* Bisect stop or poll-A timeout reached the HELD state:
		 * never continue into the handshake — publish/wake were
		 * withheld, so the fw HELLO cannot come. */
		dev_emerg(dev,
			  "BOOT-PHASE bisect stop (stop_after=%d r=%pe): binding fenced-HELD; reboot reclaims\n",
			  fw_start_stop_after, ERR_PTR(ret));
		return 0;
	}

	if (!a->fw_alive) {
		/* Poll A timeout: RUN released, no READY. The fetch
		 * discriminator answered NEGATIVE (park or bypass);
		 * state held, module pinned, RTKit pointless. */
		dev_err(dev,
			"fw_start: no SCRATCH7 READY after CPU release — ASC fetch did not reach the staged alias (kernel-context start discriminator: negative); HELD until reboot, RTKit handshake skipped\n");
		return 0;	/* bind fenced */
	}

	/* READY (and usually DONE) observed: the fetch DID translate.
	 * RTKit handshake follows in probe. */
	return 0;
}

static void ane_rtclient_unmap_engine(void *data)
{
	iounmap(((struct ane_rtclient *)data)->engine);
}

/*
 * restart_probe answers whether this box can restart the firmware
 * without a reboot. It stops the core (CPU_CONTROL <- 0, bounded wait for
 * STOPPED), cycles it through the framework ane_cpu reset (the domain
 * stays powered), and reports RVBAR and CPU_STATUS before and after: the
 * iBoot RVBAR latch must survive the reset, because kernel RVBAR writes
 * are fatal here. It never sets RUN again and keeps the module pin and
 * every DMA surface, so a reboot still reclaims. Raw ps writes froze the
 * box three times (W16 pass-2): write this only as the last act before a
 * planned reboot.
 */
static ssize_t restart_probe_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct ane_rtclient *ane = dev_get_drvdata(dev);
	u64 rv0, rv1;
	u32 st0, st1, st2;
	int ret;

	if (!ane->cpu_rst)
		return -ENODEV;

	rv0 = readq(ane->engine + ANE_ASC_RVBAR);
	st0 = readl(ane->engine + ANE_ASC_CPU_STATUS);
	writel(0, ane->engine + ANE_ASC_CPU_CONTROL);
	ret = readl_poll_timeout(ane->engine + ANE_ASC_CPU_STATUS, st1,
				 st1 & ANE_ASC_CPU_STATUS_STOPPED, 10, 100000);
	dev_emerg(dev,
		  "RESTART-PROBE stop: CPU_STATUS %08x -> %08x (%pe), RVBAR %016llx\n",
		  st0, st1, ERR_PTR(ret), rv0);
	if (ret)
		return ret;

	ret = reset_control_assert(ane->cpu_rst);
	if (!ret) {
		fsleep(2);
		ret = reset_control_deassert(ane->cpu_rst);
	}
	rv1 = readq(ane->engine + ANE_ASC_RVBAR);
	st2 = readl(ane->engine + ANE_ASC_CPU_STATUS);
	dev_emerg(dev,
		  "RESTART-PROBE reset %pe: CPU_STATUS %08x, RVBAR %016llx -> %016llx (%s)\n",
		  ERR_PTR(ret), st2, rv0, rv1,
		  rv1 == rv0 ? "latch survives" : "latch LOST");
	return ret ?: count;
}
static DEVICE_ATTR_WO(restart_probe);

static struct attribute *ane_rtclient_attrs[] = {
	&dev_attr_restart_probe.attr,
	NULL,
};

static const struct attribute_group ane_rtclient_group = {
	.attrs = ane_rtclient_attrs,
};

static int ane_rtclient_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct ane_rtclient *ane;
	u32 cpu_status, ps_cpu;
	u64 rvbar;
	bool fw_alive;
	int ep;
	int ret;

	ane = devm_kzalloc(dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = dev;
	platform_set_drvdata(pdev, ane);
	INIT_DELAYED_WORK(&ane->poll_work, ane_rtclient_post_boot);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	/* "nonposted-mmio" (inherited from /soc) sets IORESOURCE_MEM_NONPOSTED;
	 * all ANE aperture access is non-posted (posted writel froze the box). */
	if (!(res->flags & IORESOURCE_MEM_NONPOSTED))
		dev_warn(dev, "engine window is not flagged non-posted; refusing\n");
	/* No exclusive request_mem_region: the ASC mailbox child device
	 * (0x285408000) lives inside this window and its region is
	 * already claimed in the iomem tree, so an exclusive request of
	 * the parent span would conflict with our own child. */
	ane->engine = ioremap_np(res->start, resource_size(res));
	if (!ane->engine)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, ane_rtclient_unmap_engine, ane);
	if (ret)
		return ret;

	ane->cpu_rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(ane->cpu_rst))
		return dev_err_probe(dev, PTR_ERR(ane->cpu_rst),
				     "ane_cpu reset control\n");
	ret = devm_device_add_group(dev, &ane_rtclient_group);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	/* Power: genpd chain (eight islands) via runtime PM. On this
	 * box the always-on islands report off at boot and resume_and_get
	 * hangs the bind writer; when the islands already read on, skip
	 * the raise and go straight to the static sequence. */
	pm_runtime_enable(dev);
	dev_emerg(dev, "BOOT-PHASE genpd raise (eight islands) begin\n");
	if (fw_start_skip_genpd) {
		dev_emerg(dev, "BOOT-PHASE genpd raise SKIPPED (fw_start_skip_genpd=1)\n");
		ret = 0;
	} else {
		ret = pm_runtime_resume_and_get(dev);
	}
	if (ret)
		return dev_err_probe(dev, ret, "genpd raise failed\n");
	dev_emerg(dev, "BOOT-PHASE genpd raise done\n");

	/* G1 gate: ane_cpu ACTUAL must be 0xf before any further MMIO. */
	ane->pmgr = devm_of_iomap(dev, dev->of_node, 1, NULL);
	if (IS_ERR(ane->pmgr)) {
		ret = PTR_ERR(ane->pmgr);
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret,
				     "pmgr window map failed; G1 gate cannot run\n");
	}
	ps_cpu = readl(ane->pmgr + ANE_RTCLIENT_PS_CPU_ACTUAL_OFF);
	dev_emerg(dev, "ane_cpu ACTUAL = 0x%x\n", ps_cpu);
	if (FIELD_GET(ANE_PS_ACTUAL, ps_cpu) != ANE_PS_ON) {
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return -EPROBE_DEFER;
	}

	/* CPU gate. This read is the first engine access and is
	 * read-clean proven (W10) with the islands up. Default
	 * (fw_start=0): refuse unless the firmware is already alive —
	 * kernel-context RVBAR programming is fatal / sticky-latched on
	 * this box (power-dart-fwload s23/s24), start belongs to a
	 * quiesce context. fw_start=1: run the fenced start below. */
	cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
	rvbar = readq(ane->engine + ANE_ASC_RVBAR);
	dev_emerg(dev,
		  "BOOT-PHASE engine reads ok: CPU_STATUS = 0x%x, RVBAR = %016llx (bit0=%u entry=%0llx)\n",
		  cpu_status, rvbar, (u32)(rvbar & 1),
		  (u64)(rvbar & ANE_T6021_RVBAR_ADDR_MASK));

	if (!(cpu_status & ANE_ASC_CPU_STATUS_RUNNING)) {
		if (!fw_start) {
			dev_err(dev,
				"ANE firmware not alive (CPU_STATUS 0x%x) — start it from a quiesce context (m1n1/iBoot), or retry with fw_start=1; this driver will not program RVBAR\n",
				cpu_status);
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EPROBE_DEFER;
		}
		ret = ane_rtclient_fw_start(ane);
		if (ret) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return ret;
		}
		cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
		fw_alive = cpu_status & ANE_ASC_CPU_STATUS_RUNNING ||
			   (ane->fw && ane->fw->fw_alive);
		/* HELD from here on: a CPU we released may be running.
		 * genpd must never be dropped under it — bind fenced
		 * instead of unwinding power on any later failure. */
	} else {
		fw_alive = true;
	}

	if (!fw_alive) {
		/* fw_start ran, CPU released, but no READY: bind fenced
		 * and inert (state HELD, module pinned by the boot
		 * path, genpd stays up, no RTKit). */
		dev_warn(dev,
			 "binding fenced-inert (no firmware; state HELD until reboot)\n");
		return 0;
	}

	ane->rtk = devm_apple_rtkit_init(dev, ane, NULL, 0,
					 &ane_rtclient_rtkit_ops);
	if (IS_ERR(ane->rtk)) {
		ret = PTR_ERR(ane->rtk);
		ane->rtk = NULL;
		ret = dev_err_probe(dev, ret, "apple_rtkit_init failed\n");
		goto err_pm_or_hold;
	}

	/* RX path: the recv irq (ADT raw 0x374) is primary; the worker is
	 * the poll fallback that drives RX while the handshake runs. Armed
	 * before the host ack below so the fw's HELLO is never missed. */
	schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));

	if (ane->fw) {
		/* Kext order after DONE (InitializeRTBuddy 0x95ead04 ->
		 * 0x95eaee4): read the channel table, then host-ack. The
		 * fw spins on SCRATCH3 == 0x08042006 right after DONE
		 * (selene 0x7edc-0x7f10) and only then creates its
		 * endpoints and starts RTKit (HELLO). Without this write
		 * HELLO never comes. SCRATCH is the host-writable family
		 * the boot sequence already writes (W9). */
		ane_rtclient_validate_chman(ane);
		dev_info(dev,
			 "fw transport mode: S1 wrote SCRATCH6=1 -> legacy ChMan/MBI mode (fw 0x42c8: rtbuddyFW = (SCRATCH6 == 0))\n");
		if (ane->fw->booted && scratch3_ack) {
			dev_emerg(dev,
				  "BOOT-PHASE P8 host ack: SCRATCH3 <- %08x\n",
				  ANE_T6021_BOOT_ACK);
			writel(ANE_T6021_BOOT_ACK,
			       ane->engine + ANE_MBI_SCRATCH0 + 4 * 3);
		} else {
			dev_warn(dev,
				 "BOOT-PHASE P8 host ack WITHHELD (booted=%u scratch3_ack=%u): fw parks at 0x7edc, no HELLO expected\n",
				 ane->fw->booted, scratch3_ack);
		}
	}

	/* The handshake: the fw HELLOes on MGMT (v12), rtkit.c answers,
	 * EPMAP + system-endpoint STARTEP + IOP power ack follow, then
	 * boot() sets the AP power state ON. Each rtkit.c wait is 1 s;
	 * the fw reaches HELLO only after its ChMan DONE, so -ETIME is
	 * retried within hello_wait_ms. Anything else is final. */
	dev_emerg(dev, "BOOT-PHASE apple_rtkit_boot begin (bound %u ms)\n",
		  hello_wait_ms);
	{
		unsigned long deadline = jiffies + msecs_to_jiffies(hello_wait_ms);

		do {
			ret = apple_rtkit_boot(ane->rtk);
		} while (ret == -ETIME && time_before(jiffies, deadline));
	}
	if (ret) {
		dev_err(dev, "rtkit boot handshake failed: %pe (is_running=%d crashed=%d)\n",
			ERR_PTR(ret), apple_rtkit_is_running(ane->rtk),
			apple_rtkit_is_crashed(ane->rtk));
		cancel_delayed_work_sync(&ane->poll_work);
		goto err_pm_or_hold;
	}

	ane->boot_done = true;

	/* Endpoint bitmap the firmware advertised via EPMAP. Static
	 * prediction (selene endpoint table, vm 0xed0a0): 0 1 2 0x20. */
	dev_info(ane->dev, "rtkit RUNNING; fw-advertised endpoints:");
	for (ep = 0; ep < 0x100; ep++)
		if (apple_rtkit_has_endpoint(ane->rtk, ep))
			pr_cont(" %#x", ep);
	pr_cont("\n");

	if (start_app_eps)
		ane_rtclient_start_app_eps(ane);

	if (!ane->fw)
		ane_rtclient_validate_chman(ane);

	if (csne_ping)
		ane_rtclient_csne_ping(ane);

	if (!poll_rx)
		cancel_delayed_work_sync(&ane->poll_work);

	dev_info(dev,
		 "ANE RTKit client up: handshake complete (cmd_ep=%#x chman=%s)\n",
		 ane->cmd_ep, ane->chman_ok ? "validated" : "not validated");
	return 0;

err_pm_or_hold:
	if (ane->held) {
		/* A CPU we released is running: NEVER drop genpd under
		 * it. Bind fenced; reboot is the only reclamation. */
		dev_warn(dev,
			 "probe failed after CPU start (%pe) — binding fenced; power/rings/IRQ HELD until reboot\n",
			 ERR_PTR(ret));
		return 0;
	}
	pm_runtime_put_sync_suspend(dev);
	pm_runtime_disable(dev);
	return ret;
}

static void ane_rtclient_remove(struct platform_device *pdev)
{
	struct ane_rtclient *ane = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ane->poll_work);

	if (ane->held) {
		/* Wedged-pin: rmmod is already blocked by the module
		 * pin; unbind reaches this point. Preserve every
		 * surface, ring, IRQ and power-domain link — a running
		 * ASC may be fetching from them. Reboot reclaims. */
		dev_warn(&pdev->dev,
			 "remove HELD (CPU started): no teardown — reboot reclaims\n");
		return;
	}

	if (ane->rtk)
		apple_rtkit_shutdown(ane->rtk);
	if (ane->ring)
		dma_free_coherent(&pdev->dev, ANE_RTCLIENT_RING_SIZE, ane->ring,
				  ane->ring_iova);
	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id ane_rtclient_of_match[] = {
	/* The stock DTB's ane0 node. The legacy H13-path ane_t6021.ko
	 * shares this compatible and must NOT be loaded on this box. */
	{ .compatible = "apple,t6021-ane" },
	{ }
};
MODULE_DEVICE_TABLE(of, ane_rtclient_of_match);

static struct platform_driver ane_rtclient_driver = {
	.driver = {
		.name = "ane_t6021_rtclient",
		.of_match_table = ane_rtclient_of_match,
	},
	.probe = ane_rtclient_probe,
	.remove = ane_rtclient_remove,
};
module_platform_driver(ane_rtclient_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 ANE RTKit client (mainline apple_rtkit)");
