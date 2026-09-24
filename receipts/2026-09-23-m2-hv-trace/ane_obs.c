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
	char buf[512];
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
	u32 sc[8];
	int i;

	for (i = 0; i < 8; i++)
		sc[i] = readl(eng + O_SCRATCH + 4 * i);
	len = scnprintf(buf, sizeof(buf),
			"ctl=%08x status=%08x rvbar=%08x%08x out=%08x/%08x ps_cpu=%08x\n"
			"a2i0=%016llx i2a1=%016llx sc=%08x %08x %08x %08x %08x %08x %08x %08x\n",
			ctl, st, rv_hi, rv_lo, o110, o114, ps,
			a0, b1, sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], sc[6], sc[7]);
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
	if (strncmp(cmd, "snap", 4) == 0) {
		/* Post-RUN snapshot: VENC root + leaves, island ACTUALs,
		 * DART error regs. Read-only; one emerg block per call. */
		static void __iomem *vr, *d0;
		u32 ps;
		int i;

		if (!vr) {
			vr = ioremap_np(0x290280000ull, 0x9000);
			d0 = ioremap_np(0x285800000ull, 0x2000);
			if (!vr || !d0) {
				pr_emerg("ane_obs: snap ioremap FAILED\n");
				return -ENOMEM;
			}
		}
		pr_emerg("ane_obs: SNAP ctl=%08x status=%08x\n",
			 readl(eng + O_CPU_CTL), readl(eng + O_CPU_STATUS));
		pr_emerg("ane_obs: SNAP venc_root=%08x leaf=%08x %08x %08x %08x %08x\n",
			 readl(vr + 0x3e0), readl(vr + 0x8008),
			 readl(vr + 0x8010), readl(vr + 0x8018),
			 readl(vr + 0x8020), readl(vr + 0x8028));
		ps = readl(pmgr + O_PS_CPU);
		pr_emerg("ane_obs: SNAP ps_cpu=%08x\n", ps);
		pr_emerg("ane_obs: SNAP dart_err=%08x addr=%08x%08x streams=%08x tcr=%08x en=%08x\n",
			 readl(d0 + 0x100), readl(d0 + 0x174),
			 readl(d0 + 0x170), readl(d0 + 0x1c0),
			 readl(d0 + 0x1000), readl(d0 + 0xc00));
		pr_emerg("ane_obs: SNAP tcr0=%08x tcr1=%08x ttbr0=%08x ttbr1=%08x\n",
			 readl(d0 + 0x1000), readl(d0 + 0x1004),
			 readl(d0 + 0x1400), readl(d0 + 0x1404));
		return n;
	}
	if (strncmp(cmd, "stop", 4) == 0) {
		v = readl(eng + O_CPU_CTL);
		writel(v & ~0x10u, eng + O_CPU_CTL);
		pr_emerg("ane_obs: STOP ctl=%08x status=%08x\n",
			 readl(eng + O_CPU_CTL), readl(eng + O_CPU_STATUS));
		return n;
	}
	if (strncmp(cmd, "wakepwr", 7) == 0) {
		/* RTKit mgmt SET_IOP_PWR_STATE=6 ON=0x20 on EP0 A2I, then
		 * drain I2A while non-empty, logging every popped word
		 * with its type field. No RUN, no reset. */
		u64 msg = ((u64)6 << 52) | 0x20u;
		u32 c;
		int nwords = 0;

		writeq(msg, eng + 0x1408800);
		writeq(0, eng + 0x1408808);
		pr_emerg("ane_obs: WAKE sent type=6 state=0x20\n");
		c = readl(eng + O_OUT114);
		while (!(c & (1u << 17)) && nwords < 16) {
			u64 w0 = readq(eng + 0x1408830);
			u64 w1 = readq(eng + 0x1408838);
			pr_emerg("ane_obs: WAKE pop%d w0=%016llx w1=%016llx type=%llu\n",
				 nwords, w0, w1,
				 (w0 >> 52) & 0xffu);
			nwords++;
			c = readl(eng + O_OUT114);
		}
		pr_emerg("ane_obs: WAKE drained %d words, out114=%08x\n",
			 nwords, readl(eng + O_OUT114));
		return n;
	}
	if (strncmp(cmd, "ack3", 4) == 0) {
		/* Post-DONE host ack the fw spins on (selene 0x7edc). */
		writel(0x08042006u, eng + O_SCRATCH + 4 * 3);
		pr_emerg("ane_obs: ACK3 SCRATCH3 <- 08042006 (readback %08x)\n",
			 readl(eng + O_SCRATCH + 4 * 3));
		return n;
	}
	if (strncmp(cmd, "drain", 5) == 0) {
		/* Pop and log every I2A word while the FIFO is non-empty. */
		u32 c = readl(eng + O_OUT114);
		int k = 0;

		while (!(c & (1u << 17)) && k < 32) {
			u64 w0 = readq(eng + 0x1408830);
			u64 w1 = readq(eng + 0x1408838);

			pr_emerg("ane_obs: I2A pop%d w0=%016llx w1=%016llx type=%llu\n",
				 k, w0, w1, (w0 >> 52) & 0xffu);
			k++;
			c = readl(eng + O_OUT114);
		}
		pr_emerg("ane_obs: DRAIN %d words out114=%08x\n", k, c);
		return n;
	}
	if (strncmp(cmd, "clrerr", 6) == 0) {
		/* W1C the T8110 DART inst0 error + stream latches so the
		 * next RUN's fault (if any) is unambiguous. */
		static void __iomem *d0;
		u32 e;

		if (!d0) {
			d0 = ioremap_np(0x285800000ull, 0x2000);
			if (!d0)
				return -ENOMEM;
		}
		e = readl(d0 + 0x100);
		writel(0xffffffffu, d0 + 0x100);
		writel(0xffffffffu, d0 + 0x1c0);
		pr_emerg("ane_obs: CLRERR was=%08x now=%08x\n", e,
			 readl(d0 + 0x100));
		return n;
	}
	return -EINVAL;
}

static ssize_t ane_obs_phys_read(struct file *f, char __user *ubuf, size_t n,
			   loff_t *off)
{
	/* Reserved-only physical reader for the iBoot-preloaded ANE
	 * segments. Anything outside the three reserved windows is
	 * refused with -EPERM; PA comes in *off. */
	static const struct { u64 base, len; } win[] = {
		{ 0x10000848000ull, 0xc4000ull },
		{ 0x100009fc000ull, 0x393000ull },
		{ 0x10001400000ull, 0x438000ull },
	};
	void __iomem *m;
	u64 pa;
	unsigned int i;

	pa = (u64)*off;

	for (i = 0; i < ARRAY_SIZE(win); i++) {
		if (pa >= win[i].base && n <= win[i].len &&
		    pa - win[i].base <= win[i].len - n) {
			m = ioremap_np(pa, n);
			if (!m)
				return -ENOMEM;
			if (copy_to_user(ubuf, (const void __force *)m, n)) {
				iounmap(m);
				return -EFAULT;
			}
			iounmap(m);
			*off += n;
			return n;
		}
	}
	return -EPERM;
}

static loff_t ane_obs_phys_llseek(struct file *f, loff_t off, int whence)
{
	if (whence != SEEK_SET)
		return -EINVAL;
	if (off < 0)
		return -EINVAL;
	f->f_pos = off;
	return off;
}

static const struct file_operations ane_obs_phys_fops = {
	.owner = THIS_MODULE,
	.read = ane_obs_phys_read,
	.llseek = ane_obs_phys_llseek,
};

static struct miscdevice ane_obs_phys_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ane_phys",
	.fops = &ane_obs_phys_fops,
};

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
	if (misc_register(&ane_obs_phys_dev))
		return -ENODEV;
	return misc_register(&ane_obs_dev);
}

static void __exit ane_obs_exit(void)
{
	misc_deregister(&ane_obs_phys_dev);
	misc_deregister(&ane_obs_dev);
	iounmap(pmgr);
	iounmap(eng);
}

module_init(ane_obs_init);
module_exit(ane_obs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T6021 ANE read-only observer, no platform bind");
