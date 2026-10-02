// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_dsid_tm_probe — read-only, PS-guarded read of the one TM word the
 * 13.5 firmware's DSID_SET (CSNE_CMD 0x25) programs: CAneEngineExeLoopH14
 * updateDSID does a read-modify-write of 0x285c2046c, inserting the dsid
 * at bits [17:10] (fw135-dsid.txt @0x32c00; receipts/2026-10-02-ane-dsid).
 * Linux ane_t6021 never writes it; with fw_dsid_set=9 the firmware should
 * set bits [17:10] to 9 << 10 = 0x2400 and leave the rest of the word
 * alone.
 *
 * Rules (ane_dcs_ps_probe pattern): non-posted maps only, one exact
 * 4-byte map, no write. It refuses unless ane_sys ACTUAL = 0xf and the
 * seven island PS words read 0x3ff, waits 3 s, and checks the guard
 * again before each read. The address is logged before the read, so a
 * fault leaves it as the last line.
 *
 * Init takes the boot sample (seq 0). Each write of 1 to
 * /sys/module/ane_dsid_tm_probe/parameters/start takes one more sample.
 * Output lines: "ane_dsid_tm_probe: s=<seq> t=<ns> tm-dsid=0x<v>".
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

#define TM_DSID_REG	0x285c2046cull	/* CAneEngineExeLoopH14 updateDSID */
#define PS_BASE		0x28e080000ull	/* ANE pmgr PS block (DT "pmgr" reg) */
#define PS_ANE_SYS	0x260
#define PS_ISLAND0	0x4000		/* ane_sys_mpm, td, base, set1-4 */
#define PS_ISLAND_LAST	0x4030
#define PS_ACTUAL_ON	0xf0		/* ACTUAL [7:4] = 0xf */

static void __iomem *ps, *tm;
static unsigned int seq;
static DEFINE_MUTEX(tm_lock);

static bool dsid_power_on(void)
{
	u32 off;

	if ((readl(ps + PS_ANE_SYS) & PS_ACTUAL_ON) != PS_ACTUAL_ON)
		return false;
	for (off = PS_ISLAND0; off <= PS_ISLAND_LAST; off += 8)
		if ((readl(ps + off) & 0x3ff) != 0x3ff)
			return false;
	return true;
}

static void dsid_sample(void)
{
	pr_crit("s=%u t=%llu tm-dsid=%#010x\n", seq++, ktime_get_ns(),
		readl(tm));
}

static int dsid_start_set(const char *val, const struct kernel_param *kp)
{
	unsigned int n;
	int ret;

	ret = kstrtouint(val, 0, &n);
	if (ret)
		return ret;
	if (n != 1)
		return -EINVAL;
	mutex_lock(&tm_lock);
	if (!tm) {
		mutex_unlock(&tm_lock);
		return -EAGAIN;	/* init has not mapped the word */
	}
	if (!dsid_power_on()) {
		pr_crit("stopped: power left on-state\n");
		mutex_unlock(&tm_lock);
		return -EAGAIN;
	}
	dsid_sample();
	mutex_unlock(&tm_lock);
	return 0;
}

static const struct kernel_param_ops dsid_start_ops = {
	.set = dsid_start_set,
};
module_param_cb(start, &dsid_start_ops, NULL, 0200);
MODULE_PARM_DESC(start, "Write 1: take one PS-guarded sample");

static int __init dsid_tm_init(void)
{
	if (!of_machine_is_compatible("apple,t6021"))
		return -ENODEV;

	ps = ioremap_np(PS_BASE, PS_ISLAND_LAST + 4);
	if (!ps)
		return -ENOMEM;
	if (!dsid_power_on()) {
		pr_crit("refused: ane_sys %#010x islands %#x %#x %#x %#x %#x %#x %#x, no read\n",
			readl(ps + PS_ANE_SYS), readl(ps + 0x4000), readl(ps + 0x4008),
			readl(ps + 0x4010), readl(ps + 0x4018), readl(ps + 0x4020),
			readl(ps + 0x4028), readl(ps + 0x4030));
		iounmap(ps);
		return -EAGAIN;
	}
	msleep(3000);

	tm = ioremap_np(TM_DSID_REG, 4);
	if (!tm)
		goto err_map;

	/* ponytail: 50 ms lets the nbcon netconsole line leave before the
	 * first read (pr_flush() is not exported); the guard runs after it. */
	pr_crit("read %#llx\n", TM_DSID_REG);
	msleep(50);
	if (!dsid_power_on())
		goto err_power;
	dsid_sample();
	return 0;

err_power:
	pr_crit("stopped at init: power left on-state\n");
err_map:
	if (tm)
		iounmap(tm);
	tm = NULL;
	iounmap(ps);
	return -EAGAIN;
}

module_init(dsid_tm_init);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 TM dsid word 0x285c2046c read-only PS-guarded probe");
