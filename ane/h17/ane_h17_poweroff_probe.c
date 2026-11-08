// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 teardown probe: the Apple CPU power-off sequence.
 *
 * Every earlier probe raised the ANE sub-domains through genpd and then
 * dropped them with a bare dev_pm_domain_detach(). Apple's driver never
 * does that: DisableCPUClocksAndPower (ane-type 0x200 block at
 * 0xfffffe0008f2b848) runs a nine-step sequence against the PS word
 * (PMGR+0xc000) and the PWGATE window (0x300724000).
 *
 * The purpose of this module is to run that sequence in isolation, with
 * no engine MMIO at all, so a reset (or its absence) can be attributed to
 * the teardown path rather than to a read.
 *
 * Compiled sequence (receipts/2026-10-31-j700-h17-power-contract):
 *   1. PS      read32(0xc000)
 *   2. PS      write32(0xc000, value & ~0x10000000)
 *   3. PS      validate(0xc000, 0, mask 0x10000000)
 *   4. PWGATE  write32(0x268c, 3)
 *   5. PWGATE  validate(0x268c, 3, mask 3)
 *   6. PWGATE  write32(0x26b0, 0)
 *   7. PWGATE  validate(0x26b0, 0, mask 1)
 *   8. PS      write32(0xc000, 0x300)
 *   9. PS      validate(0xc000, 0, mask 0x8ff)
 *
 * Apple's validate() panics when its bool argument is zero, which is the
 * argument every traced call passes. This port never panics: a timeout is
 * logged and the next step runs, so a wedged register cannot take the
 * machine down through this module.
 *
 * Modes:
 *   apply=0 (default)  read-only: report 0xc000, 0x268c, 0x26b0 and the
 *                      PMGR words, then release the domains as before.
 *   apply=1            run the full sequence, then release the domains.
 *
 * The PWGATE window is not described by the running device tree, so it is
 * ioremapped by its ADT physical address (resource 2 of the ANE node).
 * That address is fixed for T8140; it is not a general-purpose mapping.
 *
 * Usage:
 *   sudo insmod ane_h17_poweroff_probe.ko optin=1            # dry run
 *   sudo insmod ane_h17_poweroff_probe.ko optin=1 apply=1    # sequence
 *   sudo rmmod ane_h17_poweroff_probe
 */
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/delay.h>
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
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"
#define H17_VOTE_NODE "omarchy,t8140-ane-cpu-vote"

/* PMGR ANE CPU power-state word (resource 1, 0x300700000 + 0x18000). */
#define H17_PS_CPU 0xc000
#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_PS_AUTO BIT(28)

/* PWGATE window: ADT resource 2, 0x300724000 size 0x4000. */
#define H17_PWGATE_PA 0x300724000
#define H17_PWGATE_SIZE 0x4000
#define H17_PWGATE_OFF_A 0x268c
#define H17_PWGATE_OFF_B 0x26b0
#define H17_PWGATE_A_CLOSED 3
#define H17_PWGATE_A_MASK 3
#define H17_PWGATE_B_OPEN 0
#define H17_PWGATE_B_MASK 1

#define H17_OFF_PS_WORD 0x300

#define H17_MAX_SUB 3

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700");

static bool apply;
module_param(apply, bool, 0444);
MODULE_PARM_DESC(apply, "0 = read-only dry run (default), 1 = run the power-off sequence");

static bool reads;
module_param(reads, bool, 0444);
MODULE_PARM_DESC(reads, "1 = also read the whitelisted engine registers before the teardown");

/*
 * Engine offsets with measured values on T8140 (receipts/2026-11-02-…,
 * 2026-11-03-j700-h17-poweroff-dryrun). Read only; nothing here writes
 * the engine.
 */
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_SCRATCH0 0x1880020
#define H17_MBOX_A2I_CTRL 0x1608110
#define H17_MBOX_I2A_CTRL 0x1608114

static void h17_engine_reads(struct device *dev, struct device_node *ane)
{
	void __iomem *eng;

	eng = of_iomap(ane, 0);
	if (!eng) {
		dev_err(dev, "ane_h17_poweroff engine map failed\n");
		return;
	}
	dev_info(dev, "ane_h17_poweroff reads CPU_STATUS(%#x)=%#x RVBAR(%#x)=%#llx\n",
		 H17_CPU_STATUS, readl(eng + H17_CPU_STATUS),
		 H17_RVBAR, readq(eng + H17_RVBAR));
	dev_info(dev, "ane_h17_poweroff reads SCRATCH0(%#x)=%#x A2I(%#x)=%#x I2A(%#x)=%#x\n",
		 H17_SCRATCH0, readl(eng + H17_SCRATCH0),
		 H17_MBOX_A2I_CTRL, readl(eng + H17_MBOX_A2I_CTRL),
		 H17_MBOX_I2A_CTRL, readl(eng + H17_MBOX_I2A_CTRL));
	iounmap(eng);
}

static bool ane_h17_darts_disabled(void)
{
	static const char *const paths[] = {
		H17_DART0_NODE, H17_DART1_NODE, H17_DART2_NODE,
	};
	struct device_node *node;
	unsigned int i;
	bool okay = true;

	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		node = of_find_node_by_path(paths[i]);
		if (!node || of_device_is_available(node) ||
		    !of_device_is_compatible(node, "apple,t8140-dart"))
			okay = false;
		of_node_put(node);
	}
	return okay;
}

/*
 * Bounded poll, 5000 x 10 us like Apple's validate(). Returns true when
 * the masked value reached expected. Never panics.
 */
static bool h17_validate(struct regmap *map, u32 off, u32 expected, u32 mask,
			 const char *what)
{
	unsigned int i;
	u32 val = 0;

	for (i = 0; i <= 5000; i++) {
		if (regmap_read(map, off, &val))
			return false;
		if ((val & mask) == expected)
			return true;
		udelay(10);
	}
	pr_warn("ane_h17_poweroff validate %s off=%#x expected=%#x mask=%#x last=%#x TIMEOUT\n",
		what, off, expected, mask, val);
	return false;
}

static bool h17_validate_mmio(void __iomem *base, u32 off, u32 expected,
			      u32 mask, const char *what)
{
	unsigned int i;
	u32 val = 0;

	for (i = 0; i <= 5000; i++) {
		val = readl(base + off);
		if ((val & mask) == expected)
			return true;
		udelay(10);
	}
	pr_warn("ane_h17_poweroff validate %s pa=%#lx off=%#x expected=%#x mask=%#x last=%#x TIMEOUT\n",
		(unsigned long)H17_PWGATE_PA, off, expected, mask, val);
	return false;
}

/*
 * Apple's J700 CPU power-off. Returns the number of steps that did not
 * reach their expected value, or a negative errno if the window cannot be
 * mapped.
 */
static int h17_poweroff_apple(struct regmap *map, u32 *ps_before, u32 *ps_after)
{
	void __iomem *pw;
	u32 val = 0;
	int failed = 0;

	regmap_read(map, H17_PS_CPU, ps_before);

	pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);
	if (!pw)
		return -ENOMEM;

	pr_info("ane_h17_poweroff PWGATE read 0x268c=%#x 0x26b0=%#x before sequence\n",
		readl(pw + H17_PWGATE_OFF_A), readl(pw + H17_PWGATE_OFF_B));

	/* 1-3: clear AUTO_ENABLE and wait for it to drop. */
	regmap_read(map, H17_PS_CPU, &val);
	if (regmap_write(map, H17_PS_CPU, val & ~H17_PS_AUTO)) {
		pr_warn("ane_h17_poweroff write 0xc000/auto failed\n");
		failed++;
	} else if (!h17_validate(map, H17_PS_CPU, 0, H17_PS_AUTO,
				 "0xc000/auto")) {
		failed++;
	}

	/* 4-5: close gate A. */
	writel(H17_PWGATE_A_CLOSED, pw + H17_PWGATE_OFF_A);
	if (!h17_validate_mmio(pw, H17_PWGATE_OFF_A, H17_PWGATE_A_CLOSED,
			       H17_PWGATE_A_MASK, "0x268c"))
		failed++;

	/* 6-7: release gate B. */
	writel(H17_PWGATE_B_OPEN, pw + H17_PWGATE_OFF_B);
	if (!h17_validate_mmio(pw, H17_PWGATE_OFF_B, H17_PWGATE_B_OPEN,
			       H17_PWGATE_B_MASK, "0x26b0"))
		failed++;

	/* 8-9: park the ps word. */
	regmap_write(map, H17_PS_CPU, H17_OFF_PS_WORD);
	if (!h17_validate(map, H17_PS_CPU, 0, 0x8ff, "0xc000/off"))
		failed++;

	regmap_read(map, H17_PS_CPU, ps_after);
	pr_info("ane_h17_poweroff PWGATE after 0x268c=%#x 0x26b0=%#x\n",
		readl(pw + H17_PWGATE_OFF_A), readl(pw + H17_PWGATE_OFF_B));
	iounmap(pw);

	return failed;
}

static int ane_h17_poweroff_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps, *ane;
	struct regmap *map;
	unsigned int i, nsub;
	u32 word, ps_before = 0, ps_after = 0;
	bool all_on = true;
	int count, ret = 0, failed = 0;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0)
		return count;
	nsub = min_t(unsigned int, count, H17_MAX_SUB);
	if (nsub != H17_MAX_SUB) {
		dev_err(dev, "ane_h17_poweroff RESULT verdict=REFUSED reason=%u-domains\n",
			nsub);
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
		dev_info(dev, "ane_h17_poweroff sub%u(%#x)=%#x target=%#x actual=%#x auto=%u\n",
			 i, h17_sub_off[i], word,
			 word & GENMASK(3, 0),
			 FIELD_GET(H17_PS_ACTUAL, word),
			 !!(word & H17_PS_AUTO));
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
			all_on = false;
	}
	regmap_read(map, H17_PS_CPU, &word);
	dev_info(dev, "ane_h17_poweroff ps_cpu(%#x)=%#x target=%#x actual=%#x auto=%u\n",
		 H17_PS_CPU, word, word & GENMASK(3, 0),
		 FIELD_GET(H17_PS_ACTUAL, word), !!(word & H17_PS_AUTO));
	if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
		all_on = false;
	if (!all_on) {
		dev_err(dev, "ane_h17_poweroff RESULT verdict=REFUSED reason=domains-not-on\n");
		ret = -EAGAIN;
		goto out;
	}

	if (reads)
		h17_engine_reads(dev, ane);

	if (!apply) {
		void __iomem *pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);

		if (!pw) {
			ret = -ENOMEM;
			goto out;
		}
		dev_info(dev, "ane_h17_poweroff DRYRUN pwwindow=%#lx 0x268c=%#x 0x26b0=%#x\n",
			 (unsigned long)H17_PWGATE_PA,
			 readl(pw + H17_PWGATE_OFF_A),
			 readl(pw + H17_PWGATE_OFF_B));
		iounmap(pw);
		dev_info(dev, "ane_h17_poweroff RESULT verdict=PASS reason=dry-run-read-ok\n");
		goto out;
	}

	failed = h17_poweroff_apple(map, &ps_before, &ps_after);
	if (failed < 0) {
		ret = failed;
		dev_err(dev, "ane_h17_poweroff RESULT verdict=FAIL reason=pwgate-map error=%d\n",
			ret);
		goto out;
	}
	dev_info(dev, "ane_h17_poweroff ps_cpu before=%#x after=%#x steps_failed=%d\n",
		 ps_before, ps_after, failed);
	dev_info(dev, "ane_h17_poweroff RESULT verdict=%s reason=apple-poweroff\n",
		 failed ? "PARTIAL" : "PASS");
	if (failed)
		ret = -EIO;
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

static const struct of_device_id ane_h17_poweroff_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_poweroff_of_match);

static struct platform_driver ane_h17_poweroff_driver = {
	.probe = ane_h17_poweroff_probe,
	.driver = {
		.name = "ane_h17_poweroff_probe",
		.of_match_table = ane_h17_poweroff_of_match,
	},
};

static int __init ane_h17_poweroff_init(void)
{
	struct device_node *ane, *probe;
	int ret = -EPERM;

	if (!optin || !of_machine_is_compatible("apple,j700"))
		goto refused;
	ane = of_find_node_by_path(H17_ANE_NODE);
	probe = of_find_compatible_node(NULL, NULL, H17_VOTE_NODE);
	if (!ane || !probe || of_device_is_available(ane) ||
	    !of_device_is_available(probe) ||
	    !of_device_is_compatible(ane, "apple,t8140-ane") ||
	    !ane_h17_darts_disabled()) {
		ret = -EINVAL;
		goto put_nodes;
	}
	ret = 0;
put_nodes:
	of_node_put(probe);
	of_node_put(ane);
	if (ret)
		goto refused;
	return platform_driver_register(&ane_h17_poweroff_driver);
refused:
	pr_err("ane_h17_poweroff RESULT verdict=REFUSED reason=board-optin-or-dt error=%d\n",
	       ret);
	return ret;
}

static void __exit ane_h17_poweroff_exit(void)
{
	platform_driver_unregister(&ane_h17_poweroff_driver);
}

module_init(ane_h17_poweroff_init);
module_exit(ane_h17_poweroff_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 Apple CPU power-off sequence probe");
