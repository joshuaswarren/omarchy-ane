// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 5: firmware RUN release — DRAFT, DO NOT RUN.
 *
 * Reviewed 2026-11-02 and found INCOMPLETE against the shipped H14G/H16
 * sequencer. It implements only CPU_CONTROL 0 -> 0x10 and a single
 * SCRATCH7 poll. It omits:
 *   - the scratch clear and the SCRATCH7 1 -> 0 pulse (so a stale
 *     0x08042006 can read as success);
 *   - SCRATCH6 transport-mode selection (0 = RTBuddy, 1 = legacy);
 *   - the init-structure publication and the 0xf7fbdff9 wake word;
 *   - the RTBuddy branch (HELLO-gated, no SCRATCH7 ACK).
 * It is removed from the Makefile's obj-m. See
 * receipts/2026-11-02-j700-h17-stage5-review/.
 *
 * For reference, the intended shape (shipped sequencer):
 *   S1 clear scratch0..7; SCRATCH6 = rtb_mode ? 0 : 1; pulse SCRATCH7 1->0
 *   S2 read RVBAR; write the composed entry only if not latched
 *   S3 CPU_CONTROL = 0, then = 0x10
 *   S4 poll A: SCRATCH7 == 0x08042006 (READY)
 *   S5 publish init suballoc DVA to SCRATCH0/1
 *   S6 write wake word 0xf7fbdff9 to SCRATCH7
 *   S7 poll B: SCRATCH7 == 0x08042006 (DONE), read back SCRATCH0/1
 *
 * This file is kept as a design draft only; it must be redesigned against
 * the RTBuddy branch before any hardware run.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_VOTE_NODE "omarchy,t8140-ane-cpu-vote"
#define H17_PS_OFF 0x290
#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_MAX_SUB 3

#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_SCRATCH7 0x188003c

#define H17_CPU_RUN 0x10
#define H17_FW_READY 0x08042006
#define H17_POLL_MS 500

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after stage 4 passed");

/*
 * NOTE: this draft is not built (removed from obj-m) and not run. It is
 * kept so the review receipt has a concrete reference for what was
 * wrong. Do not insmod it.
 */
static int ane_h17_start_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps, *ane;
	struct regmap *map;
	void __iomem *eng;
	unsigned int i, nsub;
	u32 word, scratch = 0;
	u64 rvbar;
	bool all_on = true, ready = false;
	int count, ret = 0;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0)
		return count;
	nsub = min_t(unsigned int, count, H17_MAX_SUB);
	if (nsub != H17_MAX_SUB) {
		dev_err(dev, "ane_h17_start RESULT stage=5 verdict=REFUSED reason=%u-domains\n", nsub);
		return -ENODEV;
	}

	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	if (!ps)
		return -ENODEV;
	map = syscon_node_to_regmap(ps->parent);
	of_node_put(ps);
	if (IS_ERR(map))
		return PTR_ERR(map);

	ane = of_find_node_by_path(H17_ANE_NODE);
	if (!ane)
		return -ENODEV;

	for (i = 0; i < nsub; i++) {
		pd[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR_OR_NULL(pd[i])) {
			ret = IS_ERR(pd[i]) ? PTR_ERR(pd[i]) : -ENODEV;
			pd[i] = NULL;
			goto out;
		}
		link[i] = device_link_add(dev, pd[i], DL_FLAG_STATELESS |
					  DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!link[i]) {
			ret = -EINVAL;
			goto out;
		}
	}

	for (i = 0; i < nsub; i++) {
		ret = regmap_read(map, h17_sub_off[i], &word);
		if (ret)
			goto out;
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
			all_on = false;
	}
	regmap_read(map, H17_PS_OFF, &word);
	if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
		all_on = false;
	if (!all_on) {
		ret = -EAGAIN;
		goto out;
	}

	eng = of_iomap(ane, 0);
	if (!eng) {
		ret = -ENOMEM;
		goto out;
	}

	rvbar = readq(eng + H17_RVBAR);
	if (!(rvbar & 1) || !(rvbar & ~1ULL)) {
		ret = -EAGAIN;
		iounmap(eng);
		goto out;
	}

	/* Only these two writes; everything else from the sequencer is
	 * missing on purpose: this draft is not to be run. */
	writel(0, eng + H17_CPU_CONTROL);
	writel(H17_CPU_RUN, eng + H17_CPU_CONTROL);

	for (i = 0; i < H17_POLL_MS; i++) {
		scratch = readl(eng + H17_SCRATCH7);
		if (scratch == H17_FW_READY) {
			ready = true;
			break;
		}
		usleep_range(1000, 2000);
	}

	dev_info(dev, "ane_h17_start draft SCRATCH7=%#x ready=%u CPU_STATUS=%#x\n",
		 scratch, ready ? 1 : 0, readl(eng + H17_CPU_STATUS));

	writel(0, eng + H17_CPU_CONTROL);
	iounmap(eng);
	ret = ready ? 0 : -ETIMEDOUT;
out:
	for (i = nsub; i-- > 0; ) {
		if (link[i])
			device_link_del(link[i]);
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	of_node_put(ane);
	return ret;
}

static const struct of_device_id ane_h17_start_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};

static struct platform_driver ane_h17_start_driver = {
	.probe = ane_h17_start_probe,
	.driver = {
		.name = "ane_h17_start_probe",
		.of_match_table = ane_h17_start_of_match,
	},
};

static int __init ane_h17_start_init(void)
{
	pr_err("ane_h17_start: draft module is not runnable; see receipts/2026-11-02-j700-h17-stage5-review/\n");
	return -EPERM;
}

static void __exit ane_h17_start_exit(void) { }

module_init(ane_h17_start_init);
module_exit(ane_h17_start_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 firmware-start DRAFT (not runnable)");
