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
 *    RVBAR and ps@2e0 writes are fatal. This driver therefore NEVER
 *    programs RVBAR or CPU_CONTROL: it refuses to bind unless the
 *    firmware is already alive (CPU_STATUS RUNNING).
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
 * CSNE_CMD layout evidence (static, kext 26A428 + selene):
 *  - header {u32 rsvd, u16 id, u8 flags, u8 rsvd} == 8 B; ids from the
 *    selene id->name table at vaddr 0xea430 (BOOT 0x10, PING 0x11,
 *    BUILDINFO 0x06, PROCEDURE_CALL 0x204, INFERENCE_CALL 0x404).
 *  - app endpoints 1..6 = INIT/T2FC/T2FH/T2HS/T2HC/T2HT (K14 cfg
 *    table __const+0x814e520); host->fw commands ride EP1 INIT.
 *  - SetupEndpoints doorbell word: offset[43:0] | size_code[51:44] |
 *    unit[53:52]; per-command word: cursor[23:0] | len[47:24]
 *    (HandleRTBuddyMessage + rtbuddyEndpointSendMessage agree).
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

#define ANE_RTCLIENT_RING_SIZE		SZ_64K	/* INIT ring, W2 cfg table */
#define ANE_RTCLIENT_CSNE_CMD_MAX	0xffffff

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

	dma_addr_t ring_iova;
	void *ring;

	bool csne_setup_done;
};

static bool csne_ping;
module_param(csne_ping, bool, 0444);
MODULE_PARM_DESC(csne_ping,
		 "After a completed RTKit handshake, announce the INIT ring and send CSNE_CMD_PING (0x11)");

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
	unsigned int i;

	base = ioremap_np(0x290288000ull, 0x40);
	if (!base) {
		dev_emerg(dev, "VENC-GATES: ioremap FAILED\n");
		return -ENOMEM;
	}

	for (i = 0; i < 4; i++) {
		void __iomem *reg = base + gates[i].off;
		u32 before = readl(reg);
		u32 after;
		int ret;

		dev_emerg(dev, "VENC-GATES %u %s @%#x: before=%08x\n",
			  gates[i].id, gates[i].name, gates[i].off,
			  before);
		if ((before & 0xf0) == 0xf0) {
			dev_emerg(dev, "VENC-GATES %u already on\n",
				  gates[i].id);
			continue;
		}
		writel((before | 0xf | BIT(28)) & ~(u32)BIT(31), reg);
		ret = readl_poll_timeout(reg, after,
					 ((after & 0xf0) == 0xf0),
					 100, 10 * 1000);
		after = readl(reg);
		dev_emerg(dev,
			  "VENC-GATES %u after=%08x ret=%pe\n",
			  gates[i].id, after, ERR_PTR(ret));
		if (ret) {
			iounmap(base);
			return -ETIMEDOUT;
		}
	}

	iounmap(base);
	return 0;
}

static bool poll_rx;
module_param(poll_rx, bool, 0444);
MODULE_PARM_DESC(poll_rx,
		 "Drive RX by apple_rtkit_poll from a workqueue even though a recv IRQ exists (fallback if raw 0x374 is not the recv line)");

/* ---- doorbell words (W2 decode) ----
 * ANE_EP_DOORBELL_* and ANE_MBI_MSG48_* come from ane_t6021.h. This
 * local encoder is the 44-bit-offset variant (the INIT ring IOVA lives
 * above 4 GiB in the dart-ane0 window; the header's u32-offset encoder
 * is the legacy MBI doorbell one). */

static inline u64 ep_doorbell_encode(u64 offset, u32 size)
{
	u64 unit = (size >= SZ_1M) ? 2 : 1;
	u32 code = DIV_ROUND_UP(size, 1u << (unit * 12));

	return (offset & ANE_EP_DOORBELL_OFFSET) |
	       FIELD_PREP(ANE_EP_DOORBELL_SIZE, code) |
	       FIELD_PREP(ANE_EP_DOORBELL_UNIT, unit);
}

/* ---- RTKit callbacks ---- */

static void ane_rtclient_recv(void *cookie, u8 ep, u64 message)
{
	struct ane_rtclient *ane = cookie;

	/* App endpoints: SetupEndpoints acks and fw->host command
	 * delivery (EP2/EP3 T2FC/T2FH). Log raw; decode on sight. */
	dev_info(ane->dev, "rtkit app msg: ep=%u msg=%016llx (offset=%#llx size_code=%#llx unit=%llu)\n",
		 ep, message,
		 message & ANE_EP_DOORBELL_OFFSET,
		 (u64)FIELD_GET(ANE_EP_DOORBELL_SIZE, message),
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

static const struct apple_rtkit_ops ane_rtclient_rtkit_ops = {
	.crashed = ane_rtclient_crashed,
	.recv_message = ane_rtclient_recv,
};

/* ---- CSNE_CMD constants (selene id table vaddr 0xea430) ---- */

#define CSNE_CMD_PING	0x11

static void ane_rtclient_csne_ping(struct ane_rtclient *ane);

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

/* Submit one header-only CSNE control command at a fixed ring slot.
 * Both ids are proven generation-stable and header-only per the
 * selene decode (BOOT 0x10 / PING 0x11 / BUILDINFO 0x06 carry no
 * payload beyond the 8-byte header). Informational only: the ring
 * doorbell word bit placement is still the W2-vs-H14RpcProtocol
 * conflict, so this path stays behind csne_ping and logs everything
 * it sends; responses arrive on the T2F* endpoints (logged raw in
 * ane_rtclient_recv). */
static void ane_rtclient_csne_cmd(struct ane_rtclient *ane, u16 id,
				  u32 cursor)
{
	struct ane_csne_hdr hdr;
	u64 msg;
	int ret;

	ane_csne_hdr_init(&hdr, id);
	memcpy(ane->ring + cursor, &hdr, sizeof(hdr));
	dma_wmb();

	msg = FIELD_PREP(ANE_MBI_MSG48_OFF, cursor) |
	      FIELD_PREP(ANE_MBI_MSG48_LEN, sizeof(hdr));
	ret = apple_rtkit_send_message(ane->rtk, ANE_T6021_EP_INIT, msg,
				       NULL, false);
	dev_info(ane->dev,
		 "csne: CSNE_CMD_%#x submit ep=%u cursor=%u len=%zu -> %pe\n",
		 id, ANE_T6021_EP_INIT, cursor, sizeof(hdr), ERR_PTR(ret));
}

static void ane_rtclient_csne_ping(struct ane_rtclient *ane)
{
	int ret;

	if (!apple_rtkit_has_endpoint(ane->rtk, ANE_T6021_EP_INIT)) {
		dev_info(ane->dev, "csne: fw did not announce EP1; no ping\n");
		return;
	}

	if (!ane->csne_setup_done) {
		/* SetupEndpoints: announce the INIT ring surface as the
		 * EP1 doorbell word, then STARTEP EP1. [The offset
		 * semantics (absolute dart IOVA vs fw-pool-relative) are
		 * [INFERENCE]; this is the informational first attempt.] */
		ret = apple_rtkit_send_message(ane->rtk, ANE_T6021_EP_INIT,
					       ep_doorbell_encode(ane->ring_iova,
								  ANE_RTCLIENT_RING_SIZE),
					       NULL, false);
		if (ret) {
			dev_err(ane->dev, "csne: SETUP doorbell send failed: %pe\n",
				ERR_PTR(ret));
			return;
		}
		ret = apple_rtkit_start_ep(ane->rtk, ANE_T6021_EP_INIT);
		if (ret) {
			dev_err(ane->dev, "csne: STARTEP(EP1) failed: %pe\n",
				ERR_PTR(ret));
			return;
		}
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

	/* Power: genpd chain (eight islands) via runtime PM. */
	pm_runtime_enable(dev);
	dev_emerg(dev, "BOOT-PHASE genpd raise (eight islands) begin\n");
	ret = pm_runtime_resume_and_get(dev);
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

	ane->ring = dmam_alloc_coherent(dev, ANE_RTCLIENT_RING_SIZE,
					&ane->ring_iova, GFP_KERNEL);
	if (!ane->ring) {
		ret = -ENOMEM;
		goto err_pm_or_hold;
	}
	dev_info(dev, "INIT ring: iova=%pad size=0x%x\n",
		 &ane->ring_iova, ANE_RTCLIENT_RING_SIZE);

	/* RX path: the recv irq (ADT raw 0x374) is primary; the worker is
	 * the poll fallback that drives RX while the handshake runs. */
	schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));

	/* The handshake itself: the fw HELLOes first on MGMT (W2), rtkit
	 * answers, EPMAP + STARTEP + SET_IOP_PWR_STATE follow, then boot()
	 * sets the AP power state ON and returns. Bounded: each rtkit
	 * completion wait is 1 s (APPLE_RTKIT_TIMEOUT in rtkit.c). */
	dev_emerg(dev, "BOOT-PHASE apple_rtkit_boot begin\n");
	ret = apple_rtkit_boot(ane->rtk);
	if (ret) {
		dev_err(dev, "rtkit boot handshake failed: %pe (is_running=%d crashed=%d)\n",
			ERR_PTR(ret), apple_rtkit_is_running(ane->rtk),
			apple_rtkit_is_crashed(ane->rtk));
		cancel_delayed_work_sync(&ane->poll_work);
		goto err_pm_or_hold;
	}

	ane->boot_done = true;

	/* Endpoint bitmap the firmware actually advertised via EPMAP —
	 * this log is the hardware answer to the open "which endpoints
	 * carry CSNE_CMD channels" item. Capture verbatim into the
	 * receipt. */
	dev_info(ane->dev, "rtkit RUNNING; fw-advertised endpoints:");
	for (ep = 0; ep < 64; ep++)
		if (apple_rtkit_has_endpoint(ane->rtk, ep))
			pr_cont(" %d", ep);
	pr_cont("\n");

	if (csne_ping)
		ane_rtclient_csne_ping(ane);

	if (!poll_rx)
		cancel_delayed_work_sync(&ane->poll_work);

	dev_info(dev, "ANE RTKit client up: handshake complete\n");
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
