// SPDX-License-Identifier: GPL-2.0-only
/*
 * pmp5_sramdump.c — Jw16AnePmp5 Phase 1d read-only firmware recovery.
 * ONE-SHOT: dumps the iBoot-staged t6000pmp image from the PMP SRAM window
 * 0x28e700000 (1 MiB) into a buffer readable from /dev/pmp5_sram.
 * Same access class proven by pmp2b Phase 0 on this chain (GKTS bootargs +
 * SRAM head reads, boot 343ad4a4, parked CPU). NO writes to any register.
 * Every 64 KiB chunk is announced with a ktime stamp BEFORE the copy, so a
 * bus hang leaves the exact last chunk in netconsole.
 */
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/vmalloc.h>

#define P_SRAM		0x28e700000ULL
#define SRAM_SIZE	0x100000ULL
#define CHUNK		0x10000ULL

static void __iomem *base;
static void *buf;

static ssize_t pmp5_read(struct file *f, char __user *u, size_t cnt, loff_t *ppos)
{
	return simple_read_from_buffer(u, cnt, ppos, buf, SRAM_SIZE);
}

static const struct file_operations pmp5_fops = {
	.owner = THIS_MODULE,
	.read = pmp5_read,
};

static struct miscdevice pmp5_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pmp5_sram",
	.fops = &pmp5_fops,
	.mode = 0444,
};

static int __init pmp5_init(void)
{
	unsigned long off;

	buf = vmalloc(SRAM_SIZE);
	if (!buf)
		return -ENOMEM;
	base = ioremap(P_SRAM, SRAM_SIZE);
	if (!base) {
		vfree(buf);
		return -EFAULT;
	}
	for (off = 0; off < SRAM_SIZE; off += CHUNK) {
		pr_info("pmp5sram: t=%lld ns PRE chunk @+%#lx\n",
			ktime_get_ns(), off);
		memcpy_fromio(buf + off, base + off, CHUNK);
		pr_info("pmp5sram: t=%lld ns GOT chunk @+%#lx head=%08x\n",
			ktime_get_ns(), off, *(u32 *)(buf + off));
	}
	if (misc_register(&pmp5_dev)) {
		iounmap(base);
		vfree(buf);
		return -ENODEV;
	}
	pr_info("pmp5sram: dump ready at /dev/pmp5_sram (%llu bytes)\n", SRAM_SIZE);
	return 0;
}

static void __exit pmp5_exit(void)
{
	misc_deregister(&pmp5_dev);
	iounmap(base);
	vfree(buf);
	pr_info("pmp5sram: unloaded\n");
}

module_init(pmp5_init);
module_exit(pmp5_exit);
MODULE_LICENSE("GPL");
