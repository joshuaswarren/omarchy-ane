// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h13_perf.c — H13 (T8103/T6001) ANE firmware perf-mode client.
 *
 * Sends CSNE_CMD_CH_PROPERTY_WRITE(prop 0x10aa, value 1) — the
 * "setting FW perf mode" message AppleH11ANEInterface issues at
 * power-on — over an RTKit session on the ANE ASC mailbox.
 *
 * Message evidence (H13 kext 9.512.0-macstudio-25G83, static): cmd id
 * 0x1f built at __TEXT_EXEC 0xfffffe0009322e14, payload pool
 * __TEXT.__const 0xfffffe000748fdc8 {channel 0, property 0x10aa},
 * value 1 at +0x10, error-string xref 0xfffffe0009322ec8; 20-byte
 * buffer {u32 0; u16 id; u16 flags; u32 channel; u32 property;
 * u32 value}; one send site in the binary (init path).
 *
 * Measured on T6001/jw16 (2026-09-24, receipts
 * 2026-09-24-ane-perf-mode-h13):
 *   - aperture-relative CPU_STATUS +0x1400048 reads 0x2a
 *     (STOPPED|IDLE) on the working Linux stack — eos parked is the
 *     M1-normal state (AneStaticStart diff: the M1 path is power,
 *     tunables, DART, TM enable; no coprocessor start).
 *   - some ASCWRAP-class reads hard-reset the SoC; unproven registers
 *     are read ONE per load behind probe_reg.
 *   - the legacy driver only pins runtime PM during its submits;
 *     an unpinned visit can die seconds later (autosuspend + DART
 *     TLB class, jwm1 bring-up rule) — so this module pins the
 *     partition with pm_runtime_resume_and_get for its whole visit.
 *
 * Safety: never binds the platform node (lookup only); non-posted
 * MMIO; bounded waits; no RTKit power-state writes; probe_only=0 and
 * boot=0 defaults refuse to touch anything beyond proven reads.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/device/bus.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/iommu.h>
#include <linux/memremap.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/unaligned.h>
#include <linux/sizes.h>

/* ---- engine aperture registers ---- */

#define ANE_H13_CPU_STATUS	0x1400048	/* 0x2a parked; bit1 STOPPED */
#define ASC_CPU_STOPPED		BIT(1)
#define ANE_H13_CPU_CONTROL	0x1400044
#define ASC_CPU_RUN_RELEASE	0x10		/* write32 0 then 0x10 */
#define ASC_IO_RVBAR		0x1050000	/* read-only here; bit0 latched */

static char *pdev_name;
module_param(pdev_name, charp, 0444);
MODULE_PARM_DESC(pdev_name,
		 "optional platform device name (auto-discovered by legacy driver 'ane' when unset)");

static unsigned long mb_off = 0x1408000;
module_param(mb_off, ulong, 0444);
MODULE_PARM_DESC(mb_off,
		 "ASC mailbox block offset inside the ane aperture (h14 layout)");

static unsigned long long aperture;
module_param(aperture, ullong, 0444);
MODULE_PARM_DESC(aperture,
		 "optional ane aperture physical base (default: reg window base 32 MiB-aligned down)");

static unsigned long aperture_size = 0x2000000;
module_param(aperture_size, ulong, 0444);
MODULE_PARM_DESC(aperture_size, "ane aperture mapping size (default 32 MiB)");

static uint t2fc_ep = 2;
module_param(t2fc_ep, uint, 0444);
MODULE_PARM_DESC(t2fc_ep, "T2F_CMD app endpoint (K14 cfg default 2; EPMAP logged at bind)");

static bool boot;
module_param(boot, bool, 0444);
MODULE_PARM_DESC(boot,
		 "Run the iBoot-staged ASC firmware (CPU_CONTROL 0 -> 0x10, M2-measured sequence) when CPU_STATUS shows stopped");

static bool perf_mode;
module_param(perf_mode, bool, 0444);
MODULE_PARM_DESC(perf_mode,
		 "After a completed handshake, send CSNE_CMD_CH_PROPERTY_WRITE(prop 0x10aa, value 1)");

static bool probe_only;
module_param(probe_only, bool, 0444);
MODULE_PARM_DESC(probe_only,
		 "Read-only reconnaissance dump, then exit");

static uint probe_reg;
module_param(probe_reg, uint, 0444);
MODULE_PARM_DESC(probe_reg,
		 "probe_only: read ONE unproven register per load (1=I2A ctrl 2=A2I ctrl 3=RVBAR 4=CPU_CONTROL); 0 = proven reads only");

static bool scratch_dump;
module_param(scratch_dump, bool, 0444);
MODULE_PARM_DESC(scratch_dump,
		 "boot: 5 s after RUN, dump the I2A outbox, A2I control and SCRATCH0-7 (ap+0x1840048..64) raw");

/* ---- ChMan boot contract (eos/t600x, 13.5 stub; decode 2026-09-29) ----
 * eos builds the selene constants: READY/DONE/ACK 0x08042006 (7 sites,
 * scratch-write vtable+0x30 idx7), wake 0xf7fbdff9 (vm 0x62c4). The fw
 * publishes READY on SCRATCH7 after RUN, waits for the host to publish
 * a DVA in SCRATCH1:SCRATCH0 and write the wake word to SCRATCH7, walks
 * SCRATCH6 1/7/8/9, publishes DONE, then spins on SCRATCH3 until the
 * host writes the ACK. Only then does RTKit HELLO come. */
#define ASC_SCRATCH_BASE	0x1840048
#define ASC_SCRATCH(i)		(ASC_SCRATCH_BASE + 4 * (i))
#define CHMAN_READY_MAGIC	0x08042006u
#define CHMAN_WAKE_MAGIC	0xf7fbdff9u

static bool chman;
module_param(chman, bool, 0444);
MODULE_PARM_DESC(chman,
		 "boot: perform the eos ChMan boot contract (READY poll, DVA publish, wake, DONE poll, SCRATCH3 ack) before the MGMT handshake");

static unsigned long long fw_iova = 0x1f000000000ULL;
module_param(fw_iova, ullong, 0444);
MODULE_PARM_DESC(fw_iova,
		 "DVA published in SCRATCH1:SCRATCH0 (25G83 VM-to-IOVA base)");

static unsigned long text_phys = 0x10000a54000UL;
module_param(text_phys, ulong, 0444);
MODULE_PARM_DESC(text_phys,
		 "latched T6001 reset-base PA; 25G83 vm0 staging address");

static unsigned long data_phys = 0x10001684000UL;
module_param(data_phys, ulong, 0444);
MODULE_PARM_DESC(data_phys,
		 "T6001 ADT DATA staging PA");

static bool stage25;
module_param(stage25, bool, 0444);
MODULE_PARM_DESC(stage25,
		 "preflight current 22G74 reset head, stage 25G83 eos TEXT+DATA and patch T6001 device fields");

/* ---- ASC mailbox (soc/apple/mailbox.c ASC variant) ---- */

#define ASC_A2I_CONTROL		0x110
#define ASC_A2I_SEND0		0x800
#define ASC_A2I_SEND1		0x808
#define ASC_I2A_CONTROL		0x114
#define ASC_I2A_RECV0		0x830
#define ASC_I2A_RECV1		0x838
#define ASC_CTRL_FULL		BIT(16)
#define ASC_CTRL_EMPTY		BIT(17)
#define ASC_I2A_OUTBOX_ENABLE	BIT(0)
#define MSG1_EP			GENMASK_ULL(31, 0)

/* ---- RTKit MGMT (drivers/soc/apple/rtkit.c shapes) ---- */

#define MGMT_TYPE		GENMASK_ULL(59, 52)
#define MGMT_HELLO		1
#define MGMT_HELLO_REPLY	2
#define MGMT_STARTEP		5
#define MGMT_EPMAP		8
#define HELLO_MINVER		GENMASK_ULL(15, 0)
#define HELLO_MAXVER		GENMASK_ULL(31, 16)
#define EPMAP_LAST		BIT_ULL(51)
#define EPMAP_BASE		GENMASK_ULL(34, 32)
#define EPMAP_BITMAP		GENMASK_ULL(31, 0)
#define EPMAP_REPLY_MORE	BIT_ULL(0)
#define STARTEP_EP		GENMASK_ULL(39, 32)
#define STARTEP_FLAG		BIT_ULL(1)
#define RTKIT_VER_MIN		11
#define RTKIT_VER_MAX		12

/* ---- CSNE command ---- */

#define CSNE_CMD_CH_PROPERTY_WRITE	0x001f
#define CSNE_PROP_FW_PERF_MODE		0x10aa
#define CSNE_PERF_MODE_ON		1
#define CSNE_PROPBUF_LEN		0x14

#define DB_OFFSET		GENMASK_ULL(43, 0)
#define DB_SIZE			GENMASK_ULL(51, 44)
#define DB_UNIT			GENMASK_ULL(53, 52)
#define CMDW_OFF		GENMASK_ULL(23, 0)
#define CMDW_LEN		GENMASK_ULL(47, 24)

#define RING_SIZE		SZ_64K
#define RX_POLL_US		2000
#define HANDSHAKE_MS		5000
#define REPLY_MS		5000

static struct ane_h13_perf {
	struct platform_device *pdev;
	void __iomem *engine;
	void __iomem *win;
	void __iomem *mb;
	void *ring;
	dma_addr_t ring_iova;
	DECLARE_BITMAP(endpoints, 64);
	bool hello_done;
	bool epmap_last;
	bool pm_pinned;
	bool asc_was_stopped;
	struct iommu_domain *dart_domain;
	u64 dart_iova[2];
	size_t dart_len[2];
} *g;

/* ---- raw mailbox ops (poll mode) ---- */

static int mb_send(struct ane_h13_perf *a, u8 ep, u64 msg)
{
	u32 ctrl;
	int ret;

	ret = readl_poll_timeout_atomic(a->mb + ASC_A2I_CONTROL, ctrl,
					!(ctrl & ASC_CTRL_FULL), 100,
					2000000);
	if (ret)
		return ret;
	dma_wmb();
	writeq_relaxed(msg, a->mb + ASC_A2I_SEND0);
	writeq_relaxed(FIELD_PREP(MSG1_EP, ep), a->mb + ASC_A2I_SEND1);
	return 0;
}

static void mgmt_handle(struct ane_h13_perf *a, u8 ep, u64 msg);

static void mb_pump(struct ane_h13_perf *a)
{
	struct device *dev = &g->pdev->dev;
	u32 ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
	int n = 0;

	while (!(ctrl & ASC_CTRL_EMPTY) && n < 64) {
		u64 msg0 = readq_relaxed(a->mb + ASC_I2A_RECV0);
		u64 msg1 = readq_relaxed(a->mb + ASC_I2A_RECV1);
		u8 ep = FIELD_GET(MSG1_EP, msg1);

		if (ep == 0)
			mgmt_handle(a, ep, msg0);
		else
			dev_info(dev, "rx: ep=%u msg=%016llx\n", ep, msg0);
		n++;
		ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
	}
}

/* ---- MGMT ---- */

static int mgmt_send(struct ane_h13_perf *a, u8 type, u64 payload)
{
	return mb_send(a, 0, FIELD_PREP(MGMT_TYPE, type) | payload);
}

static void mgmt_handle(struct ane_h13_perf *a, u8 ep, u64 msg)
{
	struct device *dev = &a->pdev->dev;
	u8 type = FIELD_GET(MGMT_TYPE, msg);

	if (ep != 0)
		return;

	switch (type) {
	case MGMT_HELLO: {
		u32 vmin = FIELD_GET(HELLO_MINVER, msg);
		u32 vmax = FIELD_GET(HELLO_MAXVER, msg);
		u32 want = min((u32)RTKIT_VER_MAX, vmax);

		dev_info(dev, "mgmt: fw HELLO ver [%u,%u]\n", vmin, vmax);
		if (vmin > RTKIT_VER_MAX || vmax < RTKIT_VER_MIN)
			return;
		mgmt_send(a, MGMT_HELLO_REPLY,
			  FIELD_PREP(HELLO_MINVER, want) |
			  FIELD_PREP(HELLO_MAXVER, want));
		a->hello_done = true;
		break;
	}
	case MGMT_HELLO_REPLY:
		dev_info(dev, "mgmt: HELLO_REPLY\n");
		a->hello_done = true;
		break;
	case MGMT_EPMAP: {
		u32 base = FIELD_GET(EPMAP_BASE, msg);
		unsigned long bmp = FIELD_GET(EPMAP_BITMAP, msg);
		int i;

		for_each_set_bit(i, &bmp, 32) {
			int epn = 32 * base + i;

			if (epn < 64)
				set_bit(epn, a->endpoints);
		}
		dev_info(dev, "mgmt: EPMAP base=%u bitmap=%08lx\n", base, bmp);
		mgmt_send(a, MGMT_EPMAP,
			  FIELD_PREP(EPMAP_BASE, base) |
			  ((msg & EPMAP_LAST) ? EPMAP_LAST : EPMAP_REPLY_MORE));
		if (msg & EPMAP_LAST) {
			a->epmap_last = true;
			dev_info(dev, "mgmt: fw-advertised endpoints:");
			for (i = 0; i < 64; i++)
				if (test_bit(i, a->endpoints))
					pr_cont(" %d", i);
			pr_cont("\n");
		}
		break;
	}
	default:
		dev_info(dev, "mgmt: unhandled type %u msg=%016llx\n",
			 type, msg);
	}
}

static int handshake(struct ane_h13_perf *a)
{
	unsigned long t0 = jiffies;
	int ret;

	ret = mgmt_send(a, MGMT_HELLO,
			FIELD_PREP(HELLO_MINVER, (u32)RTKIT_VER_MIN) |
			FIELD_PREP(HELLO_MAXVER, (u32)RTKIT_VER_MAX));
	if (ret) {
		pr_err("ane_h13_perf: mailbox A2I stuck FULL: %pe\n",
		       ERR_PTR(ret));
		return ret;
	}

	while (!(a->hello_done && a->epmap_last)) {
		mb_pump(a);
		if (a->hello_done && a->epmap_last)
			break;
		if (time_after(jiffies, t0 + msecs_to_jiffies(HANDSHAKE_MS))) {
			pr_err("ane_h13_perf: handshake timed out hello=%d epmap=%d\n",
			       a->hello_done, a->epmap_last);
			return -ETIMEDOUT;
		}
		usleep_range(RX_POLL_US, RX_POLL_US + 500);
	}
	return 0;
}

/* ---- CSNE perf-mode write ---- */

static u64 doorbell_encode(u64 offset, u32 size)
{
	u64 unit = (size >= SZ_1M) ? 2 : 1;
	u32 code = DIV_ROUND_UP(size, 1u << (unit * 12));

	return (offset & DB_OFFSET) |
	       FIELD_PREP(DB_SIZE, code) | FIELD_PREP(DB_UNIT, unit);
}

static int send_perf_mode(struct ane_h13_perf *a)
{
	struct device *dev = &a->pdev->dev;
	u8 buf[CSNE_PROPBUF_LEN] = { 0 };
	unsigned long t0 = jiffies;
	u64 msg;
	int ret;

	if (!test_bit(t2fc_ep, a->endpoints)) {
		dev_err(dev, "T2F_CMD ep %u not advertised; set t2fc_ep from the EPMAP log\n",
			t2fc_ep);
		return -EINVAL;
	}

	ret = mb_send(a, t2fc_ep, doorbell_encode(a->ring_iova, RING_SIZE));
	if (ret)
		return ret;
	mgmt_send(a, MGMT_STARTEP,
		  FIELD_PREP(STARTEP_EP, t2fc_ep) | STARTEP_FLAG);
	msleep(50);
	mb_pump(a);

	put_unaligned_le16(CSNE_CMD_CH_PROPERTY_WRITE, buf + 0x04);
	put_unaligned_le32(0, buf + 0x08);
	put_unaligned_le32(CSNE_PROP_FW_PERF_MODE, buf + 0x0c);
	put_unaligned_le32(CSNE_PERF_MODE_ON, buf + 0x10);

	memcpy(a->ring, buf, CSNE_PROPBUF_LEN);
	dma_wmb();

	msg = FIELD_PREP(CMDW_OFF, 0) | FIELD_PREP(CMDW_LEN, CSNE_PROPBUF_LEN);
	ret = mb_send(a, t2fc_ep, msg);
	dev_info(dev, "csne: CH_PROPERTY_WRITE(prop 0x10aa=1) sent ep=%u -> %pe\n",
		 t2fc_ep, ERR_PTR(ret));
	if (ret)
		return ret;

	while (time_before(jiffies, t0 + msecs_to_jiffies(REPLY_MS))) {
		u32 ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
		int n = 0;

		while (!(ctrl & ASC_CTRL_EMPTY) && n < 64) {
			u64 msg0 = readq_relaxed(a->mb + ASC_I2A_RECV0);
			u64 msg1 = readq_relaxed(a->mb + ASC_I2A_RECV1);

			if (FIELD_GET(MSG1_EP, msg1) == 0)
				mgmt_handle(a, 0, msg0);
			else
				dev_info(dev, "rx: ep=%u msg=%016llx\n",
					 FIELD_GET(MSG1_EP, msg1), msg0);
			n++;
			ctrl = readl_relaxed(a->mb + ASC_I2A_CONTROL);
		}
		if (n)
			return 0;
		usleep_range(RX_POLL_US, RX_POLL_US + 500);
	}
	dev_warn(dev, "no reply within %d ms\n", REPLY_MS);
	return -ETIMEDOUT;
}

/* ---- module plumbing ---- */

static void ane_h13_perf_cleanup(void)
{
	struct ane_h13_perf *a = g;
	int i;

	if (!a)
		return;
	g = NULL;
	if (a->dart_domain)
		for (i = 0; i < ARRAY_SIZE(a->dart_len); i++)
			if (a->dart_len[i])
				iommu_unmap(a->dart_domain, a->dart_iova[i], a->dart_len[i]);
	if (a->ring)
		dma_free_coherent(&a->pdev->dev, RING_SIZE, a->ring,
				  a->ring_iova);
	if (a->win)
		iounmap(a->win);
	if (a->engine)
		iounmap(a->engine);
	if (a->pm_pinned) {
		pm_runtime_put_sync_suspend(&a->pdev->dev);
		pm_runtime_disable(&a->pdev->dev);
	}
	put_device(&a->pdev->dev);
	kfree(a);
}

/* ---- ChMan boot contract ---- */

static u64 span_sum(const u8 *p, size_t len, u64 *alt)
{
	u64 s1 = 0, s2 = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		s1 += p[i];
		s2 += s1;
	}
	*alt = s1;
	return s2;
}

static void chman_footprint(struct ane_h13_perf *a, const char *when)
{
	static const struct { const char *name; unsigned long off; size_t len; } sp[] = {
		{ "text-head", 0, 0x400 },
		{ "text-copy", 0x7c000, 0x8000 },
		{ "data-head", 0, 0x8000 },
		{ "data-patchbay", 0x74b8, 0x241 },
		{ "data-tunables", 0x19700, 0x1e8 },
		{ "data-bootlog", 0x1c000, 0x4000 },
		{ "data-page-tables", 0x28000, 0x4000 },
		{ "data-stacktail", 0x18680, 0x80 },
		{ "data-copy", 0x3ec000, 0x8000 },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(sp); i++) {
		phys_addr_t pa = (sp[i].name[0] == 't') ? text_phys + sp[i].off :
							  data_phys + sp[i].off;
		size_t len = sp[i].len;
		u8 *va = memremap(pa, len, MEMREMAP_WC);
		u64 s1 = 0, s2 = 0;

		if (va) {
			s2 = span_sum(va, len, &s1);
			memunmap(va);
			dev_info(&a->pdev->dev, "chman: fp %s %s pa=%pa len=%zx sum=%016llx:%016llx\n",
				 when, sp[i].name, &pa, len, s2, s1);
		} else {
			dev_info(&a->pdev->dev, "chman: fp %s %s pa=%pa UNREADABLE\n",
				 when, sp[i].name, &pa);
		}
	}
}

static void chman_dump_scratch(struct ane_h13_perf *a)
{
	u32 s[8];
	int i;

	for (i = 0; i < 8; i++)
		s[i] = readl_relaxed(a->engine + ASC_SCRATCH(i));
	dev_info(&a->pdev->dev,
		 "chman: SCRATCH %08x %08x %08x %08x %08x %08x %08x %08x cpu_status=%08x\n",
		 s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
		 readl_relaxed(a->engine + ANE_H13_CPU_STATUS));
}

static int chman_contract(struct ane_h13_perf *a)
{
	struct device *dev = &a->pdev->dev;
	int t, ret = -ETIMEDOUT;
	u32 s0, s6, s7;

	chman_footprint(a, "A-pre");

	for (t = 0; t < 20; t++) {
		chman_dump_scratch(a);
		if (readl_relaxed(a->engine + ASC_SCRATCH(7)) == CHMAN_READY_MAGIC) {
			ret = 0;
			break;
		}
		msleep(1000);
	}
	if (ret) {
		dev_info(dev, "chman: POLL-A no READY (0x%08x) in 20 s\n",
			 CHMAN_READY_MAGIC);
		goto out;
	}
	dev_info(dev, "chman: POLL-A READY seen at t=%d s\n", t);

	/* publish fw DVA in SCRATCH1:SCRATCH0, then wake */
	writel_relaxed(lower_32_bits(fw_iova), a->engine + ASC_SCRATCH(0));
	writel_relaxed(upper_32_bits(fw_iova), a->engine + ASC_SCRATCH(1));
	dma_wmb();
	writel_relaxed(CHMAN_WAKE_MAGIC, a->engine + ASC_SCRATCH(7));
	dma_wmb();
	dev_info(dev, "chman: published dva=%016llx wake=0x%08x\n",
		 fw_iova, CHMAN_WAKE_MAGIC);

	ret = -EIO;
	for (t = 0; t < 15; t++) {
		msleep(1000);
		chman_dump_scratch(a);
		s0 = readl_relaxed(a->engine + ASC_SCRATCH(0));
		s6 = readl_relaxed(a->engine + ASC_SCRATCH(6));
		s7 = readl_relaxed(a->engine + ASC_SCRATCH(7));
		if (s7 == CHMAN_READY_MAGIC && (s6 >= 7 || s0 != lower_32_bits(fw_iova))) {
			dev_info(dev, "chman: POLL-B DONE at t=%d (s6=%08x s0=%08x)\n",
				 t, s6, s0);
			writel_relaxed(CHMAN_READY_MAGIC, a->engine + ASC_SCRATCH(3));
			dma_wmb();
			dev_info(dev, "chman: SCRATCH3 ACK 0x%08x written\n",
				 CHMAN_READY_MAGIC);
			ret = 0;
			break;
		}
	}
	if (ret)
		dev_info(dev, "chman: POLL-B no DONE in 15 s\n");

out:
	chman_footprint(a, "B-post");
	return ret;
}

/* ---- 25G83 staging and ADT SEG mapping ---- */
#define EOS_TEXT_LEN 0xf4000
#define EOS_DATA_FILE_LEN 0x3f8000
#define EOS_DATA_LEN 0x5f8000
#define EOS_HEAD_LEN 0x8000
#define EOS_HEAD_22_FW "ane/eos-head-22g74.bin"
#define EOS_TEXT_FW "ane/eos-text-25g83.bin"
#define EOS_DATA_FW "ane/eos-data-25g83.bin"
#define EOS_TEXT_IOVA 0x1f000000000ULL
#define EOS_DATA_IOVA 0x1f0000f4000ULL
#define EOS_PATCHBAY_OFF 0x74b8

static int h4a_patch_data(struct device *dev, u8 *buf)
{
	static const struct { u32 off; const char tag[4]; u32 len; u64 value; } pb[] = {
		{ EOS_PATCHBAY_OFF + 0x1d4, "_COS", 4, 0x6001 },
		{ EOS_PATCHBAY_OFF + 0x1e0, "RCOS", 4, 0x11 },
		{ EOS_PATCHBAY_OFF + 0x1ec, "dApC", 8, 0x285000000ULL },
		{ EOS_PATCHBAY_OFF + 0x1fc, "dArW", 8, 0x285400000ULL },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(pb); i++) {
		u32 off = pb[i].off;

		if (memcmp(buf + off, pb[i].tag, 4) ||
		    get_unaligned_le32(buf + off + 4) != pb[i].len) {
			dev_err(dev, "h4a: patchbay mismatch at DATA+%#x\n", off);
			return -EINVAL;
		}
		if (pb[i].len == 4)
			put_unaligned_le32((u32)pb[i].value, buf + off + 8);
		else
			put_unaligned_le64(pb[i].value, buf + off + 8);
		dev_info(dev, "h4a: patch %.4s DATA+%#x len=%u value=%#llx\n",
			 pb[i].tag, off, pb[i].len, pb[i].value);
	}
	return 0;
}

static int h4a_stage(struct ane_h13_perf *a)
{
	const struct firmware *head = NULL, *text = NULL, *data = NULL;
	u8 *verify = NULL, *dbuf = NULL, *va = NULL;
	u64 s1, s2;
	int ret;

	if (fw_iova != EOS_TEXT_IOVA || text_phys != 0x10000a54000UL ||
	    data_phys != 0x10001684000UL) {
		dev_err(&a->pdev->dev, "h4a: staging addresses must match the registered T6001 map\n");
		return -EINVAL;
	}
	ret = request_firmware_direct(&head, EOS_HEAD_22_FW, &a->pdev->dev);
	if (ret) goto out;
	ret = request_firmware_direct(&text, EOS_TEXT_FW, &a->pdev->dev);
	if (ret) goto out;
	ret = request_firmware_direct(&data, EOS_DATA_FW, &a->pdev->dev);
	if (ret) goto out;
	if (head->size != EOS_HEAD_LEN || text->size != EOS_TEXT_LEN ||
	    data->size != EOS_DATA_FILE_LEN) { ret = -EINVAL; goto out; }

	s2 = span_sum(text->data, text->size, &s1);
	if (s2 != 0x00002b6299c4d242ULL || s1 != 0x0000000005a2d340ULL) {
		dev_err(&a->pdev->dev, "h4a: 25G83 TEXT fletcher mismatch %016llx:%016llx\n", s2, s1);
		ret = -EBADMSG;
		goto out;
	}
	dev_info(&a->pdev->dev, "h4a: 25G83 TEXT offline fletcher matched\n");
	va = memremap(text_phys, EOS_HEAD_LEN, MEMREMAP_WC);
	if (!va) { ret = -ENOMEM; goto out; }
	verify = kmemdup(va, EOS_HEAD_LEN, GFP_KERNEL);
	if (!verify) { ret = -ENOMEM; goto out; }
	{
		/* The fw writes a few self-description words into its TEXT at
		 * runtime (25G83 capture: vm 0x423c/0x4240), so allow up to 4
		 * differing u32 words and log each; more means another image. */
		const u32 *o = (const u32 *)verify, *e = (const u32 *)head->data;
		unsigned int i, ndiff = 0;

		for (i = 0; i < EOS_HEAD_LEN / 4; i++) {
			if (o[i] == e[i])
				continue;
			if (ndiff < 8)
				dev_info(&a->pdev->dev, "h4a: head word +%#x live=%08x ref=%08x\n",
					 i * 4, o[i], e[i]);
			ndiff++;
		}
		if (ndiff > 4) {
			s2 = span_sum(verify, EOS_HEAD_LEN, &s1);
			dev_err(&a->pdev->dev,
				"h4a: reset-head mismatch (%u words) observed=%016llx:%016llx; refusing staging\n",
				ndiff, s2, s1);
			ret = -EUCLEAN;
			goto out;
		}
	}
	memunmap(va); va = NULL;
	dev_info(&a->pdev->dev, "h4a: read-only 22G74 vm0 head matched (%#x bytes)\n", EOS_HEAD_LEN);

	dbuf = kmemdup(data->data, EOS_DATA_FILE_LEN, GFP_KERNEL);
	if (!dbuf) { ret = -ENOMEM; goto out; }
	ret = h4a_patch_data(&a->pdev->dev, dbuf);
	if (ret) goto out;
	s2 = span_sum(dbuf, EOS_DATA_FILE_LEN, &s1);
	if (s2 != 0x00001903dd81b218ULL || s1 != 0x0000000000673ad7ULL) {
		dev_err(&a->pdev->dev, "h4a: patched 25G83 DATA fletcher mismatch %016llx:%016llx\n", s2, s1);
		ret = -EBADMSG;
		goto out;
	}
	dev_info(&a->pdev->dev, "h4a: patched 25G83 DATA offline fletcher matched\n");

	va = memremap(text_phys, EOS_TEXT_LEN, MEMREMAP_WC);
	if (!va) { ret = -ENOMEM; goto out; }
	dev_info(&a->pdev->dev, "h4a: stage TEXT pa=%pa len=%#x\n", &text_phys, EOS_TEXT_LEN);
	/*
	 * Warm the mapping before the first store: on 7.1.13 a store as the
	 * FIRST access to a fresh carveout mapping permission-faults (ESR
	 * L3 permission, sane RW WC PTE verified in-walk — Gap5 EXP-B.2,
	 * 3/3 cold stores faulted, 4/4 read-first writes succeeded). A
	 * full-span read both warms the translation and pre-verifies the
	 * live content is the expected 22G74 vm0.
	 */
	s2 = span_sum(va, EOS_TEXT_LEN, &s1);
	dev_info(&a->pdev->dev, "h4a: stage TEXT pre-write live sum=%016llx:%016llx\n", s2, s1);
	memcpy(va, text->data, EOS_TEXT_LEN);
	if (memcmp(va, text->data, EOS_TEXT_LEN)) { ret = -EIO; goto out; }
	memunmap(va); va = NULL;

	va = memremap(data_phys, EOS_DATA_LEN, MEMREMAP_WC);
	if (!va) { ret = -ENOMEM; goto out; }
	dev_info(&a->pdev->dev, "h4a: stage DATA pa=%pa file=%#x zero=%#x\n",
		 &data_phys, EOS_DATA_FILE_LEN, EOS_DATA_LEN - EOS_DATA_FILE_LEN);
	/* Warm before first store — same EXP-B.2 cold-store hazard as TEXT. */
	s2 = span_sum(va, EOS_DATA_LEN, &s1);
	dev_info(&a->pdev->dev, "h4a: stage DATA pre-write live sum=%016llx:%016llx (expect 0:0 zeros)\n",
		 s2, s1);
	memcpy(va, dbuf, EOS_DATA_FILE_LEN);
	memset(va + EOS_DATA_FILE_LEN, 0, EOS_DATA_LEN - EOS_DATA_FILE_LEN);
	if (memcmp(va, dbuf, EOS_DATA_FILE_LEN) ||
	    memchr_inv(va + EOS_DATA_FILE_LEN, 0, EOS_DATA_LEN - EOS_DATA_FILE_LEN)) {
		ret = -EIO;
		goto out;
	}
	s2 = span_sum(va, EOS_DATA_FILE_LEN, &s1);
	dev_info(&a->pdev->dev, "h4a: DATA readback sum=%016llx:%016llx\n", s2, s1);
	memunmap(va); va = NULL;
	ret = 0;
out:
	if (va) memunmap(va);
	kfree(verify);
	kfree(dbuf);
	if (data) release_firmware(data);
	if (text) release_firmware(text);
	if (head) release_firmware(head);
	return ret;
}

static int h4a_map_window(struct ane_h13_perf *a, struct iommu_domain *dom,
			  u64 iova, phys_addr_t phys, size_t len, unsigned int slot)
{
	const size_t page = SZ_4K;
	size_t off;
	bool any = false, all = true;
	int ret;

	for (off = 0; off < len; off += page) {
		phys_addr_t pa = iommu_iova_to_phys(dom, iova + off);
		if (pa) {
			any = true;
			if (pa != phys + off) { ret = -EEXIST; goto err; }
		} else all = false;
	}
	if (all) {
		dev_info(&a->pdev->dev, "h4a: DART window already mapped iova=%#llx pa=%pa len=%#zx\n",
			 iova, &phys, len);
		return 0;
	}
	if (any) { ret = -EEXIST; goto err; }
	ret = iommu_map(dom, iova, phys, len, IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, GFP_KERNEL);
	if (ret) goto err;
	a->dart_iova[slot] = iova;
	a->dart_len[slot] = len;
	for (off = 0; off < len; off += page) {
		if (iommu_iova_to_phys(dom, iova + off) != phys + off) {
			iommu_unmap(dom, iova, len);
			a->dart_len[slot] = 0;
			ret = -EIO;
			goto err;
		}
	}
	dev_info(&a->pdev->dev, "h4a: DART mapped iova=%#llx pa=%pa len=%#zx pages=%zu\n",
		 iova, &phys, len, len / page);
	return 0;
err:
	dev_err(&a->pdev->dev, "h4a: DART map failed iova=%#llx pa=%pa len=%#zx ret=%d\n",
		 iova, &phys, len, ret);
	return ret;
}

static int h4a_map_segments(struct ane_h13_perf *a)
{
	struct iommu_domain *dom = iommu_get_domain_for_dev(&a->pdev->dev);
	int ret;

	if (!dom) return -ENODEV;
	if (fw_iova != EOS_TEXT_IOVA) return -EINVAL;
	a->dart_domain = dom;
	ret = h4a_map_window(a, dom, EOS_TEXT_IOVA, text_phys, EOS_TEXT_LEN, 0);
	if (ret) return ret;
	return h4a_map_window(a, dom, EOS_DATA_IOVA, data_phys, EOS_DATA_LEN, 1);
}

static int match_owned(struct device *dev, const void *data)
{
	struct device_driver *drv = dev->driver;

	if (!data)
		return dev_is_platform(dev) && drv &&
		       !strcmp(drv->name, "ane");
	return dev_is_platform(dev) &&
	       !strcmp(dev_name(dev), (const char *)data);
}

static int __init ane_h13_perf_init(void)
{
	struct device *found;
	struct resource *res;
	u64 aperture_base;
	u32 cpu_status;
	int ret;

	found = bus_find_device(&platform_bus_type, NULL, pdev_name,
				match_owned);
	if (!found) {
		pr_err("ane_h13_perf: no legacy-bound ane platform device (pdev_name='%s')\n",
		       pdev_name ? pdev_name : "<auto:driver 'ane'>");
		return -ENODEV;
	}

	g = kzalloc(sizeof(*g), GFP_KERNEL);
	if (!g) {
		put_device(found);
		return -ENOMEM;
	}
	g->pdev = to_platform_device(found);
	pr_info("ane_h13_perf: found %s (driver %s)\n", dev_name(found),
		found->driver ? found->driver->name : "?");

	/* Pin the partition up for the whole visit: autosuspend after a
	 * fresh attach invalidates DART TLBs and resets the SoC (jwm1
	 * bring-up rule); the legacy driver only pins during submits. */
	pm_runtime_enable(&g->pdev->dev);
	ret = pm_runtime_resume_and_get(&g->pdev->dev);
	if (ret) {
		pr_err("ane_h13_perf: genpd raise failed: %pe\n", ERR_PTR(ret));
		goto err;
	}
	g->pm_pinned = true;

	res = platform_get_resource(g->pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("ane_h13_perf: no IORESOURCE_MEM[0]\n");
		ret = -ENODEV;
		goto err;
	}
	pr_info("ane_h13_perf: legacy reg window %pr\n", res);
	/* TM offsets are relative to the legacy window (engine+0x20000),
	 * NOT to the 32 MiB aperture — reading them aperture-relative
	 * hits unproven pages (measured hard reset). */
	g->win = ioremap_np(res->start, resource_size(res));
	if (!g->win) {
		ret = -ENOMEM;
		goto err;
	}

	aperture_base = aperture ? aperture :
				   (res->start & ~(u64)(aperture_size - 1));
	pr_info("ane_h13_perf: aperture %#llx+%#lx\n",
		aperture_base, aperture_size);
	g->engine = ioremap_np(aperture_base, aperture_size);
	if (!g->engine) {
		ret = -ENOMEM;
		goto err;
	}
	g->mb = g->engine + mb_off;

	cpu_status = readl_relaxed(g->engine + ANE_H13_CPU_STATUS);
	g->asc_was_stopped = cpu_status & ASC_CPU_STOPPED;
	pr_info("ane_h13_perf: CPU_STATUS=%08x TM_TQ_EN(win)=%08x\n",
		cpu_status, readl_relaxed(g->win + 0x2000c));

	if (probe_only) {
		static const unsigned long offs[] = {
			[1] = 0x1408114, [2] = 0x1408110,
			[3] = ASC_IO_RVBAR, [4] = ANE_H13_CPU_CONTROL,
		};

		if (probe_reg == 3) {
			u64 rv = readq_relaxed(g->engine + offs[3]);

			pr_info("ane_h13_perf: PROBE-ONLY reg[3] ap+%#lx = %016llx\n",
				offs[3], rv);
		} else if (probe_reg) {
			pr_info("ane_h13_perf: PROBE-ONLY reg[%u] ap+%#lx = %08x\n",
				probe_reg, offs[probe_reg],
				readl_relaxed(g->engine + offs[probe_reg]));
		}
		ret = -EALREADY;
		goto err;
	}

	if (cpu_status & ASC_CPU_STOPPED) {
		u32 i2a;
		u64 rvbar;

		if (!boot) {
			pr_err("ane_h13_perf: ASC parked (CPU_STATUS %08x); reload with boot=1 to run the staged firmware\n",
			       cpu_status);
			ret = -ENODEV;
			goto err;
		}
		/* Measured M2 sequence: outbox enable, then
		 * CPU_CONTROL write32 0 -> 0x10. RVBAR never written
		 * when bit0 is latched.
		 * STRUCK per AneStaticStart prerun-diff CORRECTION
		 * (c33c0d3): the kext 0x2e0 write is a pmgr ps word
		 * (phys 0x28e080000, genpd-covered) — reissuing it
		 * wedged the M2; my earlier engine+0x2e0 variant is
		 * removed. Corrected first missing NON-ps write is
		 * PWGATE+0x159c = 0 (third-window; T6001 base
		 * unresolved — do not write without it). */
		if (chman) {
			if (!stage25) {
				dev_err(&g->pdev->dev, "chman: stage25=1 is required for H4a\n");
				ret = -EINVAL;
				goto err;
			}
			chman_footprint(g, "A-prerun");
			ret = h4a_stage(g);
			if (ret)
				goto err;
			ret = h4a_map_segments(g);
			if (ret)
				goto err;
		}
		rvbar = readq_relaxed(g->engine + ASC_IO_RVBAR);
		dev_info(&g->pdev->dev, "boot: RVBAR=%016llx (bit0 latched=%d, never written)\n",
			 rvbar, (int)(rvbar & 1));
		i2a = readl_relaxed(g->mb + ASC_I2A_CONTROL);
		writel_relaxed(i2a | ASC_I2A_OUTBOX_ENABLE,
			       g->mb + ASC_I2A_CONTROL);
		dev_info(&g->pdev->dev, "boot: I2A %08x -> %08x\n",
			 i2a, readl_relaxed(g->mb + ASC_I2A_CONTROL));
		writel_relaxed(0, g->engine + ANE_H13_CPU_CONTROL);
		wmb();
		writel_relaxed(ASC_CPU_RUN_RELEASE,
			       g->engine + ANE_H13_CPU_CONTROL);
		ret = readl_poll_timeout_atomic(
			g->engine + ANE_H13_CPU_STATUS, cpu_status,
			!(cpu_status & ASC_CPU_STOPPED), 1000, 3000000);
		dev_info(&g->pdev->dev, "boot: CPU_STATUS now %08x (rc=%pe)\n",
			 cpu_status, ERR_PTR(ret));
		if (ret)
			goto err;
	}

	g->ring = dma_alloc_coherent(&g->pdev->dev, RING_SIZE,
				     &g->ring_iova, GFP_KERNEL);
	if (!g->ring) {
		ret = -ENOMEM;
		goto err;
	}
	dev_info(&g->pdev->dev, "ring: iova=%pad size=0x%x\n",
		 &g->ring_iova, RING_SIZE);

	if (scratch_dump && perf_mode) {
		/* Raw fw-liveness evidence after RUN: I2A outbox state,
		 * A2I control, and SCRATCH0-7 (m1n1 GPIO0-7 words at
		 * ap+0x1840048). Read-only, one shot. */
		msleep(5000);
		dev_info(&g->pdev->dev,
			 "SCRATCH: i2a=%08x recv0=%016llx recv1=%016llx a2i=%08x\n",
			 readl_relaxed(g->mb + ASC_I2A_CONTROL),
			 readq_relaxed(g->mb + ASC_I2A_RECV0),
			 readq_relaxed(g->mb + ASC_I2A_RECV1),
			 readl_relaxed(g->mb + ASC_A2I_CONTROL));
		for (ret = 0; ret < 8; ret++)
			dev_info(&g->pdev->dev, "SCRATCH%d=%08x\n", ret,
				 readl_relaxed(g->engine + 0x1840048 +
					       ret * 4));
	}

	if (chman) {
		u32 st = readl_relaxed(g->engine + ANE_H13_CPU_STATUS);

		if (!g->asc_was_stopped) {
			dev_err(&g->pdev->dev,
				"chman: ASC was already running at attach (attach=%08x, now=%08x) — reboot to a fresh boot; the contract is only valid right after its own RUN\n",
				st, readl_relaxed(g->engine + ANE_H13_CPU_STATUS));
			ret = -EBUSY;
			goto err;
		}
		ret = chman_contract(g);
		if (ret) {
			dev_err(&g->pdev->dev,
				"chman: boot contract did not complete: %pe\n",
				ERR_PTR(ret));
			goto err;
		}
	}

	ret = handshake(g);
	if (ret)
		goto err;
	dev_info(&g->pdev->dev, "RTKit session up (no power-state writes)\n");

	if (perf_mode)
		ret = send_perf_mode(g);
	else
		dev_info(&g->pdev->dev, "perf_mode=0: capture only\n");
	if (ret)
		goto err;
	return 0;

err:
	ane_h13_perf_cleanup();
	return ret;
}

static void __exit ane_h13_perf_exit(void)
{
	ane_h13_perf_cleanup();
}

module_init(ane_h13_perf_init);
module_exit(ane_h13_perf_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("H13 ANE firmware perf-mode client (CSNE_CMD_CH_PROPERTY_WRITE 0x10aa)");
