// ane_dartpeek.c — Gap8 EXP-2: read-only ANE dart register peek (H167 pattern)
// One insmod reads a fixed word set from the three T6001 ANE darts.
// Every MMIO address is pre-logged BEFORE the read so a hostile access
// pins itself on netconsole. Reads only; pm_runtime_get_sync wakes the
// dart through the driver's own runtime-PM callbacks (H167 precedent:
// 42 reads, no reset, no gate refusals on T8103).
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/io.h>
#include <linux/device.h>

static const char * const darts[] = {
	"285800000.iommu", "285810000.iommu", "285820000.iommu",
};

/* T8020/T6000 real register set (H167 word list) + t8110-family shadow
 * offsets (where the t8110-variant driver's writes actually land on
 * these blocks). */
static const unsigned long offs[] = {
	0x00, 0x04,			/* PARAMS1/2 */
	0x40, 0x50, 0x54,		/* ERROR, ERRADDR_LO/HI */
	0x60,				/* CONFIG */
	0x100, 0x104,			/* TCR[0..1] */
	0x200, 0x204, 0x208, 0x20c,	/* TTBR[0][0..3] */
	0x1000, 0x1004,			/* t8110 TCR shadow */
	0x1100, 0x1104, 0x1108, 0x110c,	/* t8110 TTBR shadow */
};

static void peek_one(struct device *dev, const char *name)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *res;
	void __iomem *base;
	unsigned int i;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_info("dartpeek: %s: no MEM resource\n", name);
		return;
	}
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		pr_info("dartpeek: %s: pm_runtime_get_sync rc=%d (reading anyway)\n",
			name, ret);
		pm_runtime_put_noidle(dev);
	}
	base = ioremap_np(res->start, resource_size(res));
	if (!base) {
		pr_info("dartpeek: %s: ioremap_np failed\n", name);
		if (ret >= 0)
			pm_runtime_put_sync(dev);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(offs); i++) {
		pr_info("dartpeek: %s pre-read +0x%03lx\n", name, offs[i]);
		pr_info("dartpeek: %s +0x%03lx = 0x%08x\n",
			name, offs[i], readl(base + offs[i]));
	}
	iounmap(base);
	if (ret >= 0)
		pm_runtime_put_sync(dev);
	pr_info("dartpeek: %s done\n", name);
}

static int __init dartpeek_init(void)
{
	unsigned int d;

	for (d = 0; d < ARRAY_SIZE(darts); d++) {
		struct device *dev = bus_find_device_by_name(
			&platform_bus_type, NULL, darts[d]);

		if (!dev) {
			pr_info("dartpeek: %s: not found\n", darts[d]);
			continue;
		}
		peek_one(dev, darts[d]);
		put_device(dev);
	}
	return 0;
}
module_init(dartpeek_init);

static void __exit dartpeek_exit(void) { }
module_exit(dartpeek_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gap8 read-only ANE dart register peek (H167 pattern)");
