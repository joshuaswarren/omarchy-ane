// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Read-only engine sample; genpd acquisition/release changes power state.
 * Provenance and limits: prepare-af-scan.md. No CE or firmware writes.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/io.h>
#include <linux/fs.h>

#define ANE "/soc/ane@400000000"
#define PMGR "/soc/power-management@300700000"
#define VOTE "omarchy,t8140-ane-cpu-vote"
static bool optin;
module_param(optin, bool, 0400);
static char *log_path;
module_param(log_path, charp, 0400);
static struct file *logfile;
static bool attempted;
static int result = -ENODEV;
/* Map 0x71 = this window; group = +0x1000. _printClocks derives
 * offset = group + {0x100 clock | 0x2c power} + stride*n, stride 0x10/0x1c/0x20.
 * Read the group header and the first 16 clock-gate slots.
 */
static const u32 offsets[] = {
	0x1000, 0x1004, 0x1008, 0x100c, 0x1010, 0x1014, 0x1018, 0x101c,
	0x1100, 0x1104, 0x1108, 0x110c, 0x1110, 0x1114, 0x1118, 0x111c,
	0x1120, 0x1124, 0x1128, 0x112c, 0x1130, 0x1134, 0x1138, 0x113c };
#define CLK_PA 0x3082c8000ULL
#define CLK_SIZE 0x4000

static int record(const char *phase, u32 offset, u32 value)
{
	char buf[160];
	int len = scnprintf(buf, sizeof(buf), "%s offset=%#x value=%#x\n",
			    phase, offset, value);
	ssize_t n = kernel_write(logfile, buf, len, &logfile->f_pos);
	if (n != len)
		return n < 0 ? n : -EIO;
	return vfs_fsync(logfile, 0);
}

static int scan(struct platform_device *pdev)
{
	struct device *pd[3] = { };
	bool active[3] = { };
	struct device_node *ane = NULL, *pmgr;
	struct resource res;
	struct regmap *map;
	void __iomem *base = NULL;
	u32 word;
	int i, ret;

	if (attempted)
		return -EPERM;
	attempted = true;
	ret = record("BEGIN", 0, 0);
	if (ret)
		goto done;
	if (of_count_phandle_with_args(pdev->dev.of_node, "power-domains",
				      "#power-domain-cells") != 3) {
		ret = -EINVAL;
		goto done;
	}
	ane = of_find_node_by_path(ANE);
	if (!ane || of_device_is_available(ane) ||
	    !of_device_is_compatible(ane, "apple,t8140-ane")) {
		ret = -ENODEV;
		goto done;
	}
	/* Apple declares six ANE resources; Linux maps only the first.
	 * Resource 5 is not in the DT at all, so it is used by its ADT/
	 * ioreg physical address and verified against the engine window.
	 */
	ret = of_address_to_resource(ane, 0, &res);
	if (ret)
		goto done;
	if (res.start != 0x400000000ULL || resource_size(&res) != 0x2000000) {
		ret = -EINVAL;
		goto done;
	}
	res.start = CLK_PA;
	res.end = CLK_PA + CLK_SIZE - 1;
	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		if ((offsets[i] & 3) || offsets[i] > CLK_SIZE - 4) {
			ret = -EINVAL;
			goto done;
		}
	pmgr = of_find_node_by_path(PMGR);
	if (!pmgr) {
		ret = -ENODEV;
		goto done;
	}
	map = syscon_node_to_regmap(pmgr);
	of_node_put(pmgr);
	if (IS_ERR(map)) {
		ret = PTR_ERR(map);
		goto done;
	}
	for (i = 0; i < 3; i++) {
		ret = record("BEFORE_DOMAIN", i, 0);
		if (ret)
			goto done;
		pd[i] = dev_pm_domain_attach_by_id(&pdev->dev, i);
		if (IS_ERR_OR_NULL(pd[i])) {
			ret = pd[i] ? PTR_ERR(pd[i]) : -ENODEV;
			pd[i] = NULL;
			goto done;
		}
		ret = pm_runtime_resume_and_get(pd[i]);
		if (ret < 0)
			goto done;
		active[i] = true;
		ret = regmap_read(map, 0xc000 + 8 * i, &word);
		if (ret)
			goto done;
		ret = record("DOMAIN", 0xc000 + 8 * i, word);
		if (ret)
			goto done;
		if ((word & 0xf0) != 0xf0) {
			ret = -EAGAIN;
			goto done;
		}
	}
	ret = regmap_read(map, 0x290, &word);
	if (ret)
		goto done;
	if ((word & 0xf0) != 0xf0) {
		ret = -EAGAIN;
		goto done;
	}
	base = ioremap(res.start, resource_size(&res));
	if (!base) {
		ret = -ENOMEM;
		goto done;
	}
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		ret = record("BEFORE_READ", offsets[i], 0);
		if (ret)
			goto done;
		word = readl(base + offsets[i]);
		ret = record("CLK_READ", offsets[i], word);
		if (ret)
			goto done;
	}
done:
	if (base)
		iounmap(base);
	for (i = 2; i >= 0; i--) {
		int err;
		if (active[i]) {
			err = pm_runtime_put_sync(pd[i]);
			if (!ret && err < 0)
				ret = err;
		}
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	of_node_put(ane);
	if (!ret)
		ret = record("COMPLETE", 0, 0);
	else
		record("FAILED", 0, -ret);
	result = ret;
	return ret;
}

static const struct of_device_id matches[] = {
	{ .compatible = VOTE }, { }
};
static struct platform_driver driver = {
	.probe = scan,
	.driver = { .name = "ane_h17_clk_scan", .of_match_table = matches,
		    .probe_type = PROBE_FORCE_SYNCHRONOUS },
};

static int __init start(void)
{
	static const char *const darts[] = {
		"/soc/iommu@401800000", "/soc/iommu@401820000",
		"/soc/iommu@401840000",
	};
	int i, ret;
	if (!optin || !of_machine_is_compatible("apple,j700") ||
	    !log_path || log_path[0] != '/')
		return -EPERM;
	for (i = 0; i < ARRAY_SIZE(darts); i++) {
		struct device_node *n = of_find_node_by_path(darts[i]);
		bool ok = n && !of_device_is_available(n) &&
			of_device_is_compatible(n, "apple,t8140-dart");
		of_node_put(n);
		if (!ok)
			return -ENODEV;
	}
	/* Launcher must create and sync this file and its parent first. */
	logfile = filp_open(log_path, O_WRONLY | O_APPEND | O_SYNC | O_NOFOLLOW, 0);
	if (IS_ERR(logfile))
		return PTR_ERR(logfile);
	if (!S_ISREG(file_inode(logfile)->i_mode)) {
		ret = -EINVAL;
		goto close;
	}
	ret = platform_driver_register(&driver);
	if (!ret && result) {
		platform_driver_unregister(&driver);
		ret = result;
	}
	if (!ret)
		return 0;
close:
	filp_close(logfile, NULL);
	return ret;
}
static void __exit stop(void)
{
	platform_driver_unregister(&driver);
	filp_close(logfile, NULL);
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 ANE clock-window read sample with checked durable logging");
