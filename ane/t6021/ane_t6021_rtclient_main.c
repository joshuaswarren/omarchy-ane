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
 *  - genpd/pmgr: the eight ANE islands must read ACTUAL=0xf before
 *    any MMIO (same receipt, gate G1). Runtime PM + the DT
 *    power-domains binding owns the raise; probe also verifies ACTUAL
 *    on ane_cpu (pmgr window) before the first engine read.
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
#include <linux/soc/apple/rtkit.h>
#include <linux/workqueue.h>

#include "ane_t6021.h"

/* pmgr ane_cpu ACTUAL word (ane0 reg1 window, pmgr+0x2e0) */
#define ANE_RTCLIENT_PS_CPU_ACTUAL_OFF	0x2e0

/* CPU_STATUS RUNNING bit (m1n1 ASCRegs shape) */
#define ANE_ASC_CPU_STATUS_RUNNING	BIT(0)

/* rtkit.c routes endpoints below this to its own handlers
 * (rtkit-internal.h APPLE_RTKIT_APP_ENDPOINT_START) */
#define ANE_RTKIT_APP_EP_START		0x20

#define ANE_RTCLIENT_RING_SIZE		SZ_64K	/* csne_ping host ring */

struct ane_rtclient {
	struct device *dev;
	void __iomem *engine;
	void __iomem *pmgr;
	struct apple_rtkit *rtk;

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

static bool fw_start_venc_gates;
module_param(fw_start_venc_gates, bool, 0444);
MODULE_PARM_DESC(fw_start_venc_gates,
		 "fw-start-debug: raise the four ADT ane0 clock-ids (318-321 = VENC_PIPE4/5, VENC_ME0/1) ps gates at 0x290288008/10/18/20 before the boot sequence. M2Research decode: pmgr ps-regs[15] = reg-window2+0x8000, index*8; plain ps TARGET RMW (raise, never 0), poll ACTUAL. The ANE complex sits behind VENC rails on T6021; Linux claims none of them.");

/*
 * Raise the VENC-side pmgr ps gates the ADT wires as ane0 clock-ids.
 * Plain pmgr ps-word raise (TARGET |= 0xf | AUTO_ENABLE, poll ACTUAL
 * [7:4] == 0xf, 100us poll / 10ms bound) — the same op class genpd
 * performs for every DT device; never a TARGET=0 write, so outside
 * the s24 ps-cycle fatal class. Runs BEFORE any engine write (kext
 * order: provider clock/power first). Read-logged before/after.
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
		if ((before & 0xf0) != 0xf0) {
			writel((before | 0xf | BIT(28)) & ~(u32)BIT(31),
			       root + 0x3e0);
			ret = readl_poll_timeout(root + 0x3e0, after,
						 ((after & 0xf0) == 0xf0),
						 100, 10 * 1000);
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

	/* Raise bottom-up: only the five REAL ps words (B5b scan: +000
	 * .. +020 carry the 0x300 idle signature; +028..+038 read 0 and
	 * are not ps words — raising them would time out and refuse the
	 * sequence spuriously, as B6 showed). Never a TARGET=0 write. */
	for (i = 0; i < 5; i++) {
		void __iomem *reg = base + i * 8;
		u32 before = readl(reg);
		u32 after;
		int ret;

		if ((before & 0xf0) == 0xf0)
			continue;
		dev_emerg(dev, "VENC-GATES +%03x before=%08x\n", i * 8,
			  before);
		writel((before | 0xf | BIT(28)) & ~(u32)BIT(31), reg);
		ret = readl_poll_timeout(reg, after,
					 ((after & 0xf0) == 0xf0),
					 100, 10 * 1000);
		after = readl(reg);
		dev_emerg(dev, "VENC-GATES +%03x after=%08x ret=%pe\n",
			  i * 8, after, ERR_PTR(ret));
		if (ret)
			ret_all = ret;
	}

	for (i = 0; i < 4; i++) {
		u32 v = readl(base + gates[i].off);

		dev_emerg(dev, "VENC-GATES %u %s final=%08x\n",
			  gates[i].id, gates[i].name, v);
		if ((v & 0xf0) != 0xf0)
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

	ret = ane_t6021_boot_start(a, fw_start_stop_after, fw_start_table_mode);
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
	if (!IS_ERR_OR_NULL(ane->pmgr)) {
		ps_cpu = readl(ane->pmgr + ANE_RTCLIENT_PS_CPU_ACTUAL_OFF);
		dev_emerg(dev, "ane_cpu ACTUAL = 0x%x\n", ps_cpu);
		if ((ps_cpu & 0xf) != 0xf) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EPROBE_DEFER;
		}
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
