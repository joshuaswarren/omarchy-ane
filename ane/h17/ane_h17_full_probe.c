// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17: Apple bring-up order, then watch the mailbox for RTKit.
 *
 * Derived from com.apple.driver.AppleH16ANEInterface (family 0xe0, the
 * group the driver maps ADT ane-type 0x200 onto) and
 * AppleT8140ANEHAL. See
 * receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md
 * for the instruction addresses.
 *
 * Apple's order, which differs from everything this repo has done so far:
 *
 *   B1/B2  provider enableDeviceClock / enableDevicePower   (NOT done here:
 *          needs the PMGR clock map, which Linux does not have)
 *   C5     ANE 0x18a4000  ^= 3, then poll 0x18a4008        <- first ANE MMIO
 *   D1-D7  PS 0xc000 := 0xf, PWGATE 0x268c := 0, PS 0xc000 := 0x1000000f
 *   E1     PS 0xc008 := 0xf
 *   G1-G8  SCRATCH0..7 := 0
 *   G9     SCRATCH6 := 1                                  (1, not 0)
 *   G10-11 SCRATCH7 := 1 then := 0
 *   G13-14 RVBAR read, then RVBAR written                 (Apple DOES write it)
 *   G16-17 CPU_CONTROL := 0, then := 0x10
 *
 * Every step is fsync'd to /var/log/h17-full.log before the next, so
 * a freeze names the exact operation. The run is expected to be
 * informative whichever step it dies on.
 *
 * Usage:
 *   sudo insmod ane_h17_full_probe.ko optin=1
 *   cat /var/log/h17-full.log
 */
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>

#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"
#define H17_VOTE_NODE "omarchy,t8140-ane-cpu-vote"
#define H17_LOG_PATH "/var/log/h17-full.log"

#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_MAX_SUB 3

#define H17_PS_CPU 0xc000
#define H17_PS_CPU2 0xc008
#define H17_CE_A 0x18a4000
#define H17_CE_B 0x18a4008
#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_SCRATCH0 0x1880020
#define H17_RVBAR_KEEP 0xff7dfffffffff800ULL
#define H17_RVBAR_SET 0x0081000000000001ULL

#define H17_PWGATE_PA 0x300724000
#define H17_PWGATE_SIZE 0x4000
#define H17_PWGATE_OFF_A 0x268c
#define H17_PWGATE_OFF_B 0x26b0

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700");

static struct file *logf;

static void h17_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	loff_t pos = 0;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	pr_info("ane_h17_apple %s", buf);
	if (IS_ERR_OR_NULL(logf))
		return;
	kernel_write(logf, buf, n, &pos);
	vfs_fsync(logf, 1);
}

static bool ane_h17_darts_disabled(void)
{
	static const char *const paths[] = {
		H17_DART0_NODE, H17_DART1_NODE, H17_DART2_NODE,
	};
	struct device_node *node;
	unsigned int i;
	bool okay = true;

	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		node = of_find_node_by_path(paths[i]);
		if (!node || of_device_is_available(node) ||
		    !of_device_is_compatible(node, "apple,t8140-dart"))
			okay = false;
		of_node_put(node);
	}
	return okay;
}

static bool h17_poll_mmio(void __iomem *base, u32 off, u32 expected, u32 mask,
			  const char *what)
{
	unsigned int i;
	u32 val = 0;

	for (i = 0; i <= 5000; i++) {
		val = readl(base + off);
		if ((val & mask) == expected)
			return true;
		udelay(10);
	}
	h17_log("  validate %s off=%#x expect=%#x mask=%#x last=%#x TIMEOUT\n",
		what, off, expected, mask, val);
	return false;
}

static int ane_h17_full_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps, *ane;
	struct regmap *map;
	void __iomem *eng, *pw;
	unsigned int i, nsub;
	u32 word, v;
	u64 rv;
	int count, ret = 0;

	h17_log("=== apple-order run at uptime %llus ===\n", ktime_get_seconds());

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0)
		return count;
	nsub = min_t(unsigned int, count, H17_MAX_SUB);
	if (nsub != H17_MAX_SUB) {
		h17_log("REFUSED reason=%u-domains\n", nsub);
		return -ENODEV;
	}

	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	if (!ps)
		return -ENODEV;
	map = syscon_node_to_regmap(ps->parent);
	of_node_put(ps);
	if (IS_ERR(map))
		return PTR_ERR(map);

	ane = of_find_node_by_path(H17_ANE_NODE);
	if (!ane)
		return -ENODEV;

	for (i = 0; i < nsub; i++) {
		pd[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR_OR_NULL(pd[i])) {
			ret = IS_ERR(pd[i]) ? PTR_ERR(pd[i]) : -ENODEV;
			pd[i] = NULL;
			goto out;
		}
		link[i] = device_link_add(dev, pd[i], DL_FLAG_STATELESS |
					  DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!link[i]) {
			ret = -EINVAL;
			goto out;
		}
	}
	regmap_read(map, H17_PS_CPU, &word);
	h17_log("domains raised ps_cpu=%#x actual=%#x (B1/B2 clock+power votes NOT done)\n",
		word, FIELD_GET(H17_PS_ACTUAL, word));

	eng = of_iomap(ane, 0);
	if (!eng) {
		ret = -ENOMEM;
		goto out;
	}
	pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);
	if (!pw) {
		ret = -ENOMEM;
		goto out_unmap;
	}

	/*
	 * C1-C11: AppleANETunableApplyFunction (family 0xe0, online
	 * variant). Steps and values from
	 * receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md.
	 */
	{
		u32 s, x, off;
		u64 r;

		v = readl(eng + 0x18a4010) & 3;
		s = readl(eng + 0x18a4018);
		h17_log("C1 0x18a4010 v=%u  C2 0x18a4018 s=%#x\n", v, s);
		if (((v ^ 3) & s) != 0) {
			h17_log("C3 state machine refuses (Apple panics here), abort\n");
			goto skip_c;
		}
		if (v == 3) {
			h17_log("C4 already unlocked, skip C5-C8\n");
		} else {
			writel(v ^ 3, eng + H17_CE_A);
			h17_log("C5 0x18a4000 := %#x, now %#x\n", v ^ 3,
				readl(eng + H17_CE_A));
			h17_poll_mmio(eng, H17_CE_B, v ^ 3, 3, "0x18a4008");
			h17_log("C6 0x18a4008=%#x\n", readl(eng + H17_CE_B));
			r = readq(eng + 0x18a4018);
			h17_log("C7 0x18a4018=%#llx\n", r);
			writeq(v ^ 3, eng + H17_CE_B);
			h17_log("C8 0x18a4008 := %#x (64-bit), now %#llx\n", v ^ 3,
				readq(eng + H17_CE_B));
		}

		for (i = 0; i < 34; i++) {
			off = 0x11378b8 + 0xc * i;
			x = readl(eng + off);
			writel(x | 0x100000, eng + off);
		}
		off = 0x1137a50;
		x = readl(eng + off);
		writel(x | 0x200000, eng + off);
		h17_log("C9 34 regs OR 0x100000 + 0x1137a50 OR 0x200000 done\n");

		{
			static const u32 c10[][2] = {
				{ 0x1137a5c, 0x100000 }, { 0x1137a74, 0x100000 },
				{ 0x1137a8c, 0x100000 }, { 0x1137aa4, 0x100000 },
				{ 0x1137abc, 0x280000 },
			};
			static const u32 c11[][2] = {
				{ 0x1137a68, 0x100000 }, { 0x1137a80, 0x100000 },
				{ 0x1137a98, 0x100000 }, { 0x1137ab0, 0x100000 },
				{ 0x1137ac8, 0x200000 },
			};
			unsigned int k;

			for (k = 0; k < ARRAY_SIZE(c10); k++) {
				x = readl(eng + c10[k][0]);
				writel(x | c10[k][1], eng + c10[k][0]);
			}
			h17_log("C10 secure_lock_wr_dis done\n");
			for (k = 0; k < ARRAY_SIZE(c11); k++) {
				x = readl(eng + c11[k][0]);
				writel(x | c11[k][1], eng + c11[k][0]);
			}
			h17_log("C11 secure_lock_lock done\n");
		}
		h17_log("C-block complete 0x1137ac8=%#x\n", readl(eng + 0x1137ac8));
	}
skip_c:

	/* D1-D7 */
	h17_log("D1 PS 0xc000 := 0xf\n");
	regmap_write(map, H17_PS_CPU, 0xf);
	regmap_read(map, H17_PS_CPU, &word);
	h17_log("D2 0xc000=%#x\n", word);

	h17_log("D4 PWGATE 0x268c := 0\n");
	writel(0, pw + H17_PWGATE_OFF_A);
	h17_log("D5 0x268c=%#x\n", readl(pw + H17_PWGATE_OFF_A));

	h17_log("D6 PS 0xc000 := 0x1000000f\n");
	regmap_write(map, H17_PS_CPU, 0x1000000f);
	regmap_read(map, H17_PS_CPU, &word);
	h17_log("D7 0xc000=%#x\n", word);

	/* E1 */
	h17_log("E1 PS 0xc008 := 0xf\n");
	regmap_write(map, H17_PS_CPU2, 0xf);
	regmap_read(map, H17_PS_CPU2, &word);
	h17_log("E1 0xc008=%#x\n", word);

	/* G1-G8 */
	for (i = 0; i < 8; i++) {
		h17_log("G%u SCRATCH%u := 0 (was %#x)\n", i + 1, i,
			readl(eng + H17_SCRATCH0 + 4 * i));
		writel(0, eng + H17_SCRATCH0 + 4 * i);
	}
	/* G9: SCRATCH6 := 1 */
	h17_log("G9 SCRATCH6 := 1\n");
	writel(1, eng + H17_SCRATCH0 + 4 * 6);
	h17_log("G9 SCRATCH6 now %#x\n", readl(eng + H17_SCRATCH0 + 4 * 6));
	/* G10-G11 */
	h17_log("G10 SCRATCH7 := 1, now %#x\n",
		readl(eng + H17_SCRATCH0 + 4 * 7));
	writel(0, eng + H17_SCRATCH0 + 4 * 7);
	h17_log("G11 SCRATCH7 := 0, now %#x\n",
		readl(eng + H17_SCRATCH0 + 4 * 7));

	/* G13-G14: RVBAR */
	rv = readq(eng + H17_RVBAR);
	h17_log("G13 RVBAR=%#llx\n", rv);
	h17_log("G14 RVBAR := %#llx\n", (rv & H17_RVBAR_KEEP) | H17_RVBAR_SET);
	writeq((rv & H17_RVBAR_KEEP) | H17_RVBAR_SET, eng + H17_RVBAR);
	h17_log("G14 RVBAR now %#llx\n", readq(eng + H17_RVBAR));

	/* G16-G17 */
	h17_log("G16 CPU_CONTROL := 0\n");
	writel(0, eng + H17_CPU_CONTROL);
	h17_log("G16 now %#x status %#x\n", readl(eng + H17_CPU_CONTROL),
		readl(eng + H17_CPU_STATUS));
	h17_log("G17 CPU_CONTROL := 0x10\n");
	writel(0x10, eng + H17_CPU_CONTROL);
	h17_log("G17 now %#x status %#x\n", readl(eng + H17_CPU_CONTROL),
		readl(eng + H17_CPU_STATUS));

	h17_log("sequence complete, status %#x\n", readl(eng + H17_CPU_STATUS));

	/*
	 * Observation window. The firmware, if it is running, speaks first:
	 * either a legacy SCRATCH7 publication or RTKit traffic on the
	 * mailboxes. Log every change rather than a fixed final sample.
	 */
	{
		u32 st, sc7, a2i, i2a;
		u32 p_st = ~0u, p_sc7 = ~0u, p_a2i = ~0u, p_i2a = ~0u;

		for (i = 0; i < 300; i++) {
			st = readl(eng + H17_CPU_STATUS);
			sc7 = readl(eng + H17_SCRATCH0 + 4 * 7);
			a2i = readl(eng + 0x1608110);
			i2a = readl(eng + 0x1608114);
			if (st != p_st || sc7 != p_sc7 || a2i != p_a2i ||
			    i2a != p_i2a) {
				h17_log("poll %u status=%#x sc7=%#x a2i=%#x i2a=%#x ctrl=%#x\n",
					i, st, sc7, a2i, i2a,
					readl(eng + H17_CPU_CONTROL));
				p_st = st; p_sc7 = sc7;
				p_a2i = a2i; p_i2a = i2a;
			}
			msleep(10);
		}
		h17_log("observation done status=%#x sc7=%#x a2i=%#x i2a=%#x\n",
			readl(eng + H17_CPU_STATUS),
			readl(eng + H17_SCRATCH0 + 4 * 7),
			readl(eng + 0x1608110), readl(eng + 0x1608114));
	}

	writel(0, eng + H17_CPU_CONTROL);
	iounmap(pw);
out_unmap:
	iounmap(eng);
out:
	for (i = nsub; i-- > 0; ) {
		if (link[i])
			device_link_del(link[i]);
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	h17_log("released\n");
	of_node_put(ane);
	return ret;
}

static const struct of_device_id ane_h17_full_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_full_of_match);

static struct platform_driver ane_h17_full_driver = {
	.probe = ane_h17_full_probe,
	.driver = {
		.name = "ane_h17_full_probe",
		.of_match_table = ane_h17_full_of_match,
	},
};

static int __init __maybe_unused ane_h17_full_init(void)
{
	struct device_node *ane, *probe;
	int ret = -EPERM;

	if (!optin || !of_machine_is_compatible("apple,j700"))
		goto refused;
	ane = of_find_node_by_path(H17_ANE_NODE);
	probe = of_find_compatible_node(NULL, NULL, H17_VOTE_NODE);
	if (!ane || !probe || of_device_is_available(ane) ||
	    !of_device_is_available(probe) ||
	    !of_device_is_compatible(ane, "apple,t8140-ane") ||
	    !ane_h17_darts_disabled()) {
		ret = -EINVAL;
		goto put_nodes;
	}
	ret = 0;
put_nodes:
	of_node_put(probe);
	of_node_put(ane);
	if (ret)
		goto refused;

	logf = filp_open(H17_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
			 0644);
	if (IS_ERR(logf)) {
		pr_err("ane_h17_full cannot open %s: %ld\n",
		       H17_LOG_PATH, PTR_ERR(logf));
		logf = NULL;
	}
	return platform_driver_register(&ane_h17_full_driver);
refused:
	pr_err("ane_h17_full RESULT verdict=REFUSED reason=board-optin-or-dt error=%d\n",
	       ret);
	return ret;
}

static void __exit ane_h17_full_exit(void)
{
	platform_driver_unregister(&ane_h17_full_driver);
	if (!IS_ERR_OR_NULL(logf)) {
		vfs_fsync(logf, 1);
		filp_close(logf, NULL);
	}
}

/* Historical experiment: C9-C11 confuse descriptor file offsets with MMIO.
 * Keep the source for receipt reproducibility, but never execute this path.
 */
static int __init ane_h17_full_refuse(void)
{
	pr_err("ane_h17_full REFUSED: invalid AF descriptor decoding; use reviewed AF scan\n");
	return -EOPNOTSUPP;
}
module_init(ane_h17_full_refuse);
module_exit(ane_h17_full_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 Apple bring-up order probe (family 0xe0)");
