// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 power-on + HAL tunable probe.
 *
 * Everything before this module raised the ANE sub-domains through genpd
 * and stopped there. The PMGR words then read TARGET=ACTUAL=0xf, but the
 * PWGATE words still hold Apple's *off* values:
 *
 *     0x268c = 0x3   (closed)      0x26b0 = 0x0
 *
 * A write to the engine in that state froze the machine immediately, on
 * a write of 0 into a register that already read 0
 * (receipts/2026-11-03-j700-h17-any-write-freezes).
 *
 * The observed power-up does not stop at the ps-word. For the
 * 0x200 ANE type it runs:
 *
 *   1. PS      write32(0xc000, 0xf)
 *   2. PS      validate(0xc000, 0xff, mask 0xff)
 *   3. PWGATE  write32(0x268c, 0)
 *   4. PWGATE  validate(0x268c, 0, mask 3)
 *   5. PS      write32(0xc000, 0x1000000f)
 *   6. PS      validate(0xc000, 0x100000ff, mask 0x100000ff)
 *
 * This probe runs those six steps, then repeats the exact write that
 * froze the box before: CPU_CONTROL := 0.
 *
 *   - If the write now completes, the closed gate was the cause and the
 *     write path opens up.
 *   - If the box freezes at step 3 or 4, the gate write itself is the
 *     hazard and the log names it.
 *   - If it freezes at the CPU_CONTROL write, the gate was not the cause.
 *
 * Apple's validate() panics on timeout. This port never does: bounded
 * poll, log, continue.
 *
 * Usage:
 *   sudo insmod ane_h17_tunable_probe.ko optin=1
 *   cat /var/log/h17-poweron.log
 */
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/fs.h>
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
#include <linux/uaccess.h>

#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"
#define H17_VOTE_NODE "omarchy,t8140-ane-cpu-vote"
#define H17_LOG_PATH "/var/log/h17-tunable.log"

#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_MAX_SUB 3

#define H17_PS_CPU 0xc000
#define H17_CPU_STATUS 0x1600048
#define H17_CPU_CONTROL 0x1600044
#define H17_RVBAR 0x1050000

#define H17_PWGATE_PA 0x300724000
#define H17_PWGATE_SIZE 0x4000
#define H17_PWGATE_OFF_A 0x268c
#define H17_PWGATE_OFF_B 0x26b0

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700");

static bool write_test = true;
module_param(write_test, bool, 0444);
MODULE_PARM_DESC(write_test, "1 = after the power-on sequence, write CPU_CONTROL := 0");

static struct file *logf;

static void h17_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	loff_t pos = 0;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	pr_info("ane_h17_tunable %s", buf);
	if (IS_ERR_OR_NULL(logf))
		return;
	kernel_write(logf, buf, n, &pos);
	vfs_fsync(logf, 1);
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

static bool h17_poll(struct regmap *map, u32 off, u32 expected, u32 mask,
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
	h17_log("validate %s off=%#x expected=%#x mask=%#x last=%#x TIMEOUT\n",
		what, off, expected, mask, val);
	return false;
}

static bool h17_poll_mmio(void __iomem *base, u32 off, u32 expected, u32 mask,
			  const char *what)
{
	unsigned int i;
	u32 val = 0;

	for (i = 0; i <= 5000; i++) {
		val = readl(base + off);
		if ((val & mask) == expected)
			return true;
		udelay(10);
	}
	h17_log("validate %s pa=%#lx off=%#x expected=%#x mask=%#x last=%#x TIMEOUT\n",
		(unsigned long)H17_PWGATE_PA, off, expected, mask, val);
	return false;
}

static int ane_h17_tunable_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps, *ane;
	struct regmap *map;
	void __iomem *eng, *pw;
	unsigned int i, nsub;
	u32 word;
	int count, ret = 0;

	h17_log("=== run at uptime %llus write_test=%d ===\n",
		ktime_get_seconds(), write_test);

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0)
		return count;
	nsub = min_t(unsigned int, count, H17_MAX_SUB);
	if (nsub != H17_MAX_SUB) {
		h17_log("REFUSED reason=%u-domains\n", nsub);
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

	regmap_read(map, H17_PS_CPU, &word);
	h17_log("domains raised, ps_cpu=%#x target=%#x actual=%#x\n", word,
		word & GENMASK(3, 0), FIELD_GET(H17_PS_ACTUAL, word));

	pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);
	if (!pw) {
		h17_log("pwgate map failed\n");
		ret = -ENOMEM;
		goto out;
	}
	h17_log("before: 0x268c=%#x 0x26b0=%#x\n",
		readl(pw + H17_PWGATE_OFF_A), readl(pw + H17_PWGATE_OFF_B));

	/* 1-2 */
	h17_log("step1 PS 0xc000 := 0xf\n");
	regmap_write(map, H17_PS_CPU, 0xf);
	h17_log("step2 validate 0xff\n");
	h17_poll(map, H17_PS_CPU, 0xff, 0xff, "0xc000/on");

	/* 3-4: open the gate. */
	h17_log("step3 PWGATE 0x268c := 0\n");
	writel(0, pw + H17_PWGATE_OFF_A);
	h17_log("step4 validate 0x268c mask3 == 0, now=%#x\n",
		readl(pw + H17_PWGATE_OFF_A));
	h17_poll_mmio(pw, H17_PWGATE_OFF_A, 0, 3, "0x268c");

	/* 5-6: raise the auto-enable bit. */
	h17_log("step5 PS 0xc000 := 0x1000000f\n");
	regmap_write(map, H17_PS_CPU, 0x1000000f);
	h17_log("step6 validate 0x100000ff, now=%#x\n",
		readl(pw + H17_PWGATE_OFF_A));
	h17_poll(map, H17_PS_CPU, 0x100000ff, 0x100000ff, "0xc000/auto");

	regmap_read(map, H17_PS_CPU, &word);
	h17_log("power-on done ps_cpu=%#x 0x268c=%#x 0x26b0=%#x\n", word,
		readl(pw + H17_PWGATE_OFF_A), readl(pw + H17_PWGATE_OFF_B));
	iounmap(pw);

	/*
	 * The tunable-apply call's unconditional _Legacy writes, in
	 * the order the HAL applies them. Each is logged before and after,
	 * so a freeze names the exact register.
	 */
	{
		void __iomem *t;
		u32 v;

		t = ioremap(0x1984000, 0x1000);
		if (t) {
			v = readl(t);
			h17_log("tunable mcw(0x1984000) before=%#x\n", v);
			writel(v & ~0x1f00, t);
			h17_log("tunable mcw(0x1984000) after=%#x\n", readl(t));
			iounmap(t);
		}

		t = ioremap(0x1988000, 0x1000);
		if (t) {
			h17_log("tunable ctrl_lock(0x1988000) before=%#x\n", readl(t));
			writel(0x111, t);
			h17_log("tunable ctrl_lock(0x1988000) after=%#x\n", readl(t));
			iounmap(t);
		}

		t = ioremap(0x1880000 + 0x1c000, 0x1000);
		if (t) {
			v = readl(t + 0xf4);
			h17_log("tunable sne_ctrl(0x1881c0f4) before=%#x\n", v);
			writel(v & ~0x3f, t + 0xf4);
			h17_log("tunable sne_ctrl(0x1881c0f4) after=%#x\n",
				readl(t + 0xf4));
			iounmap(t);
		}

		t = ioremap(0x18ac000, 0x1000);
		if (t) {
			v = readl(t + 0x4c);
			h17_log("tunable iso_gapf(0x18ac04c) before=%#x\n", v);
			writel(v & ~1, t + 0x4c);
			h17_log("tunable iso_gapf(0x18ac04c) after=%#x\n",
				readl(t + 0x4c));
			iounmap(t);
		}
		h17_log("tunables done\n");
	}

	eng = of_iomap(ane, 0);
	if (!eng) {
		h17_log("engine map failed\n");
		ret = -ENOMEM;
		goto out;
	}
	h17_log("read RVBAR=%#llx CPU_STATUS=%#x CPU_CONTROL=%#x\n",
		readq(eng + H17_RVBAR), readl(eng + H17_CPU_STATUS),
		readl(eng + H17_CPU_CONTROL));

	if (write_test) {
		h17_log("WRITE BEGIN CPU_CONTROL(%#x) := 0\n", H17_CPU_CONTROL);
		writel(0, eng + H17_CPU_CONTROL);
		h17_log("WRITE END   CPU_CONTROL now=%#x status=%#x\n",
			readl(eng + H17_CPU_CONTROL),
			readl(eng + H17_CPU_STATUS));
	}
	iounmap(eng);

	/* Close the gate again on the way out. */
	pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);
	if (pw) {
		regmap_read(map, H17_PS_CPU, &word);
		regmap_write(map, H17_PS_CPU, word & ~BIT(28));
		writel(3, pw + H17_PWGATE_OFF_A);
		writel(0, pw + H17_PWGATE_OFF_B);
		regmap_write(map, H17_PS_CPU, 0x300);
		iounmap(pw);
	}
	h17_log("closed gate\n");
out:
	for (i = nsub; i-- > 0; ) {
		if (link[i])
			device_link_del(link[i]);
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	h17_log("released\n");
	of_node_put(ane);
	return ret;
}

static const struct of_device_id ane_h17_tunable_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_tunable_of_match);

static struct platform_driver ane_h17_tunable_driver = {
	.probe = ane_h17_tunable_probe,
	.driver = {
		.name = "ane_h17_tunable_probe",
		.of_match_table = ane_h17_tunable_of_match,
	},
};

static int __init __maybe_unused ane_h17_tunable_init(void)
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

	logf = filp_open(H17_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
			 0644);
	if (IS_ERR(logf)) {
		pr_err("ane_h17_tunable cannot open %s: %ld\n",
		       H17_LOG_PATH, PTR_ERR(logf));
		logf = NULL;
	}
	return platform_driver_register(&ane_h17_tunable_driver);
refused:
	pr_err("ane_h17_tunable RESULT verdict=REFUSED reason=board-optin-or-dt error=%d\n",
	       ret);
	return ret;
}

static void __exit ane_h17_tunable_exit(void)
{
	platform_driver_unregister(&ane_h17_tunable_driver);
	if (!IS_ERR_OR_NULL(logf)) {
		vfs_fsync(logf, 1);
		filp_close(logf, NULL);
	}
}

/* Historical experiment maps engine offsets as absolute physical addresses. */
static int __init ane_h17_tunable_refuse(void)
{
	pr_err("ane_h17_tunable REFUSED: invalid absolute MMIO addresses\n");
	return -EOPNOTSUPP;
}
module_init(ane_h17_tunable_refuse);
module_exit(ane_h17_tunable_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 ANE power-on sequence probe (gate open before engine write)");
