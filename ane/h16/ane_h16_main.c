// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h16 — OPT-IN EXPERIMENTAL bring-up module for the H16/H17-family
 * ANE (the Apple 27.0 kext generation: T8132, T6040, T6041; H17 rows
 * live in ane/h17/). No silicon has run this module. It registers no
 * DRM device, performs no inference and sends no CSNE command.
 *
 * Three stages, selected by the "stage" module parameter:
 *
 *   stage=dt: parse the running DT (engine/pmgr windows from the soc
 *   row, ps word offsets, the firmware pin) and print the word groups
 *   with their evidence tier. No MMIO access of any kind: the first
 *   run on a volunteer machine is hardware-silent.
 *
 *   stage=status (default): enable the ANE power domains named in the
 *   device node, wait until every pmgr ANE state word reads ACTUAL=0xf
 *   (the standing rule from the T6021 program: never touch the engine
 *   window before that), then log RVBAR, CPU_STATUS, SCRATCH0..7 and
 *   the mailbox control words. Read-only apart from the power domains.
 *
 *   stage=boot (EXPERIMENTAL, firmware-boot smoke only): everything
 *   "status" does, plus: parse the boot ADT from the reserved-memory
 *   phram "adt" region, read /arm-io/aneX segment-ranges, validate the
 *   iBoot-preloaded firmware against the pinned 27.0 payload (only
 *   differences inside the _rtk_patchbay and _rtk_tunables sections are
 *   accepted, plus at most one entry/DATA-base-looking u64 per segment),
 *   copy the validated image into a DART-mapped coherent buffer at the
 *   same IOVAs the ADT names, write SCRATCH7=0, latch or write RVBAR,
 *   release the CPU (CPU_CONTROL 0 then 0x10) and poll SCRATCH7 for the
 *   wake word 0x08042006 (kext ANE_Init, macOS 27.0). If the firmware
 *   wakes, poll the ASC mailbox for an RTKit HELLO, answer it, ack
 *   EPMAP and STARTEP the system endpoints. Then stop: no channel-
 *   manager publication and no CSNE CONFIG_GET — the 27.0 host contract
 *   for those is not derived yet (receipts/2026-10-03-ane-h16).
 *
 * The module refuses to probe unless its opt-in key is set:
 *   insmod ane_h16.ko optin=t8132 stage=status
 * There is no MODULE_DEVICE_TABLE, so nothing autoloads it.
 *
 * After stage=boot starts the firmware the module must not be unloaded;
 * remove() refuses and the machine needs a reboot to park the ANE again.
 */
#include <crypto/sha2.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ane_h16.h"

static char *optin;
module_param(optin, charp, 0444);
MODULE_PARM_DESC(optin,
		 "Opt-in key: must equal the SoC row name (t8132, t6040, t6041, ...) or probe returns -EPERM.");

static char *stage = "status";
module_param(stage, charp, 0444);
MODULE_PARM_DESC(stage,
		 "dt/0: parse the DT and print word groups, no MMIO. status/1 (default): power up and log registers only. boot/3: firmware-boot smoke (EXPERIMENTAL). 2 is not an H16 module stage (the ladder's fw-pin step runs in userspace).");

static unsigned int ps_wait_ms = 500;
module_param(ps_wait_ms, uint, 0444);
MODULE_PARM_DESC(ps_wait_ms,
		 "Timeout waiting for each pmgr ANE state to read ACTUAL=0xf (default 500).");

static unsigned int boot_wait_ms = 3000;
module_param(boot_wait_ms, uint, 0444);
MODULE_PARM_DESC(boot_wait_ms,
		 "Timeout waiting for the SCRATCH7 wake word after CPU release (default 3000).");

static unsigned int hello_wait_ms;
module_param(hello_wait_ms, uint, 0444);
MODULE_PARM_DESC(hello_wait_ms,
		 "After the wake word, poll the ASC mailbox for an RTKit HELLO this long (default 0 = skip; 1000 is the lab value).");

#define ANE_H16_WAKE_ACK		0x08042006u
#define ANE_H16_CPU_RUN_RELEASE		0x10u
/* RVBAR fold: kext ANE_Init 27.0 writes base | (iova & mask); the base
 * supplies bit 0 (valid), the mask clears bits 0-10, 48 and 55. */
#define ANE_H16_RVBAR_ENTRY_BASE	0x0081000000000001ull
#define ANE_H16_RVBAR_ADDR_MASK		0xff7efffffffff800ull

/* ASC mailbox v4 block at soc->mbox (drivers/soc/apple/mailbox.c). */
#define ANE_H16_MBOX_A2I_CTRL		0x110
#define ANE_H16_MBOX_I2A_CTRL		0x114
#define ANE_H16_MBOX_A2I_SEND0		0x800
#define ANE_H16_MBOX_A2I_SEND1		0x808
#define ANE_H16_MBOX_I2A_RECV0		0x830
#define ANE_H16_MBOX_I2A_RECV1		0x838
#define ANE_H16_MBOX_CTRL_FULL		BIT(16)
#define ANE_H16_MBOX_CTRL_EMPTY		BIT(17)

/* RTKit management wire format (drivers/soc/apple/rtkit.c): the mailbox
 * carries {payload, endpoint} and the payload's top byte names the
 * management message type. */
#define ANE_H16_EP_MGMT			0x00
#define ANE_H16_EP_CRASHLOG		0x01
#define ANE_H16_EP_SYSLOG		0x02
#define ANE_H16_EP_DEBUG		0x03
#define ANE_H16_EP_IOREPORT		0x04
#define ANE_H16_EP_OSLOG		0x08
#define ANE_H16_EP_TRACEKIT		0x0a
#define ANE_H16_APP_EP_START		0x20
#define ANE_H16_MGMT_TYPE		GENMASK_ULL(59, 52)
#define ANE_H16_MGMT_HELLO		1
#define ANE_H16_MGMT_HELLO_REPLY	2
#define ANE_H16_MGMT_STARTEP		5
#define ANE_H16_MGMT_SET_IOP_PWR_STATE	6
#define ANE_H16_MGMT_SET_IOP_PWR_STATE_ACK 7
#define ANE_H16_MGMT_EPMAP		8
#define ANE_H16_MGMT_EPMAP_REPLY	8
#define ANE_H16_MGMT_HELLO_MINVER	GENMASK_ULL(15, 0)
#define ANE_H16_MGMT_HELLO_MAXVER	GENMASK_ULL(31, 16)
#define ANE_H16_MGMT_EPMAP_LAST		BIT_ULL(51)
#define ANE_H16_MGMT_EPMAP_BASE		GENMASK_ULL(34, 32)
#define ANE_H16_MGMT_EPMAP_BITMAP	GENMASK_ULL(31, 0)
#define ANE_H16_MGMT_EPMAP_REPLY_MORE	BIT_ULL(0)
#define ANE_H16_MGMT_STARTEP_EP		GENMASK_ULL(39, 32)
#define ANE_H16_MGMT_STARTEP_FLAG	BIT_ULL(1)
#define ANE_H16_MGMT_PWR_STATE		GENMASK_ULL(15, 0)
#define ANE_H16_RTKIT_MAX_VERSION	2

struct ane_h16_seg {
	u64 phys;	/* ADT segment-ranges word 0 */
	u64 iova;	/* the address the ASC sees (remap) */
	u64 size;
};

struct ane_h16 {
	struct device *dev;
	const struct ane_h16_soc *soc;
	void __iomem *engine;
	void __iomem *pmgr;
	struct ane_h16_seg segs[2];
	bool fw_touched;
	bool fw_started;
	bool hello_done;
	void *stage_cpu;
	dma_addr_t stage_dma;
	size_t stage_size;
	struct iommu_domain *domain;
};

/* ---- pmgr state words (the read-only-after-ACTUAL=0xf rule) ---- */

/* apple-pmgr-pwrstate word: TARGET bits 3:0, ACTUAL bits 7:4; 0xf is
 * fully on (ane/t6021/ane_t6021.h ANE_PS_TARGET/ANE_PS_ACTUAL/ANE_PS_ON;
 * docs/t6021-ane-bringup-findings.md section 4 gate). */
#define ANE_H16_PS_ACTUAL	GENMASK(7, 4)
#define ANE_H16_PS_ON		0xf

static const char *const ane_h16_ps_names[] = {
	"ANE_SYS", "ANE_MPM", "ANE_CPU", "ANE_TD", "ANE_BASE",
};

/* ---- RESULT line: the machine-readable record every stage emits,
 * same grammar as ane_h15_main.c ("ane_h16 RESULT stage=%u soc=%s
 * verdict=%s reason=%s"). collect_deep.py scoops these from dmesg and
 * the ladder judges them; every refusal carries one too.
 */

static void ane_h16_result(struct ane_h16 *ane, unsigned int stage_idx,
			   const char *verdict, const char *reason)
{
	if (ane && ane->dev)
		dev_crit(ane->dev,
			 "ane_h16 RESULT stage=%u soc=%s verdict=%s reason=%s\n",
			 stage_idx, ane->soc->name, verdict, reason);
	else
		pr_crit("ane_h16 RESULT stage=%u soc=%s verdict=%s reason=%s\n",
			stage_idx, "?", verdict, reason);
}

/* Module stages: 0=dt, 1=status, 3=boot. 2 is the ladder's fw-pin
 * step, which runs in userspace (omarchy-ane-firmware-fetch), not in
 * this module, so stage=2 is refused as unknown.
 */
static int ane_h16_stage_idx(const char *name, unsigned int *out)
{
	if (!strcmp(name, "dt") || !strcmp(name, "0")) {
		*out = 0;
		return 0;
	}
	if (!strcmp(name, "status") || !strcmp(name, "1")) {
		*out = 1;
		return 0;
	}
	if (!strcmp(name, "boot") || !strcmp(name, "3")) {
		*out = 3;
		return 0;
	}
	return -EINVAL;
}

static bool ane_h16_ps_on(struct ane_h16 *ane, unsigned int i, u32 *v)
{
	*v = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
	return FIELD_GET(ANE_H16_PS_ACTUAL, *v) == ANE_H16_PS_ON;
}

/* Every ANE word must read ACTUAL == 0xf, all at once, before any
 * engine MMIO: each word is polled to 0xf, then all five are read
 * again so a domain that dropped while a later one came up refuses. */
static int ane_h16_ps_wait(struct ane_h16 *ane)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(ps_wait_ms);
	unsigned int i;
	u32 v;

	for (i = 0; i < ARRAY_SIZE(ane->soc->ps_off); i++) {
		while (!ane_h16_ps_on(ane, i, &v)) {
			if (time_after(jiffies, deadline)) {
				dev_err(ane->dev,
					"ane_h16 ps word=%s off=%#x stuck value=%#x actual=%#lx; refusing to touch the engine window\n",
					ane_h16_ps_names[i], ane->soc->ps_off[i],
					v, FIELD_GET(ANE_H16_PS_ACTUAL, v));
				return -ETIMEDOUT;
			}
			usleep_range(100, 200);
		}
	}
	for (i = 0; i < ARRAY_SIZE(ane->soc->ps_off); i++) {
		if (!ane_h16_ps_on(ane, i, &v)) {
			dev_err(ane->dev,
				"ane_h16 ps word=%s off=%#x dropped value=%#x after the wait; refusing to touch the engine window\n",
				ane_h16_ps_names[i], ane->soc->ps_off[i], v);
			return -EIO;
		}
		dev_info(ane->dev,
			 "ane_h16 ps word=%s off=%#x value=%#x actual=0xf pass=true\n",
			 ane_h16_ps_names[i], ane->soc->ps_off[i], v);
	}
	return 0;
}

/* ---- engine registers ---- */

static u32 ane_h16_rd32(struct ane_h16 *ane, u32 off)
{
	return readl_relaxed(ane->engine + off);
}

static u64 ane_h16_rd64(struct ane_h16 *ane, u32 off)
{
	return readq_relaxed(ane->engine + off);
}

static void ane_h16_wr32(struct ane_h16 *ane, u32 off, u32 v)
{
	writel_relaxed(v, ane->engine + off);
}

static void ane_h16_wr64(struct ane_h16 *ane, u32 off, u64 v)
{
	writeq_relaxed(v, ane->engine + off);
}

static int ane_h16_status(struct ane_h16 *ane)
{
	const struct ane_h16_soc *s = ane->soc;
	u64 rvbar = ane_h16_rd64(ane, s->rvbar);
	unsigned int i;

	dev_info(ane->dev, "RVBAR (%#x) = %#llx (%slatched)\n", s->rvbar,
		 rvbar, (rvbar & 1) ? "" : "un");
	dev_info(ane->dev, "CPU_STATUS (%#x) = %#x\n", s->cpu_status,
		 ane_h16_rd32(ane, s->cpu_status));
	for (i = 0; i < 8; i++)
		dev_info(ane->dev, "SCRATCH%u (%#x) = %#x\n", i,
			 s->scratch0 + 4 * i,
			 ane_h16_rd32(ane, s->scratch0 + 4 * i));
	dev_info(ane->dev, "mbox a2i_ctrl (%#x) = %#x\n",
		 s->mbox + ANE_H16_MBOX_A2I_CTRL,
		 ane_h16_rd32(ane, s->mbox + ANE_H16_MBOX_A2I_CTRL));
	dev_info(ane->dev, "mbox i2a_ctrl (%#x) = %#x\n",
		 s->mbox + ANE_H16_MBOX_I2A_CTRL,
		 ane_h16_rd32(ane, s->mbox + ANE_H16_MBOX_I2A_CTRL));
	return 0;
}

/* ---- boot ADT, read from the reserved-memory phram "adt" region ---- */

static const void *adt;
static size_t adt_len;

struct adt_node {
	u32 off;
	u32 nprops;
	u32 nchildren;
};

static struct adt_node adt_node_at(u32 off)
{
	struct adt_node n = { .off = off };

	n.nprops = le32_to_cpup(adt + off);
	n.nchildren = le32_to_cpup(adt + off + 4);
	return n;
}

/* Offset just past the last byte of n's whole subtree. */
static u32 adt_subtree_end(struct adt_node n)
{
	u32 off = n.off + 8;
	unsigned int i;

	for (i = 0; i < n.nprops; i++) {
		u32 size = le32_to_cpup(adt + off + 32) & 0x7fffffff;

		off += 36 + ALIGN(size, 4);
	}
	for (i = 0; i < n.nchildren; i++) {
		struct adt_node c = adt_node_at(off);

		off = adt_subtree_end(c);
	}
	return off;
}

static struct adt_node adt_child(struct adt_node n, unsigned int idx)
{
	u32 off;
	unsigned int i;

	off = n.off + 8;
	for (i = 0; i < n.nprops; i++) {
		u32 size = le32_to_cpup(adt + off + 32) & 0x7fffffff;

		off += 36 + ALIGN(size, 4);
	}
	for (i = 0; i < idx; i++)
		off = adt_subtree_end(adt_node_at(off));
	return adt_node_at(off);
}

static int adt_prop(struct adt_node n, const char *name, const void **data,
		    u32 *size)
{
	u32 off = n.off + 8;
	unsigned int i;

	for (i = 0; i < n.nprops; i++) {
		u32 len = le32_to_cpup(adt + off + 32) & 0x7fffffff;

		if (!strncasecmp(adt + off, name, 32)) {
			*data = adt + off + 36;
			*size = len;
			return 0;
		}
		off += 36 + ALIGN(len, 4);
	}
	return -ENOENT;
}

static int ane_h16_adt_open(struct ane_h16 *ane)
{
	struct device_node *np, *rm;
	const char *label;
	struct resource res;
	int ret = -ENODEV;

	rm = of_find_node_by_name(NULL, "reserved-memory");
	if (!rm)
		goto missing;
	for_each_child_of_node(rm, np) {
		label = of_get_property(np, "label", NULL);
		if (!label || strcmp(label, "adt"))
			continue;
		if (of_address_to_resource(np, 0, &res))
			continue;
		adt = devm_memremap(ane->dev, res.start, resource_size(&res),
				    MEMREMAP_WB);
		if (IS_ERR(adt)) {
			ret = PTR_ERR(adt);
			adt = NULL;
			of_node_put(np);
			goto out;
		}
		adt_len = resource_size(&res);
		dev_info(ane->dev, "ADT: %pOF at %#llx+%#llx\n", np,
			 (unsigned long long)res.start,
			 (unsigned long long)adt_len);
		ret = 0;
		of_node_put(np);
		goto out;
	}
missing:
	dev_err(ane->dev,
		"no reserved-memory node labelled \"adt\": cannot read segment-ranges; refusing\n");
out:
	of_node_put(rm);
	return ret;
}

/* Find /arm-io's child whose ane-type is ane_type; NULL-safe. */
static int adt_find_ane(u32 ane_type, struct adt_node *out)
{
	struct adt_node root = adt_node_at(0);
	struct adt_node arm_io;
	const void *data;
	u32 size;
	unsigned int i;

	for (i = 0; i < root.nchildren; i++) {
		struct adt_node c = adt_child(root, i);

		if (!adt_prop(c, "name", &data, &size) && size > 7 &&
		    !strncasecmp(data, "arm-io", 7)) {
			arm_io = c;
			break;
		}
	}
	if (i == root.nchildren)
		return -ENOENT;

	for (i = 0; i < arm_io.nchildren; i++) {
		struct adt_node c = adt_child(arm_io, i);

		if (!adt_prop(c, "ane-type", &data, &size) && size >= 4 &&
		    le32_to_cpup(data) == ane_type) {
			*out = c;
			return 0;
		}
	}
	return -ENOENT;
}

/* Decode one segment-ranges property (two entries) into segs[2].
 * Entry is {phys, iova, remap, size} (u64 each); the ASC sees
 * "remap". The 3-word form {phys, remap, size} is accepted too; the
 * log keeps the decoded values so a wrong pick is visible.
 */
static int ane_h16_seg_decode(struct ane_h16 *ane, const void *data,
			      u32 size, struct ane_h16_seg *segs)
{
	unsigned int stride = size / 2;
	unsigned int i;

	if (size % 2 || (stride != 24 && stride != 32)) {
		dev_err(ane->dev,
			"segment-ranges: %u bytes is not two 3- or 4-word entries\n",
			size);
		return -EINVAL;
	}
	for (i = 0; i < 2; i++) {
		u64 v[4] = {};

		memcpy(v, data + i * stride, stride);
		segs[i].phys = le64_to_cpu(v[0]);
		segs[i].iova = stride == 32 ?
			le64_to_cpu(v[2]) : le64_to_cpu(v[1]);
		segs[i].size = le64_to_cpu(v[stride / 8 - 1]);
	}
	return 0;
}

static int ane_h16_segments(struct ane_h16 *ane)
{
	const struct ane_h16_fw *fw = ane->soc->fw;
	struct adt_node ane_node;
	const void *data;
	u32 size;
	int ret;

	if (ane_h16_adt_open(ane))
		return -ENODEV;
	if (adt_find_ane(ane->soc->ane_type, &ane_node)) {
		dev_err(ane->dev,
			"ADT: no /arm-io child with ane-type %#x; refusing\n",
			ane->soc->ane_type);
		return -ENOENT;
	}
	if (adt_prop(ane_node, "segment-ranges", &data, &size)) {
		dev_err(ane->dev,
			"ADT: /arm-io ane node has no segment-ranges: iBoot did not preload ANE firmware for this boot, or the ADT differs; refusing\n");
		return -ENOENT;
	}
	ret = ane_h16_seg_decode(ane, data, size, ane->segs);
	if (ret)
		return ret;
	if ((ane->segs[0].size != ane->soc->fw->text_vmsize) ||
	    (ane->segs[1].size < fw->data_filesize)) {
		dev_err(ane->dev,
			"segment sizes %#llx/%#llx do not match the pinned image (%#x/%#x)\n",
			ane->segs[0].size, ane->segs[1].size,
			fw->text_vmsize, fw->data_filesize);
		return -EINVAL;
	}
	dev_info(ane->dev,
		 "segment-ranges: TEXT phys %#llx iova %#llx size %#llx; DATA phys %#llx iova %#llx size %#llx\n",
		 ane->segs[0].phys, ane->segs[0].iova, ane->segs[0].size,
		 ane->segs[1].phys, ane->segs[1].iova, ane->segs[1].size);
	return 0;
}

/* ---- stage 0: dt-only parse, no MMIO ---- */

static void ane_h16_stage_dt(struct ane_h16 *ane)
{
	const struct ane_h16_soc *s = ane->soc;
	unsigned int i;

	dev_info(ane->dev, "stage=dt: soc=%s ane-type=%#x", s->name,
		 s->ane_type);
	for (i = 0; i < 2; i++) {
		struct resource res;

		if (!of_address_to_resource(ane->dev->of_node, i, &res))
			dev_info(ane->dev, "reg[%u] pa=%#llx size=%#llx (%s)",
				 i, (unsigned long long)res.start,
				 (unsigned long long)resource_size(&res),
				 i ? "pmgr" : "engine");
	}
	dev_info(ane->dev, "ane_h16 dt irq cells=%d iommu cells=%d",
		 of_property_count_u32_elems(ane->dev->of_node, "interrupts"),
		 of_property_count_u32_elems(ane->dev->of_node, "iommus"));
	/* Word groups with their evidence tier (h15 tier scale: 0 =
	 * measured address and role, touched only behind the ps guard;
	 * 3 = forbidden). The H16 rows are kext-derived and calibrated
	 * against the known T6021 row, hence tier 0.
	 */
	for (i = 0; i < 5; i++)
		dev_info(ane->dev,
			 "ane_h16 group=pmgr tier=0 word=%s off=%#x (ps guard word)",
			 ane_h16_ps_names[i], s->ps_off[i]);
	dev_info(ane->dev, "ane_h16 group=engine-ro tier=0 word=RVBAR off=%#x",
		 s->rvbar);
	dev_info(ane->dev,
		 "ane_h16 group=engine-ro tier=0 word=CPU_STATUS off=%#x",
		 s->cpu_status);
	dev_info(ane->dev,
		 "ane_h16 group=engine-ro tier=0 word=SCRATCH0 off=%#x (+4n, n<8)",
		 s->scratch0);
	dev_info(ane->dev,
		 "ane_h16 group=engine-ro tier=0 word=MBOX off=%#x (a2i/i2a ctrl +0x110/+0x114, send +0x800/+0x808, recv +0x830/+0x838)",
		 s->mbox);
	dev_info(ane->dev,
		 "ane_h16 group=engine-boot tier=0 word=CPU_CONTROL off=%#x (write 0 then 0x10, stage=boot only)",
		 s->cpu_control);
	dev_info(ane->dev,
		 "ane_h16 group=engine-boot tier=0 word=SCRATCH7 off=%#x (cleared, then polled for 0x08042006 at stage=boot)",
		 s->scratch0 + 7 * 4);
	dev_info(ane->dev,
		 "ane_h16 group=forbidden tier=3 word=CORESIGHT off=0x1010000 (never touched)");
	if (s->fw)
		dev_info(ane->dev, "fw pin: name=%s size=%#x", s->fw->name,
			 s->fw->size);
	else
		dev_info(ane->dev, "fw pin: none (stage=boot refuses)");
	/* The boot ADT, when the reserved-memory phram region exists on
	 * this boot: whether iBoot preloaded ANE firmware on a Linux
	 * boot is an open question (H9), and its absence here is a
	 * finding, not a failure.
	 */
	{
		struct ane_h16_seg segs[2];
		struct adt_node ane_node;
		const void *data;
		u32 size;

		if (!ane_h16_adt_open(ane) &&
		    !adt_find_ane(s->ane_type, &ane_node) &&
		    !adt_prop(ane_node, "segment-ranges", &data, &size) &&
		    !ane_h16_seg_decode(ane, data, size, segs)) {
			unsigned int e;

			for (e = 0; e < 2; e++)
				dev_info(ane->dev,
					 "adt seg%u phys=%#llx remap=%#llx size=%#llx (iBoot preload present)",
					 e, segs[e].phys, segs[e].iova,
					 segs[e].size);
		} else {
			dev_info(ane->dev,
				 "adt: no reserved-memory \"adt\" region, ane node or segment-ranges: iBoot preload presence on this boot is unknown; stage=boot refuses");
		}
	}
	ane_h16_result(ane, 0, "PASS", "dt-parse-only");
}

/* ---- firmware: pin check, preload diff, staged copy ---- */

static int ane_h16_fw_pin(struct ane_h16 *ane, const struct firmware **fwp)
{
	const struct ane_h16_fw *img = ane->soc->fw;
	const struct firmware *fw;
	u8 digest[SHA256_DIGEST_SIZE];
	int ret;

	ret = request_firmware(&fw, img->name, ane->dev);
	if (ret)
		return ret;
	if (fw->size != img->size) {
		dev_err(ane->dev, "%s: %zu bytes, pinned image is %u\n",
			img->name, fw->size, img->size);
		release_firmware(fw);
		return -EINVAL;
	}
	sha256(fw->data, fw->size, digest);
	if (memcmp(digest, img->sha256, sizeof(digest))) {
		dev_err(ane->dev, "%s: sha256 mismatch against the pinned image\n",
			img->name);
		release_firmware(fw);
		return -EINVAL;
	}
	*fwp = fw;
	return 0;
}

/* True when a differing u64 is one iBoot is expected to have patched:
 * an entry/DATA-base IOVA, or a nonzero iBoot-only value (stack guard,
 * SOC id, engine addresses) over a zero in the file. Logged either way;
 * only the copy decision depends on this. */
static bool ane_h16_preload_patched_u64(struct ane_h16 *ane, u64 file,
					u64 live)
{
	if (live == ane->segs[0].iova || live == ane->segs[1].iova)
		return true;
	if ((live & ANE_H16_RVBAR_ADDR_MASK) ==
	    (ane->segs[0].iova & ANE_H16_RVBAR_ADDR_MASK))
		return true;
	return live && !file;
}

/* Build the staged image: the pinned file laid out at its vm addresses,
 * then every accepted iBoot difference copied in from the preload.
 * Anything else that differs refuses the boot. */
static int ane_h16_stage(struct ane_h16 *ane, const struct firmware *fw)
{
	const struct ane_h16_fw *img = ane->soc->fw;
	const unsigned long allowed[][2] = {
		{ img->patchbay_vm, 0x261 },
		{ img->tunables_vm, 0x6a0 },
	};
	/* The preload maps the image vm layout: segment 0 at vm 0,
	 * segment 1 at data_vm. */
	const u64 seg_vm[2] = { 0, img->data_vm };
	const u64 seg_file_off[2] = { 0x4000, img->data_fileoff };
	void __iomem *pre[2] = {};
	unsigned int s, i;
	int ret = 0;

	ane->stage_size = ALIGN(img->data_vm + img->data_vmsize, SZ_16K);
	ane->stage_cpu = dma_alloc_coherent(ane->dev, ane->stage_size,
					    &ane->stage_dma, GFP_KERNEL);
	if (!ane->stage_cpu)
		return -ENOMEM;
	memcpy(ane->stage_cpu, fw->data + 0x4000, img->text_vmsize);
	memcpy(ane->stage_cpu + img->data_vm, fw->data + img->data_fileoff,
	       img->data_filesize);

	for (s = 0; s < 2; s++) {
		pre[s] = devm_memremap(ane->dev, ane->segs[s].phys,
				       ane->segs[s].size, MEMREMAP_WB);
		if (IS_ERR(pre[s])) {
			ret = PTR_ERR(pre[s]);
			pre[s] = NULL;
			goto out;
		}
	}
	for (s = 0; s < 2 && !ret; s++) {
		u64 expect_bytes = s ? img->data_filesize : img->text_vmsize;

		for (i = 0; i < ane->segs[s].size; i += 8) {
			u64 a = 0, b;
			u8 *dst;
			unsigned int k;
			bool in_allowed = false;

			if (i + 8 <= expect_bytes)
				memcpy(&a, fw->data + seg_file_off[s] + i, 8);
			memcpy(&b, pre[s] + i, 8);
			if (a == b)
				continue;
			for (k = 0; k < ARRAY_SIZE(allowed); k++)
				if (seg_vm[s] + i >= allowed[k][0] &&
				    seg_vm[s] + i < allowed[k][0] + allowed[k][1])
					in_allowed = true;
			if (!in_allowed &&
			    !ane_h16_preload_patched_u64(ane, a, b)) {
				dev_err(ane->dev,
					"preload diff seg%u vm %#llx: file %#llx live %#llx: preload is not the pinned image; refusing\n",
					s, seg_vm[s] + i, a, b);
				ret = -EINVAL;
				break;
			}
			dev_info(ane->dev,
				 "preload diff seg%u vm %#llx: file %#llx live %#llx (%s; copied)\n",
				 s, seg_vm[s] + i, a, b,
				 in_allowed ? "patchbay/tunables" : "single word");
			if (seg_vm[s] + i + 8 > ane->stage_size) {
				dev_err(ane->dev,
					"preload diff outside the staged image\n");
				ret = -EINVAL;
				break;
			}
			dst = ane->stage_cpu + seg_vm[s] + i;
			memcpy(dst, &b, 8);
		}
	}
out:
	for (s = 0; s < 2; s++)
		if (pre[s])
			devm_memunmap(ane->dev, pre[s]);
	return ret;
}

/* ---- DART mapping of the staged image at the ADT IOVAs ---- */

static int ane_h16_map_stage(struct ane_h16 *ane)
{
	const struct ane_h16_fw *img = ane->soc->fw;
	const u64 seg_vm[2] = { 0, img->data_vm };
	unsigned int s;
	int ret;

	ane->domain = iommu_get_domain_for_dev(ane->dev);
	if (!ane->domain) {
		dev_err(ane->dev,
			"device has no iommu domain: the overlay must give the node apple,t8110-dart iommus\n");
		return -ENODEV;
	}
	if (!IS_ALIGNED(virt_to_phys(ane->stage_cpu), SZ_16K)) {
		dev_err(ane->dev, "staged buffer not 16 KiB aligned\n");
		return -EINVAL;
	}
	for (s = 0; s < 2; s++) {
		u64 off;

		for (off = 0; off < ane->segs[s].size; off += SZ_16K) {
			u64 pa = virt_to_phys(ane->stage_cpu + seg_vm[s] + off);
			u64 iova = ane->segs[s].iova + off;
			size_t len = min_t(u64, SZ_16K, ane->segs[s].size - off);

			if (seg_vm[s] + off + len > ane->stage_size) {
				dev_err(ane->dev, "mapping outside the staged image\n");
				return -EINVAL;
			}
			if (iommu_iova_to_phys(ane->domain, iova)) {
				dev_err(ane->dev,
					"iova %#llx already mapped; refusing to alias over it\n",
					iova);
				return -EBUSY;
			}
			ret = iommu_map(ane->domain, iova, pa, len,
					IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
			if (ret) {
				dev_err(ane->dev, "iommu_map %#llx: %d\n", iova, ret);
				return ret;
			}
		}
	}
	dev_info(ane->dev,
		 "staged image (%zu KiB) mapped at iova %#llx (TEXT) and %#llx (DATA)\n",
		 ane->stage_size / 1024, ane->segs[0].iova, ane->segs[1].iova);
	return 0;
}

static void ane_h16_unmap_stage(struct ane_h16 *ane)
{
	unsigned int s;

	if (!ane->domain)
		return;
	for (s = 0; s < 2; s++)
		iommu_unmap(ane->domain, ane->segs[s].iova, ane->segs[s].size);
}

/* ---- polled ASC mailbox (RTKit management only) ---- */

static int ane_h16_mbox_send(struct ane_h16 *ane, u8 ep, u64 msg)
{
	u32 ctrl;
	int ret;

	ret = readl_poll_timeout(ane->engine + ane->soc->mbox +
				 ANE_H16_MBOX_A2I_CTRL, ctrl,
				 !(ctrl & ANE_H16_MBOX_CTRL_FULL), 100, 500000);
	if (ret)
		return ret;
	writeq_relaxed(msg, ane->engine + ane->soc->mbox + ANE_H16_MBOX_A2I_SEND0);
	writeq_relaxed(ep, ane->engine + ane->soc->mbox + ANE_H16_MBOX_A2I_SEND1);
	return 0;
}

static int ane_h16_mbox_mgmt_send(struct ane_h16 *ane, u8 type, u64 msg)
{
	msg &= ~ANE_H16_MGMT_TYPE;
	msg |= FIELD_PREP(ANE_H16_MGMT_TYPE, type);
	return ane_h16_mbox_send(ane, ANE_H16_EP_MGMT, msg);
}

static void ane_h16_rtkit_rx_mgmt(struct ane_h16 *ane, u64 msg)
{
	u8 type = FIELD_GET(ANE_H16_MGMT_TYPE, msg);

	switch (type) {
	case ANE_H16_MGMT_HELLO: {
		u32 minv = FIELD_GET(ANE_H16_MGMT_HELLO_MINVER, msg);
		u32 maxv = FIELD_GET(ANE_H16_MGMT_HELLO_MAXVER, msg);
		u32 v = min(maxv, (u32)ANE_H16_RTKIT_MAX_VERSION);

		dev_info(ane->dev,
			 "RTKit HELLO: min %u max %u -> replying version %u\n",
			 minv, maxv, v);
		if (minv > ANE_H16_RTKIT_MAX_VERSION)
			dev_err(ane->dev, "RTKit: firmware min version too new\n");
		else
			ane_h16_mbox_mgmt_send(ane, ANE_H16_MGMT_HELLO_REPLY,
					       FIELD_PREP(ANE_H16_MGMT_HELLO_MINVER, v) |
					       FIELD_PREP(ANE_H16_MGMT_HELLO_MAXVER, v));
		ane->hello_done = true;
		break;
	}
	case ANE_H16_MGMT_EPMAP: {
		unsigned long bitmap = FIELD_GET(ANE_H16_MGMT_EPMAP_BITMAP, msg);
		u32 base = FIELD_GET(ANE_H16_MGMT_EPMAP_BASE, msg);
		u64 reply = FIELD_PREP(ANE_H16_MGMT_EPMAP_BASE, base);
		unsigned int ep;

		dev_info(ane->dev, "RTKit EPMAP: base %u bitmap %lx\n",
			 base, bitmap);
		if (msg & ANE_H16_MGMT_EPMAP_LAST)
			reply |= ANE_H16_MGMT_EPMAP_LAST;
		else
			reply |= ANE_H16_MGMT_EPMAP_REPLY_MORE;
		ane_h16_mbox_mgmt_send(ane, ANE_H16_MGMT_EPMAP_REPLY, reply);
		if (!(msg & ANE_H16_MGMT_EPMAP_LAST))
			break;
		for_each_set_bit(ep, &bitmap, 32) {
			u8 e = 32 * base + ep;

			switch (e) {
			case ANE_H16_EP_MGMT:
				break;
			case ANE_H16_EP_CRASHLOG:
			case ANE_H16_EP_SYSLOG:
			case ANE_H16_EP_IOREPORT:
			case ANE_H16_EP_DEBUG:
			case ANE_H16_EP_OSLOG:
			case ANE_H16_EP_TRACEKIT:
				dev_info(ane->dev, "RTKit: STARTEP system endpoint %#x\n", e);
				ane_h16_mbox_mgmt_send(ane, ANE_H16_MGMT_STARTEP,
						       FIELD_PREP(ANE_H16_MGMT_STARTEP_EP, e) |
						       ANE_H16_MGMT_STARTEP_FLAG);
				break;
			default:
				dev_info(ane->dev, "RTKit: endpoint %#x announced (not started; no app endpoint is used by this smoke)\n", e);
			}
		}
		break;
	}
	case ANE_H16_MGMT_SET_IOP_PWR_STATE:
		dev_info(ane->dev, "RTKit: SET_IOP_PWR_STATE %u -> ack\n",
			 (unsigned int)FIELD_GET(ANE_H16_MGMT_PWR_STATE, msg));
		ane_h16_mbox_mgmt_send(ane, ANE_H16_MGMT_SET_IOP_PWR_STATE_ACK,
				       FIELD_PREP(ANE_H16_MGMT_PWR_STATE,
						  FIELD_GET(ANE_H16_MGMT_PWR_STATE, msg)));
		break;
	default:
		dev_info(ane->dev, "RTKit: unhandled mgmt type %u (%#llx)\n",
			 type, msg);
	}
}

static int ane_h16_rtkit_poll(struct ane_h16 *ane)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(hello_wait_ms);
	u32 ctrl;
	int ret;

	/* T8140+ ASC mailboxes need their outbox enable bit before use; do
	 * the same read-modify-write here, it is a no-op if already set. */
	ctrl = readl_relaxed(ane->engine + ane->soc->mbox + ANE_H16_MBOX_I2A_CTRL);
	writel_relaxed(ctrl | BIT(0),
		       ane->engine + ane->soc->mbox + ANE_H16_MBOX_I2A_CTRL);

	for (;;) {
		u64 msg;
		u8 ep;

		ctrl = readl_relaxed(ane->engine + ane->soc->mbox +
				     ANE_H16_MBOX_I2A_CTRL);
		if (ctrl & ANE_H16_MBOX_CTRL_EMPTY) {
			if (ane->hello_done ||
			    time_after_eq(jiffies, deadline))
				break;
			usleep_range(1000, 2000);
			continue;
		}
		msg = readq_relaxed(ane->engine + ane->soc->mbox + ANE_H16_MBOX_I2A_RECV0);
		ep = readq_relaxed(ane->engine + ane->soc->mbox + ANE_H16_MBOX_I2A_RECV1);
		switch (ep) {
		case ANE_H16_EP_MGMT:
			ane_h16_rtkit_rx_mgmt(ane, msg);
			break;
		case ANE_H16_EP_CRASHLOG:
		case ANE_H16_EP_IOREPORT:
			/* ioreport type 0x8/0xc must be echoed back. */
			ane_h16_mbox_send(ane, ep, msg);
			break;
		default:
			dev_info(ane->dev,
				 "RTKit: message to endpoint %#x (%#llx) not serviced by this smoke (no shmem/syslog support); stopping the handshake\n",
				 ep, msg);
			return 0;
		}
	}
	ret = ane->hello_done ? 0 : -ETIMEDOUT;
	dev_info(ane->dev, "RTKit poll done: HELLO %sreceived%s\n",
		 ane->hello_done ? "" : "not ",
		 ret ? " (mailbox stayed empty; the 27.0 firmware may not speak RTKit when brought up this way — see the receipt)" : "");
	return ret;
}

/* ---- boot stage ---- */

static int ane_h16_boot(struct ane_h16 *ane)
{
	const struct ane_h16_soc *s = ane->soc;
	const struct firmware *fw;
	u64 rvbar;
	int ret;

	ret = ane_h16_segments(ane);
	if (ret) {
		ane_h16_result(ane, 3, "REFUSED", "segment-ranges");
		return ret;
	}
	ret = ane_h16_fw_pin(ane, &fw);
	if (ret) {
		ane_h16_result(ane, 3, "REFUSED", "fw-pin");
		return ret;
	}
	ret = ane_h16_stage(ane, fw);
	if (ret) {
		ane_h16_result(ane, 3, "REFUSED", "preload-diff");
		goto free_stage;
	}
	ret = ane_h16_map_stage(ane);
	if (ret) {
		ane_h16_result(ane, 3, "REFUSED", "dart-map");
		goto free_stage;
	}

	rvbar = ane_h16_rd64(ane, s->rvbar);
	if (rvbar & 1) {
		u64 entry = rvbar & ANE_H16_RVBAR_ADDR_MASK;

		if (entry != (ane->segs[0].iova & ANE_H16_RVBAR_ADDR_MASK)) {
			dev_err(ane->dev,
				"RVBAR latched at %#llx but the staged TEXT iova is %#llx; refusing\n",
				entry, ane->segs[0].iova);
			ane_h16_result(ane, 3, "REFUSED", "rvbar-latched");
			ret = -EBUSY;
			goto unmap;
		}
		dev_info(ane->dev, "RVBAR latched at the staged entry %#llx\n", entry);
	} else {
		u64 v = ANE_H16_RVBAR_ENTRY_BASE |
			(ane->segs[0].iova & ANE_H16_RVBAR_ADDR_MASK);

		dev_info(ane->dev, "RVBAR unlatched; writing %#llx\n", v);
		ane_h16_wr64(ane, s->rvbar, v);
	}

	ane->fw_touched = true;

	/* kext ANE_Init cold-boot order (macOS 27.0): SCRATCH7 = 0, RVBAR
	 * (skipped when latched), CPU_CONTROL 0 then 0x10, then poll
	 * SCRATCH7 for the wake word. */
	ane_h16_wr32(ane, s->scratch0 + 7 * 4, 0);
	ane_h16_wr32(ane, s->cpu_control, 0);
	wmb();
	ane_h16_wr32(ane, s->cpu_control, ANE_H16_CPU_RUN_RELEASE);
	{
		unsigned long deadline = jiffies + msecs_to_jiffies(boot_wait_ms);
		u32 ack = 0;

		while ((ack = ane_h16_rd32(ane, s->scratch0 + 7 * 4)) !=
		       ANE_H16_WAKE_ACK) {
			if (time_after(jiffies, deadline)) {
				dev_emerg(ane->dev,
					"no SCRATCH7 wake word within %u ms (last %#x); firmware did not start; leaving the mapping in place, reboot before retrying\n",
					boot_wait_ms, ack);
				ane_h16_result(ane, 3, "FAIL", "boot-timeout");
				ret = -ETIMEDOUT;
				/* deliberate: mapping and staged buffer
				 * stay in place - the wedged firmware
				 * may still DMA into them; reboot to
				 * park
				 */
				goto rel_fw;
			}
			usleep_range(1000, 2000);
		}
		dev_info(ane->dev,
			 "SCRATCH7 wake word %#x: firmware is running (CPU_STATUS %#x)\n",
			 ack, ane_h16_rd32(ane, s->cpu_status));
	}
	ane->fw_started = true;

	if (hello_wait_ms)
		ane_h16_rtkit_poll(ane);
	else
		dev_info(ane->dev,
			 "RTKit poll skipped (hello_wait_ms=0); set 1000 to probe for HELLO/EPMAP/STARTEP\n");
	ane_h16_result(ane, 3, "PASS",
		       ane->hello_done ? "boot-hello" :
		       hello_wait_ms ? "boot-nortkit" : "boot-wake");
	ret = 0;
	goto rel_fw;
unmap:
	ane_h16_unmap_stage(ane);
free_stage:
	if (ane->stage_cpu) {
		dma_free_coherent(ane->dev, ane->stage_size, ane->stage_cpu,
				  ane->stage_dma);
		ane->stage_cpu = NULL;
	}
rel_fw:
	release_firmware(fw);
	return ret;
}

/* ---- driver ---- */

static int ane_h16_probe(struct platform_device *pdev)
{
	const struct ane_h16_soc *soc = of_device_get_match_data(&pdev->dev);
	struct ane_h16 *ane;
	unsigned int stage_idx;
	u32 ane_type;
	int ret;

	if (!optin || strcmp(optin, soc->name)) {
		dev_err(&pdev->dev,
			"not probing: optin key must be \"%s\" (optin=%s). This module is EXPERIMENTAL and untested on silicon.\n",
			soc->name, optin ? optin : "(unset)");
		return -EPERM;
	}
	if (of_property_read_u32(pdev->dev.of_node, "apple,ane-type", &ane_type) ||
	    ane_type != soc->ane_type) {
		dev_err(&pdev->dev,
			"device apple,ane-type mismatch with the %s row; refusing\n",
			soc->name);
		return -EINVAL;
	}
	ret = ane_h16_stage_idx(stage, &stage_idx);
	if (ret) {
		dev_err(&pdev->dev, "unknown stage \"%s\" (want dt, status or boot)\n",
			stage);
		return ret;
	}

	ane = devm_kzalloc(&pdev->dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = &pdev->dev;
	ane->soc = soc;
	platform_set_drvdata(pdev, ane);

	if (stage_idx == 0) {
		ane_h16_stage_dt(ane);
		return 0;
	}

	ane->engine = devm_platform_ioremap_resource_byname(pdev, "engine");
	ane->pmgr = devm_platform_ioremap_resource_byname(pdev, "pmgr");
	if (IS_ERR(ane->engine))
		return PTR_ERR(ane->engine);
	if (IS_ERR(ane->pmgr))
		return PTR_ERR(ane->pmgr);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "power-domains bring-up failed: %d\n", ret);
		pm_runtime_disable(&pdev->dev);
		return ret;
	}
	ret = ane_h16_ps_wait(ane);
	if (ret) {
		ane_h16_result(ane, 1, "FAIL", "pmgr-actual-stuck");
		goto rpm_off;
	}

	ret = ane_h16_status(ane);
	if (ret)
		goto rpm_off;
	if (stage_idx == 1) {
		ane_h16_result(ane, 1, "PASS", "ps-guard+reads");
		goto rpm_off;
	}
	ret = ane_h16_boot(ane);
rpm_off:
	pm_runtime_put_sync(&pdev->dev);
	if (ane->fw_started)
		dev_emerg(&pdev->dev,
			  "firmware started: do NOT unload this module; reboot to park the ANE\n");
	return ret;
}

static void ane_h16_remove(struct platform_device *pdev)
{
	struct ane_h16 *ane = platform_get_drvdata(pdev);

	if (ane->fw_touched) {
		dev_emerg(&pdev->dev,
			  "remove with the firmware released: leaving power on, reboot required\n");
		pm_runtime_get_noresume(&pdev->dev);
		return;
	}
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id ane_h16_of_match[] = {
	{ .compatible = "apple,t8132-ane", .data = &ane_t8132_soc },
	{ .compatible = "apple,t6040-ane", .data = &ane_t6040_soc },
	{ .compatible = "apple,t6041-ane", .data = &ane_t6041_soc },
	{ }
};

static struct platform_driver ane_h16_driver = {
	.driver = {
		.name = "ane_h16",
		.suppress_bind_attrs = true,
		.of_match_table = ane_h16_of_match,
	},
	.probe = ane_h16_probe,
	.remove = ane_h16_remove,
};
module_platform_driver(ane_h16_driver);

MODULE_AUTHOR("Joshua Warren");
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H16-family ANE bring-up module (status + firmware-boot smoke; no inference)");
MODULE_LICENSE("Dual MIT/GPL");
