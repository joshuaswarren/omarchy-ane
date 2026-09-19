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

/* First-resume bring-up evidence, in bisect order, read-only; names
 * each stage before it runs (the H13 driver's resume discipline,
 * receipt 2026-09-18-t6021-overlay-abort §2). */
static void ane_t6021_first_resume(struct ane_t6021 *ane)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];

	/* Stage 1: pmgr reads only (proven-safe read class on every
	 * raise; SET window word 0 alongside — never written). */
	dev_info(ane->dev,
		 "ANE-resume: genpd raise complete; pmgr island words next\n");
	for (unsigned int i = 0; i < ARRAY_SIZE(ane_t6021_pmgr_words); i++)
		dev_info(ane->dev, "ANERD pmgr+%#05x act=%08x\n",
			 ane_t6021_pmgr_words[i],
			 readl(ane->base[ANE_T6021_REG_PMGR] +
			       ane_t6021_pmgr_words[i]));
	dev_info(ane->dev, "ANERD set+0 act=%08x\n",
		 readl(ane->base[ANE_T6021_REG_SET]));

	/* Stage 2: the RTKit/ASC status region — the block-relative first
	 * touch W1 chose (phase1 §2; RVBAR read is device-proven safe).
	 * CPU_CONTROL and mailbox controls are read-suspect per phase1
	 * §3; the named dev_info line above is the netconsole flush point
	 * for the first live pass. */
	dev_info(ane->dev, "ANERD islands done; ASC status block next\n");
	dev_info(ane->dev,
		 "ANERD rvbar=%08x vers=%08x rtb_status=%08x gpio0=%08x cpu_ctl=%08x i2a=%08x a2i=%08x\n",
		 readl(eng + ANE_ASC_RVBAR), readl(eng + ANE_ASC_VERS),
		 readl(eng + ANE_ASC_RTB_STATUS),
		 readl(eng + ANE_ASC_RTB_GPIO0),
		 readl(eng + ANE_ASC_CPU_CONTROL),
		 readl(eng + ANE_MBOX_I2A_CONTROL),
		 readl(eng + ANE_MBOX_A2I_CONTROL));
}

static __maybe_unused int ane_t6021_runtime_resume(struct device *dev)
{
	struct ane_t6021 *ane = dev_get_drvdata(dev);

	if (!ane->booted)
		ane_t6021_first_resume(ane);

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

	/* Managed power first: the six-domain pmgr chain raises through
	 * the supplier links before any register is touched. */
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
		 "loaded ane_t6021 %s (skeleton: RTKit bring-up; CSNE_CMD submission = W4)\n",
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
