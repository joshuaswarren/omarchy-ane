// SPDX-License-Identifier: GPL-2.0
// pmp_dvfs.c (pmp5 build) — Phase 2 scoped module for jw16 PMP DVFS token ladder.
// Maps pmgr reg[113] = 0x400004000 (0x4000) ONLY (DVFS_CMD +0xa00,
// DVFS_ON +0x2000). Exposes sysfs one-rung-at-a-time + readback.
//
// NEVER writes PS-words, fabric/pmgr pstate words, or PLL words (out of scope).
// Per-rung protocol (matching the macOS 25G83 capture):
//   DVFS_ON <- 1
//   DVFS_CMD <- 0x80000000   (idle token)
//   per rung: DVFS_CMD <- 0x80000000 | (prev<<4) | new  (>=5 ms spacing)
//   readback after each rung
// Reverse rungs + DVFS_ON <- 0 on close.
//
// pmp5 fixes vs the pmp4 copy (never compiled before): __iomem type typo,
// undeclared `dev` in the rung path, and the >=5 ms spacing now sleeps the
// REMAINDER (usleep_range) instead of ~5 s. ioctl numbers unchanged.

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/ktime.h>

#define DVFS_BASE 0x400004000UL
#define DVFS_SIZE 0x4000
#define DVFS_CMD_OFF 0xa00
#define DVFS_ON_OFF  0x2000

static void __iomem *pbase;
static u32 last_token;
static int last_state = -1;
static u64 last_us;

static u32 dvfs_read(u32 off) { return ioread32(pbase + off); }
static void dvfs_write(u32 off, u32 v) { iowrite32(v, pbase + off); }

static void space_out(void)
{
	u64 now = ktime_get_ns();
	if (last_us) {
		u64 delta_us = (now - last_us) / 1000;
		if (delta_us < 5000)
			usleep_range(5000 - (unsigned int)delta_us, 6000);
	}
	last_us = ktime_get_ns();
}

static ssize_t dvfs_state_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sprintf(buf, "last_token=0x%x last_state=%d\n", last_token, last_state);
}
static DEVICE_ATTR(state, 0444, dvfs_state_show, NULL);

static struct attribute *dvfs_attrs[] = { &dev_attr_state.attr, NULL };
static struct attribute_group dvfs_attr_group = { .attrs = dvfs_attrs };
static const struct attribute_group *dvfs_groups[] = { &dvfs_attr_group, NULL };

#define DVFS_IOCTL_RUNG _IOW('D', 1, u32)
#define DVFS_IOCTL_ON   _IO(0, 2)
#define DVFS_IOCTL_OFF  _IO(0, 3)
#define DVFS_IOCTL_READ_CMD _IOR('D', 4, u32)
#define DVFS_IOCTL_READ_ON  _IOR('D', 5, u32)

static long dvfs_unlocked_ioctl(struct file *f, unsigned int c, unsigned long a)
{
	u32 arg, token, rb;

	switch (c) {
	case DVFS_IOCTL_RUNG:
		if (copy_from_user(&arg, (u32 __user *)a, sizeof(arg)))
			return -EFAULT;
		{
			u32 prev = (arg >> 4) & 0xff;
			u32 new = arg & 0xff;
			if (new > 5)
				return -EINVAL;
			space_out();
			token = 0x80000000u | (prev << 4) | new;
			dvfs_write(DVFS_CMD_OFF, token);
			last_token = token;
			last_state = (int)new;
		}
		rb = dvfs_read(DVFS_CMD_OFF);
		pr_info("pmp_dvfs: rung prev=%u new=%u token=0x%x readback=0x%x\n",
			(arg >> 4) & 0xff, arg & 0xff, token, rb);
		return rb;
	case DVFS_IOCTL_ON:
		space_out();
		dvfs_write(DVFS_ON_OFF, 1);
		return 0;
	case DVFS_IOCTL_OFF:
		space_out();
		dvfs_write(DVFS_ON_OFF, 0);
		last_us = 0;
		return 0;
	case DVFS_IOCTL_READ_CMD:
		return dvfs_read(DVFS_CMD_OFF);
	case DVFS_IOCTL_READ_ON:
		return dvfs_read(DVFS_ON_OFF);
	}
	return -ENOTTY;
}

static const struct file_operations dvfs_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = dvfs_unlocked_ioctl,
};

static struct miscdevice dvfs_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pmp_dvfs",
	.fops = &dvfs_fops,
	.groups = dvfs_groups,
};

static int __init dvfs_init(void)
{
	pbase = ioremap_np(DVFS_BASE, DVFS_SIZE);
	if (!pbase)
		return -ENOMEM;
	pr_info("pmp_dvfs: mapped 0x%lx (0x%x); initial CMD=0x%x ON=0x%x\n",
		DVFS_BASE, DVFS_SIZE, dvfs_read(DVFS_CMD_OFF), dvfs_read(DVFS_ON_OFF));
	if (misc_register(&dvfs_misc)) {
		iounmap(pbase);
		return -ENODEV;
	}
	return 0;
}

static void __exit dvfs_exit(void)
{
	misc_deregister(&dvfs_misc);
	if (pbase)
		iounmap(pbase);
}

module_init(dvfs_init);
module_exit(dvfs_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phase 2 PMP DVFS token ladder for jw16 (T6001, ANE perf)");
MODULE_AUTHOR("omp / AnePmp5");
