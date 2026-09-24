// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_ascdbg.c — debugfs MMIO vehicle for the ANE ASC (H13/T6001).
 *
 * Finds the legacy-bound "ane" platform device, pins its power domain
 * for the whole visit (autosuspend after attach resets the SoC), maps
 * the 32 MiB engine aperture non-posted and exposes ONE debugfs file,
 * /sys/kernel/debug/ane_ascdbg/cmd. Every command names one bounded
 * access and is printed to the kernel log BEFORE the access happens,
 * so a hard reset pins the hostile address on netconsole.
 * Commands (offsets are aperture-relative, hex or decimal):
 *   r32 OFF            r64 OFF
 *   w32 OFF VAL        w64 OFF VAL
 *   p32 OFF MASK WANT MS       poll until (v & MASK) == WANT (p64: 64-bit)
 *   s32 OFF N US               sample N times, US apart, print each
 *   d32 OFF N                  dump N consecutive words
 *   pcs OFF N                  Apple UTTDBG PC sample (xnu model_dep.c:
 *                              32-bit read triggers, 64-bit read returns)
 *   itr ED INSTR MS            stuff INSTR into EDITR, wait EDSCR.ITE
 *   phys PA N              read N words of physical memory (memremap)
 * Reading the file returns the last result line.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device/bus.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#define APERTURE_SIZE	0x2000000
#define RESULT_LEN	4096

static struct {
	struct platform_device *pdev;
	void __iomem *engine;
	struct dentry *dir;
	struct mutex lock;
	char result[RESULT_LEN];
	size_t rlen;
} g;

static int match_ane(struct device *dev, const void *data)
{
	return dev_is_platform(dev) && dev->driver &&
	       !strcmp(dev->driver->name, "ane");
}

static void __printf(1, 2) res(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	g.rlen += vscnprintf(g.result + g.rlen, RESULT_LEN - g.rlen, fmt, ap);
	va_end(ap);
}

static int check_off(unsigned long off, unsigned int width)
{
	if (off + width > APERTURE_SIZE || (off & (width - 1))) {
		res("EINVAL off %#lx width %u\n", off, width);
		return -EINVAL;
	}
	return 0;
}

static int run_cmd(char *line)
{
	char op[8];
	unsigned long a[4] = { 0 };
	int n, i;

	n = sscanf(line, "%7s %li %li %li %li", op, &a[0], &a[1], &a[2], &a[3]);
	if (n < 2)
		return -EINVAL;
	pr_info("ane_ascdbg: %s %#lx %#lx %#lx %#lx\n", op, a[0], a[1], a[2], a[3]);

	if (!strcmp(op, "r32")) {
		u32 v;

		if (check_off(a[0], 4))
			return -EINVAL;
		v = readl_relaxed(g.engine + a[0]);
		res("r32 %#lx = %08x\n", a[0], v);
	} else if (!strcmp(op, "r64")) {
		u64 v;

		if (check_off(a[0], 8))
			return -EINVAL;
		v = readq_relaxed(g.engine + a[0]);
		res("r64 %#lx = %016llx\n", a[0], v);
	} else if (!strcmp(op, "w32")) {
		if (n < 3 || check_off(a[0], 4))
			return -EINVAL;
		writel_relaxed((u32)a[1], g.engine + a[0]);
		wmb();
		res("w32 %#lx <= %08x\n", a[0], (u32)a[1]);
	} else if (!strcmp(op, "w64")) {
		if (n < 3 || check_off(a[0], 8))
			return -EINVAL;
		writeq_relaxed((u64)a[1], g.engine + a[0]);
		wmb();
		res("w64 %#lx <= %016llx\n", a[0], (u64)a[1]);
	} else if (!strcmp(op, "p32")) {
		unsigned long t0 = jiffies;
		u32 v;

		if (n < 5 || check_off(a[0], 4))
			return -EINVAL;
		do {
			v = readl_relaxed(g.engine + a[0]);
			if ((v & a[1]) == a[2]) {
				res("p32 %#lx = %08x OK after %u ms\n", a[0], v,
				    jiffies_to_msecs(jiffies - t0));
				break;
			}
			usleep_range(200, 400);
		} while (time_before(jiffies, t0 + msecs_to_jiffies(a[3])));
		if ((v & a[1]) != a[2])
			res("p32 %#lx = %08x TIMEOUT %lu ms\n", a[0], v, a[3]);
	} else if (!strcmp(op, "p64")) {
		unsigned long t0 = jiffies;
		u64 v;

		if (n < 5 || check_off(a[0], 8))
			return -EINVAL;
		do {
			v = readq_relaxed(g.engine + a[0]);
			if ((v & a[1]) == a[2]) {
				res("p64 %#lx = %016llx OK after %u ms\n", a[0], v,
				    jiffies_to_msecs(jiffies - t0));
				break;
			}
			usleep_range(200, 400);
		} while (time_before(jiffies, t0 + msecs_to_jiffies(a[3])));
		if ((v & a[1]) != a[2])
			res("p64 %#lx = %016llx TIMEOUT %lu ms\n", a[0], v, a[3]);
	} else if (!strcmp(op, "pcs")) {
		u64 pc = 0;

		if (n < 3 || check_off(a[0], 8) || a[1] > 100000)
			return -EINVAL;
		for (i = 0; i < a[1] && !pc; i++) {
			(void)readl_relaxed(g.engine + a[0]);
			pc = readq_relaxed(g.engine + a[0]);
		}
		if (pc >> 48)
			pc |= 0xffff000000000000ull;
		res("pcs %#lx = %016llx after %d tries\n", a[0], pc, i);
	} else if (!strcmp(op, "itr")) {
		unsigned long t0 = jiffies;
		u32 scr;

		if (n < 4 || check_off(a[0], 4) || a[0] + 0x90 > APERTURE_SIZE)
			return -EINVAL;
		writel_relaxed((u32)a[1], g.engine + a[0] + 0x84);
		do {
			scr = readl_relaxed(g.engine + a[0] + 0x88);
			if (scr & (BIT(24) | BIT(6)))
				break;
			usleep_range(50, 100);
		} while (time_before(jiffies, t0 + msecs_to_jiffies(a[2])));
		res("itr %08x EDSCR=%08x%s%s\n", (u32)a[1], scr,
		    (scr & BIT(24)) ? " ITE" : " (no ITE)",
		    (scr & BIT(6)) ? " ERR" : "");
		if (scr & BIT(6)) {
			writel_relaxed(BIT(2), g.engine + a[0] + 0x90);
			res("  ERR cleared via EDRCR.CSE, EDSCR=%08x\n",
			    readl_relaxed(g.engine + a[0] + 0x88));
		}
	} else if (!strcmp(op, "s32")) {
		u32 v, last = 0;
		unsigned int same = 0;

		if (n < 4 || check_off(a[0], 4) || a[1] > 100000)
			return -EINVAL;
		for (i = 0; i < a[1]; i++) {
			v = readl_relaxed(g.engine + a[0]);
			if (i && v == last) {
				same++;
			} else {
				if (same)
					res("  x%u more\n", same);
				same = 0;
				res("s32 %#lx [%d] = %08x\n", a[0], i, v);
			}
			last = v;
			if (a[2])
				usleep_range(a[2], a[2] + a[2] / 4 + 1);
		}
		if (same)
			res("  x%u more\n", same);
	} else if (!strcmp(op, "phys")) {
		void *p;
		unsigned int count;

		count = a[1];
		if (n < 3 || count > 256 || a[0] & 3)
			return -EINVAL;
		p = memremap(a[0], count * 4, MEMREMAP_WB);
		if (!p) {
			res("phys %#lx memremap failed\n", a[0]);
			return -EIO;
		}
		for (i = 0; i < count; i++)
			res("phys %#lx = %08x\n", a[0] + 4 * i,
			    readl_relaxed(p + 4 * i));
		memunmap(p);
	} else if (!strcmp(op, "d32")) {
		if (n < 3 || check_off(a[0], 4) || a[1] > 256 ||
		    check_off(a[0] + 4 * a[1] - 4, 4))
			return -EINVAL;
		for (i = 0; i < a[1]; i++)
			res("d32 %#lx = %08x\n", a[0] + 4 * i,
			    readl_relaxed(g.engine + a[0] + 4 * i));
	} else {
		return -EINVAL;
	}
	pr_info("ane_ascdbg: %s", g.result);
	return 0;
}

static ssize_t cmd_write(struct file *f, const char __user *ubuf, size_t len,
			 loff_t *ppos)
{
	char line[128];
	int ret;

	if (len >= sizeof(line))
		return -E2BIG;
	if (copy_from_user(line, ubuf, len))
		return -EFAULT;
	line[len] = 0;
	mutex_lock(&g.lock);
	g.rlen = 0;
	g.result[0] = 0;
	ret = run_cmd(line);
	mutex_unlock(&g.lock);
	return ret ? ret : len;
}

static ssize_t cmd_read(struct file *f, char __user *ubuf, size_t len,
			loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&g.lock);
	ret = simple_read_from_buffer(ubuf, len, ppos, g.result, g.rlen);
	mutex_unlock(&g.lock);
	return ret;
}

static const struct file_operations cmd_fops = {
	.owner = THIS_MODULE,
	.write = cmd_write,
	.read = cmd_read,
	.llseek = default_llseek,
};

static void ane_ascdbg_cleanup(void)
{
	debugfs_remove_recursive(g.dir);
	if (g.engine)
		iounmap(g.engine);
	if (g.pdev) {
		pm_runtime_put_sync_suspend(&g.pdev->dev);
		pm_runtime_disable(&g.pdev->dev);
		put_device(&g.pdev->dev);
	}
}

static int __init ane_ascdbg_init(void)
{
	struct device *found;
	struct resource *r;
	u64 base;
	int ret;

	found = bus_find_device(&platform_bus_type, NULL, NULL, match_ane);
	if (!found) {
		pr_err("ane_ascdbg: no legacy-bound ane device\n");
		return -ENODEV;
	}
	g.pdev = to_platform_device(found);
	mutex_init(&g.lock);

	pm_runtime_enable(found);
	ret = pm_runtime_resume_and_get(found);
	if (ret) {
		pr_err("ane_ascdbg: genpd raise failed: %pe\n", ERR_PTR(ret));
		pm_runtime_disable(found);
		put_device(found);
		g.pdev = NULL;
		return ret;
	}

	r = platform_get_resource(g.pdev, IORESOURCE_MEM, 0);
	if (!r) {
		ret = -ENODEV;
		goto err;
	}
	base = r->start & ~(u64)(APERTURE_SIZE - 1);
	g.engine = ioremap_np(base, APERTURE_SIZE);
	if (!g.engine) {
		ret = -ENOMEM;
		goto err;
	}
	g.dir = debugfs_create_dir("ane_ascdbg", NULL);
	debugfs_create_file("cmd", 0600, g.dir, NULL, &cmd_fops);
	pr_info("ane_ascdbg: %s aperture %#llx mapped, CPU_STATUS=%08x\n",
		dev_name(found), base, readl_relaxed(g.engine + 0x1400048));
	return 0;
err:
	ane_ascdbg_cleanup();
	return ret;
}

static void __exit ane_ascdbg_exit(void)
{
	ane_ascdbg_cleanup();
}

module_init(ane_ascdbg_init);
module_exit(ane_ascdbg_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("ANE ASC debugfs MMIO vehicle (pre-logged single accesses)");
