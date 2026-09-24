// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h13_perf.c — H13 (T8103/T6001) ANE firmware perf-mode client.
 *
 * Sends CSNE_CMD_CH_PROPERTY_WRITE(prop 0x10aa, value 1) — the
 * "setting FW perf mode" message AppleH11ANEInterface issues at
 * power-on — to the pre-loaded ANE firmware over an RTBuddy-style
 * RTKit session brought up on the ANE ASC mailbox. The firmware owns
 * the ANE clock; in perf mode it raises it through its own power
 * firmware link (receipt 2026-09-22-ane-dvfs: Linux programs no ANE
 * clock; macOS sustains 140 ms whole-encoder vs Linux 440).
 *
 * Message evidence (H13 kext 9.512.0-macstudio-25G83, static):
 *   - command id 0x1f built at __TEXT_EXEC 0xfffffe0009322e14
 *     (mov w20,#0x1f; strh), payload pool __TEXT.__const
 *     0xfffffe000748fdc8 = {channel 0, property 0x10aa}, value 1 at
 *     +0x10, error string "...CH_PROPERTY_WRITE for setting FW perf
 *     mode failed" xref 0xfffffe0009322ec8. One send site in the
 *     binary (init path, alongside cpuLoadScore watermark writes
 *     prop 0x1803/0x1804 and fw-log prop 0xa1).
 *   - 20-byte buffer: {u32 0; u16 id; u16 flags; u32 channel;
 *     u32 property; u32 value} (w2=0x14 send length; the stack word
 *     at +0x14 the kext also fills is the by-reference length, x3).
 *   - transport: RTBuddy RPC (the kext carries no TM-queue or ASC
 *     mailbox constants; m1n1's TM path is not the kext's). Endpoint
 *     numbers default to the K14 cfg mapping (EP1 INIT, EP2 T2F_CMD,
 *     ... EP6 T2H_TERM); the firmware's EPMAP answer is the runtime
 *     authority and is logged verbatim.
 *
 * Safety posture (jw16 is a serving box until handed over):
 *   - never binds the ane platform node (the legacy ane.ko owns it);
 *     it only looks the probed device up and refuses if absent.
 *   - all engine MMIO non-posted (ioremap_np), reads before writes,
 *     every wait bounded.
 *   - perf_mode=0 (default): handshake + endpoint bitmap capture +
 *     logs only. No CSNE command is sent.
 *   - no RTKit power-state writes in either mode: the firmware is
 *     already ON (ADT "pre-loaded"=1, legacy submits running); IOP/AP
 *     power management stays with the production stack. Module exit
 *     sends nothing (no quiesce, no reset).
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/device/bus.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>

/* ---- engine aperture registers (m1n1 ane.py / t6021 rtclient) ---- */

#define ANE_H13_CPU_STATUS		0x1400048
#define ANE_H13_CPU_STATUS_RUNNING	BIT(0)

static char *pdev_name;
module_param(pdev_name, charp, 0444);
MODULE_PARM_DESC(pdev_name,
		 "optional: platform device name of the legacy-bound ane node (auto-discovered by driver 'ane' when unset; e.g. 285c04000.ane on t6001 boots)");

static unsigned long mb_off = 0x1408000;
module_param(mb_off, ulong, 0444);
MODULE_PARM_DESC(mb_off,
		 "ASC mailbox block offset inside the engine aperture (h14-verified layout; runtime-confirmed by the HELLO/HELLO_REPLY exchange)");

static unsigned long long aperture;
module_param(aperture, ullong, 0444);
MODULE_PARM_DESC(aperture,
		 "optional: ane aperture physical base (default: legacy reg window base 32 MiB-aligned down; t6001: 0x285c04000 -> 0x284000000)");

static unsigned long aperture_size = 0x2000000;
module_param(aperture_size, ulong, 0444);
MODULE_PARM_DESC(aperture_size,
		 "ane aperture mapping size (default 32 MiB, the ADT ane0 range0 length)");

static uint t2fc_ep = 2;
module_param(t2fc_ep, uint, 0444);
MODULE_PARM_DESC(t2fc_ep,
		 "T2F_CMD app endpoint number (K14 cfg table default 2; EPMAP bitmap logged at bind)");

static bool perf_mode;
module_param(perf_mode, bool, 0444);
MODULE_PARM_DESC(perf_mode,
		 "After a completed handshake, send CSNE_CMD_CH_PROPERTY_WRITE(prop 0x10aa, value 1) - the FW perf-mode enable macOS sends at power-on");

/* ---- ASC mailbox (soc/apple/mailbox.c ASC variant) ---- */

#define ASC_A2I_CONTROL		0x110
#define ASC_A2I_SEND0		0x800
#define ASC_A2I_SEND1		0x808
#define ASC_I2A_CONTROL		0x114
#define ASC_I2A_RECV0		0x830
#define ASC_I2A_RECV1		0x838
#define ASC_CTRL_FULL		BIT(16)
#define ASC_CTRL_EMPTY		BIT(17)
#define MSG1_EP			GENMASK_ULL(31, 0)

/* ---- RTKit MGMT (drivers/soc/apple/rtkit.c shapes) ---- */

#define MGMT_TYPE		GENMASK_ULL(59, 52)
#define MGMT_HELLO		1
#define MGMT_HELLO_REPLY	2
#define MGMT_STARTEP		5
#define MGMT_EPMAP		8
#define HELLO_MINVER		GENMASK_ULL(15, 0)
#define HELLO_MAXVER		GENMASK_ULL(31, 16)
#define EPMAP_LAST		BIT_ULL(51)
#define EPMAP_BASE		GENMASK_ULL(34, 32)
#define EPMAP_BITMAP		GENMASK_ULL(31, 0)
#define EPMAP_REPLY_MORE	BIT_ULL(0)
#define STARTEP_EP		GENMASK_ULL(39, 32)
#define STARTEP_FLAG		BIT_ULL(1)
#define RTKIT_VER_MIN		11
#define RTKIT_VER_MAX		12

/* ---- CSNE command (kext-derived, see header comment) ---- */

#define CSNE_CMD_CH_PROPERTY_WRITE	0x001f
#define CSNE_PROP_FW_PERF_MODE		0x10aa
#define CSNE_PERF_MODE_ON		1
#define CSNE_PROPBUF_LEN		0x14

/* doorbell words (t6021 rtclient / HandleRTBuddyMessage decode) */
#define DB_OFFSET		GENMASK_ULL(43, 0)
#define DB_SIZE			GENMASK_ULL(51, 44)
#define DB_UNIT			GENMASK_ULL(53, 52)
#define CMDW_OFF		GENMASK_ULL(23, 0)
#define CMDW_LEN		GENMASK_ULL(47, 24)

#define RING_SIZE		SZ_64K
#define RX_POLL_US		2000
#define HANDSHAKE_MS		5000
#define REPLY_MS		5000

static struct ane_h13_perf {
	struct platform_device *pdev;
	void __iomem *engine;
	void __iomem *mb;
	void *ring;
	dma_addr_t ring_iova;
	DECLARE_BITMAP(endpoints, 64);
	bool hello_done;
	bool epmap_last;
} *g;

static void mgmt_handle(struct ane_h13_perf *a, u8 ep, u64 msg);

/* ---- raw mailbox ops (poll mode, no IRQs) ---- */

static int mb_send(struct ane_h13_perf *a, u8 ep, u64 msg)
{
	u32 ctrl;
	int ret;

	ret = readl_poll_timeout_atomic(a->mb + ASC_A2I_CONTROL, ctrl,
					!(ctrl & ASC_CTRL_FULL), 100,
					2000000);
	if (ret)
		return ret;

	dma_wmb();
	writeq_relaxed(msg, a->mb + ASC_A2I_SEND0);
	writeq_relaxed(FIELD_PREP(MSG1_EP, ep), a->mb + ASC_A2I_SEND1);
	return 0;
}

/* drain everything pending; MGMT messages are serviced, the rest logged */
static void mb_pump(struct ane_h13_perf *a)
{
	struct device *dev = &a->pdev->dev;
	u32 ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
	int n = 0;

	while (!(ctrl & ASC_CTRL_EMPTY) && n < 64) {
		u64 msg0 = readq_relaxed(a->mb + ASC_I2A_RECV0);
		u64 msg1 = readq_relaxed(a->mb + ASC_I2A_RECV1);
		u8 ep = FIELD_GET(MSG1_EP, msg1);

		if (ep == 0)
			mgmt_handle(a, ep, msg0);
		else
			dev_info(dev, "rx: ep=%u msg=%016llx\n", ep, msg0);
		n++;
		ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
	}
}

/* ---- MGMT ---- */

static int mgmt_send(struct ane_h13_perf *a, u8 type, u64 payload)
{
	return mb_send(a, 0, FIELD_PREP(MGMT_TYPE, type) | payload);
}

static void mgmt_handle(struct ane_h13_perf *a, u8 ep, u64 msg)
{
	struct device *dev = &a->pdev->dev;
	u8 type = FIELD_GET(MGMT_TYPE, msg);

	if (ep != 0)
		return;

	switch (type) {
	case MGMT_HELLO: {
		u32 vmin = FIELD_GET(HELLO_MINVER, msg);
		u32 vmax = FIELD_GET(HELLO_MAXVER, msg);
		u32 want = min((u32)RTKIT_VER_MAX, vmax);

		dev_info(dev, "mgmt: fw HELLO ver [%u,%u]\n", vmin, vmax);
		if (vmin > RTKIT_VER_MAX || vmax < RTKIT_VER_MIN) {
			dev_err(dev, "mgmt: version window unsupported\n");
			return;
		}
		mgmt_send(a, MGMT_HELLO_REPLY,
			  FIELD_PREP(HELLO_MINVER, want) |
			  FIELD_PREP(HELLO_MAXVER, want));
		a->hello_done = true;
		break;
	}
	case MGMT_HELLO_REPLY:
		dev_info(dev, "mgmt: HELLO_REPLY\n");
		a->hello_done = true;
		break;
	case MGMT_EPMAP: {
		u32 base = FIELD_GET(EPMAP_BASE, msg);
		unsigned long bmp = FIELD_GET(EPMAP_BITMAP, msg);
		int i;

		for_each_set_bit(i, &bmp, 32) {
			int epn = 32 * base + i;

			if (epn < 64)
				set_bit(epn, a->endpoints);
		}
		dev_info(dev, "mgmt: EPMAP base=%u bitmap=%08lx\n", base, bmp);
		mgmt_send(a, MGMT_EPMAP,
			  FIELD_PREP(EPMAP_BASE, base) |
			  ((msg & EPMAP_LAST) ? EPMAP_LAST : EPMAP_REPLY_MORE));
		if (msg & EPMAP_LAST) {
			a->epmap_last = true;
			dev_info(dev, "mgmt: fw-advertised endpoints:");
			for (i = 0; i < 64; i++)
				if (test_bit(i, a->endpoints))
					pr_cont(" %d", i);
			pr_cont("\n");
		}
		break;
	}
	default:
		dev_info(dev, "mgmt: unhandled type %u msg=%016llx\n",
			 type, msg);
	}
}

static int handshake(struct ane_h13_perf *a)
{
	struct device *dev = &a->pdev->dev;
	unsigned long t0 = jiffies;
	int ret;

	/* Host-initiated HELLO: the firmware is already running (legacy
	 * submits execute), so it may not open the conversation itself. */
	ret = mgmt_send(a, MGMT_HELLO,
			FIELD_PREP(HELLO_MINVER, (u32)RTKIT_VER_MIN) |
			FIELD_PREP(HELLO_MAXVER, (u32)RTKIT_VER_MAX));
	if (ret) {
		dev_err(dev, "mailbox A2I stuck FULL: %pe\n", ERR_PTR(ret));
		return ret;
	}

	while (!(a->hello_done && a->epmap_last)) {
		mb_pump(a);

		if (a->hello_done && a->epmap_last)
			break;
		if (time_after(jiffies, t0 + msecs_to_jiffies(HANDSHAKE_MS))) {
			dev_err(dev, "handshake timed out: hello=%d epmap_last=%d (mailbox silent? wrong mb_off?)\n",
				a->hello_done, a->epmap_last);
			return -ETIMEDOUT;
		}
		usleep_range(RX_POLL_US, RX_POLL_US + 500);
	}
	return 0;
}

/* ---- CSNE command send (T2F_CMD ring) ---- */

static u64 doorbell_encode(u64 offset, u32 size)
{
	u64 unit = (size >= SZ_1M) ? 2 : 1;
	u32 code = DIV_ROUND_UP(size, 1u << (unit * 12));

	return (offset & DB_OFFSET) |
	       FIELD_PREP(DB_SIZE, code) |
	       FIELD_PREP(DB_UNIT, unit);
}

static int send_perf_mode(struct ane_h13_perf *a)
{
	struct device *dev = &a->pdev->dev;
	u8 buf[CSNE_PROPBUF_LEN] = { 0 };
	unsigned long t0 = jiffies;
	u64 msg;
	int ret;

	if (!test_bit(t2fc_ep, a->endpoints)) {
		dev_err(dev, "T2F_CMD ep %u not advertised by fw; read the EPMAP log and set t2fc_ep\n",
			t2fc_ep);
		return -EINVAL;
	}

	/* SetupEndpoints: announce the ring surface, STARTEP, settle. */
	ret = mb_send(a, t2fc_ep, doorbell_encode(a->ring_iova, RING_SIZE));
	if (ret)
		return ret;
	mgmt_send(a, MGMT_STARTEP,
		  FIELD_PREP(STARTEP_EP, t2fc_ep) | STARTEP_FLAG);
	msleep(50);
	mb_pump(a);

	/* CSNE_CMD_CH_PROPERTY_WRITE: {0, 0x1f, 0, ch 0, prop 0x10aa,
	 * value 1}, 0x14 bytes — the exact H13 kext payload. */
	put_unaligned_le16(CSNE_CMD_CH_PROPERTY_WRITE, buf + 0x04);
	put_unaligned_le32(0, buf + 0x08);			/* channel */
	put_unaligned_le32(CSNE_PROP_FW_PERF_MODE, buf + 0x0c);
	put_unaligned_le32(CSNE_PERF_MODE_ON, buf + 0x10);

	memcpy(a->ring, buf, CSNE_PROPBUF_LEN);
	dma_wmb();

	msg = FIELD_PREP(CMDW_OFF, 0) |
	      FIELD_PREP(CMDW_LEN, CSNE_PROPBUF_LEN);
	ret = mb_send(a, t2fc_ep, msg);
	dev_info(dev, "csne: CH_PROPERTY_WRITE(prop 0x%x=1) sent ep=%u -> %pe; awaiting reply\n",
		 CSNE_PROP_FW_PERF_MODE, t2fc_ep, ERR_PTR(ret));
	if (ret)
		return ret;

	while (time_before(jiffies, t0 + msecs_to_jiffies(REPLY_MS))) {
		u32 ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
		int n = 0;

		while (!(ctrl & ASC_CTRL_EMPTY) && n < 64) {
			u64 msg0 = readq_relaxed(a->mb + ASC_I2A_RECV0);
			u64 msg1 = readq_relaxed(a->mb + ASC_I2A_RECV1);
			u8 ep = FIELD_GET(MSG1_EP, msg1);

			if (ep == 0)
				mgmt_handle(a, ep, msg0);
			else
				dev_info(dev, "rx: ep=%u msg=%016llx\n",
					 ep, msg0);
			n++;
			ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
		}
		if (n)
			return 0;
		usleep_range(RX_POLL_US, RX_POLL_US + 500);
	}
	dev_warn(dev, "no reply to CH_PROPERTY_WRITE within %d ms (read the rx log)\n",
		 REPLY_MS);
	return -ETIMEDOUT;
}

/* ---- module plumbing (single instance, manual lifetime) ---- */

static void ane_h13_perf_cleanup(void)
{
	struct ane_h13_perf *a = g;

	if (!a)
		return;
	g = NULL;
	if (a->ring)
		dma_free_coherent(&a->pdev->dev, RING_SIZE, a->ring,
				  a->ring_iova);
	if (a->engine)
		iounmap(a->engine);
	put_device(&a->pdev->dev);
	kfree(a);
}

static int match_owned(struct device *dev, const void *data)
{
	struct device_driver *drv = dev->driver;

	if (!pdev_name)
		return dev_is_platform(dev) && drv &&
		       !strcmp(drv->name, "ane");
	return dev_is_platform(dev) &&
	       !strcmp(dev_name(dev), (const char *)data);
}

static int __init ane_h13_perf_init(void)
{
	struct device *found;
	struct resource *res;
	u64 aperture_base;
	u32 cpu_status;
	int ret;

	/* owned-by-production gate: the legacy ane driver must have
	 * claimed the node first, so this module can never steal it. */
	found = bus_find_device(&platform_bus_type, NULL, pdev_name,
				match_owned);
	if (!found) {
		pr_err("ane_h13_perf: no legacy-bound ane platform device found (pdev_name='%s', driver gate 'ane')\n",
		       pdev_name ? pdev_name : "<auto>");
		return -ENODEV;
	}

	g = kzalloc(sizeof(*g), GFP_KERNEL);
	if (!g) {
		put_device(found);
		return -ENOMEM;
	}
	g->pdev = to_platform_device(found);
	pr_info("ane_h13_perf: found %s (driver %s)\n", dev_name(found),
		found->driver ? found->driver->name : "?");

	res = platform_get_resource(g->pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("ane_h13_perf: %s has no IORESOURCE_MEM[0]\n",
		       dev_name(&g->pdev->dev));
		ret = -ENODEV;
		goto err;
	}
	pr_info("ane_h13_perf: legacy reg window %pr (nonposted=%d)\n", res,
		!(res->flags & IORESOURCE_MEM_NONPOSTED) ? 0 : 1);

	/* The legacy node's window is a sub-window of the ane aperture
	 * (t6001 boots: reg 0x285c04000 inside range0 0x284000000..32M).
	 * The mailbox and CPU_STATUS live at aperture-relative offsets,
	 * so map the whole aperture: param override, else the reg window
	 * base aligned down to the 32 MiB aperture granularity. */
	aperture_base = aperture ? aperture :
				   (res->start & ~(u64)(aperture_size - 1));
	pr_info("ane_h13_perf: aperture %#llx+%#lx (param aperture=%llx)\n",
		aperture_base, aperture_size, aperture);
	g->engine = ioremap_np(aperture_base, aperture_size);
	if (!g->engine) {
		pr_err("ane_h13_perf: ioremap_np(aperture %#llx+%#lx) failed\n",
		       aperture_base, aperture_size);
		ret = -ENOMEM;
		goto err;
	}
	pr_info("ane_h13_perf: aperture mapped ok (reading CPU_STATUS at ap+%#x)\n",
		ANE_H13_CPU_STATUS);

	cpu_status = readl_relaxed(g->engine + ANE_H13_CPU_STATUS);
	pr_info("ane_h13_perf: CPU_STATUS raw read done = 0x%x\n", cpu_status);
	dev_info(&g->pdev->dev, "CPU_STATUS @aperture+0x%x = 0x%x (running=%d)\n",
		 ANE_H13_CPU_STATUS, cpu_status,
		 !!(cpu_status & ANE_H13_CPU_STATUS_RUNNING));
	if (!(cpu_status & ANE_H13_CPU_STATUS_RUNNING)) {
		dev_err(&g->pdev->dev, "ANE firmware not alive; refusing\n");
		ret = -ENODEV;
		goto err;
	}

	g->mb = g->engine + mb_off;
	dev_info(&g->pdev->dev, "ASC mailbox @aperture+0x%lx: a2i=%08x i2a=%08x\n",
		 mb_off,
		 readl_relaxed(g->mb + ASC_A2I_CONTROL),
		 readl_relaxed(g->mb + ASC_I2A_CONTROL));

	g->ring = dma_alloc_coherent(&g->pdev->dev, RING_SIZE,
				     &g->ring_iova, GFP_KERNEL);
	if (!g->ring) {
		dev_err(&g->pdev->dev, "coherent ring alloc failed (device dma config)\n");
		ret = -ENOMEM;
		goto err;
	}
	dev_info(&g->pdev->dev, "ring: iova=%pad size=0x%x\n",
		 &g->ring_iova, RING_SIZE);

	ret = handshake(g);
	if (ret)
		goto err;

	dev_info(&g->pdev->dev, "RTKit session up (no power-state writes; fw already ON)\n");

	if (perf_mode)
		ret = send_perf_mode(g);
	else
		dev_info(&g->pdev->dev, "perf_mode=0: handshake capture only, no CSNE command sent\n");
	if (ret)
		goto err;
	return 0;

err:
	ane_h13_perf_cleanup();
	return ret;
}

static void __exit ane_h13_perf_exit(void)
{
	ane_h13_perf_cleanup();
}

module_init(ane_h13_perf_init);
module_exit(ane_h13_perf_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("H13 ANE firmware perf-mode client (CSNE_CMD_CH_PROPERTY_WRITE 0x10aa)");
