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
 * After the reset, the machine must land on the stock default entry. On
 * the M2/GRUB rig the test boot is selected one-shot (grub-reboot ->
 * next_entry in grubenv, consumed by GRUB on the next boot) - measured
 * 2026-10-04. On the M1 Limine rig a LoaderEntryOneShot variable written
 * from Linux does NOT survive the reset (U-Boot's runtime EFI variable
 * store is RAM-volatile, measured in this PR): there, run the hang test
 * on the stock default boot (no one-shot involved) or edit the boot-time
 * variable store. See tools/wdt-hang-test/README.md.
 *
 * Register access policy: the module drives the WDT only through the
 * watchdog device the kernel's apple_wdt driver already registered
 * (ops->set_timeout/start/ping on the core-owned watchdog_device). If no
 * apple_wdt device is registered, it falls back to mapping ONLY the
 * DT-described "apple,wdt" window (reg[0]) - never a raw address outside
 * that window.
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

/*
 * WD1 block offsets and CTRL bits, pinned to drivers/watchdog/apple_wdt.c
 * at omarchy-linux branch josh/ane-driver-aurora, commit efe6e359 (lines
 * 42-44: WD1_CUR_TIME 0x10, WD1_BITE_TIME 0x14, WD1_CTRL 0x1c; line 52:
 * APPLE_WDT_CTRL_RESET_EN BIT(2)).
 */
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
MODULE_PARM_DESC(disarm_on_unload, "stop the WDT on clean unload instead of restoring the pre-insmod hardware state");
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
	u32 dt_prior_ctrl;		/* DT mode: WD1_CTRL read at arm time */
	u32 dt_prior_bite;		/* DT mode: WD1_BITE_TIME read at arm time */
	struct watchdog_device *wdd;	/* core mode only */
	const struct watchdog_ops *ops;
	unsigned int old_timeout;
	bool old_running;		/* core mode: HW running before insmod */
	struct task_struct *petter;
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

	/* Snapshot the pre-insmod WD1 state before changing anything. */
	wdt.dt_prior_ctrl = readl_relaxed(wdt.dt_base + APPLE_WDT_WD1_CTRL);
	wdt.dt_prior_bite = readl_relaxed(wdt.dt_base + APPLE_WDT_WD1_BITE_TIME);
	writel_relaxed(0, wdt.dt_base + APPLE_WDT_WD1_CUR_TIME);
	writel_relaxed((u32)(wdt.dt_clk_rate * timeout_sec),
		       wdt.dt_base + APPLE_WDT_WD1_BITE_TIME);
	writel_relaxed(APPLE_WDT_CTRL_RESET_EN,
		       wdt.dt_base + APPLE_WDT_WD1_CTRL);
	pr_info("DT arm: clk %llu Hz, BITE tick %u\n", wdt.dt_clk_rate,
		(u32)(wdt.dt_clk_rate * timeout_sec));
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
		if (disarm_on_unload || !wdt.old_running) {
			/* Leave the hardware as it was found: stopped. */
			wdt.ops->stop(wdt.wdd);
			return;
		}
		/*
		 * The watchdog was running before insmod (its owner, e.g.
		 * systemd, keeps pinging it): put the previous timeout back
		 * and leave it running. Dropping only the petting would
		 * leave the short test timeout latched, which the slower
		 * owner ping could miss.
		 */
		wdt.ops->set_timeout(wdt.wdd, wdt.old_timeout);
		return;
	}
	if (disarm_on_unload) {
		writel_relaxed(0, wdt.dt_base + APPLE_WDT_WD1_CTRL);
		return;
	}
	/*
	 * Restore the exact pre-insmod WD1 state, CTRL first: WD1_CTRL may
	 * still hold the RESET_EN written by wdt_arm() and dt_prior_bite is
	 * typically 0 in the fallback path (nothing has armed the WDT since
	 * boot), so writing BITE_TIME into the still-armed watchdog would
	 * program a zero-length bite on a live counter. CTRL = 0 stops it;
	 * only then do the prior pair go back.
	 */
	writel_relaxed(0, wdt.dt_base + APPLE_WDT_WD1_CTRL);
	writel_relaxed(wdt.dt_prior_bite,
		       wdt.dt_base + APPLE_WDT_WD1_BITE_TIME);
	writel_relaxed(wdt.dt_prior_ctrl, wdt.dt_base + APPLE_WDT_WD1_CTRL);
}

static int hang_thread(void *arg __always_unused)
{
	unsigned long deadline;
	unsigned long next_pet = jiffies + ping_interval_sec * HZ;
	int ret;

	if (hang_cpu >= 0) {
		ret = set_cpus_allowed_ptr(current, cpumask_of(hang_cpu));
		if (ret)
			pr_warn("could not pin to cpu %d (%d); running unpinned\n",
				hang_cpu, ret);
	}

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
	 * Deliberate hard hang. Every keepalive write resets WD1's counter,
	 * so the bite fires wdt_timeout_sec after the last pet, whoever
	 * made it. With no other producer the last pet was at most
	 * ping_interval_sec ago: <= wdt_timeout_sec + ping_interval_sec.
	 * Another producer (systemd) keeps resetting the counter after
	 * these pings stop, deferring the reset by up to its own ping
	 * interval -- worst case on the M2 rig (60 s ping, 30 s timeout):
	 * 90 s. A producer pinging FASTER than wdt_timeout_sec defeats the
	 * test entirely: the counter never expires. The spin polls
	 * kthread_stop, so an emergency rmmod can always end the test
	 * cleanly; during a window nobody unloads and the WDT reset is the
	 * expected end.
	 */
	pr_emerg("hanging now on cpu %d: pings stopped, WDT reset expected in <= %us with no other keepalive producer (another producer defers it by up to its own ping interval); with a one-shot boot selection the reset lands on the stock default entry\n",
		 raw_smp_processor_id(), wdt_timeout_sec + ping_interval_sec);

	local_irq_disable();
	preempt_disable();
	while (!kthread_should_stop())
		cpu_relax();
	local_irq_enable();
	preempt_enable();
	return 0;
}

static int __init wdt_hang_test_init(void)
{
	struct device *dev = NULL;
	struct watchdog_device *wdd = NULL;
	bool want_core, want_dt;
	u32 freq;
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
	if (hang_cpu >= (int)nr_cpu_ids) {
		pr_err("hang_cpu %d outside [0, %d]\n", hang_cpu,
		       (int)nr_cpu_ids - 1);
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
		/*
		 * apple_wdt takes the reference clock from the clock
		 * framework; the plain DT node has no clk object, so read
		 * "clock-frequency" when present and fall back to the
		 * documented 24 MHz clkref. The arm path logs the
		 * programmed tick so a window operator can compare the
		 * observed reset time against it.
		 */
		ret = of_property_read_u32(np, "clock-frequency", &freq);
		of_node_put(np);
		if (!wdt.dt_base) {
			pr_err("failed to map the DT apple-wdt window\n");
			put_device(dev);
			return -ENOMEM;
		}
		if (!ret && freq) {
			wdt.dt_clk_rate = freq;
			pr_info("core path unavailable; DT fallback, clock-frequency %u Hz\n",
				freq);
		} else {
			wdt.dt_clk_rate = 24000000;
			pr_info("core path unavailable; DT fallback, no clock-frequency in the node, assuming the documented 24 MHz clkref\n");
		}
	}

	wdt.dev = dev;
	wdt.wdd = wdd;
	if (wdd) {
		wdt.ops = wdd->ops;
		wdt.old_timeout = wdd->timeout;
		wdt.old_running = watchdog_hw_running(wdd);
		if (watchdog_active(wdd))
			pr_warn("/dev/watchdog0 has a userspace owner: its keepalive interval must stay above wdt_timeout_sec (%us) or it defeats the hang test; a slower producer defers the reset by up to its own interval (M2 rig: 60 s systemd ping + 30 s timeout -> up to 90 s)\n",
				wdt_timeout_sec);
	}

	ret = wdt_arm(wdt_timeout_sec);
	if (ret) {
		pr_err("failed to arm the watchdog: %d\n", ret);
		goto err_restore;
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
	if (wdt.dt_base)
		iounmap(wdt.dt_base);
	if (dev)
		put_device(dev);
	return ret;
}

static void __exit wdt_hang_test_exit(void)
{
	/*
	 * kthread_stop() always completes: the hang spin polls
	 * kthread_should_stop(), so even a mid-hang unload aborts the spin
	 * cleanly and the pre-insmod watchdog state is restored.
	 */
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
