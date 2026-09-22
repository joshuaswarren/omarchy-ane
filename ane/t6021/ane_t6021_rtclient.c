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

static bool poll_rx;
module_param(poll_rx, bool, 0444);
MODULE_PARM_DESC(poll_rx,
		 "Drive RX by apple_rtkit_poll from a workqueue even though a recv IRQ exists (fallback if raw 0x374 is not the recv line)");

/* ---- doorbell words (W2 decode, mirrored from ane_t6021.c) ---- */

#define ANE_EP_DOORBELL_OFFSET	GENMASK_ULL(43, 0)
#define ANE_EP_DOORBELL_SIZE	GENMASK_ULL(51, 44)
#define ANE_EP_DOORBELL_UNIT	GENMASK_ULL(53, 52)

#define ANE_MBI_MSG48_OFF	GENMASK_ULL(23, 0)
#define ANE_MBI_MSG48_LEN	GENMASK_ULL(47, 24)

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

static void ane_rtclient_csne_ping(struct ane_rtclient *ane)
{
	struct ane_csne_hdr hdr;
	u32 cursor = 0;
	u64 msg;
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

	ane_csne_hdr_init(&hdr, CSNE_CMD_PING);
	memcpy(ane->ring + cursor, &hdr, sizeof(hdr));
	dma_wmb();

	msg = FIELD_PREP(ANE_MBI_MSG48_OFF, cursor) |
	      FIELD_PREP(ANE_MBI_MSG48_LEN, sizeof(hdr));
	ret = apple_rtkit_send_message(ane->rtk, ANE_T6021_EP_INIT, msg,
				       NULL, false);
	dev_info(ane->dev, "csne: PING submit ep=1 cursor=%u len=%zu -> %pe\n",
		 cursor, sizeof(hdr), ERR_PTR(ret));
}

/* ---- probe ---- */

static int ane_rtclient_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct ane_rtclient *ane;
	u32 cpu_status, ps_cpu;
	int ep;
	int ret;

	ane = devm_kzalloc(dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = dev;
	INIT_DELAYED_WORK(&ane->poll_work, ane_rtclient_post_boot);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	/* "nonposted-mmio" in the DT node sets IORESOURCE_MEM_NONPOSTED;
	 * devm_ioremap_resource then maps with ioremap_np. All ANE
	 * aperture access is non-posted (posted writel froze the box). */
	if (!(res->flags & IORESOURCE_MEM_NONPOSTED))
		dev_warn(dev, "engine window is not flagged non-posted; refusing\n");
	ane->engine = devm_ioremap_resource(dev, res);
	if (IS_ERR(ane->engine))
		return PTR_ERR(ane->engine);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	/* Power: genpd chain (eight islands) via runtime PM. */
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "genpd raise failed\n");

	/* G1 gate: ane_cpu ACTUAL must be 0xf before any further MMIO. */
	ane->pmgr = devm_of_iomap(dev, dev->of_node, 1, NULL);
	if (!IS_ERR_OR_NULL(ane->pmgr)) {
		ps_cpu = readl(ane->pmgr + ANE_RTCLIENT_PS_CPU_ACTUAL_OFF);
		dev_info(dev, "ane_cpu ACTUAL = 0x%x\n", ps_cpu);
		if ((ps_cpu & 0xf) != 0xf) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EPROBE_DEFER;
		}
	}

	/* The CPU must already run: kernel-context RVBAR programming is
	 * fatal / sticky-latched on this box (power-dart-fwload s23/s24).
	 * This read is the first engine access and is read-clean proven
	 * (W10) with the islands up. */
	cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
	dev_info(dev, "CPU_STATUS = 0x%x\n", cpu_status);
	if (!(cpu_status & ANE_ASC_CPU_STATUS_RUNNING)) {
		dev_err(dev, "ANE firmware not alive (CPU_STATUS 0x%x) — start it from a quiesce context (m1n1/iBoot); this driver will not program RVBAR\n",
			cpu_status);
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return -EPROBE_DEFER;
	}

	ane->rtk = devm_apple_rtkit_init(dev, ane, NULL, 0,
					 &ane_rtclient_rtkit_ops);
	if (IS_ERR(ane->rtk)) {
		ret = dev_err_probe(dev, PTR_ERR(ane->rtk),
				    "apple_rtkit_init failed\n");
		goto err_pm;
	}

	ane->ring = dmam_alloc_coherent(dev, ANE_RTCLIENT_RING_SIZE,
					&ane->ring_iova, GFP_KERNEL);
	if (!ane->ring) {
		ret = -ENOMEM;
		goto err_pm;
	}
	dev_info(dev, "INIT ring: iova=%pad size=0x%x\n",
		 &ane->ring_iova, ANE_RTCLIENT_RING_SIZE);

	platform_set_drvdata(pdev, ane);

	/* RX path: the recv irq (ADT raw 0x374) is primary; the worker is
	 * the poll fallback that drives RX while the handshake runs. */
	schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));

	/* The handshake itself: the fw HELLOes first on MGMT (W2), rtkit
	 * answers, EPMAP + STARTEP + SET_IOP_PWR_STATE follow, then boot()
	 * sets the AP power state ON and returns. */
	ret = apple_rtkit_boot(ane->rtk);
	if (ret) {
		dev_err(dev, "rtkit boot handshake failed: %pe (is_running=%d crashed=%d)\n",
			ERR_PTR(ret), apple_rtkit_is_running(ane->rtk),
			apple_rtkit_is_crashed(ane->rtk));
		cancel_delayed_work_sync(&ane->poll_work);
		goto err_pm;
	}

	ane->boot_done = true;

	dev_info(ane->dev, "rtkit RUNNING; announced app endpoints:");
	for (ep = 1; ep <= 6; ep++)
		if (apple_rtkit_has_endpoint(ane->rtk, ep))
			pr_cont(" %d", ep);
	pr_cont("\n");

	if (csne_ping)
		ane_rtclient_csne_ping(ane);

	if (!poll_rx)
		cancel_delayed_work_sync(&ane->poll_work);

	dev_info(dev, "ANE RTKit client up: handshake complete\n");
	return 0;

err_pm:
	pm_runtime_put_sync_suspend(dev);
	pm_runtime_disable(dev);
	return ret;
}

static void ane_rtclient_remove(struct platform_device *pdev)
{
	struct ane_rtclient *ane = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ane->poll_work);
	apple_rtkit_shutdown(ane->rtk);
	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id ane_rtclient_of_match[] = {
	{ .compatible = "apple,t6021-ane-rtkit" },
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
