// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17: Apple bring-up order, then RTKit receive/reply.
 *
 * Derived from the H16 ANE driver family the host OS ships (family 0xe0, the
 * group the driver maps ADT ane-type 0x200 onto) and
 * the T8140 ANE HAL. See
 * receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md
 * for the instruction addresses.
 *
 * Apple's order, which differs from everything this repo has done so far:
 *
 *   B1/B2  provider clock and power enable calls           (NOT done here:
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
 * Every step is fsync'd to /var/log/h17-hal.log before the next, so
 * a freeze names the exact operation. The run is expected to be
 * informative whichever step it dies on.
 *
 * Usage:
 *   sudo insmod ane_h17_hal_probe.ko optin=1
 *   cat /var/log/h17-hal.log
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
#define H17_LOG_PATH "/var/log/h17-hal.log"

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

static char *log_path;
module_param(log_path, charp, 0400);
MODULE_PARM_DESC(log_path, "absolute durable log path supplied by the launcher");

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

static int ane_h17_hal_probe(struct platform_device *pdev)
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

	/* C5-C8: the first ANE MMIO Apple performs. */
	h17_log("C5 read CE_A(%#x)\n", H17_CE_A);
	v = readl(eng + H17_CE_A);
	h17_log("C5 CE_A was %#x, writing %#x\n", v, v ^ 3);
	writel(v ^ 3, eng + H17_CE_A);
	h17_log("C5 CE_A now %#x\n", readl(eng + H17_CE_A));
	h17_log("C6 read CE_B(%#x) = %#x\n", H17_CE_B, readl(eng + H17_CE_B));

/* T8140 ANE HAL online tunables: figs_1 (35) + figs_4 (2).
 * kernelcache 7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe
 * new = (live & ~mask) | (value & mask), then width-4 store.
 */
static const u32 ane_hal_tunables[][3] = {
	{ 0x00000000, 0x800000ff, 0x8000001b },
	{ 0x00000004, 0x80ff00ff, 0x800c000d },
	{ 0x00000008, 0x80ff00ff, 0x800c000d },
	{ 0x0000000c, 0x80ff00ff, 0x800c000d },
	{ 0x00000010, 0x80ff00ff, 0x80000003 },
	{ 0x00000014, 0x80ff00ff, 0x80000003 },
	{ 0x00000018, 0x800000ff, 0x80000013 },
	{ 0x0000001c, 0x800000ff, 0x80000020 },
	{ 0x00000020, 0x80ff00ff, 0x8012000c },
	{ 0x00000024, 0x80ff00ff, 0x80180006 },
	{ 0x00000028, 0x80ff00ff, 0x80070003 },
	{ 0x0000002c, 0x80ff00ff, 0x80070003 },
	{ 0x00000030, 0x800000ff, 0x80000015 },
	{ 0x00000034, 0x800000ff, 0x80000011 },
	{ 0x00000038, 0x800000ff, 0x80000011 },
	{ 0x00000100, 0x000000ff, 0x000000ff },
	{ 0x00000108, 0x00000006, 0x00000000 },
	{ 0x00000304, 0x3f3f0000, 0x02020000 },
	{ 0x00000314, 0x80000000, 0x80000000 },
	{ 0x0000031c, 0x80000000, 0x80000000 },
	{ 0x00000324, 0x80000000, 0x80000000 },
	{ 0x00000408, 0xffff0020, 0x00120020 },
	{ 0x00000700, 0x00000400, 0x00000400 },
	{ 0x00000748, 0x07ff07ff, 0x00200030 },
	{ 0x00000800, 0x00000400, 0x00000400 },
	{ 0x00000848, 0x07ff07ff, 0x00100030 },
	{ 0x00000900, 0x00000400, 0x00000400 },
	{ 0x00000948, 0x07ff07ff, 0x01000100 },
	{ 0x00000b00, 0xffffffff, 0x7fffffff },
	{ 0x00000b04, 0x00000100, 0x00000100 },
	{ 0x00004008, 0x00000001, 0x00000000 },
	{ 0x0000400c, 0x00000001, 0x00000000 },
	{ 0x00004014, 0x00000001, 0x00000000 },
	{ 0x00004018, 0x00000001, 0x00000000 },
	{ 0x0000401c, 0x00000001, 0x00000001 },
	{ 0x00000338, 0xffffffff, 0x00050020 },
	{ 0x0000033c, 0xffffffff, 0x000a0030 },
};

	/* Apple's HAL tunables, applied before any PS/PWGATE work. */
	for (i = 0; i < ARRAY_SIZE(ane_hal_tunables); i++) {
		u32 off = ane_hal_tunables[i][0];
		u32 mask = ane_hal_tunables[i][1];
		u32 val = ane_hal_tunables[i][2];
		u32 live;

		h17_log("HAL BEFORE off=%#x mask=%#x val=%#x\n", off, mask, val);
		live = readl(eng + off);
		writel((live & ~mask) | (val & mask), eng + off);
		h17_log("HAL AFTER  off=%#x live=%#x now=%#x\n", off, live,
			readl(eng + off));
	}
	h17_log("HAL tunables done count=%u\n",
		(unsigned int)ARRAY_SIZE(ane_hal_tunables));

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
	 * Mailbox receive. The previous run watched only the control words,
	 * never I2A_R0/R1, so "silent" was never actually tested. ASC mailbox
	 * layout and protocol per rtkit/h14_rtkit_hello.py:
	 *   A2I_CTRL 0x110 bit16 = full, I2A_CTRL 0x114 bit17 = empty
	 *   A2I_S0 0x800 lo/hi, A2I_S1 0x808 ep
	 *   I2A_R0 0x830 lo/hi, I2A_R1 0x838 ep
	 */
	{
		u32 ctl, lo, hi, ep, t, n = 0;
		u64 m;

		for (i = 0; i < 400; i++) {
			ctl = readl(eng + 0x1608114);
			if ((ctl >> 17) & 1) {
				msleep(5);
				continue;
			}
			lo = readl(eng + 0x1608830);
			hi = readl(eng + 0x1608834);
			ep = readl(eng + 0x1608838) & 0xff;
			m = lo | ((u64)hi << 32);
			t = (m >> 52) & 0xff;
			h17_log("RX ep=%#x type=%#x msg=%#llx i2a_ctrl=%#x\n",
				ep, t, m, ctl);
			if (ep == 0 && t == 1) {
				u32 mn = m & 0xffff, mx = (m >> 16) & 0xffff;
				u32 want = mx > 12 ? 12 : mx;
				u64 reply = ((u64)2 << 52) | ((u64)want << 16) | want;
				h17_log("HELLO min=%u max=%u replying version %u\n",
					mn, mx, want);
				if (!((readl(eng + 0x1608110) >> 16) & 1)) {
					writel((u32)reply, eng + 0x1608800);
					writel((u32)(reply >> 32), eng + 0x1608804);
					writel(0, eng + 0x1608808);
					h17_log("TX HELLO_REPLY done\n");
				} else {
					h17_log("TX skipped: A2I full\n");
				}
			}
			n++;
			if (n >= 8)
				break;
		}
		h17_log("mailbox done rx=%u i2a_ctrl=%#x a2i_ctrl=%#x\n", n,
			readl(eng + 0x1608114), readl(eng + 0x1608110));
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

static const struct of_device_id ane_h17_hal_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_hal_of_match);

static struct platform_driver ane_h17_hal_driver = {
	.probe = ane_h17_hal_probe,
	.driver = {
		.name = "ane_h17_hal_probe",
		.of_match_table = ane_h17_hal_of_match,
	},
};

static int __init ane_h17_hal_init(void)
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

	if (!log_path || log_path[0] != '/') {
		pr_err("ane_h17_hal REFUSED: log_path must be absolute\n");
		return -EINVAL;
	}
	logf = filp_open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
			 0644);
	if (IS_ERR(logf)) {
		pr_err("ane_h17_hal cannot open %s: %ld\n",
		       H17_LOG_PATH, PTR_ERR(logf));
		logf = NULL;
	}
	return platform_driver_register(&ane_h17_hal_driver);
refused:
	pr_err("ane_h17_hal RESULT verdict=REFUSED reason=board-optin-or-dt error=%d\n",
	       ret);
	return ret;
}

static void __exit ane_h17_hal_exit(void)
{
	platform_driver_unregister(&ane_h17_hal_driver);
	if (!IS_ERR_OR_NULL(logf)) {
		vfs_fsync(logf, 1);
		filp_close(logf, NULL);
	}
}

module_init(ane_h17_hal_init);
module_exit(ane_h17_hal_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 Apple bring-up order probe (family 0xe0)");
