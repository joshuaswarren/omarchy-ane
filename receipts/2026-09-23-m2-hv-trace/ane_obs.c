// SPDX-License-Identifier: GPL-2.0-only
/* Minimal ANE observer: no platform bind, no power-domains, no supplier
 * links, no genpd. ioremap_np the engine + pmgr windows, read only. */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#define ANE_ENG_PHYS		0x284000000ull
#define ANE_ENG_LEN		0x2000000ull
#define ANE_PMGR_PHYS		0x28e080000ull
#define ANE_PMGR_LEN		0x10000ull

#define O_CPU_CTL		0x1400044u
#define O_CPU_STATUS		0x1400048u
#define O_RVBAR_LO		0x1050000u
#define O_OUT110		0x1408110u
#define O_OUT114		0x1408114u
#define O_A2I0			0x1408800u
#define O_I2A1			0x1408838u
#define O_SCRATCH		0x1840048u
#define O_PS_CPU		0x2e0u

static void __iomem *eng;
static void __iomem *pmgr;

static ssize_t ane_obs_read(struct file *f, char __user *ubuf, size_t n,
			    loff_t *off)
{
	char buf[384];
	int len;
	u32 ctl = readl(eng + O_CPU_CTL);
	u32 st = readl(eng + O_CPU_STATUS);
	u32 rv_lo = readl(eng + O_RVBAR_LO);
	u32 rv_hi = readl(eng + O_RVBAR_LO + 4);
	u32 o110 = readl(eng + O_OUT110);
	u32 o114 = readl(eng + O_OUT114);
	u32 ps = readl(pmgr + O_PS_CPU);
	u64 a0 = readq(eng + O_A2I0);
	u64 b1 = readq(eng + O_I2A1);
	u32 sc[4];
	int i;

	for (i = 0; i < 4; i++)
		sc[i] = readl(eng + O_SCRATCH + 4 * i);
	len = scnprintf(buf, sizeof(buf),
			"ctl=%08x status=%08x rvbar=%08x%08x out=%08x/%08x ps_cpu=%08x\n"
			"a2i0=%016llx i2a1=%016llx sc=%08x %08x %08x %08x\n",
			ctl, st, rv_hi, rv_lo, o110, o114, ps,
			a0, b1, sc[0], sc[1], sc[2], sc[3]);
	if (*off >= len || n == 0)
		return 0;
	if (n > (size_t)(len - *off))
		n = len - *off;
	if (copy_to_user(ubuf, buf + *off, n))
		return -EFAULT;
	*off += n;
	return n;
}

static ssize_t ane_obs_write(struct file *f, const char __user *ubuf,
			     size_t n, loff_t *off)
{
	char cmd[16];
	size_t k = n < sizeof(cmd) - 1 ? n : sizeof(cmd) - 1;
	u32 v;

	if (copy_from_user(cmd, ubuf, k))
		return -EFAULT;
	cmd[k] = 0;
	if (strncmp(cmd, "run", 3) == 0) {
		v = readl(eng + O_OUT114);
		writel(v | 0x1u, eng + O_OUT114);
		pr_emerg("ane_obs: outbox 114 %08x -> %08x\n", v,
			 readl(eng + O_OUT114));
		writel(0x0u, eng + O_CPU_CTL);
		writel(0x10u, eng + O_CPU_CTL);
		pr_emerg("ane_obs: RUN ctl=%08x status=%08x\n",
			 readl(eng + O_CPU_CTL), readl(eng + O_CPU_STATUS));
		return n;
	}
	if (strncmp(cmd, "stop", 4) == 0) {
		v = readl(eng + O_CPU_CTL);
		writel(v & ~0x10u, eng + O_CPU_CTL);
		pr_emerg("ane_obs: STOP ctl=%08x status=%08x\n",
			 readl(eng + O_CPU_CTL), readl(eng + O_CPU_STATUS));
		return n;
	}
	return -EINVAL;
}

static const struct file_operations ane_obs_fops = {
	.owner = THIS_MODULE,
	.read = ane_obs_read,
	.write = ane_obs_write,
};

static struct miscdevice ane_obs_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ane_obs",
	.fops = &ane_obs_fops,
};

static int __init ane_obs_init(void)
{
	eng = ioremap_np(ANE_ENG_PHYS, ANE_ENG_LEN);
	if (!eng)
		return -ENOMEM;
	pmgr = ioremap_np(ANE_PMGR_PHYS, ANE_PMGR_LEN);
	if (!pmgr) {
		iounmap(eng);
		return -ENOMEM;
	}
	pr_emerg("ane_obs: mapped eng=%p pmgr=%p; ctl=%08x status=%08x ps=%08x\n",
		 eng, pmgr, readl(eng + O_CPU_CTL),
		 readl(eng + O_CPU_STATUS), readl(pmgr + O_PS_CPU));
	return misc_register(&ane_obs_dev);
}

static void __exit ane_obs_exit(void)
{
	misc_deregister(&ane_obs_dev);
	iounmap(pmgr);
	iounmap(eng);
}

module_init(ane_obs_init);
module_exit(ane_obs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T6021 ANE read-only observer, no platform bind");
