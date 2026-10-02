// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_dcs_ps_probe — read-only samples of the two T6021 pmgr performance-
 * state words that Linux apple-pmgr-misc reads once at probe: fabric-ps
 * 0x28e20c000 and dcs-ps 0x28e20c400 (pmgr reg[40], DT "fabric-ps" and
 * "dcs-ps"). On macOS the PMP moves them from ANE bandwidth thresholds
 * (ADT iop-pmp-nub ane0-*-bw-threshold); under Linux the PMP is off and
 * the words keep what iBoot left. The probe shows whether they change
 * while an encoder CALL loop runs.
 *
 * Rules (ane_afbridge_probe pattern): non-posted maps only, two exact
 * 4-byte maps, no write. It refuses unless ane_sys ACTUAL = 0xf and the
 * seven island PS words read 0x3ff, waits 3 s, and checks the guard
 * again before each sample. The first read of each word is logged
 * before the read, so a fault leaves the address as the last line.
 *
 * Init reads the idle sample (seq 0). Each write of N (1-600) to
 * /sys/module/ane_dcs_ps_probe/parameters/start then takes N samples
 * 100 ms apart in the writer's context. Output lines are raw words:
 * "ane_dcs_ps_probe: s=<seq> t=<ns> fabric-ps=0x<v> dcs-ps=0x<v>".
 * There is no module_exit: the maps stay until reboot, which is the only
 * unload.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define FABRIC_PS	0x28e20c000ull
#define DCS_PS		0x28e20c400ull
#define PS_BASE		0x28e080000ull	/* ANE pmgr PS block (DT "pmgr" reg) */
#define PS_ANE_SYS	0x260
#define PS_ISLAND0	0x4000		/* ane_sys_mpm, td, base, set1-4 */
#define PS_ISLAND_LAST	0x4030
#define PS_ACTUAL_ON	0xf0		/* ACTUAL [7:4] = 0xf */
#define MAX_SAMPLES	600

static void __iomem *ps, *fabric, *dcs;
static unsigned int seq;
static DEFINE_MUTEX(dcs_lock);

static bool dcs_power_on(void)
{
	u32 off;

	if ((readl(ps + PS_ANE_SYS) & PS_ACTUAL_ON) != PS_ACTUAL_ON)
		return false;
	for (off = PS_ISLAND0; off <= PS_ISLAND_LAST; off += 8)
		if ((readl(ps + off) & 0x3ff) != 0x3ff)
			return false;
	return true;
}

static void dcs_sample(void)
{
	u64 t = ktime_get_ns();
	u32 f = readl(fabric), d = readl(dcs);

	pr_crit("s=%u t=%llu fabric-ps=%#010x dcs-ps=%#010x\n", seq++, t, f, d);
}

static int dcs_start_set(const char *val, const struct kernel_param *kp)
{
	unsigned int n, i;
	int ret;

	ret = kstrtouint(val, 0, &n);
	if (ret)
		return ret;
	if (!n || n > MAX_SAMPLES)
		return -EINVAL;
	mutex_lock(&dcs_lock);
	if (!dcs) {
		ret = -EAGAIN;	/* init has not mapped the words */
		goto out;
	}
	for (i = 0; i < n; i++) {
		if (i)
			msleep(100);
		if (!dcs_power_on()) {
			pr_crit("stopped at s=%u: power left on-state, %u of %u read\n",
				seq, i, n);
			ret = -EAGAIN;
			break;
		}
		dcs_sample();
	}
out:
	mutex_unlock(&dcs_lock);
	return ret;
}

static const struct kernel_param_ops dcs_start_ops = {
	.set = dcs_start_set,
};
module_param_cb(start, &dcs_start_ops, NULL, 0200);
MODULE_PARM_DESC(start, "Write N (1-600): take N samples 100 ms apart");

static int __init dcs_init(void)
{
	u64 t;
	u32 f, d;

	if (!of_machine_is_compatible("apple,t6021"))
		return -ENODEV;

	ps = ioremap_np(PS_BASE, PS_ISLAND_LAST + 4);
	if (!ps)
		return -ENOMEM;
	if (!dcs_power_on()) {
		pr_crit("refused: ane_sys %#010x islands %#x %#x %#x %#x %#x %#x %#x, no read\n",
			readl(ps + PS_ANE_SYS), readl(ps + 0x4000), readl(ps + 0x4008),
			readl(ps + 0x4010), readl(ps + 0x4018), readl(ps + 0x4020),
			readl(ps + 0x4028), readl(ps + 0x4030));
		iounmap(ps);
		return -EAGAIN;
	}
	msleep(3000);

	fabric = ioremap_np(FABRIC_PS, 4);
	dcs = ioremap_np(DCS_PS, 4);
	if (!fabric || !dcs)
		goto err_map;

	/* ponytail: 50 ms lets the nbcon netconsole line leave before each
	 * first read (pr_flush() is not exported); the guard runs after it. */
	pr_crit("read %#llx\n", FABRIC_PS);
	msleep(50);
	if (!dcs_power_on())
		goto err_power;
	t = ktime_get_ns();
	f = readl(fabric);
	pr_crit("read %#llx\n", DCS_PS);
	msleep(50);
	if (!dcs_power_on())
		goto err_power;
	d = readl(dcs);
	pr_crit("s=%u t=%llu fabric-ps=%#010x dcs-ps=%#010x (idle)\n",
		seq++, t, f, d);
	return 0;

err_power:
	pr_crit("stopped at init: power left on-state\n");
err_map:
	if (fabric)
		iounmap(fabric);
	if (dcs)
		iounmap(dcs);
	fabric = dcs = NULL;
	iounmap(ps);
	return -EAGAIN;
}

module_init(dcs_init);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 fabric-ps / dcs-ps read-only sampling probe");
