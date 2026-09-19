// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* T6021 (H14 / J414c) ANE platform driver skeleton.
 *
 * Architecture (W1/W2 receipts; this is NOT the H13 ane driver model):
 * the fw owns the task manager and the 8 task queues (CSneTMDrvH14 —
 * the kext never programs the H13-style host TM/TQ window and first
 * touch on it external-aborted, proven twice 2026-09-18), so the host
 * brings up an RTKit mailbox connection on the ASC block at ANE
 * +0x1600000 and talks CSNE_CMD over RTBuddy app endpoints. Submission
 * is W4; this skeleton probes, maps, attaches power, requests the
 * mailbox IRQ and speaks the RTKit MGMT handshake.
 *
 * Probe makes no engine-window write and no SET write. First-touch
 * reads at resume are the phase-1-proven RTKit/ASC status region; the
 * +0x1600000 block itself is only write-evidenced (phase1 §3: one
 * ambiguous reset association) — the first on-device run must ride
 * netconsole with the named-stage dev_info lines as the flush points.
 */

#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include "ane_t6021.h"

/*
 * Repo tier discipline (ane/src/ane_drv.c): the constants for this SoC
 * are complete and static-decode proven, but nothing has ever run on
 * t6021 silicon under Linux, so the skeleton binds only behind
 * allow_unqualified=1 — loudly, and never half-probing: the gate fires
 * before any power domain, MMIO or IRQ interaction.
 */
static bool allow_unqualified;
module_param(allow_unqualified, bool, 0444);
MODULE_PARM_DESC(allow_unqualified,
		 "Bind the unqualified T6021 skeleton (W3: no execution proven on silicon)");

/* pmgr island words this device consumes, inside the "pmgr" window:
 * ane_cpu@2e0, ane_sys_mpm@4000, ane_td@4008, ane_base@4010,
 * ane_set1..4@4018-4030 (overlay phandle chain). */
static const unsigned int ane_t6021_pmgr_words[] = {
	0x2e0, 0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x4030
};

static const char *const ane_t6021_reg_names[ANE_T6021_REG_COUNT] = {
	"engine", "pmgr", "set"
};

/* ps-word fields (apple-pmgr-pwrstate layout); constants mirror the
 * device-proven rtkit/h14_bringup.py (PS_CLEAR/PS_ACTIVE/poll). The
 * W3 death discriminator (receipt
 * 2026-09-19-h14-init-sequence-kext-trace): engine-window access while
 * any island word is below ACTUAL=0xf, or with ane_cpu AUTO_ENABLE
 * set, hard-resets t6021 — so the resume walk is gated on the verified
 * phase1 end state (all eight ACTUAL=0xf, BUSY clear, ane_cpu 0x3ff). */
#define ANE_PS_ON		0xf
#define ANE_PS_ACTUAL		GENMASK(7, 4)
#define ANE_PS_BUSY		BIT(11)
#define ANE_PS_AUTO_ENABLE	BIT(28)
#define ANE_PS_CLEAR		(BIT(31) | BIT(28) | GENMASK(27, 24) | \
				 GENMASK(19, 16) | BIT(12) | BIT(10) | \
				 GENMASK(3, 0))

static void ane_t6021_detach_genpd(struct ane_t6021 *ane)
{
	for (int i = ane->pd_count - 1; i >= 0; i--) {
		if (ane->pd_link[i])
			device_link_del(ane->pd_link[i]);
		if (!IS_ERR_OR_NULL(ane->pd_dev[i]))
			dev_pm_domain_detach(ane->pd_dev[i], true);
	}
	ane->pd_count = 0;
}

static int ane_t6021_attach_genpd(struct ane_t6021 *ane)
{
	struct device *dev = ane->dev;
	int count;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 1)
		return count < 0 ? count : -EINVAL;
	if (count == 1)
		return dev->pm_domain ? 0 : -EPROBE_DEFER;

	ane->pd_dev = devm_kcalloc(dev, count, sizeof(*ane->pd_dev),
				   GFP_KERNEL);
	ane->pd_link = devm_kcalloc(dev, count, sizeof(*ane->pd_link),
				    GFP_KERNEL);
	if (!ane->pd_dev || !ane->pd_link)
		return -ENOMEM;

	for (ane->pd_count = 0; ane->pd_count < count; ane->pd_count++) {
		ane->pd_dev[ane->pd_count] =
			dev_pm_domain_attach_by_id(dev, ane->pd_count);
		if (IS_ERR(ane->pd_dev[ane->pd_count])) {
			int err = PTR_ERR(ane->pd_dev[ane->pd_count]);

			ane_t6021_detach_genpd(ane);
			return err;
		}

		ane->pd_link[ane->pd_count] =
			device_link_add(dev, ane->pd_dev[ane->pd_count],
					DL_FLAG_STATELESS |
					DL_FLAG_PM_RUNTIME |
					DL_FLAG_RPM_ACTIVE);
		if (!ane->pd_link[ane->pd_count]) {
			dev_pm_domain_detach(ane->pd_dev[ane->pd_count],
					     true);
			ane_t6021_detach_genpd(ane);
			return -EINVAL;
		}
	}

	return 0;
}

/* First-resume bring-up in phase1 order; every stage logs before it
 * runs so netconsole pins any stall, and the whole engine-window walk
 * sits behind the eight-word power gate. */
static int ane_t6021_first_resume(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	void __iomem *pmgr = ane->base[ANE_T6021_REG_PMGR];
	unsigned int i;
	u32 cpu;

	/* Stage 1: pmgr reads only (proven-safe read class on every
	 * raise state).  The genpd chain raised all eight islands
	 * parent-first, ane_cpu last (overlay list order). */
	dev_info(ane->dev,
		 "ANE-resume: genpd raise complete; pmgr island words next\n");
	for (i = 0; i < ARRAY_SIZE(ane_t6021_pmgr_words); i++)
		dev_info(ane->dev, "ANERD pmgr+%#05x act=%08x\n",
			 ane_t6021_pmgr_words[i],
			 readl(pmgr + ane_t6021_pmgr_words[i]));

	/* Stage 2: clear AUTO_ENABLE on ane_cpu — the phase1 RMW
	 * (`(v & ~PS_CLEAR) | 0xf`, poll ACTUAL=0xf && BUSY clear).
	 * The kernel genpd raise leaves bit 28 set (W3: 0x1f0003ff);
	 * the proven-safe end state is 0x3ff. */
	cpu = readl(pmgr + 0x2e0);
	writel((cpu & ~ANE_PS_CLEAR) | ANE_PS_ON, pmgr + 0x2e0);
	readx_poll_timeout(readl, pmgr + 0x2e0, cpu,
			   FIELD_GET(ANE_PS_ACTUAL, cpu) == ANE_PS_ON &&
			   !(cpu & ANE_PS_BUSY),
			   1000, 500000);
	if (FIELD_GET(ANE_PS_ACTUAL, cpu) != ANE_PS_ON ||
	    (cpu & (ANE_PS_BUSY | ANE_PS_AUTO_ENABLE))) {
		dev_err(ane->dev,
			"ANEGATE ane_cpu=%08x (want act=0xf busy=0 auto=0) — refusing block access\n",
			cpu);
		return -EIO;
	}
	dev_info(ane->dev, "ANERD ane_cpu=%08x auto_enable cleared\n", cpu);

	/* Stage 3: the gate — all eight islands ACTUAL=0xf, BUSY clear,
	 * before anything inside the engine aperture or the SET window
	 * is touched. */
	for (i = 0; i < ARRAY_SIZE(ane_t6021_pmgr_words); i++) {
		u32 v = readl(pmgr + ane_t6021_pmgr_words[i]);

		if (FIELD_GET(ANE_PS_ACTUAL, v) != ANE_PS_ON ||
		    (v & ANE_PS_BUSY)) {
			dev_err(ane->dev,
				"ANEGATE pmgr+%#05x act=%08x not on — refusing block access\n",
				ane_t6021_pmgr_words[i], v);
			return -EIO;
		}
	}

	/* Stage 4: SET word 0 (read-only by repo rule; proven-safe read
	 * in W2 and W3), then the phase1-proven ASC status whitelist
	 * ONLY (S2 read clean under the full eight-word raise).
	 * CPU_CONTROL and the +0x1608xxx mailbox controls stay out of
	 * this walk: read-suspect per phase1 §3, and the RTKit
	 * transport exercises them itself. */
	dev_info(ane->dev, "ANEGATE pass; ASC status whitelist next\n");
	dev_info(ane->dev, "ANERD set+0 act=%08x\n",
		 readl(ane->base[ANE_T6021_REG_SET]));
	dev_info(ane->dev, "ANERD rvbar=%08x\n", readl(eng + ANE_ASC_RVBAR));
	dev_info(ane->dev, "ANERD edprcr=%08x\n", readl(eng + ANE_ASC_EDPRCR));
	dev_info(ane->dev, "ANERD vers=%08x\n", readl(eng + ANE_ASC_VERS));
	dev_info(ane->dev, "ANERD rtb_status=%08x rtb_7c=%08x\n",
		 readl(eng + ANE_ASC_RTB_STATUS),
		 readl(eng + ANE_ASC_RTB_STATUS_UNK7C));
	for (i = 0; i < 8; i++)
		dev_info(ane->dev, "ANERD gpio%u=%08x\n", i,
			 readl(eng + ANE_ASC_RTB_GPIO0 + 4 * i));
	return 0;
}

static __maybe_unused int ane_t6021_runtime_resume(struct device *dev)
{
	struct ane_t6021 *ane = dev_get_drvdata(dev);
	int err;

	if (!ane->booted) {
		err = ane_t6021_first_resume(ane);
		if (err)
			return err;
	}

	/* The fw (brought up by iBoot, phase1 §1) opens the exchange with
	 * MGMT HELLO on its own; the mailbox IRQ thread drains it. Drain
	 * once here in case the HELLO landed before the IRQ was
	 * requested. */
	ane_t6021_rtkit_drain(ane);
	return 0;
}

static const struct dev_pm_ops ane_t6021_pm_ops = {
	RUNTIME_PM_OPS(NULL, ane_t6021_runtime_resume, NULL)
};

static int ane_t6021_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct ane_t6021 *ane;
	int err;

	if (!allow_unqualified) {
		dev_err(dev,
			"apple,t6021-ane: skeleton binds only behind ane_t6021.allow_unqualified=1 (W3: no execution proven on this silicon; W4 submission pending)\n");
		return -ENODEV;
	}
	dev_warn(dev,
		 "UNQUALIFIED T6021 bind forced by allow_unqualified: RTKit bring-up path only, CSNE_CMD submission is W4\n");

	ane = devm_kzalloc(dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = dev;
	platform_set_drvdata(pdev, ane);

	/* Managed power first: the eight-island pmgr chain raises
	 * parent-first (sys_mpm→td→base→set1..4, ane_cpu last — the
	 * overlay list order) through the supplier links before any
	 * register is touched. */
	err = ane_t6021_attach_genpd(ane);
	if (err < 0) {
		dev_err(dev, "failed to attach power domains: %d\n", err);
		return err;
	}

	ane->irq = platform_get_irq_byname(pdev, "ane");
	if (ane->irq < 0) {
		err = ane->irq;
		goto detach_genpd;
	}

	/* All three windows map by address outside the resource API:
	 * the ADT puts the dart-ane0 windows inside the 32 MiB engine
	 * aperture (block-relative W3 architecture) and the pmgr island
	 * inside the PMGR syscon block, so region requests collide
	 * with the bound darts / pmgr driver (-EBUSY, live probe
	 * 2026-09-19). SET additionally stays read-only by repo rule
	 * (direct SET writes external-abort; H13 precedent); no
	 * engine-window write exists in this driver either. */
	for (unsigned int i = 0; i < ANE_T6021_REG_COUNT; i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   ane_t6021_reg_names[i]);
		if (!res) {
			err = -EINVAL;
			goto detach_genpd;
		}
		ane->base[i] = devm_ioremap(dev, res->start,
					    resource_size(res));
		if (!ane->base[i]) {
			err = -ENOMEM;
			goto detach_genpd;
		}
	}

	err = ane_t6021_rtkit_init(ane);
	if (err < 0)
		goto detach_genpd;

	pm_runtime_enable(dev);
	err = pm_runtime_resume_and_get(dev);
	if (err < 0)
		goto disable_pm;

	/* After power: AIC2 mailbox interrupt (raw 884; dart-ane0's 885
	 * belongs to the dart driver and is never requested here). */
	err = devm_request_threaded_irq(dev, ane->irq, NULL,
					ane_t6021_rtkit_irq_thread,
					IRQF_ONESHOT, "ane_t6021", ane);
	if (err < 0) {
		dev_err(dev, "failed to request ane irq: %d\n", err);
		goto put_pm;
	}

	dev_info(dev,
		 "loaded ane_t6021 %s (RTKit bring-up; 8-domain power gate; CSNE_CMD submission = W4)\n",
		 ANE_T6021_MODULE_VERSION);
	return 0;

put_pm:
	pm_runtime_put_noidle(dev);
disable_pm:
	pm_runtime_disable(dev);
	ane_t6021_rtkit_shutdown(ane);
detach_genpd:
	ane_t6021_detach_genpd(ane);
	return err;
}

static void ane_t6021_remove(struct platform_device *pdev)
{
	struct ane_t6021 *ane = platform_get_drvdata(pdev);

	/* devm irq actions run after remove(): free the mailbox IRQ
	 * before the rings it drains go away. */
	devm_free_irq(ane->dev, ane->irq, ane);
	pm_runtime_disable(ane->dev);
	pm_runtime_put_noidle(ane->dev);
	ane_t6021_rtkit_shutdown(ane);
	ane_t6021_detach_genpd(ane);
}

static const struct of_device_id ane_t6021_of_match[] = {
	{ .compatible = "apple,t6021-ane" },
	{ }
};
MODULE_DEVICE_TABLE(of, ane_t6021_of_match);

static struct platform_driver ane_t6021_driver = {
	.probe = ane_t6021_probe,
	.remove = ane_t6021_remove,
	.driver = {
		.name = "ane_t6021",
		.suppress_bind_attrs = true,
		.pm = pm_ptr(&ane_t6021_pm_ops),
		.of_match_table = ane_t6021_of_match,
	},
};
module_platform_driver(ane_t6021_driver);

MODULE_AUTHOR("Joshua Warren");
MODULE_DESCRIPTION("Apple Neural Engine driver, T6021/H14 (RTKit skeleton)");
MODULE_VERSION(ANE_T6021_MODULE_VERSION);
MODULE_LICENSE("Dual MIT/GPL");
