// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h16 — OPT-IN EXPERIMENTAL bring-up module for the H16/H17-family
 * ANE (the shipped driver generation: T8132, T6040, T6041; H17 rows
 * live in ane/h17/). It has run on J700/T8140 silicon in the opt-in
 * `stage=boot` path (RESULT PASS boot-wake, 2026-11-04: see
 * receipts/2026-11-04-j700-h17-first-firmware-wake/README.md); the
 * generic H16 rows have not run on any device. It registers no
 * DRM device, performs no inference and sends no CSNE command.
 *
 * Three stages, selected by the "stage" module parameter:
 *
 *   stage=dt: parse the running DT (engine/pmgr windows from the soc
 *   row, ps word offsets, the firmware pin) and print the word groups
 *   with their evidence tier. No MMIO access of any kind: the first
 *   run on a volunteer machine is hardware-silent.
 *
 *   stage=status (needs confirm_read=1): enable the ANE power domains
 *   named in the device node, wait until every pmgr ANE state word
 *   reads ACTUAL=0xf (the standing rule from the T6021 program: never
 *   touch the engine window before that), then log RVBAR, CPU_STATUS and
 *   the two mailbox control words. The SCRATCH words are read only with
 *   read_scratch=1. Read-only apart from the power domains.
 *
 *   stage=boot (EXPERIMENTAL, firmware-boot smoke only): everything
 *   "status" does, plus: parse the boot ADT from the reserved-memory
 *   phram "adt" region, read /arm-io/aneX segment-ranges, validate the
 *   iBoot-preloaded firmware against the pinned payload (only
 *   differences inside the _rtk_patchbay and _rtk_tunables sections are
 *   accepted, plus at most one entry/DATA-base-looking u64 per segment),
 *   copy the validated image into a DART-mapped coherent buffer at the
 *   same IOVAs the ADT names, write SCRATCH7=0, latch or write RVBAR,
 *   release the CPU (CPU_CONTROL 0 then 0x10) and poll SCRATCH7 for the
 *   wake word 0x08042006 (firmware startup). If the firmware
 *   wakes, poll the ASC mailbox for an RTKit HELLO, answer it, ack
 *   EPMAP and STARTEP the system endpoints. Then stop: no channel-
 *   manager publication and no CSNE CONFIG_GET — the pinned host contract
 *   for those is not derived yet (receipts/2026-10-03-ane-h16).
 *
 *   On the t8140 (H17) row the boot stage additionally offers the opt-in
 *   irq_read=1 diagnostic: after the wake word and the recipe steps
 *   19-21, it reads the IRQ status word at the SoC row's irq_status
 *   (0x188c000) exactly once and logs it. Read-only: it never touches
 *   the IRQ ack register (0x1890000), rings no doorbell and submits no
 *   task (firmware-load-path receipt §13.3 pins the status/ack pair;
 *   a register table is not device behaviour, and one read of an
 *   unacked status word is a value, not a protocol).
 *
 *   chanreg=1 is a separate, default-off T8140 table-discovery rung:
 *   back the full FWIM arena, allocate coherent descriptor/IPC/HEAP,
 *   publish through SCRATCH0/1/7, copy only an owned IPC table, and
 *   STOP before SCRATCH3 registration. No channel-buffer initialization,
 *   doorbell, task or IRQ ack. Source contract: e1c4839, receipt sec 16.
 *
 * The module refuses to probe unless its opt-in key is set:
 *   insmod ane_h16.ko optin=t8132 stage=status confirm_read=1
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
#include <linux/pm_domain.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "../h17/bringup_order.h"
#include <linux/string.h>
#include <linux/types.h>

#include "ane_h16.h"
#include "ane_h16_overlay.h"
#include "../h17/chanreg.h"

static char *optin;
module_param(optin, charp, 0444);
MODULE_PARM_DESC(optin,
		 "Opt-in key: must equal the SoC row name (t8132, t6040, t6041, ...) or probe returns -EPERM.");

static char *stage = "dt";
module_param(stage, charp, 0444);
MODULE_PARM_DESC(stage,
		 "dt/0 (default): parse the DT and print word groups, no MMIO. status/1: power up and log registers only; needs confirm_read=1. boot/3: firmware-boot smoke (EXPERIMENTAL); needs confirm_read=1. 2 is not an H16 module stage (the ladder's fw-pin step runs in userspace).");

static bool confirm_read;
module_param(confirm_read, bool, 0444);
MODULE_PARM_DESC(confirm_read,
		 "Required for stage=status and stage=boot. Without it the module refuses before it maps, powers or reads anything. Default off.");

static bool read_scratch;
module_param(read_scratch, bool, 0444);
MODULE_PARM_DESC(read_scratch,
		 "stage=status: also read the eight SCRATCH words. Nobody has read them on an M4, and a read the block does not answer can hang the machine. Default off.");

static bool apply_overlay;
module_param(apply_overlay, bool, 0444);
MODULE_PARM_DESC(apply_overlay,
		 "optin=t6040 only. The tree holds the ANE node, its five power controllers and its DART nodes but marks them disabled: enable the five power controllers and the ANE node and complete the ANE node (pmgr window, reg-names, apple,ane-type, power-domains) in one change set, then register the driver. The DART nodes are not touched. Unloading reverts it. Default off.");

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

static bool irq_read;
module_param(irq_read, bool, 0444);
MODULE_PARM_DESC(irq_read,
		 "t8140 stage=boot only: after the SCRATCH7 wake word, read the IRQ status word at the SoC row's irq_status (0x188c000) exactly once and log it. Read-only: no IRQ ack access, no doorbell, no task. Default 0 (off).");

static bool scratch_read;
module_param(scratch_read, bool, 0444);
MODULE_PARM_DESC(scratch_read,
		 "t8140 stage=boot only: after the SCRATCH7 wake word, snapshot the eight ANE scratch words (SCRATCH0..7, the row's scratch0..scratch0+28) with exactly one read each and log them in order. Read-only: no scratch write beyond the boot recipe, no IRQ ack access, no doorbell, no task. Default 0 (off).");

static bool chanreg;
module_param(chanreg, bool, 0444);
MODULE_PARM_DESC(chanreg,
		 "t8140 stage=boot only: experimental initialization-descriptor/table-discovery rung. Requires the recorded initial scratch geometry; allocates coherent DM/IPC/HEAP, writes SCRATCH0/1/7, bounds-checks and copies the IPC table, then STOPS before SCRATCH3 registration. No doorbell, IRQ ack or task. Default 0 (off).");

static bool register_channels;
module_param(register_channels, bool, 0444);
MODULE_PARM_DESC(register_channels,
		 "t8140 stage=boot only: separate experimental channel-registration rung. Requires chanreg=1; validates every table buffer against owned logical IPC, initializes only source-traced type-0 control blocks, writes SCRATCH3 and waits for its clear ack, then stops. No doorbell, IRQ ack or task. Default 0 (off).");

#define ANE_H16_WAKE_ACK		0x08042006u
#define ANE_H16_CPU_RUN_RELEASE		0x10u
/* RVBAR fold: firmware startup uses base | (iova & mask); the base
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

#define ANE_H16_MAX_PD	5

struct ane_h17_chanbuf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
	u64 iova;
	u64 mapped;
};

struct ane_h16 {
	struct device *dev;
	const struct ane_h16_soc *soc;
	void __iomem *engine;
	void __iomem *pmgr;
	struct device *pd_dev[ANE_H16_MAX_PD];
	struct device_link *pd_link[ANE_H16_MAX_PD];
	int pd_count;
	struct ane_h16_seg segs[2];
	void __iomem *pwgate;	/* H17 reg[2]: PS/PWGATE bank (overlay) */
	unsigned int order_seq;	/* durable h17-order sequence number */
	bool fw_touched;
	bool fw_started;
	bool hello_done;
	int boot_error;		/* result retained when binding must survive a timeout */
	void *stage_cpu;
	dma_addr_t stage_dma;
	size_t stage_size;
	struct iommu_domain *domain;
	u64 stage_mapped[2];	/* owned contiguous prefixes, never foreign ranges */
	struct ane_h17_chanbuf chanbuf[3]; /* DM, IPC, HEAP; retained after release */
	u8 *chan_table;
};

/* Release every domain this module attached, newest first. */
static void ane_h16_detach_pds(struct ane_h16 *ane)
{
	int i;

	for (i = ane->pd_count - 1; i >= 0; i--) {
		if (ane->pd_link[i])
			device_link_del(ane->pd_link[i]);
		if (ane->pd_dev[i])
			dev_pm_domain_detach(ane->pd_dev[i], true);
		ane->pd_link[i] = NULL;
		ane->pd_dev[i] = NULL;
	}
	ane->pd_count = 0;
}

/* A platform device attaches a power domain by itself only when exactly one is listed. With several,
 * the driver attaches each one, as ane_h15 does. */
static int ane_h16_attach_pds(struct ane_h16 *ane)
{
	struct device *dev = ane->dev;
	int count, i;

	count = of_count_phandle_with_args(dev->of_node, "power-domains", "#power-domain-cells");
	if (count <= 1)
		return 0;
	if (count > ANE_H16_MAX_PD) {
		dev_err(dev, "too many power-domains: %d\n", count);
		return -EINVAL;
	}
	for (i = 0; i < count; i++) {
		struct device *pd = dev_pm_domain_attach_by_id(dev, i);

		if (IS_ERR_OR_NULL(pd)) {
			int err = pd ? PTR_ERR(pd) : -ENODEV;

			dev_err(dev, "attach power-domain %d: %d\n", i, err);
			ane_h16_detach_pds(ane);
			return err;
		}
		ane->pd_dev[i] = pd;
		ane->pd_count = i + 1;
		ane->pd_link[i] = device_link_add(dev, pd, DL_FLAG_STATELESS |
						  DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!ane->pd_link[i]) {
			dev_err(dev, "link power-domain %d\n", i);
			ane_h16_detach_pds(ane);
			return -EINVAL;
		}
	}
	dev_info(dev, "attached %d ANE power-domains\n", ane->pd_count);
	return 0;
}

static void ane_h16_unmap(struct ane_h16 *ane)
{
	if (ane->engine)
		iounmap(ane->engine);
	if (ane->pmgr)
		iounmap(ane->pmgr);
	ane->engine = NULL;
	ane->pmgr = NULL;
}

/* of_iomap maps without a region request. The pmgr driver already owns part of the pmgr window, so a requested
 * mapping (devm_platform_ioremap_resource_byname) would fail with a resource conflict. */
static int ane_h16_map(struct ane_h16 *ane)
{
	struct device_node *np = ane->dev->of_node;
	int e = of_property_match_string(np, "reg-names", "engine");
	int p = of_property_match_string(np, "reg-names", "pmgr");

	if (e < 0 || p < 0) {
		dev_err(ane->dev, "the node needs reg-names \"engine\" and \"pmgr\"\n");
		return -EINVAL;
	}
	ane->engine = of_iomap(np, e);
	ane->pmgr = of_iomap(np, p);
	if (!ane->engine || !ane->pmgr) {
		dev_err(ane->dev, "could not map the engine and pmgr windows\n");
		ane_h16_unmap(ane);
		return -ENXIO;
	}
	return 0;
}

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

/* The address goes to the log before the read, so a hang names the register. */
static void ane_h16_note_read(struct ane_h16 *ane, const char *name, u32 off)
{
	dev_info(ane->dev, "ane_h16 reading %s at engine+%#x\n", name, off);
}

static int ane_h16_status(struct ane_h16 *ane)
{
	const struct ane_h16_soc *s = ane->soc;
	u64 rvbar;
	unsigned int i;

	ane_h16_note_read(ane, "RVBAR", s->rvbar);
	rvbar = ane_h16_rd64(ane, s->rvbar);
	dev_info(ane->dev, "RVBAR (%#x) = %#llx (%slatched)\n", s->rvbar,
		 rvbar, (rvbar & 1) ? "" : "un");
	ane_h16_note_read(ane, "CPU_STATUS", s->cpu_status);
	dev_info(ane->dev, "CPU_STATUS (%#x) = %#x\n", s->cpu_status,
		 ane_h16_rd32(ane, s->cpu_status));
	ane_h16_note_read(ane, "mbox a2i_ctrl", s->mbox + ANE_H16_MBOX_A2I_CTRL);
	dev_info(ane->dev, "mbox a2i_ctrl (%#x) = %#x\n",
		 s->mbox + ANE_H16_MBOX_A2I_CTRL,
		 ane_h16_rd32(ane, s->mbox + ANE_H16_MBOX_A2I_CTRL));
	ane_h16_note_read(ane, "mbox i2a_ctrl", s->mbox + ANE_H16_MBOX_I2A_CTRL);
	dev_info(ane->dev, "mbox i2a_ctrl (%#x) = %#x\n",
		 s->mbox + ANE_H16_MBOX_I2A_CTRL,
		 ane_h16_rd32(ane, s->mbox + ANE_H16_MBOX_I2A_CTRL));
	if (!read_scratch)
		return 0;
	for (i = 0; i < 8; i++) {
		ane_h16_note_read(ane, "SCRATCH", s->scratch0 + 4 * i);
		dev_info(ane->dev, "SCRATCH%u (%#x) = %#x\n", i,
			 s->scratch0 + 4 * i,
			 ane_h16_rd32(ane, s->scratch0 + 4 * i));
	}
	return 0;
}

/* ---- boot ADT, read from the reserved-memory phram "adt" region ---- */

static const void *adt;
static size_t adt_len;

/* Every ADT offset is u32; refusing larger regions keeps them representable. */
#define ADT_MAX_LEN (1U << 30)

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

	if ((u64)off + 8 > adt_len)
		return n;	/* past the mapping: an empty node ends the walk */
	n.nprops = le32_to_cpup(adt + off);
	n.nchildren = le32_to_cpup(adt + off + 4);
	return n;
}

/* Real boot ADTs nest a handful of levels; this only bounds hostile data. */
#define ADT_MAX_DEPTH 32

/* Truncated or too-deep data ends every later walk at an empty node. */
static u32 adt_end_sentinel(void)
{
	return adt_len;
}

/*
 * Offset just past the last byte of the subtree rooted at off. Iterative:
 * todo[d] counts the nodes still to enter at depth d, so stack use is fixed
 * for any input. Each node and property consumes bytes, so the walk is also
 * bounded by adt_len.
 */
static u32 adt_subtree_end(u32 off)
{
	u32 todo[ADT_MAX_DEPTH] = { 1 };
	unsigned int depth = 0;

	for (;;) {
		u32 nprops, nchildren, i;

		while (!todo[depth]) {
			if (!depth)
				return off;
			depth--;
		}
		todo[depth]--;
		if ((u64)off + 8 > adt_len)
			return adt_end_sentinel();
		nprops = le32_to_cpup(adt + off);
		nchildren = le32_to_cpup(adt + off + 4);
		off += 8;
		for (i = 0; i < nprops; i++) {
			u64 extent;

			if ((u64)off + 36 > adt_len)
				return adt_end_sentinel();
			extent = (u64)off + 36 +
				 ALIGN(le32_to_cpup(adt + off + 32) & 0x7fffffff, 4);
			if (extent > adt_len)
				return adt_end_sentinel();
			off = extent;
		}
		if (nchildren) {
			if (++depth >= ADT_MAX_DEPTH)
				return adt_end_sentinel();
			todo[depth] = nchildren;
		}
	}
}

static struct adt_node adt_child(struct adt_node n, unsigned int idx)
{
	u32 off;
	unsigned int i;

	off = n.off + 8;
	for (i = 0; i < n.nprops; i++) {
		u32 size;
		u64 extent;

		if ((u64)off + 36 > adt_len)
			break;
		size = le32_to_cpup(adt + off + 32) & 0x7fffffff;
		extent = (u64)off + 36 + ALIGN(size, 4);
		if (extent > adt_len) {
			/* truncated body: end the walk at an empty node
			 * instead of wrapping the offset */
			return adt_node_at(adt_end_sentinel());
		}
		off = extent;
	}
	for (i = 0; i < idx; i++) {
		if ((u64)off + 8 > adt_len)
			break;
		off = adt_subtree_end(off);
	}
	return adt_node_at(off);
}

static int adt_prop(struct adt_node n, const char *name, const void **data,
		    u32 *size)
{
	u32 off = n.off + 8;
	unsigned int i;

	for (i = 0; i < n.nprops; i++) {
		u32 len;
		u64 extent;

		if ((u64)off + 36 > adt_len)
			return -ENOENT;
		len = le32_to_cpup(adt + off + 32) & 0x7fffffff;
		extent = (u64)off + 36 + ALIGN(len, 4);
		/* The whole body must lie inside the mapping: returning
		 * or advancing past a truncated extent hands the caller
		 * bytes read from outside the ADT.
		 */
		if (extent > adt_len)
			return -EOVERFLOW;
		if (!strncasecmp(adt + off, name, 32)) {
			*data = adt + off + 36;
			*size = len;
			return 0;
		}
		if (extent > (u32)~0ULL)
			return -EOVERFLOW;
		off = extent;
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
		if (resource_size(&res) > ADT_MAX_LEN) {
			dev_err(ane->dev,
				"ADT: region %#llx bytes exceeds the %#x limit; refusing\n",
				(unsigned long long)resource_size(&res), ADT_MAX_LEN);
			ret = -EFBIG;
			of_node_put(np);
			goto out;
		}
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
 * The ASC sees remap. The shipped driver reads the TEXT size at entry
 * +0x18 and the DATA size at entry +0x38 as 32-bit words: size is u32,
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
	struct adt_node root;
	const void *data;
	u32 size;
	int ret;

	if (ane_h16_adt_open(ane))
		return -ENODEV;
	root = adt_node_at(0);
	if (!root.nchildren) {
		dev_err(ane->dev,
			"ADT: region is not an ADT (root %u/%u); refusing\n",
			root.nprops, root.nchildren);
		return -EINVAL;
	}
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
	/* The descriptor advertises the 0x380000 FWIM arena, not just the
	 * 0x36c000 image segments. Back its remaining 0x14000 with owned,
	 * zeroed coherent memory too. Default wake-only layout is unchanged.
	 */
	if (chanreg)
		ane->stage_size = ANE_H17_FW_BYTES;
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

static u64 ane_h16_stage_alias_size(struct ane_h16 *ane, unsigned int s)
{
	return chanreg && s == 1 ? ANE_H17_FW_BYTES - 0xc0000 : ane->segs[s].size;
}

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
	if (chanreg && (ane->segs[0].iova != ANE_H17_DMA_BASE ||
			ane->segs[0].size != 0xc0000 ||
			ane->segs[1].size != 0x2ac000 ||
			ane->soc->fw->data_vm != 0xc0000 ||
			ane->soc->fw->data_vmsize != 0x2ac000 ||
			ane->segs[1].iova != ane->segs[0].iova + 0xc0000))
		return -EINVAL;
	/* Fixed aliases must be excluded from the DMA API's IOVA allocator
	 * before coherent allocation. A no-reg reserved-memory region with
	 * iommu-addresses supplies IOMMU_RESV_RESERVED at domain setup.
	 */
	iommu_get_resv_regions(ane->dev, &regions);
	for (s = 0; s < 2; s++) {
		bool covered = false;
		u64 start = ane->segs[s].iova;
		u64 size = ane_h16_stage_alias_size(ane, s);

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
	if (!ret && chanreg) {
		bool covered = false;

		list_for_each_entry(region, &regions, list) {
			if (region->type == IOMMU_RESV_RESERVED &&
			    ane_h17_full_range(region->start, region->length,
					       ANE_H17_DM_IOVA, ANE_H17_CHAN_ALIAS_BYTES)) {
				covered = true;
				break;
			}
		}
		if (!covered) {
			dev_err(ane->dev, "channel alias IOVAs not reserved; update manual overlay before reboot\n");
			ret = -EINVAL;
		} else {
			dev_info(ane->dev,
				 "H17 ALIAS_RESERVATION start=%#llx bytes=%#llx type=IOMMU_RESV_RESERVED\n",
				 (u64)ANE_H17_DM_IOVA,
				 (u64)ANE_H17_CHAN_ALIAS_BYTES);
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
		u64 alias_size = ane_h16_stage_alias_size(ane, s);

		for (off = 0; off < alias_size; off += SZ_16K) {
			u64 pa;
			u64 iova = ane->segs[s].iova + off;
			u64 source = ane->stage_dma + seg_vm[s] + off;
			size_t len = min_t(u64, SZ_16K, alias_size - off);

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

static bool ane_h17_overlap(u64 a, u64 asize, u64 b, u64 bsize)
{
	return asize && bsize && (a <= b ? b - a < asize : a - b < bsize);
}

static int ane_h17_alloc_chanbuf(struct ane_h16 *ane, unsigned int index,
				 size_t bytes)
{
	const u64 aliases[] = { ANE_H17_DM_IOVA, ANE_H17_IPC_IOVA, ANE_H17_HEAP_IOVA };
	struct ane_h17_chanbuf *b;
	u64 off;
	unsigned int i;
	int ret;

	/* This path runs only after wake: partial aliases/backing are retained
	 * on every refusal, like the existing staging mappings. Never retry.
	 */
	if (!ane->fw_touched || index >= ARRAY_SIZE(aliases))
		return -EINVAL;
	b = &ane->chanbuf[index];
	if (b->cpu || b->mapped)
		return -EBUSY;
	b->iova = aliases[index];

	b->size = ALIGN(bytes, SZ_16K);
	if (!ane_h17_channel_range(b->iova, b->size))
		return -EINVAL;
	b->cpu = dma_alloc_coherent(ane->dev, b->size, &b->dma, GFP_KERNEL);
	if (!b->cpu)
		return -ENOMEM;
	/* CPU was already released: even partial/failed preparation keeps all
	 * allocations until reboot, with the binding and staged firmware.
	 */
	memset(b->cpu, 0, b->size);
	if (!IS_ALIGNED(b->dma, SZ_16K) ||
	    !ane_h17_full_range(ANE_H17_DMA_BASE,
				 ANE_H17_DMA_END - ANE_H17_DMA_BASE,
				 b->dma, b->size))
		return -EINVAL;
	if (ane_h17_overlap(b->dma, b->size, ane->stage_dma, ane->stage_size))
		return -EBUSY;
	if (ane_h17_overlap(b->dma, b->size, ANE_H17_DM_IOVA,
			    ANE_H17_CHAN_ALIAS_BYTES))
		return -EBUSY;
	for (i = 0; i < 2; i++)
		if (ane_h17_overlap(b->dma, b->size,
				    ane->segs[i].iova, ane->stage_mapped[i]))
			return -EBUSY;
	for (i = 0; i < index; i++)
		if (ane_h17_overlap(b->dma, b->size,
				    ane->chanbuf[i].dma, ane->chanbuf[i].size))
			return -EBUSY;
	for (off = 0; off < b->size; off += SZ_16K) {
		u64 pa = iommu_iova_to_phys(ane->domain, b->dma + off);

		if (!pa || !IS_ALIGNED(pa, SZ_16K))
			return -EFAULT;
		if (iommu_iova_to_phys(ane->domain, b->iova + off))
			return -EBUSY;
		ret = iommu_map(ane->domain, b->iova + off, pa, SZ_16K,
				IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
		if (ret)
			return ret;
		b->mapped += SZ_16K;
		if (iommu_iova_to_phys(ane->domain, b->iova + off) != pa)
			return -EFAULT;
	}
	dev_info(ane->dev, "H17 TABLE_BACKING index=%u dma=%#llx alias=%#llx mapped=%#llx\n",
		 index, (u64)b->dma, b->iova, b->mapped);
	return 0;
}

static int ane_h17_register_channels(struct ane_h16 *ane,
				     struct ane_h17_chanbuf *ipc)
{
	const struct ane_h16_soc *s = ane->soc;
	struct ane_h17_channel_init_range init[ANE_H17_CHANNEL_COUNT];
	u32 init_count = 0, i;
	unsigned long deadline;
	int ret;

	if (!ane_h17_validate_channel_table(ane->chan_table,
					    ipc->iova, ANE_H17_IPC_BYTES,
					    init, &init_count)) {
		dev_err(ane->dev,
			"H17 REGISTER refused malformed, unowned, or overlapping channel table\n");
		ane_h16_result(ane, 3, "REFUSED", "channel-table-envelope");
		return -EINVAL;
	}
	for (i = 0; i < init_count; i++) {
		u8 *block = (u8 *)ipc->cpu + init[i].offset;
		u64 off;

		for (off = 0; off < init[i].bytes;
		     off += ANE_H17_CHANNEL_SLOT_BYTES) {
			u8 *slot = block + off;

			/* J700's type-0 channel constructor stores flag, then clears
			 * the two 64-bit control words. The coherent IPC arena was
			 * zeroed at allocation; leave the rest of each 0x40 block
			 * unchanged, matching the traced stores.
			 */
			ane_h17_put_le(slot, 1, 8);
			memset(slot + 8, 0, 16);
		}
		dev_info(ane->dev,
			 "H17 CHANNEL_INIT index=%u offset=%#llx bytes=%#llx entries=%#llx\n",
			 init[i].index, init[i].offset, init[i].bytes,
			 init[i].bytes / ANE_H17_CHANNEL_SLOT_BYTES);
	}
	/* Coherent CPU writes must be visible before firmware sees registration. */
	dma_wmb();
	dev_info(ane->dev,
		 "H17 CHANNEL_REGISTER request=%#x scratch3=%#x\n",
		 ANE_H16_WAKE_ACK, s->scratch0 + 12);
	ane_h16_wr32(ane, s->scratch0 + 12, ANE_H16_WAKE_ACK);
	deadline = jiffies + msecs_to_jiffies(1000);
	do {
		ret = ane_h16_rd32(ane, s->scratch0 + 12);
		if (!ret) {
			dev_info(ane->dev,
				 "H17 CHANNEL_REGISTER_ACK value=0\n");
			dev_info(ane->dev,
				 "H17 CHANNEL_REGISTER_STOP after ack; no doorbell, IRQ ack or task\n");
			ane_h16_result(ane, 3, "PASS", "boot-channel-registration");
			return 0;
		}
		if (time_after(jiffies, deadline))
			break;
		usleep_range(1000, 2000);
	} while (1);
	dev_err(ane->dev,
		"H17 CHANNEL_REGISTER timeout scratch3=%#x; buffers retained\n",
		ret);
	ane_h16_result(ane, 3, "FAIL", "channel-registration-timeout");
	return -ETIMEDOUT;
}

static int ane_h17_discover_table(struct ane_h16 *ane)
{
	const struct ane_h16_soc *s = ane->soc;
	const size_t sizes[] = {
		ANE_H17_DM_BYTES, ANE_H17_IPC_BYTES, ANE_H17_HEAP_BYTES,
	};
	struct ane_h17_chanbuf *dm = &ane->chanbuf[0];
	struct ane_h17_chanbuf *ipc = &ane->chanbuf[1];
	struct ane_h17_chanbuf *heap = &ane->chanbuf[2];
	u8 digest[SHA256_DIGEST_SIZE];
	u32 count, ipc_last, heap_bytes;
	u64 address, offset;
	unsigned long deadline;
	unsigned int i;
	int ret;

	/* First-run envelope, not arbitrary firmware-controlled allocations.
	 * Source: e1c4839 receipt sec 16; snapshot: 6610ec7 boot 3f9aa97c.
	 */
	count = ane_h16_rd32(ane, s->scratch0);
	ipc_last = ane_h16_rd32(ane, s->scratch0 + 4);
	heap_bytes = ane_h16_rd32(ane, s->scratch0 + 12);
	if (count != 8 || ipc_last != ANE_H17_IPC_BYTES - 1 ||
	    heap_bytes != ANE_H17_HEAP_BYTES ||
	    ane->stage_size != ANE_H17_FW_BYTES ||
	    ane->segs[0].size != 0xc0000 ||
	    ane->segs[1].size != 0x2ac000 ||
	    ane->segs[1].iova != ane->segs[0].iova + 0xc0000) {
		dev_err(ane->dev,
			"H17 TABLE refused geometry count=%u ipc_last=0x%08x heap=0x%08x\n",
			count, ipc_last, heap_bytes);
		ane_h16_result(ane, 3, "REFUSED", "table-input-envelope");
		return -EINVAL;
	}
	ane->chan_table = kvzalloc(ANE_H17_TABLE_BYTES, GFP_KERNEL);
	if (!ane->chan_table) {
		ane_h16_result(ane, 3, "FAIL", "table-copy-allocation");
		return -ENOMEM;
	}
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		ret = ane_h17_alloc_chanbuf(ane, i, sizes[i]);
		if (ret) {
			ane_h16_result(ane, 3, "REFUSED", "table-dma-allocation-or-range");
			return ret;
		}
	}
	if (!ane_h17_channel_layout(dm->iova, dm->size, ipc->iova, ipc->size)) {
		ane_h16_result(ane, 3, "REFUSED", "table-address-layout");
		return -EINVAL;
	}
	ane_h17_encode_descriptor(dm->cpu, ane->segs[0].iova,
				  dm->iova, ipc->iova, heap->iova);
	sha256(dm->cpu, ANE_H17_DESC_BYTES, digest);
	dev_info(ane->dev,
		 "H17 TABLE_PREPARE dm=%#llx ipc=%#llx ipc_bytes=0xc441 heap=%#llx descriptor_bytes=0x180 sha256=%*phN\n",
		 dm->iova, ipc->iova, heap->iova,
		 SHA256_DIGEST_SIZE, digest);
	/* Publish only completely mapped aliases of owned coherent memory.
	 * stage_domain checked their DMA-allocator reservations before staging.
	 */
	dma_wmb();
	ane_h16_wr32(ane, s->scratch0, (u32)dm->iova);
	ane_h16_wr32(ane, s->scratch0 + 4, dm->iova >> 32);
	wmb();
	ane_h16_wr32(ane, s->scratch0 + 28, ANE_H17_TABLE_REQUEST);
	deadline = jiffies + msecs_to_jiffies(1000);
	while (ane_h16_rd32(ane, s->scratch0 + 28) != ANE_H16_WAKE_ACK) {
		if (time_after(jiffies, deadline)) {
			ane_h16_result(ane, 3, "FAIL", "table-reply-timeout");
			return -ETIMEDOUT;
		}
		usleep_range(1000, 2000);
	}
	address = ane_h16_rd32(ane, s->scratch0 + 4);
	address = (address << 32) | ane_h16_rd32(ane, s->scratch0);
	if (!ane_h17_full_range(ipc->iova, ANE_H17_IPC_BYTES,
				 address, ANE_H17_TABLE_BYTES)) {
		dev_err(ane->dev, "H17 TABLE refused reply address=%#llx\n", address);
		ane_h16_result(ane, 3, "REFUSED", "table-reply-outside-ipc");
		return -EINVAL;
	}
	offset = address - ipc->iova;
	dma_rmb();
	memcpy(ane->chan_table, (u8 *)ipc->cpu + offset, ANE_H17_TABLE_BYTES);
	sha256(ane->chan_table, ANE_H17_TABLE_BYTES, digest);
	dev_info(ane->dev,
		 "H17 TABLE_COPY address=%#llx bytes=0x800 count=8 sha256=%*phN\n",
		 address, SHA256_DIGEST_SIZE, digest);
	for (i = 0; i < 8; i++) {
		const u8 *entry = ane->chan_table + i * 0x100;

		/* Hex names cannot inject lines into the rung log. Discovery mode
		 * only observes these fields; the separately opted-in registration
		 * path validates every buffer before touching type-0 blocks.
		 */
		dev_info(ane->dev,
			 "H17 CHANNEL index=%u name_hex=%*phN type=0x%08x source=0x%08x slots=0x%08x buffer=0x%016llx\n",
			 i, 32, entry, (u32)ane_h17_get_le(entry + 0x40, 4),
			 (u32)ane_h17_get_le(entry + 0x44, 4),
			 (u32)ane_h17_get_le(entry + 0x48, 4),
			 ane_h17_get_le(entry + 0x50, 8));
	}
	if (register_channels)
		return ane_h17_register_channels(ane, ipc);
	dev_info(ane->dev,
		 "H17 TABLE_STOP before SCRATCH3 registration; buffers retained; no doorbell, IRQ ack or task\n");
	ane_h16_result(ane, 3, "PASS", "boot-table-discovery");
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
		 ret ? " (mailbox stayed empty; the pinned firmware may not speak RTKit when brought up this way — see the receipt)" : "");
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

	/* firmware cold-boot order: SCRATCH7 = 0, RVBAR
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
	if (s->h17_order) {
		/* Opt-in post-wake diagnostic (irq_read=1): a single
		 * read-only snapshot of the firmware->host IRQ status
		 * word, taken before any ack or channel work, to record
		 * which lines the just-started firmware asserts. The
		 * ack register (0x1890000, W1C 0xffff) is NOT read or
		 * written here: the status bits stay un-acked, exactly
		 * as a firmware that never raised them would leave
		 * them.
		 */
		if (irq_read)
			dev_info(ane->dev,
				 "H17 IRQ_STATUS off=%#x value=0x%08x (single read-only post-wake snapshot; ack register untouched)\n",
				 s->irq_status, ane_h16_rd32(ane, s->irq_status));
		/* Opt-in post-wake diagnostic (scratch_read=1): the
		 * channel-registration handshake (receipt sec 14) puts
		 * the firmware-published channel count in SCRATCH0,
		 * carries the firmware's table-ready ack in SCRATCH7,
		 * and leaves SCRATCH3 for the host's registration word
		 * (which we never write). One read of each of the
		 * eight words, in order, logged once; no scratch
		 * writes beyond the boot recipe, no IRQ ack access, no
		 * doorbell, no task.
		 */
		if (scratch_read) {
			unsigned int n;
			u32 sw[8];

			for (n = 0; n < 8; n++)
				sw[n] = ane_h16_rd32(ane,
						     s->scratch0 + 4 * n);
			dev_info(ane->dev,
				 "H17 SCRATCH off=%#x values=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x (single read-only post-wake snapshot; scratch words untouched)\n",
				 s->scratch0, sw[0], sw[1], sw[2], sw[3],
				 sw[4], sw[5], sw[6], sw[7]);
		}
		/* The H16 RTKit management endpoint layout (mbox=0x1608000,
		 * A2I/I2A +0x110/+0x114) is a T8132 template, not a J700
		 * mapping. J700's kext reaches READY via SCRATCH7 and then
		 * uses its firmware command path in later requests; it does
		 * not poll this mailbox after startup. Do not claim HELLO/EPMAP
		 * support or touch the unqualified candidate, even when the
		 * H16-only hello_wait_ms parameter is set.
		 */
		if (chanreg)
			dev_info(ane->dev,
				 "H17 post-wake RTKit management poll skipped: table discovery uses scratch handshake only\n");
		else
			dev_info(ane->dev,
				 "H17 post-wake RTKit management poll skipped: H16 mailbox offset is unqualified; RESULT covers wake only\n");
	}
	else if (hello_wait_ms)
		ane_h16_rtkit_poll(ane);
	else
		dev_info(ane->dev,
			 "RTKit poll skipped (hello_wait_ms=0); set 1000 to probe for HELLO/EPMAP/STARTEP\n");
	if (chanreg) {
		ret = ane_h17_discover_table(ane);
		goto rel_fw;
	}
	ane_h16_result(ane, 3, "PASS",
		       ane->hello_done ? "boot-hello" :
		       (hello_wait_ms && !s->h17_order) ?
		       "boot-nortkit" : "boot-wake");
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
	if (chanreg && (!soc->h17_order || stage_idx != 3)) {
		dev_err(&pdev->dev, "chanreg requires t8140 stage=boot\n");
		return -EINVAL;
	}
	if (register_channels && (!chanreg || !soc->h17_order || stage_idx != 3)) {
		dev_err(&pdev->dev,
			"register_channels requires chanreg=1 and t8140 stage=boot\n");
		return -EINVAL;
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

	if (!confirm_read) {
		ane_h16_result(ane, stage_idx, "REFUSED", "confirm_read-required");
		return -EPERM;
	}

	ret = ane_h16_attach_pds(ane);
	if (ret)
		return ret;

	/* The windows are mapped WITHOUT request_region: on T8140 the ANE
	 * DARTs (0x4018x_0000) sit inside the engine window and the ps-probe
	 * pmgr-pwrstate devices sit inside the pmgr window, so the sibling
	 * drivers' reservations make a whole-window request fail -EBUSY.
	 * of_iomap itself does not reserve; the ownership guarantee is the
	 * DT reg layout, not the region accounting. */
	ret = ane_h16_map(ane);
	if (ret) {
		ane_h16_detach_pds(ane);
		return ret;
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
			ane_h16_unmap(ane);
			ane_h16_detach_pds(ane);
			return -ENXIO;
		}
		ane->pwgate = devm_ioremap(&pdev->dev, res->start,
					   resource_size(res));
		if (!ane->pwgate) {
			ane_h16_unmap(ane);
			ane_h16_detach_pds(ane);
			return -ENOMEM;
		}
	}

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		/* With several domains attached by id, the RPM_ACTIVE links already resumed them, and ane_h15 saw this device's get_sync
		 * return -EACCES on a T6030 with the cause unknown. Only that code, only with domains attached, is let through: the
		 * ACTUAL gate in ps_wait is the real power check. */
		if (ane->pd_count > 0 && ret == -EACCES) {
			dev_info(&pdev->dev, "engine powered via %d domain links (get_sync %d ignored)\n", ane->pd_count, ret);
		} else {
			dev_err(&pdev->dev, "power-domains bring-up failed: %d\n", ret);
			/* a failed get_sync still took a usage count */
			pm_runtime_put_noidle(&pdev->dev);
			pm_runtime_disable(&pdev->dev);
			ane_h16_unmap(ane);
			ane_h16_detach_pds(ane);
			return ret;
		}
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
pd_off:
	pm_runtime_put_sync(&pdev->dev);
	if (ret) {
		pm_runtime_disable(&pdev->dev);
		ane_h16_unmap(ane);
		ane_h16_detach_pds(ane);
	}
	return ret;
}

static void ane_h16_remove(struct platform_device *pdev)
{
	struct ane_h16 *ane = platform_get_drvdata(pdev);

	if (ane->fw_touched) {
		dev_emerg(&pdev->dev,
			  "remove with the firmware released: leaving power on, reboot required\n");
		return;
	}
	pm_runtime_disable(&pdev->dev);
	ane_h16_unmap(ane);
	ane_h16_detach_pds(ane);
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

static int __init ane_h16_init(void)
{
	return ane_h16_register(&ane_h16_driver, optin, apply_overlay);
}

static void __exit ane_h16_exit(void)
{
	ane_h16_unregister(&ane_h16_driver);
}

module_init(ane_h16_init);
module_exit(ane_h16_exit);

MODULE_AUTHOR("Joshua Warren");
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H16-family ANE bring-up module (status + firmware-boot smoke; no inference)");
MODULE_LICENSE("Dual MIT/GPL");
