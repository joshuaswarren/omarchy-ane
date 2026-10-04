// SPDX-License-Identifier: GPL-2.0
/*
 * wdt_hang_test - arm the Apple SoC watchdog, pet it from a kernel thread,
 * then deliberately hang the CPU with interrupts off.
 *
 * Purpose: prove the "unattended reset to stock" recovery path used before
 * running experimental kernel probes that may wedge the SoC. The module:
 *
 *   1. arms the Apple WDT (WD1) with a short timeout (default 30 s),
 *   2. keeps it petted by a kernel thread while the system is healthy,
 *   3. after hang_delay_sec (default 15 s) spins forever with IRQs off,
 *      so the pings stop and the WDT resets the machine.
 *
 * After the reset, firmware must boot the stock default entry because the
 * experimental boot was selected one-shot (Limine LoaderEntryOneShot on the
 * M1 rig, GRUB next_entry/grubenv on the M2 rig).
 *
 * Register access policy: the module drives the WDT only through the
 * watchdog device the kernel's apple_wdt driver already registered
 * (ops->set_timeout/start/ping on the core-owned watchdog_device). If no
 * apple_wdt device is registered, it falls back to mapping ONLY the
 * DT-described "apple,wdt" window (reg[0]) - never a raw address outside
 * that window. Register layout follows drivers/watchdog/apple_wdt.c.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/watchdog.h>

/* WD1 block offsets and CTRL bits, from drivers/watchdog/apple_wdt.c */
#define APPLE_WDT_WD1_CUR_TIME		0x10
#define APPLE_WDT_WD1_BITE_TIME		0x14
#define APPLE_WDT_WD1_CTRL		0x1c
#define APPLE_WDT_CTRL_RESET_EN		BIT(2)

/* apple_wdt identifies itself through struct watchdog_info.identity */
#define APPLE_WDT_IDENTITY		"Apple SoC Watchdog"

#define TIMEOUT_MIN_SEC 5
#define TIMEOUT_MAX_SEC 120

static uint wdt_timeout_sec = 30;
static uint ping_interval_sec = 7;
static uint hang_delay_sec = 15;
static int hang_cpu = -1;
static bool disarm_on_unload;
static char *mode = "auto";		/* auto | core | dt */

module_param(wdt_timeout_sec, uint, 0444);
MODULE_PARM_DESC(wdt_timeout_sec, "watchdog hardware timeout in seconds (5-120)");
module_param(ping_interval_sec, uint, 0444);
MODULE_PARM_DESC(ping_interval_sec, "kernel-thread pet interval in seconds");
module_param(hang_delay_sec, uint, 0444);
MODULE_PARM_DESC(hang_delay_sec, "seconds of healthy petting before hanging");
module_param(hang_cpu, int, 0444);
MODULE_PARM_DESC(hang_cpu, "CPU the hang thread runs on (-1 = don't pin)");
module_param(disarm_on_unload, bool, 0444);
MODULE_PARM_DESC(disarm_on_unload, "stop the WDT on clean unload instead of restoring the previous timeout");
module_param(mode, charp, 0444);
MODULE_PARM_DESC(mode, "wdt access: auto | core (registered device) | dt (DT window)");

/*
 * The apple_wdt driver stores its state as dev_get_drvdata(pdev) with
 * struct watchdog_device as the FIRST member, so the drvdata pointer is
 * also a valid struct watchdog_device pointer. The identity string check
 * below guards that assumption; it fails loudly on any layout change.
 */
static int apple_wdt_dev_match(struct device *dev, const void *data __always_unused)
{
	return dev->driver && !strcmp(dev->driver->name, "apple-watchdog");
}

struct wdt_ctrl {
	struct device *dev;		/* non-NULL in core mode */
	void __iomem *dt_base;		/* non-NULL in dt mode */
	u64 dt_clk_rate;
	struct watchdog_device *wdd;	/* core mode only */
	const struct watchdog_ops *ops;
	unsigned int old_timeout;
	struct task_struct *petter;
	bool hung;
	/* serialize access to whichever control path was chosen */
	struct mutex lock;
};

static struct wdt_ctrl wdt;

static int wdt_arm(unsigned int timeout_sec)
{
	int ret = 0;

	guard(mutex)(&wdt.lock);

	if (wdt.wdd) {
		ret = wdt.ops->set_timeout(wdt.wdd, timeout_sec);
		if (ret)
			return ret;
		return wdt.ops->start(wdt.wdd);
	}

	writel_relaxed(0, wdt.dt_base + APPLE_WDT_WD1_CUR_TIME);
	writel_relaxed((u32)(wdt.dt_clk_rate * timeout_sec),
		       wdt.dt_base + APPLE_WDT_WD1_BITE_TIME);
	writel_relaxed(APPLE_WDT_CTRL_RESET_EN,
		       wdt.dt_base + APPLE_WDT_WD1_CTRL);
	return 0;
}

static void wdt_pet(void)
{
	guard(mutex)(&wdt.lock);

	if (wdt.wdd) {
		wdt.ops->ping(wdt.wdd);
		return;
	}
	writel_relaxed(0, wdt.dt_base + APPLE_WDT_WD1_CUR_TIME);
}

static void wdt_disarm_or_restore(void)
{
	guard(mutex)(&wdt.lock);

	if (wdt.wdd) {
		if (disarm_on_unload) {
			wdt.ops->stop(wdt.wdd);
			return;
		}
		/*
		 * Do not just stop petting: the hardware keeps the short
		 * test timeout, and the owner of /dev/watchdog0 (systemd)
		 * may ping slower than that, which would reset the box
		 * shortly after a clean unload. Put the previous timeout
		 * back so the healthy system keeps running unchanged.
		 */
		wdt.ops->set_timeout(wdt.wdd, wdt.old_timeout);
		return;
	}
	writel_relaxed(0, wdt.dt_base + APPLE_WDT_WD1_CTRL);
}

static int hang_thread(void *arg __always_unused)
{
	unsigned long deadline;
	unsigned long next_pet = jiffies + ping_interval_sec * HZ;

	if (hang_cpu >= 0)
		set_cpus_allowed_ptr(current, cpumask_of(hang_cpu));

	pr_info("armed: timeout %us, petting every %us, hang in %us (cpu %d)\n",
		wdt_timeout_sec, ping_interval_sec, hang_delay_sec, hang_cpu);

	deadline = jiffies + hang_delay_sec * HZ;
	while (!kthread_should_stop() && time_before(jiffies, deadline)) {
		ssleep(1);
		if (time_after_eq(jiffies, next_pet)) {
			wdt_pet();
			next_pet = jiffies + ping_interval_sec * HZ;
		}
	}
	if (kthread_should_stop())
		return 0;

	/*
	 * Deliberate hard hang. The last pet was at most ping_interval_sec
	 * ago, so the WDT fires within wdt_timeout_sec + ping_interval_sec.
	 * From here the thread is unkillable; only the WDT reset (or a
	 * hard reset) ends it. rmmod during the spin just blocks.
	 */
	WRITE_ONCE(wdt.hung, true);
	pr_emerg("hanging now on cpu %d: pings stopped, WDT reset expected in <= %us; next boot must be the stock default entry\n",
		 raw_smp_processor_id(), wdt_timeout_sec + ping_interval_sec);

	local_irq_disable();
	preempt_disable();
	for (;;)
		cpu_relax();
	return 0;			/* not reached */
}

static int __init wdt_hang_test_init(void)
{
	struct device *dev = NULL;
	struct watchdog_device *wdd = NULL;
	bool want_core, want_dt;
	int ret;

	if (wdt_timeout_sec < TIMEOUT_MIN_SEC || wdt_timeout_sec > TIMEOUT_MAX_SEC) {
		pr_err("wdt_timeout_sec %u outside [%d, %d]\n",
		       wdt_timeout_sec, TIMEOUT_MIN_SEC, TIMEOUT_MAX_SEC);
		return -EINVAL;
	}
	if (!ping_interval_sec || ping_interval_sec > wdt_timeout_sec / 2) {
		pr_err("ping_interval_sec %u must be in [1, timeout/2]\n",
		       ping_interval_sec);
		return -EINVAL;
	}
	if (hang_delay_sec > 300) {
		pr_err("hang_delay_sec %u too large (max 300)\n", hang_delay_sec);
		return -EINVAL;
	}

	want_core = !strcmp(mode, "auto") || !strcmp(mode, "core");
	want_dt = !strcmp(mode, "auto") || !strcmp(mode, "dt");
	if (!want_core && !want_dt) {
		pr_err("mode must be auto, core or dt\n");
		return -EINVAL;
	}

	mutex_init(&wdt.lock);

	if (want_core) {
		dev = bus_find_device(&platform_bus_type, NULL, NULL,
				      apple_wdt_dev_match);
		if (dev) {
			wdd = dev_get_drvdata(dev);
			if (!wdd || !wdd->ops || !wdd->info ||
			    strcmp(wdd->info->identity, APPLE_WDT_IDENTITY) ||
			    !wdd->ops->set_timeout || !wdd->ops->start ||
			    !wdd->ops->ping) {
				pr_warn("found apple-watchdog device but it does not look like apple_wdt; refusing to drive it\n");
				put_device(dev);
				dev = NULL;
				wdd = NULL;
				if (!want_dt)
					return -ENODEV;
			}
		} else if (!want_dt) {
			pr_err("no registered apple_wdt device and mode=core\n");
			return -ENODEV;
		}
	}

	if (!wdd && want_dt) {
		struct device_node *np;

		np = of_find_compatible_node(NULL, NULL, "apple,wdt");
		if (!np) {
			pr_err("no apple_wdt device registered and no \"apple,wdt\" DT node\n");
			put_device(dev);
			return -ENODEV;
		}
		wdt.dt_base = of_iomap(np, 0);
		of_node_put(np);
		if (!wdt.dt_base) {
			pr_err("failed to map the DT apple-wdt window\n");
			put_device(dev);
			return -ENOMEM;
		}
		/* 24 MHz reference clock (clkref) on all released Apple SoCs */
		wdt.dt_clk_rate = 24000000;
		pr_info("core path unavailable; using DT window fallback at 24 MHz\n");
	}

	wdt.dev = dev;
	wdt.wdd = wdd;
	if (wdd) {
		wdt.ops = wdd->ops;
		wdt.old_timeout = wdd->timeout;
	}

	ret = wdt_arm(wdt_timeout_sec);
	if (ret) {
		pr_err("failed to arm the watchdog: %d\n", ret);
		goto err_unmap;
	}

	wdt.petter = kthread_run(hang_thread, NULL, "wdt_hang_test");
	if (IS_ERR(wdt.petter)) {
		ret = PTR_ERR(wdt.petter);
		wdt.petter = NULL;
		goto err_restore;
	}

	return 0;

err_restore:
	wdt_disarm_or_restore();
err_unmap:
	if (wdt.dt_base)
		iounmap(wdt.dt_base);
	if (dev)
		put_device(dev);
	return ret;
}

static void __exit wdt_hang_test_exit(void)
{
	if (READ_ONCE(wdt.hung)) {
		/*
		 * Unload during the hang: kthread_stop() would block forever
		 * and the machine is about to reset anyway. Leave it be.
		 */
		pr_emerg("unload during hang; WDT reset imminent\n");
		return;
	}
	kthread_stop(wdt.petter);
	wdt_disarm_or_restore();
	if (wdt.dt_base)
		iounmap(wdt.dt_base);
	if (wdt.dev)
		put_device(wdt.dev);
	pr_info("unloaded; watchdog restored\n");
}

module_init(wdt_hang_test_init);
module_exit(wdt_hang_test_exit);

MODULE_DESCRIPTION("Apple SoC watchdog arm/pet/hang recovery test");
MODULE_AUTHOR("Joshua Warren <816217+joshuaswarren@users.noreply.github.com>");
MODULE_LICENSE("GPL");
