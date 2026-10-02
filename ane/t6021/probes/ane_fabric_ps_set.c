// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_fabric_ps_set — LAB module: move the T6021 fabric-ps DESIRED field
 * between the Linux value 5 and the macOS value 6, one guarded write per
 * request. fabric-ps is the pmgr-misc word 0x28e20c000 (DT "fabric-ps");
 * macOS 27 holds 0x666 idle and under ANE load, Linux holds iBoot's 0x555.
 *
 * Layout: DESIRED [3:0] is the only field sourced for this word: the one
 * apple-pmgr-misc.c itself read-modify-writes at offset 0
 * (APPLE_CLKGEN_PSTATE_DESIRED) on suspend/resume. This module does the
 * same RMW and nothing else. Bits [11:4] have no source for a clkgen word
 * (it is not in the pmgr ps-regs list); both observed values repeat
 * DESIRED in all three nibbles, so the readback is the whole word:
 * done = 0x666 (or 0x555 back), anything else after 1 s = -ETIMEDOUT
 * with the word logged (e.g. 0x556: only DESIRED moved).
 *
 * Rules: j414c/t6021 only; one non-posted 4-byte map of that word, nothing
 * else mapped; init reads and refuses unless the word is exactly 0x555.
 * Writing 6 to /sys/module/ane_fabric_ps_set/parameters/go requires the
 * word to read exactly 0x555; writing 5 requires DESIRED = 6 set by this
 * module. No other value exists. Every write is logged before it happens,
 * then the word is polled every 10 ms for up to 1 s. There is no
 * module_exit: a reboot reverts the register and is the only unload.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define FABRIC_PS	0x28e20c000ull
#define PS_DESIRED	GENMASK(3, 0)
#define WORD_OF(ps)	((ps) * 0x111)	/* 6 -> 0x666, 5 -> 0x555 */
#define PS_LINUX	5
#define PS_MACOS	6

static void __iomem *fabric;
static bool raised;
static DEFINE_MUTEX(fps_lock);

static int fps_set(u32 target)
{
	u64 t0 = ktime_get_ns();
	u32 old, val, cur, last;
	int i;

	old = readl(fabric);
	if (target == PS_MACOS ? old != WORD_OF(PS_LINUX) :
	    !raised || FIELD_GET(PS_DESIRED, old) != PS_MACOS) {
		pr_crit("refused go=%u: word %#010x\n", target, old);
		return -EPERM;
	}
	val = (old & ~PS_DESIRED) | FIELD_PREP(PS_DESIRED, target);
	pr_crit("write %#llx: %#010x -> %#010x\n", FABRIC_PS, old, val);
	msleep(50);	/* let the line leave before the write */
	writel(val, fabric);
	raised = target == PS_MACOS;

	last = val;
	for (i = 0; i <= 100; i++) {
		cur = readl(fabric);
		if (cur != last || i == 0)
			pr_crit("poll t=+%lluus word=%#010x\n",
				(ktime_get_ns() - t0) / 1000, cur);
		last = cur;
		if (cur == WORD_OF(target)) {
			pr_crit("go=%u done: word=%#010x\n", target, cur);
			return 0;
		}
		msleep(10);
	}
	pr_crit("go=%u: word not %#x after 1 s: %#010x\n", target,
		WORD_OF(target), cur);
	return -ETIMEDOUT;
}

static int fps_go_set(const char *arg, const struct kernel_param *kp)
{
	unsigned int target;
	int ret;

	ret = kstrtouint(arg, 0, &target);
	if (ret)
		return ret;
	if (target != PS_LINUX && target != PS_MACOS)
		return -EINVAL;
	mutex_lock(&fps_lock);
	ret = fps_set(target);
	mutex_unlock(&fps_lock);
	return ret;
}

static const struct kernel_param_ops fps_go_ops = {
	.set = fps_go_set,
};
module_param_cb(go, &fps_go_ops, NULL, 0200);
MODULE_PARM_DESC(go, "Write 6 (from 0x555) or 5 (back): one guarded DESIRED RMW");

static int __init fps_init(void)
{
	u32 v;

	if (!of_machine_is_compatible("apple,t6021") ||
	    !of_machine_is_compatible("apple,j414c"))
		return -ENODEV;

	fabric = ioremap_np(FABRIC_PS, 4);
	if (!fabric)
		return -ENOMEM;
	pr_crit("read %#llx\n", FABRIC_PS);
	msleep(50);
	v = readl(fabric);
	if (v != WORD_OF(PS_LINUX)) {
		pr_crit("refused: word %#010x, expected %#x\n", v,
			WORD_OF(PS_LINUX));
		iounmap(fabric);
		return -EPERM;
	}
	pr_crit("ready: word %#010x\n", v);
	return 0;
}

module_init(fps_init);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 j414c fabric-ps DESIRED 5<->6 guarded lab write");
