// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T8140/J700 stage 0: inspect the boot ADT in reserved RAM. This module
 * never maps ANE registers, enables a power domain or submits a task.
 * omarchy-ane 063ae8e data/ane-soc/t8140.json (A1, F1) pins ane-type
 * and firmware layout;
 * aurora-linux 68df87370aba t8140.dtsi provides the reserved ADT node.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "../h15/ane_h15_adt.h"

#define H17_ANE_TYPE		0x200
#define H17_TEXT_SIZE		0xc0000
#define H17_DATA_SIZE		0x2ac000
#define H17_ADT_MAX_SIZE	(2 * 1024 * 1024)

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 for the read-only T8140/J700 ADT stage");

static int __init ane_h17_adt_probe_init(void)
{
	struct device_node *reserved, *child;
	struct ane_h15_adt_node ane_node = {};
	struct ane_h15_adt adt;
	struct ane_h15_seg segs[2] = {};
	void *mapped;
	const u8 *raw;
	struct resource res;
	u32 raw_size = 0;
	int ret;

	if (!optin || !of_machine_is_compatible("apple,j700")) {
		pr_err("ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=REFUSED reason=board-or-optin\n");
		return -EPERM;
	}

	reserved = of_find_node_by_name(NULL, "reserved-memory");
	if (!reserved) {
		ret = -ENOENT;
		goto no_region;
	}
	for_each_child_of_node(reserved, child) {
		const char *label;

		if (of_property_read_string(child, "label", &label) ||
		    strcmp(label, "adt"))
			continue;
		ret = of_address_to_resource(child, 0, &res);
		of_node_put(child);
		if (ret)
			goto put_reserved;
		goto found;
	}
	ret = -ENOENT;
	goto put_reserved;

found:
	if (!resource_size(&res) || resource_size(&res) > H17_ADT_MAX_SIZE) {
		ret = -EINVAL;
		goto put_reserved;
	}
	adt.len = resource_size(&res);
	mapped = memremap(res.start, adt.len, MEMREMAP_WB);
	if (!mapped) {
		ret = -ENOMEM;
		goto put_reserved;
	}
	adt.b = mapped;

	ret = ane_h15_adt_find_ane(&adt, H17_ANE_TYPE, &ane_node);
	if (ret) {
		pr_info("ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=REFUSED reason=ane-node-or-format code=%d\n", ret);
		goto unmap;
	}
	ret = ane_h15_adt_prop(&adt, ane_node, "segment-ranges",
				&raw, &raw_size);
	if (ret == ANE_H15_ENOENT) {
		pr_info("ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=PASS reason=no-segment-ranges\n");
		ret = 0;
		goto unmap;
	}
	if (ret)
		goto malformed;

	if (raw_size != 64) {
		ret = ANE_H15_EFORMAT;
		goto malformed;
	}
	{
		size_t off = raw - adt.b;
		unsigned int i;

		for (i = 0; i < 8; i++)
			pr_info("ane_h17_adt_probe segment-word[%u]=%#llx\n", i,
				(unsigned long long)ane_h15_adt_rd64(&adt, off + 8 * i));
	}
	ret = ane_h15_adt_segments(&adt, ane_node, segs, 0, 0);
	if (ret)
		goto malformed;
	/* ADT segment-ranges word 3 packs the byte count in the low 32
	 * bits and flags in the high 32. On this J700 boot TEXT has flag 1.
	 * The H15 helper preserves the whole word, so split it here. */
	pr_info("ane_h17_adt_probe TEXT flags=%#llx DATA flags=%#llx\n",
		(unsigned long long)(segs[0].size >> 32),
		(unsigned long long)(segs[1].size >> 32));
	segs[0].size &= 0xffffffffull;
	segs[1].size &= 0xffffffffull;
	pr_info("ane_h17_adt_probe TEXT phys=%#llx iova=%#llx size=%#llx\n",
		(unsigned long long)segs[0].phys,
		(unsigned long long)segs[0].iova,
		(unsigned long long)segs[0].size);
	pr_info("ane_h17_adt_probe DATA phys=%#llx iova=%#llx size=%#llx\n",
		(unsigned long long)segs[1].phys,
		(unsigned long long)segs[1].iova,
		(unsigned long long)segs[1].size);
	if (segs[0].size != H17_TEXT_SIZE ||
	    segs[1].size < H17_DATA_SIZE) {
		pr_info("ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=PASS reason=unexpected-segment-size\n");
		ret = 0;
		goto unmap;
	}
	pr_info("ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=PASS reason=segments\n");
	ret = 0;
	goto unmap;

malformed:
	pr_err("ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=REFUSED reason=segment-format code=%d bytes=%u\n",
	       ret, raw_size);
unmap:
	memunmap(mapped);
put_reserved:
	of_node_put(reserved);
no_region:
	if (ret < 0)
		pr_err("ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=REFUSED reason=adt-region error=%d\n", ret);
	return ret > 0 ? -EINVAL : ret;
}

static void __exit ane_h17_adt_probe_exit(void)
{
}

module_init(ane_h17_adt_probe_init);
module_exit(ane_h17_adt_probe_exit);

MODULE_DESCRIPTION("Opt-in read-only T8140 boot ADT ANE segment probe");
MODULE_LICENSE("Dual MIT/GPL");
