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
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "../h17/bringup_order.h"
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
#define ANE_H16_MAX_PDS 8	/* DT power-domains cap (T8140 uses 4) */
	void __iomem *engine;
	void __iomem *pmgr;
	struct ane_h16_seg segs[2];
	void __iomem *pwgate;	/* H17 reg[2]: PS/PWGATE bank (overlay) */
	unsigned int order_seq;	/* durable h17-order sequence number */
	struct device *pd_vdev[ANE_H16_MAX_PDS]; /* one per DT power domain */
	unsigned int pd_nvdev;
	bool fw_touched;
	bool fw_started;
	bool hello_done;
	int boot_error;		/* result retained when binding must survive a timeout */
	void *stage_cpu;
	dma_addr_t stage_dma;
	size_t stage_size;
	struct iommu_domain *domain;
	u64 stage_mapped[2];	/* owned contiguous prefixes, never foreign ranges */
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

static unsigned int ane_h16_ps_count(const struct ane_h16_soc *soc)
{
	return soc->ps_count ? soc->ps_count : ARRAY_SIZE(soc->ps_off);
}

static const char *ane_h16_ps_name(const struct ane_h16_soc *soc, unsigned int i)
{
	static const char *const h17_names[] = {
		"ANE_SYS", "ANE_SUB0", "ANE_SUB1", "ANE_SUB2",
	};

	/* The sub-island roles on T8140 are not yet assigned. */
	if (soc->ane_type == 0x200 && i < ARRAY_SIZE(h17_names))
		return h17_names[i];
	return ane_h16_ps_names[i];
}

static bool ane_h16_ps_on(struct ane_h16 *ane, unsigned int i, u32 *v)
{
	*v = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
	return FIELD_GET(ANE_H16_PS_ACTUAL, *v) == ANE_H16_PS_ON;
}

/* Every valid ANE word must read ACTUAL == 0xf before any engine MMIO:
 * each word is polled to 0xf, then the valid prefix is read
 * again so a domain that dropped while a later one came up refuses. */
static int ane_h16_ps_wait(struct ane_h16 *ane)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(ps_wait_ms);
	unsigned int i;
	u32 v;

	for (i = 0; i < ane_h16_ps_count(ane->soc); i++) {
		while (!ane_h16_ps_on(ane, i, &v)) {
			if (time_after(jiffies, deadline)) {
				dev_err(ane->dev,
					"ane_h16 ps word=%s off=%#x stuck value=%#x actual=%#lx; refusing to touch the engine window\n",
					ane_h16_ps_name(ane->soc, i), ane->soc->ps_off[i],
					v, FIELD_GET(ANE_H16_PS_ACTUAL, v));
				return -ETIMEDOUT;
			}
			usleep_range(100, 200);
		}
	}
	for (i = 0; i < ane_h16_ps_count(ane->soc); i++) {
		if (!ane_h16_ps_on(ane, i, &v)) {
			dev_err(ane->dev,
				"ane_h16 ps word=%s off=%#x dropped value=%#x after the wait; refusing to touch the engine window\n",
				ane_h16_ps_name(ane->soc, i), ane->soc->ps_off[i], v);
			return -EIO;
		}
		dev_info(ane->dev,
			 "ane_h16 ps word=%s off=%#x value=%#x actual=0xf pass=true\n",
			 ane_h16_ps_name(ane->soc, i), ane->soc->ps_off[i], v);
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

/* Validate the complete tree before the offset-only walkers below run.
 * All offsets must fit u32 and recursion is bounded independently of data.
 */
static int adt_validate_node(size_t *off, unsigned int depth)
{
	u32 nprops, nchildren, i;

	if (depth > 32 || *off > adt_len || adt_len - *off < 8)
		return -EINVAL;
	nprops = le32_to_cpup(adt + *off);
	nchildren = le32_to_cpup(adt + *off + 4);
	*off += 8;
	for (i = 0; i < nprops; i++) {
		size_t bytes;

		if (adt_len - *off < 36)
			return -EINVAL;
		bytes = le32_to_cpup(adt + *off + 32) & 0x7fffffff;
		*off += 36;
		bytes = ALIGN(bytes, 4);
		if (bytes > adt_len - *off)
			return -EINVAL;
		*off += bytes;
	}
	for (i = 0; i < nchildren; i++)
		if (adt_validate_node(off, depth + 1))
			return -EINVAL;
	return 0;
}

static int adt_validate(void)
{
	size_t off = 0;

	if (!adt || adt_len > (u32)~0U)
		return -EINVAL;
	return adt_validate_node(&off, 0);
}

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
		ret = adt_validate();
		if (ret) {
			dev_err(ane->dev, "ADT: malformed or truncated tree; refusing\n");
			adt = NULL;
			adt_len = 0;
			of_node_put(np);
			goto out;
		}
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

		if (!adt_prop(c, "name", &data, &size) && size >= 7 &&
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
 * A 32-byte entry is {u64 phys, virt, remap; u32 size, auxiliary}.
 * The ASC sees remap. mapFwCTRRRegion in retained kernelcache SHA256
 * 7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe
 * uses ldr w9,[x20,#0x18] at 0xfffffe0008f440a0 (TEXT), and
 * ldr w9,[x20,#0x38] at 0xfffffe0008f440c0 (DATA): size is u32,
 * not u64. The final dword's meaning is not established here.
 * The legacy 24-byte {u64 phys, remap, size} form is also accepted.
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
		segs[i].size = stride == 32 ?
			le32_to_cpup(data + i * stride + 24) : le64_to_cpu(v[2]);
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
	    (ane->segs[1].size < fw->data_filesize) ||
	    (ane->segs[1].size > fw->data_vmsize)) {
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

static int ane_h16_admit_preload(struct ane_h16 *ane, bool enforce);
static void ane_h16_pd_release(struct ane_h16 *ane);

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
	for (i = 0; i < ane_h16_ps_count(s); i++)
		dev_info(ane->dev,
			 "ane_h16 group=pmgr tier=0 word=%s off=%#x (ps guard word)",
			 ane_h16_ps_name(s, i), s->ps_off[i]);
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
		/* Record the ownership verdict with the addresses, not just
		 * the addresses (metadata only; no mapping or read). */
		if (!ane_h16_adt_open(ane) && !adt_find_ane(s->ane_type, &ane_node) &&
		    !adt_prop(ane_node, "segment-ranges", &data, &size) &&
		    !ane_h16_seg_decode(ane, data, size, segs))
			ane_h16_admit_preload(ane, false);
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

/* Ownership admission before any mapping or read of the preload DRAM.
 * On T8140 the iBoot preload lives below Linux's first usable /memory
 * range and has no reserved-memory coverage: it is owned by exclusion
 * from the allocator, not by a reservation. "Segment inside a
 * reservation" is therefore NOT the admission test (that was T6021's
 * case, where the address is inside RAM); the test here is that each
 * segment is DRAM-decoded but disjoint from (a) every other segment,
 * (b) every System RAM range and (c) every reserved-memory reg.
 * An in_ram elsewhere would mean allocator-owned memory: refuse.
 */
static int ane_h16_admit_preload(struct ane_h16 *ane, bool enforce)
{
	unsigned int s;
	int ret = 0;

	for (s = 0; s < 2; s++) {
		u64 start = ane->segs[s].phys, size = ane->segs[s].size;
		unsigned int c;

		if (!size || start + size <= start ||
		    !IS_ALIGNED(start | size, SZ_16K)) {
			dev_err(ane->dev,
				"preload seg%u phys %#llx size %#llx is empty, wrapping or unaligned\n",
				s, start, size);
			ret = -EINVAL;
			goto out;
		}
		for (c = 0; c < 2; c++) {
			if (c == s)
				continue;
			if (start < ane->segs[c].phys + ane->segs[c].size &&
			    ane->segs[c].phys < start + size) {
				dev_err(ane->dev,
					"preload seg%u and seg%u overlap\n",
					s, c);
				ret = -EINVAL;
				goto out;
			}
		}
		if (region_intersects(start, size, IORESOURCE_SYSTEM_RAM,
				      IORES_DESC_NONE) != REGION_DISJOINT) {
			dev_err(ane->dev,
				"preload seg%u phys %#llx-%#llx intersects Linux System RAM; not an owned preload window\n",
				s, start, start + size);
			ret = -EINVAL;
			goto out;
		}
	}
	{
		struct device_node *rm = of_find_node_by_name(NULL,
							      "reserved-memory");
		struct device_node *child;

		if (rm)
		for_each_child_of_node(rm, child) {
			struct resource res;
			unsigned int idx;

			for (idx = 0;
			     !of_address_to_resource(child, idx, &res);
			     idx++) {
				unsigned int c;

				for (c = 0; c < 2; c++) {
					if (ane->segs[c].phys <
					    res.start + resource_size(&res) &&
					    res.start <
					    ane->segs[c].phys + ane->segs[c].size) {
						dev_err(ane->dev,
							"preload seg%u overlaps reserved-memory %pOFn reg%u\n",
							c, child, idx);
						ret = -EINVAL;
						goto out;
					}
				}
			}
		}
	}
out:
	for (s = 0; s < 2; s++)
		dev_info(ane->dev,
			 "preload owned seg%u phys %#llx-%#llx: %s (%s; ram_disjoint=%d)\n",
			 s, ane->segs[s].phys,
			 ane->segs[s].phys + ane->segs[s].size,
			 ret ? "refused" : "admitted",
			 enforce ? "enforced" : "log-only", ret ? 0 :
			 region_intersects(ane->segs[s].phys,
					   ane->segs[s].size,
					   IORESOURCE_SYSTEM_RAM,
					   IORES_DESC_NONE) == REGION_DISJOINT);
	return ret;
}

/* Build the staged image: the pinned file laid out at its vm addresses,
 * then position-whitelisted bytes copied from one snapshot per segment.
 * No value-based exception for zero file words, BSS, or IOVA-like values.
 * Caller must establish quiescence/admit physical ranges before reading.
 */
static int ane_h16_stage(struct ane_h16 *ane, const struct firmware *fw)
{
	const struct ane_h16_fw *img = ane->soc->fw;
	const unsigned long allowed[][2] = {
		{ img->patchbay_vm, 0x261 },
		{ img->tunables_vm, 0x6a0 },
		{ img->ce_env_vm, img->ce_env_size },
	};
	/* The preload maps the image vm layout: segment 0 at vm 0,
	 * segment 1 at data_vm. */
	const u64 seg_vm[2] = { 0, img->data_vm };
	const u64 seg_file_off[2] = { 0x4000, img->data_fileoff };
	void *mapped;
	u8 *pre[2] = {};
	u64 accepted = 0;
	u8 digest[SHA256_DIGEST_SIZE];
	unsigned int s, i;
	int ret = 0;

	ret = dma_set_mask_and_coherent(ane->dev, DMA_BIT_MASK(64));
	if (ret)
		return ret;
	ane->stage_size = ALIGN(img->data_vm + img->data_vmsize, SZ_16K);
	ane->stage_cpu = dma_alloc_coherent(ane->dev, ane->stage_size,
					    &ane->stage_dma, GFP_KERNEL);
	if (!ane->stage_cpu)
		return -ENOMEM;
	memset(ane->stage_cpu, 0, ane->stage_size);
	memcpy(ane->stage_cpu, fw->data + 0x4000, img->text_vmsize);
	memcpy(ane->stage_cpu + img->data_vm, fw->data + img->data_fileoff,
	       img->data_filesize);

	for (s = 0; s < 2; s++) {
		pre[s] = kvzalloc(ane->segs[s].size, GFP_KERNEL);
		if (!pre[s]) {
			ret = -ENOMEM;
			goto out;
		}
		mapped = devm_memremap(ane->dev, ane->segs[s].phys,
				      ane->segs[s].size, MEMREMAP_WB);
		if (IS_ERR(mapped)) {
			ret = PTR_ERR(mapped);
			goto out;
		}
		memcpy(pre[s], mapped, ane->segs[s].size);
		devm_memunmap(ane->dev, mapped);
	}
	for (s = 0; s < 2 && !ret; s++) {
		u64 expect_bytes = s ? img->data_filesize : img->text_vmsize;

		for (i = 0; i < ane->segs[s].size; i++) {
			u8 a = 0, b = pre[s][i];
			unsigned int k;
			bool in_allowed = false;

			if (i < expect_bytes)
				a = fw->data[seg_file_off[s] + i];
			if (a == b)
				continue;
			for (k = 0; k < ARRAY_SIZE(allowed); k++)
				if (allowed[k][1] && seg_vm[s] + i >= allowed[k][0] &&
				    seg_vm[s] + i - allowed[k][0] < allowed[k][1])
					in_allowed = true;
			if (!in_allowed) {
				dev_err(ane->dev,
					"preload diff seg%u vm %#llx outside byte whitelist; refusing\n",
					s, seg_vm[s] + i);
				ret = -EINVAL;
				break;
			}
			if (seg_vm[s] + i >= ane->stage_size) {
				dev_err(ane->dev,
					"preload diff outside the staged image\n");
				ret = -EINVAL;
				break;
			}
			((u8 *)ane->stage_cpu)[seg_vm[s] + i] = b;
			accepted++;
		}
	}
	if (!ret) {
		sha256(ane->stage_cpu, ane->stage_size, digest);
		dev_info(ane->dev, "staged image accepted_diff_bytes=%llu sha256=%*phN\n",
			 accepted, SHA256_DIGEST_SIZE, digest);
	}
out:
	for (s = 0; s < 2; s++)
		kvfree(pre[s]);
	return ret;
}

/* ---- DART mapping of the staged image at the ADT IOVAs ---- */

static int ane_h16_stage_domain(struct ane_h16 *ane)
{
	LIST_HEAD(regions);
	struct iommu_resv_region *region;
	unsigned int s;
	int ret = 0;

	ane->domain = iommu_get_domain_for_dev(ane->dev);
	if (!ane->domain || !iommu_is_dma_domain(ane->domain)) {
		dev_err(ane->dev, "staging requires a DMA-API IOMMU domain\n");
		return -ENODEV;
	}
	/* Fixed aliases must be excluded from the DMA API's IOVA allocator
	 * before coherent allocation. A no-reg reserved-memory region with
	 * iommu-addresses supplies IOMMU_RESV_RESERVED at domain setup.
	 */
	iommu_get_resv_regions(ane->dev, &regions);
	for (s = 0; s < 2; s++) {
		bool covered = false;
		u64 start = ane->segs[s].iova, size = ane->segs[s].size;

		if (!size || start + size < start ||
		    ane->segs[s].phys + size < ane->segs[s].phys ||
		    !IS_ALIGNED(start | size | ane->segs[s].phys, SZ_16K)) {
			ret = -EINVAL;
			break;
		}
		list_for_each_entry(region, &regions, list) {
			u64 offset;

			if (region->type != IOMMU_RESV_RESERVED || start < region->start)
				continue;
			offset = start - region->start;
			if (offset <= region->length && size <= region->length - offset) {
				covered = true;
				break;
			}
		}
		if (!covered) {
			dev_err(ane->dev, "firmware alias IOVA is not reserved from DMA allocation\n");
			ret = -EINVAL;
			break;
		}
	}
	iommu_put_resv_regions(ane->dev, &regions);
	return ret;
}

static int ane_h16_map_stage(struct ane_h16 *ane)
{
	const struct ane_h16_fw *img = ane->soc->fw;
	const u64 seg_vm[2] = { 0, img->data_vm };
	unsigned int s;
	int ret;

	if (!ane->domain || !IS_ALIGNED(ane->stage_dma, SZ_16K)) {
		dev_err(ane->dev, "staged DMA IOVA not 16 KiB aligned or domain missing\n");
		return -EINVAL;
	}
	for (s = 0; s < 2; s++) {
		u64 off;

		for (off = 0; off < ane->segs[s].size; off += SZ_16K) {
			u64 pa;
			u64 iova = ane->segs[s].iova + off;
			u64 source = ane->stage_dma + seg_vm[s] + off;
			size_t len = min_t(u64, SZ_16K, ane->segs[s].size - off);

			if (seg_vm[s] + off + len > ane->stage_size) {
				dev_err(ane->dev, "mapping outside the staged image\n");
				return -EINVAL;
			}
			/* A coherent CPU pointer may be remapped/vmalloc-backed. The
			 * DMA handle is an IOVA; resolve its backing page via the DMA
			 * domain, never virt_to_phys(cpu_pointer) or dma_handle-as-PA.
			 */
			pa = iommu_iova_to_phys(ane->domain, source);
			if (!pa || !IS_ALIGNED(pa, SZ_16K)) {
				dev_err(ane->dev, "staged DMA page is untranslated or unaligned\n");
				return -EFAULT;
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
			ane->stage_mapped[s] += len;
		}
	}
	dev_info(ane->dev,
		 "staged image (%zu KiB) mapped at iova %#llx (TEXT) and %#llx (DATA)\n",
		 ane->stage_size / 1024, ane->segs[0].iova, ane->segs[1].iova);
	return 0;
}

static int ane_h16_unmap_stage(struct ane_h16 *ane)
{
	unsigned int s;
	int ret = 0;

	if (!ane->domain)
		return 0;
	for (s = 0; s < 2; s++) {
		u64 owned = ane->stage_mapped[s];

		if (!owned)
			continue;
		if (iommu_unmap(ane->domain, ane->segs[s].iova, owned) != owned) {
			dev_err(ane->dev,
				"short stage unmap: retaining buffer; reboot required\n");
			ret = -EIO;
		} else {
			ane->stage_mapped[s] = 0;
		}
	}
	return ret;
}

/* ---- H17 (T8140) ordered clock/power enable, kext steps 1-8 ----
 *
 * Adapter between the device's MMIO and the checked ordering helper
 * (ane/h17/bringup_order.h): CE handshake + 37 HAL tunables + PS/PWGATE
 * enable with bounded polls. Every access is durably logged (dev_info
 * with a monotonic sequence number, before it is performed) via the
 * pending callback; a failed log stops the sequence before MMIO.
 * Sources: firmware-load-path receipt §5.4 rows 1-8 (commit 0049f0b
 * lineage), CE/tunables decode at 1e4e9e7. On J700 this is the COLD
 * branch (this[0xa2] bit0 clear on first init; warm SCRATCH7:=1 is the
 * other branch, see order-helper receipt) — this code performs neither
 * warm nor cold scratch writes; the caller owns those.
 */

static int ane_h17_order_pending(void *ctx, enum h17_order_action action,
				 enum h17_order_bank bank, unsigned int off,
				 unsigned int width, unsigned long long value)
{
	static const char *const names[] = { "engine", "ps", "pwgate" };
	struct ane_h16 *ane = ctx;

	dev_info(ane->dev,
		 "h17-order seq=%u %s %s off=%#x width=%u val=%#llx\n",
		 ++ane->order_seq,
		 action == H17_ORDER_WRITE ? "write" : "read",
		 names[bank], off, width, value);
	return 0;
}

static void __iomem *ane_h17_order_base(struct ane_h16 *ane,
					enum h17_order_bank bank,
					size_t *size)
{
	switch (bank) {
	case H17_ORDER_ENGINE:
		*size = 0x2000000;
		return ane->engine;
	case H17_ORDER_PS:
		*size = 0x14000;	/* overlay pmgr window */
		return ane->pmgr;
	case H17_ORDER_PWGATE:
		*size = 0x4000;	/* receipt §2 PWGATE window */
		return ane->pwgate;
	}
	return NULL;
}

static int ane_h17_order_rw_check(struct ane_h16 *ane,
				  enum h17_order_bank bank,
				  unsigned int off, unsigned int width,
				  void __iomem **base)
{
	size_t size;

	*base = ane_h17_order_base(ane, bank, &size);
	if (!*base)
		return -ENODEV;
	if (off > size || width > size || off + width > size)
		return -ERANGE;
	return 0;
}

static int ane_h17_order_read(void *ctx, enum h17_order_bank bank,
			      unsigned int off, unsigned int width,
			      unsigned long long *value)
{
	struct ane_h16 *ane = ctx;
	void __iomem *base;
	int ret = ane_h17_order_rw_check(ane, bank, off, width, &base);

	if (ret)
		return ret;
	switch (width) {
	case 4:
		*value = readl_relaxed(base + off);
		return 0;
	case 8:
		*value = readq_relaxed(base + off);
		return 0;
	}
	return -EINVAL;
}

static int ane_h17_order_write(void *ctx, enum h17_order_bank bank,
			       unsigned int off, unsigned int width,
			       unsigned long long value)
{
	struct ane_h16 *ane = ctx;
	void __iomem *base;
	int ret = ane_h17_order_rw_check(ane, bank, off, width, &base);

	if (ret)
		return ret;
	switch (width) {
	case 4:
		writel_relaxed((u32)value, base + off);
		return 0;
	case 8:
		writeq_relaxed(value, base + off);
		return 0;
	}
	return -EINVAL;
}

static int ane_h17_order_delay(void *ctx, unsigned int us)
{
	(void)ctx;
	udelay(us);
	return 0;
}

static int ane_h16_h17_enable(struct ane_h16 *ane)
{
	const struct h17_order_ops ops = {
		ane_h17_order_pending, ane_h17_order_read,
		ane_h17_order_write, ane_h17_order_delay,
	};
	int ret;

	/* Steps 1-8, checked and logged. On failure the caller refuses
	 * the boot BEFORE any CPU release; resources stay put. */
	ret = h17_order_prepare(&ops, ane);
	if (ret)
		ane_h16_result(ane, 3, "REFUSED", "h17-order");
	return ret;
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
	ret = ane_h16_stage_domain(ane);
	if (ret) {
		ane_h16_result(ane, 3, "REFUSED", "dma-domain-or-alias-reservation");
		return ret;
	}
	/* Own the preload DRAM before anything may map or read it. */
	ret = ane_h16_admit_preload(ane, true);
	if (ret) {
		ane_h16_result(ane, 3, "REFUSED", "preload-ownership");
		return ret;
	}
	/* Quiescence: the wake word set means a firmware from an earlier
	 * attempt in this boot may still be running against this DRAM.
	 * Snapshots and CPU release refuse; reboot is the recovery. */
	if (ane_h16_rd32(ane, s->scratch0 + 7 * 4) == ANE_H16_WAKE_ACK) {
		dev_err(ane->dev,
			"SCRATCH7 already carries the wake word: firmware not quiescent; reboot before retrying\n");
		ane_h16_result(ane, 3, "REFUSED", "not-quiescent");
		return -EINVAL;
	}
	/* H17 steps 1-8: CE handshake, HAL tunables, PS/PWGATE enable,
	 * every access logged and polled through the ordering helper.
	 * Runs before any staging snapshot so the preload is read only
	 * once clocks and power are in the kext's post-enable state. */
	if (s->h17_order) {
		ret = ane_h16_h17_enable(ane);
		if (ret)
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
		goto unmap;
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
	 * SCRATCH7 for the wake word. H17 (steps 9, 10, 12 of the recipe)
	 * additionally clears all eight scratch words, sets SCRATCH6 and
	 * then clears SCRATCH7 — the cold branch (order-helper receipt). */
	if (s->h17_order) {
		unsigned int n;

		for (n = 0; n < 8; n++)
			ane_h16_wr32(ane, s->scratch0 + 4 * n, 0);
		ane_h16_wr32(ane, s->scratch0 + 6 * 4, 1);
	}
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

	/* H17 steps 19-21, only ever after the wake word (recipe §5.4:
	 * field 0x128 := 1, field 0x120 := 0, doorbell enable setBit 0).
	 * Step 17 is absent: this[0xa4] is always 0 on J700. */
	if (s->h17_order) {
		ane_h16_wr32(ane, s->wake_cfg1, 1);
		ane_h16_wr32(ane, s->wake_cfg2, 0);
		ane_h16_wr32(ane, s->doorbell_en,
			     ane_h16_rd32(ane, s->doorbell_en) | 1);
	}
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
	if (ane_h16_unmap_stage(ane)) {
		/* Some translations may still reference the buffer. Never free it. */
		ret = -EIO;
		goto rel_fw;
	}
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
	if (ane_h16_ps_count(soc) > ARRAY_SIZE(soc->ps_off))
		return -EINVAL;

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

	/* The windows are mapped WITHOUT request_region: on T8140 the ANE
	 * DARTs (0x4018x_0000) sit inside the engine window and the ps-probe
	 * pmgr-pwrstate devices sit inside the pmgr window, so the sibling
	 * drivers' reservations make a whole-window request fail -EBUSY.
	 * ioremap itself does not reserve; the ownership guarantee is the
	 * DT reg layout, not the region accounting. */
	{
		struct resource *res = platform_get_resource_byname(pdev,
								    IORESOURCE_MEM,
								    "engine");

		if (!res)
			return -ENXIO;
		ane->engine = devm_ioremap(&pdev->dev, res->start,
					   resource_size(res));
		if (!ane->engine)
			return -ENOMEM;
	}
	{
		struct resource *res = platform_get_resource_byname(pdev,
								    IORESOURCE_MEM,
								    "pmgr");

		if (!res)
			return -ENXIO;
		ane->pmgr = devm_ioremap(&pdev->dev, res->start,
					 resource_size(res));
		if (!ane->pmgr)
			return -ENOMEM;
	}
	if (soc->h17_order) {
		/* kext "PWGATE" provider window, reg[2] of /arm-io/ane
		 * (receipt §2: DT 0x3_00724000 size 0x4000). Required for
		 * the H17 boot path; absent means the overlay is stale. */
		struct resource *res = platform_get_resource_byname(pdev,
								    IORESOURCE_MEM,
								    "pwgate");

		if (!res) {
			dev_err(&pdev->dev,
				"H17 boot requires the pwgate reg window; update the overlay\n");
			return -ENXIO;
		}
		ane->pwgate = devm_ioremap(&pdev->dev, res->start,
					   resource_size(res));
		if (!ane->pwgate)
			return -ENOMEM;
	}

	/* Multi-domain consumers are attached by the DRIVER, not the core:
	 * genpd_dev_pm_attach() skips devices with more than one
	 * power-domains entry, so pm_runtime_get_sync() would power
	 * nothing. Attach every listed domain to a virtual device and
	 * hold each runtime-resumed; the ps_wait gate then verifies the
	 * ACTUAL bits at the register level. */
	{
		int num_pds = of_count_phandle_with_args(pdev->dev.of_node,
							 "power-domains",
							 "#power-domain-cells");
		unsigned int idx;

		if (num_pds <= 0 || num_pds > ANE_H16_MAX_PDS) {
			dev_err(&pdev->dev,
				"device has %d power-domains; refusing to touch the engine window\n",
				num_pds);
			return num_pds < 0 ? num_pds : -ENODEV;
		}
		for (idx = 0; idx < num_pds; idx++) {
			struct device *vdev = dev_pm_domain_attach_by_id(
				&pdev->dev, idx);

			if (IS_ERR(vdev)) {
				ret = PTR_ERR(vdev);
				dev_err(&pdev->dev,
					"power-domain %u attach failed: %d\n",
					idx, ret);
				goto pd_unwind;
			}
			ane->pd_vdev[idx] = vdev;
			ane->pd_nvdev++;
			ret = pm_runtime_get_sync(vdev);
			if (ret < 0) {
				dev_err(&pdev->dev,
					"power-domain %u vote failed: %d\n",
					idx, ret);
				pm_runtime_put_sync(vdev);
				dev_pm_domain_detach(vdev, true);
				ane->pd_vdev[idx] = NULL;
				ane->pd_nvdev--;
				goto pd_unwind;
			}
		}
		dev_info(&pdev->dev, "attached and held %u power domains\n",
			 ane->pd_nvdev);
	}
	ret = ane_h16_ps_wait(ane);
	if (ret) {
		ane_h16_result(ane, 1, "FAIL", "pmgr-actual-stuck");
		goto pd_off;
	}

	ret = ane_h16_status(ane);
	if (ret)
		goto pd_off;
	if (stage_idx == 1) {
		ane_h16_result(ane, 1, "PASS", "ps-guard+reads");
		goto pd_off;
	}
	ret = ane_h16_boot(ane);
	if (ane->fw_touched) {
		/* CPU release is an ownership boundary, even on timeout. Returning
		 * an error would let failed-probe devres cleanup discard mappings
		 * that firmware may still use. Keep the device bound, power held,
		 * and module pinned until reboot. RESULT (not insmod's status)
		 * reports the actual boot outcome.
		 */
		ane->boot_error = ret;
		__module_get(THIS_MODULE);
		dev_emerg(&pdev->dev,
			  "CPU release attempted: boot_error=%d; retaining binding, mappings and power; do NOT unload; reboot required\n",
			  ret);
		return 0;
	}
pd_unwind:
	ane_h16_pd_release(ane);
	return ret;
pd_off:
	ane_h16_pd_release(ane);
	return ret;
}

static void ane_h16_pd_release(struct ane_h16 *ane)
{
	unsigned int i;

	for (i = 0; i < ane->pd_nvdev; i++) {
		if (!ane->pd_vdev[i])
			continue;
		pm_runtime_put_sync(ane->pd_vdev[i]);
		dev_pm_domain_detach(ane->pd_vdev[i], true);
		ane->pd_vdev[i] = NULL;
	}
	ane->pd_nvdev = 0;
}

static void ane_h16_remove(struct platform_device *pdev)
{
	struct ane_h16 *ane = platform_get_drvdata(pdev);

	if (ane->fw_touched) {
		dev_emerg(&pdev->dev,
			  "remove with the firmware released: leaving power on, reboot required\n");
		return;
	}
	ane_h16_pd_release(ane);
}

static const struct of_device_id ane_h16_of_match[] = {
	{ .compatible = "apple,t8132-ane", .data = &ane_t8132_soc },
	{ .compatible = "apple,t6040-ane", .data = &ane_t6040_soc },
	{ .compatible = "apple,t6041-ane", .data = &ane_t6041_soc },
	{ .compatible = "apple,t8140-ane", .data = &ane_t8140_soc },
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
