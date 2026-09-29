// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_t6021_rtclient.c — T6021 (H14 / M2) installed ANE driver.
 *
 * One module, two surfaces:
 *   - boot/transport: fenced Linux-context firmware start (fw_start=1
 *     default — the proven add-path config), shared boot/fwload
 *     contract units, then the legacy 13.5 ChMan transport for the
 *     three load-time commands (LOAD_PROGRAM 0x200, CREATE_PROCESS
 *     0x202, PROCEDURE_CALL 0x204) driven through the ioctls in this
 *     file;
 *   - DRM accel: BO_INIT/BO_FREE plus PROG_LOAD/PROC_CREATE/EXEC per
 *     ane/src/uapi/drm/ane_accel.h (ABI 2). Sections ride BOs the
 *     user supplies; one global mutex serializes every firmware
 *     command; completion = the legacy exchange's reply PLUS a
 *     TQ-idle poll gated on the pmgr PS words. EXEC also returns the
 *     fw's target-to-host slots (the sequencer's per-step drain).
 *
 * Compiled defaults are the load-run.sh parameter list verbatim — a
 * bare `insmod ane_t6021.ko` is the proven add-path configuration on
 * boot 3ab812a3 (fw_load=1 fw_start=1 fw_start_dapf=0 legacy_only=1
 * legacy_query=1 scratch3_ack=1 poll_rx=1 hello_wait_ms=1000 ...).
 * Remaining parameters are overridable from sysfs for bisection only.
 *
 * Division of labor (receipts/2026-09-22-t6021-rtkit-port):
 *  - CPU start: kernel-context start is the proven-safe route on this
 *    box — m1n1 RTBuddy path is dormant (W5/W6 MGMT session wall) and
 *    the 13.5 (22G74) selene image brings up the legacy ChMan
 *    transport after DONE without RTKit. Boot is fenced in
 *    ane_t6021_boot.c (entry fold, RVBAR skip-or-fold, scratch,
 *    RUN, poll A/B, publish/wake) and audited in
 *    tools/h14_boot_regression.c.
 *  - genpd/pmgr: Runtime PM + the DT power-domains binding owns the
 *    raise; probe verifies ACTUAL on ane_cpu (pmgr window) before
 *    the first engine read (G1 gate). The pre-CPU engine table block
 *    is intentionally NOT written (preboot_table_mode = 2 = SKIP, the
 *    2026-09-20-era live-fault discriminator). macOS-form mpm_off is
 *    skipped: with fw_start_mpm_off=0 (the proven config) the
 *    boot-raised ane_sys_mpm stays on; the boot writes do not depend
 *    on it.
 *  - mailbox: ane_rtclient_validate_chman sees the 'IPC ' surface the
 *    fw published during boot; the three load-time commands travel
 *    the IO ring (channel 1) over ane_rtclient_legacy_exchange. Each
 *    command advances the cursor on the next 64-byte slot — resending
 *    to slot 0 wedges the ring (sequencer rule).
 *  - non-posted MMIO (DT "nonposted-mmio" -> IORESOURCE_MEM_NONPOSTED)
 *    is mandatory throughout the ANE aperture; posted writel froze the
 *    box.
 *  - Firmware alias: fw_alias_reserved=1 maps the iBoot-reserved
 *    SEG0/SEGi phys at the latched RVBAR entry (proven); the
 *    subsequent kernel writes into the entry region translate through
 *    dart-ane0 instead of faulting.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/util_macros.h>
#include <linux/workqueue.h>

#include <drm/drm_accel.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_mm.h>

#include "ane_t6021.h"
#include "ane_t6021_boot.h"

#include "uapi/drm/ane_accel.h" /* quoted so the in-tree UAPI wins */

/* pmgr ane_cpu ACTUAL word (ane0 reg1 window, pmgr+0x2e0) */
#define ANE_RTCLIENT_PS_CPU_ACTUAL_OFF	0x2e0

/* TM base for the eight TQ status words (one per queue) */
#define ANE_TM_BASE			0x285c00000ull
#define ANE_TM_TQ_STATUS_STRIDE		0x2c
#define ANE_TM_TQ_STATUS_OFF		0x20804
#define ANE_TM_TQ_STATUS(q)		(ANE_TM_BASE + ANE_TM_TQ_STATUS_OFF + \
					 (u64)(q) * ANE_TM_TQ_STATUS_STRIDE)
#define ANE_TM_TQ_STATUS_IDLE		0x81
#define ANE_TM_TQ_STATUS_BUSY		0x70

/* CPU_STATUS bits (m1n1 ASCRegs shape) */
#define ANE_ASC_CPU_STATUS_RUNNING	BIT(0)
#define ANE_ASC_CPU_STATUS_STOPPED	BIT(1)

/* Boot-time module parameters — defaults are the proven add-path
 * parameter list (boot 3ab812a3 / load-run.sh). fw_load, fw_extra_ram
 * and fw_alias_reserved live in ane_t6021_fwload.c and
 * boot_prevent_nap in ane_t6021_boot.c (single registration each);
 * their compiled defaults are the proven values. Remaining knobs are
 * overridable from sysfs for bisection only. */
static bool fw_start = true;
module_param(fw_start, bool, 0444);
MODULE_PARM_DESC(fw_start,
		 "Fenced Linux-context firmware start: stage + alias, then the boot sequence; default on (proven add-path config).");

static int fw_start_table_mode = 2;
module_param(fw_start_table_mode, int, 0444);
MODULE_PARM_DESC(fw_start_table_mode,
		 "Pre-CPU engine table block: 0 abort, 1 write, 2 skip (default, proven config).");

static bool fw_start_rtb_mode;
module_param(fw_start_rtb_mode, bool, 0444);
MODULE_PARM_DESC(fw_start_rtb_mode,
		 "RTBuddy/RTKit-app-endpoint select; default off (legacy ChMan transport).");

/* Drive RX by apple_rtkit_poll from a workqueue even though a recv
 * IRQ exists (lab poll_rx=1, the proven add-path value: the raw recv
 * line is unproven, so the worker is what services HELLO/EPMAP). */
static bool poll_rx = true;
module_param(poll_rx, bool, 0444);
MODULE_PARM_DESC(poll_rx,
		 "Drive RX by apple_rtkit_poll from a workqueue (default on, proven config).");

/* STARTEP every fw-announced app endpoint (>= 0x20) after a successful
 * RTKit handshake (lab start_app_eps=1 default). */
static bool start_app_eps = true;
module_param(start_app_eps, bool, 0444);
MODULE_PARM_DESC(start_app_eps,
		 "STARTEP every fw-announced app endpoint after the handshake (default on).");

static bool scratch3_ack = true;
module_param(scratch3_ack, bool, 0444);
MODULE_PARM_DESC(scratch3_ack,
		 "After DONE, write SCRATCH3 = 0x08042006 (selene 0x7edc, kext 0x95eaee4). Default on; 0 withholds.");

static bool legacy_only = true;
module_param(legacy_only, bool, 0444);
MODULE_PARM_DESC(legacy_only,
		 "Use the pinned 13.5 legacy ChMan transport (default on, proven add-path config).");

static bool legacy_query = true;
module_param(legacy_query, bool, 0444);
MODULE_PARM_DESC(legacy_query,
		 "Service bounded startup allocations and CONFIG_GET; default on (proven config).");

static unsigned int hello_wait_ms = 1000;
module_param(hello_wait_ms, uint, 0444);
MODULE_PARM_DESC(hello_wait_ms,
		 "Upper bound for the RTKit boot handshake (rtkit.c waits are 1 s each); the lab proven value is 1000.");

#define ANE_LEGACY_ALLOCS 128
#define ANE_LEGACY_BYTES SZ_16M

struct ane_legacy_buffer {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

struct ane_rtclient {
	struct device *dev;
	void __iomem *engine;
	void __iomem *pmgr;
	struct apple_rtkit *rtk;
	struct reset_control *cpu_rst;
	struct delayed_work poll_work;

	/* fw_start=1 view over the shared boot/fwload contract units. */
	struct ane_t6021 *fw;

	/* A CPU we released is (or may be) running: state HELD — surfaces,
	 * rings, IRQ, power links preserved; no unwind; reboot reclaims
	 * (wedged-pin rule). */
	bool held;

	bool boot_done;

	/* ChMan descriptor table validated against the 'IPC ' surface. */
	bool chman_ok;
	struct ane_legacy_buffer legacy_buffers[ANE_LEGACY_ALLOCS];
	u32 legacy_allocated;
	size_t legacy_bytes;
	u32 legacy_malloc_cursor;
	u32 legacy_cmd_cursor[ANE_T6021_CHMAN_COUNT];
};

/* BO table — kernels own the cached IOVA for every section passed to
 * PROG_LOAD and the IO buffer passed to EXEC until BO_FREE (the
 * power/state HELD rule keeps the table around while CPU started). */
struct ane_t6021_bo {
	struct list_head node;
	u32 handle;
	void *cpu;			/* coherent mapping */
	dma_addr_t dma;
	size_t size;
	bool kernel_owned;		/* PROG_LOAD section copy */
};

static LIST_HEAD(ane_t6021_bo_list);
static DEFINE_MUTEX(ane_t6021_bo_lock);
static u32 ane_t6021_next_handle = 1;

/* Serializes every firmware command (LOAD/CREATE/CALL) so the
 * cursor-on-next-64-byte-slot rule and the PMGR/TM gate cannot race. */
static DEFINE_MUTEX(ane_t6021_fw_lock);

/* Forward declaration (defined below). */
static int ane_rtclient_legacy_exchange(struct ane_rtclient *ane,
				       struct ane_legacy_buffer *command,
				       size_t length, u16 opcode,
				       unsigned int channel,
				       unsigned int timeout_ms);

/* ---- boot-contract helpers (shared with boot.c / fwload.c) ---- */

static int ane_rtclient_legacy_alloc(struct ane_rtclient *ane, u64 size)
{
	struct ane_legacy_buffer *buffer;

	if (!size || !IS_ALIGNED(size, SZ_16K) || size > SZ_2M ||
	    ane->legacy_allocated == ANE_LEGACY_ALLOCS ||
	    size > ANE_LEGACY_BYTES - ane->legacy_bytes)
		return -E2BIG;
	buffer = &ane->legacy_buffers[ane->legacy_allocated];
	buffer->cpu = dma_alloc_coherent(ane->dev, size, &buffer->dma,
					 GFP_KERNEL);
	if (!buffer->cpu)
		return -ENOMEM;
	if (!IS_ALIGNED(buffer->dma, SZ_16K) ||
	    (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, buffer->dma, size))) {
		dma_free_coherent(ane->dev, size, buffer->cpu, buffer->dma);
		buffer->cpu = NULL;
		return -ERANGE;
	}
	memset(buffer->cpu, 0, size);
	buffer->size = size;
	ane->legacy_bytes += size;
	ane->legacy_allocated++;
	return 0;
}

/* Validate the chman table the fw published in the 'IPC ' surface. */
static void ane_rtclient_validate_chman(struct ane_rtclient *ane)
{
	struct ane_t6021 *a = ane->fw;
	const struct ane_t6021_chman_desc *t;
	unsigned int i, bad;

	if (!a || !a->boot_ipc) {
		dev_info(ane->dev,
			 "chman: no host IPC surface — table not validated\n");
		return;
	}
	if (a->boot_ipc_size < ANE_T6021_CHMAN_TOTAL) {
		dev_warn(ane->dev,
			 "chman: IPC surface %#llx bytes < fw layout %#x — table not validated\n",
			 a->boot_ipc_size, ANE_T6021_CHMAN_TOTAL);
		return;
	}

	dma_rmb();
	t = a->boot_ipc;
	bad = ane_t6021_chman_check(t, a->boot_ipc_iova);
	for (i = 0; i < ANE_T6021_CHMAN_COUNT; i++) {
		const struct ane_t6021_chman_desc *d = &t[i];
		const struct ane_t6021_chman_static *s = &ane_t6021_chman_layout[i];

		dev_info(ane->dev,
			 "chman[%u]: name=\"%.*s\" type=%u bit=%u size=%#llx %s (static: %s/%u/%u/%#llx/ipc+%#x)\n",
			 i, ANE_T6021_CHMAN_NAME_LEN, d->name, d->type, d->bit,
			 d->size,
			 (bad & BIT(i)) ? "MISMATCH" : "OK",
			 s->name, s->type, s->bit, s->size, s->off);
	}

	ane->chman_ok = !bad;
	dev_info(ane->dev, "chman: table %s (mismatch mask %#x)\n",
		 bad ? "NOT VALIDATED" : "VALIDATED", bad);
}

/* Legacy ChMan exchange (post-DONE 13.5 transport). Resends to the same
 * 64-byte IO slot would wedge the ring; cursor advances after each
 * completed command (selene fw decodes bit0 = host-owned). */
static int ane_rtclient_legacy_exchange(struct ane_rtclient *ane,
				       struct ane_legacy_buffer *command,
				       size_t length, u16 opcode,
				       unsigned int channel,
				       unsigned int timeout_ms)
{
	struct ane_t6021 *a = ane->fw;
	u64 *io, *malloc_ring, header;
	u64 *t2h_buf, *t2h_ioq;
	void __iomem *ipi;
	unsigned int cursor = ane->legacy_malloc_cursor;
	unsigned long deadline;
	u32 *reply;
	int result = -ETIMEDOUT;

	if (!ane->held || !ane->chman_ok ||
	    ane_t6021_chman_check(a->boot_ipc, a->boot_ipc_iova))
		return -EPROTO;
	ipi = ioremap_np(0x285844000ull, 0xc004);
	if (!ipi)
		return -ENOMEM;
	if (length < 8 || length > command->size ||
	    channel >= ANE_T6021_CHMAN_COUNT) {
		result = -EINVAL;
		goto out;
	}
	io = a->boot_ipc + ane_t6021_chman_layout[channel].off +
	     (size_t)ane->legacy_cmd_cursor[channel] * 64;
	malloc_ring = a->boot_ipc + ane_t6021_chman_layout[5].off;
	t2h_buf = a->boot_ipc + ane_t6021_chman_layout[4].off;
	t2h_ioq = a->boot_ipc + ane_t6021_chman_layout[6].off;

	if (!(READ_ONCE(io[0]) & 1)) {
		result = -EBUSY;
		goto out;
	}
	reply = command->cpu;
	((u16 *)reply)[2] = opcode;
	WRITE_ONCE(io[1], length);
	WRITE_ONCE(io[2], length);
	dma_wmb();
	WRITE_ONCE(io[0], command->dma);
	dma_wmb();
	writel(BIT(ane_t6021_chman_layout[channel].bit), ipi);
	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	while (time_before(jiffies, deadline)) {
		u32 pending = readl(ipi + 0x8000);

		if (pending) {
			writel(pending, ipi + 0xc000);
			mb();
		}
		header = READ_ONCE(io[0]);
		if (!(header & 1)) {
			/* malloc-ring service: the fw may ask for an
			 * allocation while we wait; service those until
			 * the command slot flips back. */
			u64 *slot = malloc_ring + cursor * 8;
			u64 alloc_hdr = READ_ONCE(slot[0]);

			if (!(alloc_hdr & 1)) {
				u64 size, tag;
				struct ane_legacy_buffer *buf;

				dma_rmb();
				size = READ_ONCE(slot[1]);
				tag = READ_ONCE(slot[2]);
				/* Lab rule: a valid MALLOC slot carries
				 * header 0 and a u32 tag; anything else
				 * is a malformed ring and we stop. */
				if (alloc_hdr || tag > U32_MAX) {
					result = -EOPNOTSUPP;
					goto out;
				}
				if (ane_rtclient_legacy_alloc(ane, size)) {
					result = -ENOMEM;
					goto out;
				}
				buf = &ane->legacy_buffers[ane->legacy_allocated - 1];
				WRITE_ONCE(slot[1], 0);
				WRITE_ONCE(slot[2], ane->legacy_allocated);
				dma_wmb();
				WRITE_ONCE(slot[0], buf->dma | 1);
				dma_wmb();
				writel(BIT(5), ipi);
				cursor = (cursor + 1) %
					ane_t6021_chman_layout[5].size;
			}
		}
		header = READ_ONCE(io[0]);
		if (header & 1) {
			dma_rmb();
			result = (header == (command->dma | 1) &&
				  READ_ONCE(io[1]) == length &&
				  READ_ONCE(io[2]) == 0 &&
				  ((u16 *)reply)[2] == opcode &&
				  ((u16 *)reply)[3] == 0) ? 0 : -EPROTO;
			ane->legacy_cmd_cursor[channel] =
				(ane->legacy_cmd_cursor[channel] + 1) %
				ane_t6021_chman_layout[channel].size;
			goto out;
		}
		/* legacy_fast_poll=1 (the proven add-path cadence): poll
		 * the rings every 50 us instead of sleeping 1-2 ms. */
		udelay(50);
	}
	dev_info(ane->dev, "LEGACY timeout ch=%u io=%016llx\n",
		 channel, READ_ONCE(io[0]));
out:
	ane->legacy_malloc_cursor = cursor;
	iounmap(ipi);
	return result;
}

/* Validate the pmgr PS words at 0x28e084000..0x28e084030 read 0x3ff
 * before touching the TM window (a TM read while compute domains are
 * off hangs the SoC). */
static int ane_rtclient_pm_pwrstate_ok(void)
{
	void __iomem *pm = ioremap_np(0x28e084000ull, 0x40);
	u32 v;
	unsigned int i;
	int ret = 0;

	if (!pm)
		return -ENOMEM;
	for (i = 0; i <= 0x30; i += 8) {
		v = readl(pm + i);
		if ((v & 0x3ff) != 0x3ff) {
			ret = -EIO;
			break;
		}
	}
	iounmap(pm);
	return ret;
}

/* Wait for all eight TQ status words to read 0x81 (idle). A read of
 * the TM window is gated on ane_rtclient_pm_pwrstate_ok; the watchdog
 * deadline is timeout_ms. Returns 0 on idle, -ETIMEDOUT on stall. */
static int ane_rtclient_tq_idle_poll(unsigned int timeout_ms)
{
	void __iomem *tm;
	unsigned int q;
	unsigned long deadline;
	int ret;

	ret = ane_rtclient_pm_pwrstate_ok();
	if (ret)
		return ret;
	tm = ioremap_np(ANE_TM_BASE + ANE_TM_TQ_STATUS_OFF - 0x40,
			ANE_TM_TQ_STATUS_STRIDE * ANE_T6021_CHMAN_COUNT + 0x40);
	if (!tm)
		return -ENOMEM;
	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	while (time_before(jiffies, deadline)) {
		for (q = 0; q < ANE_T6021_CHMAN_COUNT; q++) {
			if (readl(tm + q * ANE_TM_TQ_STATUS_STRIDE) !=
			    ANE_TM_TQ_STATUS_IDLE)
				break;
		}
		if (q == ANE_T6021_CHMAN_COUNT) {
			iounmap(tm);
			return 0;
		}
		usleep_range(1000, 2000);
	}
	iounmap(tm);
	return -ETIMEDOUT;
}

/* Return every firmware-owned slot on a target-to-host ring to the fw
 * (legacy_t2h_ack=1, the lab default): the fw publishes by writing the
 * DMA address with bit0 clear; the host returns the slot by setting
 * bit0 and ringing the channel's doorbell bit, as the allocation ring
 * does. The sequencer drained channels 4/6 after every step; the
 * ioctls drain them after every completed exchange. */
static unsigned int ane_rtclient_drain_t2h(struct ane_rtclient *ane,
					   unsigned int channel)
{
	const struct ane_t6021_chman_static *c = &ane_t6021_chman_layout[channel];
	unsigned int n = 0, slot_i = ane->legacy_cmd_cursor[channel];
	void __iomem *ipi = NULL;

	if (!ane->fw || !ane->fw->boot_ipc)
		return 0;
	while (n < c->size) {
		u64 *slot = ane->fw->boot_ipc + c->off + (size_t)slot_i * 64;
		u64 hdr = READ_ONCE(slot[0]), len = READ_ONCE(slot[1]);

		if (hdr & 1)
			break;
		dma_rmb();
		dev_info(ane->dev, "T2H ch=%s slot=%u hdr=%016llx len=%#llx\n",
			 c->name, slot_i, hdr, len);
		n++;
		WRITE_ONCE(slot[0], hdr | 1);
		dma_wmb();
		if (!ipi)
			ipi = ioremap_np(0x285844000ull, 0xc004);
		if (ipi)
			writel(BIT(c->bit), ipi);
		slot_i = (slot_i + 1) % c->size;
		ane->legacy_cmd_cursor[channel] = slot_i;
	}
	if (ipi)
		iounmap(ipi);
	return n;
}

/* Mark the device quarantined: a timed-out command left the firmware
 * queue state unknown. The only safe next step is to refuse further
 * ioctls until a reboot reclaims the surfaces (wedged-pin rule). */
static atomic_t ane_t6021_quarantined = ATOMIC_INIT(0);

static int ane_rtclient_legacy_exchange_with_tq_idle(struct ane_rtclient *ane,
						     struct ane_legacy_buffer *command,
						     size_t length, u16 opcode,
						     unsigned int channel,
						     unsigned int timeout_ms)
{
	int ret;

	ret = ane_rtclient_legacy_exchange(ane, command, length, opcode,
					   channel, timeout_ms);
	if (ret) {
		atomic_set(&ane_t6021_quarantined, 1);
		return ret;
	}
	/* Firmware ack is the slot reply; the TQ-idle poll is the
	 * completion proof — only after all eight queues idle does the
	 * result become visible to the CPU. The poll is the contract a
	 * successful EXEC guarantees (the user ABI). */
	ret = ane_rtclient_tq_idle_poll(timeout_ms);
	if (ret) {
		atomic_set(&ane_t6021_quarantined, 1);
		return ret;
	}
	/* The fw talks back on the target-to-host rings (fwlog, perf);
	 * hand those slots back so the rings never fill (the sequencer
	 * did this per step; same ack, channels 4 and 6). */
	ane_rtclient_drain_t2h(ane, 4);
	ane_rtclient_drain_t2h(ane, 6);
	/* dma_sync: the section BO + the io BO were allocated
	 * cache-coherent (dma_alloc_coherent), so no cache clean is
	 * needed for the device write half; the CPU-side read after
	 * EXEC needs an invalidate to pull the post-call bytes back.
	 * The ane_bo caller owns that mapping; the legacy buffer holds
	 * only the command slot, already coherent. */
	return 0;
}

/* ---- LOAD / CREATE / CALL wrappers ---- */

/* Build a LOAD_PROGRAM (0x200) message in a kernel-owned buffer. The
 * section bytes live in the BOs the user supplied (section_ptr), the
 * generic binds (bufferId, bo_handle, size) patch the generic section
 * at iova +0x20 / +0x28 once the BO is copied. Returns 0 with
 * *prog_id set on success. */
static int ane_rtclient_load_program(struct ane_rtclient *ane,
				     const struct drm_ane_prog_load *user,
				     __u32 *prog_id)
{
	struct drm_ane_section *sections;
	struct drm_ane_generic_bind *binds;
	struct ane_legacy_buffer *command;
	size_t binds_size;
	int ret, i;

	if (user->section_count < 1 ||
	    user->section_count > ANE_T6021_LOAD_SEC_COUNT ||
	    user->generic_count > ANE_M2_MAX_BINDS)
		return -EINVAL;

	sections = kmalloc_array(user->section_count, sizeof(*sections),
				GFP_KERNEL);
	binds = kmalloc_array(user->generic_count, sizeof(*binds),
			      GFP_KERNEL);
	if (!sections || !binds) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(sections,
			   u64_to_user_ptr(user->sections_ptr),
			   user->section_count * sizeof(*sections))) {
		ret = -EFAULT;
		goto out;
	}
	binds_size = user->generic_count * sizeof(*binds);
	if (binds_size && copy_from_user(binds,
					u64_to_user_ptr(user->generic_ptr),
					binds_size)) {
		ret = -EFAULT;
		goto out;
	}

	ret = ane_rtclient_legacy_alloc(ane, SZ_16K);
	if (ret)
		goto out;
	command = &ane->legacy_buffers[ane->legacy_allocated - 1];
	memset(command->cpu, 0, SZ_16K);
	/* One 0x30-byte section record at +8 + slot * 0x30 per supplied
	 * section; unsupplied slots stay zero-filled (flags = 0), which
	 * is exactly the proven add-path image (six populated records:
	 * ids 1,2,3,4,5 then tdprop id 7 at slot 6). flags bit0 = 1,
	 * id = the firmware section identity the caller carries in
	 * drm_ane_section.id, iova at +0x18, size at +0x20. */
	for (i = 0; i < user->section_count; i++) {
		struct ane_t6021_bo *bo = NULL, *b;

		list_for_each_entry(b, &ane_t6021_bo_list, node) {
			if (b->handle == sections[i].bo_handle) {
				bo = b;
				break;
			}
		}
		if (!bo || sections[i].offset + sections[i].size > bo->size) {
			ret = -EINVAL;
			goto out;
		}
		{
			u64 slot_base = 0x8 + (u64)i * 0x30;
			u8 *cmd = command->cpu;

			*(u32 *)(cmd + slot_base + 0x00) = cpu_to_le32(1);
			*(u32 *)(cmd + slot_base + 0x04) =
				cpu_to_le32(sections[i].id);
			*(u64 *)(cmd + slot_base + 0x18) =
				cpu_to_le64(bo->dma + sections[i].offset);
			*(u64 *)(cmd + slot_base + 0x20) =
				cpu_to_le64(sections[i].size);
		}
	}
	/* Generic binds are accepted for ABI compatibility and unused:
	 * the LOAD record already carries iova + size, and the generic
	 * section image is prepared by userspace (the proven add path
	 * ships it inside the section bytes, generic.bin). */

	/* ProgramId placeholder; the firmware writes the assigned id
	 * back at +0x1b8. */
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x1b8) = cpu_to_le32(U32_MAX);
	}

	/* The proven add-path LOAD command is 0x1C0 bytes: the nine
	 * 0x30 records end at 0x1b8, ProgramId at +0x1b8, 8 bytes of
	 * zero tail. sizeof(struct ane_csne_cmd_load_program) is only
	 * 0x1b8, so the length is pinned here (h14_seq_first_add.py
	 * load_step). */
	BUILD_BUG_ON(sizeof(struct ane_csne_cmd_load_program) != 0x1b8);
	ret = ane_rtclient_legacy_exchange_with_tq_idle(ane, command,
						       0x1c0,
						       CSNE_CMD_LOAD_PROGRAM,
						       1, 5000);
	if (!ret) {
		u8 *cmd = command->cpu;

		*prog_id = le32_to_cpu(*(u32 *)(cmd + 0x1b8));
		if (*prog_id == U32_MAX) {
			ret = -EPROTO;
			goto out;
		}
	}

out:
	kfree(sections);
	kfree(binds);
	return ret;
}

static int ane_rtclient_create_process(struct ane_rtclient *ane,
				       __u32 prog_id, __u32 *proc_id)
{
	struct ane_legacy_buffer *command;
	int ret;

	ret = ane_rtclient_legacy_alloc(ane, SZ_16K);
	if (ret)
		return ret;
	command = &ane->legacy_buffers[ane->legacy_allocated - 1];
	memset(command->cpu, 0, SZ_16K);
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x08) = cpu_to_le32(prog_id);
		*(u32 *)(cmd + 0x0c) = cpu_to_le32(U32_MAX);
	}
	ret = ane_rtclient_legacy_exchange_with_tq_idle(ane, command, 0x10,
						       CSNE_CMD_CREATE_PROCESS,
						       1, 3000);
	if (!ret) {
		u8 *cmd = command->cpu;

		*proc_id = le32_to_cpu(*(u32 *)(cmd + 0x0c));
		if (*proc_id == U32_MAX)
			ret = -EPROTO;
	}
	return ret;
}

static int ane_rtclient_procedure_call(struct ane_rtclient *ane,
				      const struct drm_ane_exec *user)
{
	struct drm_ane_exec_io *ios;
	struct ane_legacy_buffer *command;
	size_t ios_size;
	size_t cmd_size;
	int ret, i;

	if (user->count < 1 || user->count > ANE_M2_MAX_BINDS)
		return -EINVAL;
	if (user->priority < 2 || user->priority > 7)
		return -EINVAL;
	if (!user->prog_id || !user->proc_id)
		return -EINVAL;

	ios_size = (size_t)user->count * sizeof(*ios);
	ios = kmalloc(ios_size, GFP_KERNEL);
	if (!ios)
		return -ENOMEM;
	if (copy_from_user(ios, u64_to_user_ptr(user->io_ptr), ios_size)) {
		kfree(ios);
		return -EFAULT;
	}

	cmd_size = sizeof(struct ane_csne_cmd_procedure_call) +
		   (size_t)user->count * sizeof(struct ane_csne_io_elem);
	if (cmd_size > SZ_16K) {
		kfree(ios);
		return -E2BIG;
	}

	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		kfree(ios);
		return -ETIMEDOUT;
	}
	ret = ane_rtclient_legacy_alloc(ane, SZ_16K);
	if (ret) {
		mutex_unlock(&ane_t6021_fw_lock);
		kfree(ios);
		return ret;
	}
	command = &ane->legacy_buffers[ane->legacy_allocated - 1];
	memset(command->cpu, 0, SZ_16K);
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x08) = cpu_to_le32(user->prog_id);
		*(u32 *)(cmd + 0x0c) = cpu_to_le32(user->proc_id);
		*(u64 *)(cmd + 0x10) = cpu_to_le64(0);
		*(u32 *)(cmd + 0x18) = cpu_to_le32(user->priority);
		*(u64 *)(cmd + 0x20) = cpu_to_le64(0xADD0);
		*(u32 *)(cmd + 0x28) = cpu_to_le32(user->count);
		for (i = 0; i < user->count; i++) {
			struct ane_t6021_bo *bo = NULL, *b;
			u64 slot_base = 0x60 + (u64)i * 0x30;

			list_for_each_entry(b, &ane_t6021_bo_list, node) {
				if (b->handle == ios[i].bo_handle) {
					bo = b;
					break;
				}
			}
			if (!bo) {
				ret = -EINVAL;
				goto unlock;
			}
			if (ios[i].size > bo->size) {
				ret = -EINVAL;
				goto unlock;
			}
			*(u32 *)(cmd + slot_base + 0x00) = cpu_to_le32(1);
			*(u32 *)(cmd + slot_base + 0x04) =
				cpu_to_le32(ios[i].buffer_id);
			*(u32 *)(cmd + slot_base + 0x08) =
				cpu_to_le32(ios[i].type);
			*(u64 *)(cmd + slot_base + 0x18) =
				cpu_to_le64(bo->dma);
			*(u64 *)(cmd + slot_base + 0x20) =
				cpu_to_le64(ios[i].size);
		}
		ret = ane_rtclient_legacy_exchange_with_tq_idle(ane, command,
							       cmd_size,
							       CSNE_CMD_PROCEDURE_CALL,
							       1,
							       user->timeout_ms ?
							       user->timeout_ms : 5000);
	}
unlock:
	mutex_unlock(&ane_t6021_fw_lock);
	kfree(ios);
	return ret;
}

/* ---- DRM accel glue (minimal BO + ioctl table) ---- */

struct ane_t6021_drm {
	struct drm_device drm;
	struct device *dev;
	struct ane_rtclient *ane;
};

static struct ane_t6021_drm *to_ane_t6021_drm(struct drm_device *drm)
{
	return container_of(drm, struct ane_t6021_drm, drm);
}

static int ane_t6021_bo_init_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct drm_ane_bo_init *args = data;
	struct ane_t6021_bo *bo;
	struct ane_rtclient *ane;

	if (args->size == 0 || args->size > SZ_16M)
		return -EINVAL;
	ane = to_ane_t6021_drm(drm)->ane;
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	bo->cpu = dma_alloc_coherent(drm->dev, args->size, &bo->dma,
				     GFP_KERNEL);
	if (!bo->cpu) {
		kfree(bo);
		return -ENOMEM;
	}
	/* Every fw-visible DMA surface must be 16 KiB aligned and clear
	 * of the firmware entry alias (receipt 2026-09-20-t6021-entry-alias:
	 * the same invariant ane_rtclient_legacy_alloc enforces). */
	if (!IS_ALIGNED(bo->dma, SZ_16K) ||
	    (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, bo->dma,
						    args->size))) {
		dma_free_coherent(drm->dev, args->size, bo->cpu, bo->dma);
		kfree(bo);
		return -ERANGE;
	}
	bo->size = args->size;
	bo->kernel_owned = false;
	mutex_lock(&ane_t6021_bo_lock);
	bo->handle = ane_t6021_next_handle++;
	if (bo->handle == 0)
		bo->handle = ane_t6021_next_handle++;
	list_add_tail(&bo->node, &ane_t6021_bo_list);
	mutex_unlock(&ane_t6021_bo_lock);
	args->handle = bo->handle;
	args->offset = 0;
	return 0;
}

static int ane_t6021_bo_free_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct drm_ane_bo_free *args = data;
	struct ane_t6021_bo *bo = NULL, *b;

	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry(b, &ane_t6021_bo_list, node) {
		if (b->handle == args->handle) {
			bo = b;
			break;
		}
	}
	if (!bo) {
		mutex_unlock(&ane_t6021_bo_lock);
		return -ENOENT;
	}
	list_del(&bo->node);
	mutex_unlock(&ane_t6021_bo_lock);
	if (bo->cpu)
		dma_free_coherent(drm->dev, bo->size, bo->cpu, bo->dma);
	kfree(bo);
	return 0;
}

static int ane_t6021_submit_ioctl(struct drm_device *drm, void *data,
				  struct drm_file *file)
{
	return -ENOTTY; /* ABI 1 SUBMIT is rejected on T6021/M2 */
}

static int ane_t6021_prog_load_ioctl(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct ane_t6021_drm *adrm = to_ane_t6021_drm(drm);
	struct drm_ane_prog_load *args = data;
	int ret;

	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ETIMEDOUT;
	}
	ret = ane_rtclient_load_program(adrm->ane, args, &args->prog_id_out);
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

static int ane_t6021_proc_create_ioctl(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct ane_t6021_drm *adrm = to_ane_t6021_drm(drm);
	struct drm_ane_proc_create *args = data;
	int ret;

	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ETIMEDOUT;
	}
	args->proc_id_out = 0;
	ret = ane_rtclient_create_process(adrm->ane, args->prog_id,
					  &args->proc_id_out);
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

static int ane_t6021_exec_ioctl(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct ane_t6021_drm *adrm = to_ane_t6021_drm(drm);
	struct drm_ane_exec *args = data;

	return ane_rtclient_procedure_call(adrm->ane, args);
}

static const struct drm_ioctl_desc ane_t6021_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ANE_BO_INIT, ane_t6021_bo_init_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_BO_FREE, ane_t6021_bo_free_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_SUBMIT, ane_t6021_submit_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_PROG_LOAD, ane_t6021_prog_load_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_PROC_CREATE, ane_t6021_proc_create_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_EXEC, ane_t6021_exec_ioctl, 0),
};

/* Version reported through DRM_IOCTL_VERSION: ABI 2 (T6021). */
static const struct drm_driver ane_t6021_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_COMPUTE_ACCEL,
	.ioctls = ane_t6021_ioctls,
	.num_ioctls = ARRAY_SIZE(ane_t6021_ioctls),
	.major = ANE_ABI_M2_MAJOR,
	.minor = 0,
	.name = "ane",
	.desc = "Apple Neural Engine (T6021/M2)",
};

/* ---- RTKit ops (kept on the boot path; the legacy ChMan transport
 * is used by the ioctls once probe confirms legacy_only / chman_ok). */

static void ane_rtclient_recv(void *cookie, u8 ep, u64 message)
{
	struct ane_rtclient *ane = cookie;

	dev_info(ane->dev,
		 "rtkit app msg: ep=%#x msg=%016llx\n", ep, message);
}

static void ane_rtclient_crashed(void *cookie, const void *crashlog,
				 size_t size)
{
	struct ane_rtclient *ane = cookie;

	dev_err(ane->dev, "rtkit: coprocessor crashed (crashlog %zu bytes)\n",
		size);
	print_hex_dump(KERN_ERR, "ANE crashlog: ", DUMP_PREFIX_OFFSET, 16, 1,
		       crashlog, min_t(size_t, size, 256), false);
}

static int ane_rtclient_shmem_setup(void *cookie,
				    struct apple_rtkit_shmem *bfr)
{
	struct ane_rtclient *ane = cookie;

	if (bfr->iova) {
		dev_warn(ane->dev,
			 "rtkit: fw-provided shmem iova=%pad size=%#zx — refused\n",
			 &bfr->iova, bfr->size);
		return -EINVAL;
	}
	bfr->buffer = dma_alloc_coherent(ane->dev, bfr->size, &bfr->iova,
					 GFP_KERNEL);
	if (!bfr->buffer)
		return -ENOMEM;
	if (ane->fw && !ane_t6021_fw_alias_iova_ok(ane->fw, bfr->iova,
						   bfr->size)) {
		dev_err(ane->dev,
			"rtkit: shmem grant %pad+%#zx overlaps the fw alias — refusing\n",
			&bfr->iova, bfr->size);
		dma_free_coherent(ane->dev, bfr->size, bfr->buffer, bfr->iova);
		bfr->buffer = NULL;
		return -EBUSY;
	}
	return 0;
}

static void ane_rtclient_shmem_destroy(void *cookie,
				       struct apple_rtkit_shmem *bfr)
{
	struct ane_rtclient *ane = cookie;

	if (!bfr->buffer)
		return;
	if (ane->held) {
		dev_warn(ane->dev,
			 "rtkit: shmem %pad HELD (CPU started) — not freed\n",
			 &bfr->iova);
		return;
	}
	dma_free_coherent(ane->dev, bfr->size, bfr->buffer, bfr->iova);
}

static const struct apple_rtkit_ops ane_rtclient_rtkit_ops = {
	.crashed = ane_rtclient_crashed,
	.recv_message = ane_rtclient_recv,
	.shmem_setup = ane_rtclient_shmem_setup,
	.shmem_destroy = ane_rtclient_shmem_destroy,
};

/* ---- poll worker: RX fallback while the recv line is unproven ----
 * (lab poll_rx=1). Runs every 10 ms until the handshake completes,
 * then every second while poll_rx stays on. Armed BEFORE the host ack
 * so the fw's HELLO is never missed. */
static void ane_rtclient_post_boot(struct work_struct *w)
{
	struct ane_rtclient *ane =
		container_of(to_delayed_work(w), struct ane_rtclient,
			     poll_work);

	apple_rtkit_poll(ane->rtk);

	if (!ane->boot_done) {
		schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));
		return;
	}

	if (poll_rx)
		schedule_delayed_work(&ane->poll_work, HZ);
}

/* STARTEP every fw-announced app endpoint (>= 0x20; the fw mgmt
 * dispatcher starts an endpoint on flag bit 1). Same call every Asahi
 * RTKit client makes; the lab ran it with start_app_eps=1. */
static void ane_rtclient_start_app_eps(struct ane_rtclient *ane)
{
	int ep;

	for (ep = 0x20; ep < 0x100; ep++) {
		int ret;

		if (!apple_rtkit_has_endpoint(ane->rtk, ep))
			continue;
		ret = apple_rtkit_start_ep(ane->rtk, ep);
		dev_info(ane->dev, "rtkit: STARTEP app ep %#x -> %pe\n",
			 ep, ERR_PTR(ret));
	}
}

/* Multi-domain genpd attach, ownership-correct and idempotent. */
struct ane_rtclient_pd {
	struct list_head list;
	struct device *dev;
	struct device **pd_dev;
	struct device_link **pd_link;
	int count;
};
static LIST_HEAD(ane_rtclient_pd_list);
static DEFINE_MUTEX(ane_rtclient_pd_lock);
static bool ane_rtclient_pinned;

static void ane_rtclient_pd_free(struct ane_rtclient_pd *pd)
{
	list_del(&pd->list);
	kfree(pd->pd_dev);
	kfree(pd->pd_link);
	kfree(pd);
}

static int ane_rtclient_attach_genpd(struct ane_rtclient *ane)
{
	struct device *dev = ane->dev;
	struct ane_rtclient_pd *pd = NULL;
	int count, i, err = 0;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count == -ENOENT)
		return 0;
	if (count < 0)
		return count;
	if (count <= 1)
		return 0;

	mutex_lock(&ane_rtclient_pd_lock);
	list_for_each_entry(pd, &ane_rtclient_pd_list, list)
		if (pd->dev == dev)
			goto found;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd) {
		err = -ENOMEM;
		goto out;
	}
	INIT_LIST_HEAD(&pd->list);
	pd->dev = dev;
	pd->pd_dev = kcalloc(count, sizeof(*pd->pd_dev), GFP_KERNEL);
	pd->pd_link = kcalloc(count, sizeof(*pd->pd_link), GFP_KERNEL);
	if (!pd->pd_dev || !pd->pd_link) {
		ane_rtclient_pd_free(pd);
		pd = NULL;
		err = -ENOMEM;
		goto out;
	}
	list_add_tail(&pd->list, &ane_rtclient_pd_list);

found:
	for (i = pd->count; i < count; i++) {
		if (!pd->pd_dev[i]) {
			pd->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
			if (IS_ERR_OR_NULL(pd->pd_dev[i])) {
				err = IS_ERR(pd->pd_dev[i]) ?
				      PTR_ERR(pd->pd_dev[i]) : -ENODEV;
				pd->pd_dev[i] = NULL;
				goto out;
			}
		}
		if (!pd->pd_link[i]) {
			if (!ane_rtclient_pinned) {
				if (!try_module_get(THIS_MODULE)) {
					err = -ENODEV;
					goto out;
				}
				ane_rtclient_pinned = true;
			}
			pd->pd_link[i] =
				device_link_add(dev, pd->pd_dev[i],
						DL_FLAG_STATELESS |
						DL_FLAG_PM_RUNTIME |
						DL_FLAG_RPM_ACTIVE);
			if (!pd->pd_link[i]) {
				err = -EINVAL;
				goto out;
			}
		}
		pd->count = i + 1;
	}

	dev_emerg(dev, "BOOT-PHASE genpd domains attached: %d\n", count);
out:
	mutex_unlock(&ane_rtclient_pd_lock);
	return err;
}

static int ane_rtclient_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct ane_rtclient *ane;
	struct ane_t6021 *a;
	u32 cpu_status, ps_cpu;
	u64 rvbar;
	int ret;

	if (!ane_t6021_fwload_options_ok()) {
		dev_err(dev,
			"invalid firmware RAM-grant options; refusing before power access\n");
		return -EINVAL;
	}

	/* Early, BEFORE any allocation/power: legacy_only must never be
	 * rejected after the CPU release, where an unwind could drop
	 * domains under a running ASC (lab probe order). */
	if (legacy_only && (!fw_start || fw_start_rtb_mode)) {
		dev_err(dev,
			"legacy_only=1 requires fw_start=1 and fw_start_rtb_mode=0\n");
		return -EINVAL;
	}
	if (legacy_query && !legacy_only) {
		dev_err(dev, "legacy_query requires legacy_only=1\n");
		return -EINVAL;
	}

	ane = devm_kzalloc(dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = dev;
	platform_set_drvdata(pdev, ane);
	INIT_DELAYED_WORK(&ane->poll_work, ane_rtclient_post_boot);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	if (!(res->flags & IORESOURCE_MEM_NONPOSTED))
		dev_warn(dev, "engine window is not flagged non-posted\n");
	ane->engine = ioremap_np(res->start, resource_size(res));
	if (!ane->engine)
		return -ENOMEM;
	ane->cpu_rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(ane->cpu_rst))
		return dev_err_probe(dev, PTR_ERR(ane->cpu_rst),
				     "ane_cpu reset control\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = ane_rtclient_attach_genpd(ane);
	if (ret)
		return dev_err_probe(dev, ret, "extra genpd attach\n");
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "genpd raise failed\n");

	ane->pmgr = devm_of_iomap(dev, dev->of_node, 1, NULL);
	if (IS_ERR(ane->pmgr)) {
		ret = PTR_ERR(ane->pmgr);
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret,
				     "pmgr window map failed; G1 gate cannot run\n");
	}
	ps_cpu = readl(ane->pmgr + ANE_RTCLIENT_PS_CPU_ACTUAL_OFF);
	dev_emerg(dev, "ane_cpu ACTUAL = 0x%x\n", ps_cpu);
	if (FIELD_GET(ANE_PS_ACTUAL, ps_cpu) != ANE_PS_ON) {
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return -EPROBE_DEFER;
	}

	cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
	rvbar = readq(ane->engine + ANE_ASC_RVBAR);
	dev_emerg(dev,
		  "BOOT-PHASE engine reads ok: CPU_STATUS = 0x%x, RVBAR = %016llx (bit0=%u)\n",
		  cpu_status, rvbar, (u32)(rvbar & 1));

	if (!(cpu_status & ANE_ASC_CPU_STATUS_RUNNING)) {
		if (!fw_start) {
			dev_err(dev,
				"ANE firmware not alive (CPU_STATUS 0x%x) — start it from a quiesce context, or retry with fw_start=1\n",
				cpu_status);
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EPROBE_DEFER;
		}

		/* Lab fw_start order: staging requires fw_load and an
		 * IOMMU-mapped device BEFORE any allocation/staging. */
		if (!ane_t6021_fwload_requested()) {
			dev_err(dev,
				"fw_start: requires fw_load=1 (no staged firmware)\n");
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EINVAL;
		}
		if (!device_iommu_mapped(dev)) {
			dev_err(dev,
				"fw_start: device not IOMMU-mapped — a staged DVA/entry alias would be untranslated; refusing\n");
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EINVAL;
		}

		a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
		if (!a) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -ENOMEM;
		}
		a->dev = dev;
		a->base[ANE_T6021_REG_ENGINE] = ane->engine;
		a->irq = -1;
		a->power_gated = true;
		ane->fw = a;

		dev_emerg(dev, "BOOT-PHASE fwload stage+alias begin\n");
		ret = ane_t6021_fwload_probe(a);
		if (ret) {
			dev_err_probe(dev, ret, "fw_start: staging failed\n");
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return ret;
		}
		if (!ane_t6021_rvbar_entry_ok(a->fw_iova)) {
			dev_err(dev, "fw_start: staged iova %pad invalid\n",
				&a->fw_iova);
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return -EINVAL;
		}

		dev_emerg(dev, "BOOT-PHASE dispatch (table_mode=%d)\n",
			  fw_start_table_mode);
		ret = ane_t6021_boot_start(a, 0, fw_start_table_mode,
					   fw_start_rtb_mode);
		if (ret == -ENODATA || ret == -EAGAIN ||
		    ret == -EBUSY || ret == -ECANCELED) {
			ane_t6021_fwload_remove(a);
			ane->fw = NULL;
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return ret;
		}

		ane->held = true;
		cpu_status = readl(ane->engine + ANE_ASC_CPU_STATUS);
		dev_emerg(dev,
			  "BOOT-PHASE sequence returned %pe (cpu_started=%u fw_alive=%u booted=%u) CPU_STATUS=0x%x\n",
			  ERR_PTR(ret), a->cpu_started, a->fw_alive, a->booted,
			  cpu_status);
		if (!a->fw_alive && !fw_start_rtb_mode) {
			dev_err(dev,
				"fw_start: poll A timeout, no READY — HELD until reboot, RTKit handshake skipped\n");
			return 0;
		}
	}

	if (legacy_only) {
		ane_rtclient_validate_chman(ane);
		if (ane->fw && ane->fw->booted && scratch3_ack &&
		    ane->chman_ok &&
		    ane_t6021_chman_host_init(ane->fw->boot_ipc,
					     ane->fw->boot_ipc_size,
					     ane->fw->boot_ipc_iova)) {
			int hello_ret = 0;

			dma_wmb();
			dev_emerg(dev,
				  "LEGACY P8 host ack: SCRATCH3 <- %08x\n",
				  ANE_T6021_BOOT_ACK);
			/* The 13.5 fw HELLOes on the RTKit MGMT endpoint
			 * after the ack; with the recv line unproven the
			 * poll worker is the only RX path, so init rtkit
			 * and arm the worker BEFORE writing the ack. */
			if (hello_wait_ms && !ane->rtk) {
				ane->rtk = devm_apple_rtkit_init(dev, ane,
								NULL, 0,
								&ane_rtclient_rtkit_ops);
				if (IS_ERR(ane->rtk)) {
					dev_err(dev,
						"LEGACY hello: rtkit init %pe\n",
						ane->rtk);
					ane->rtk = NULL;
				} else {
					schedule_delayed_work(&ane->poll_work,
							      msecs_to_jiffies(10));
				}
			}
			writel(ANE_T6021_BOOT_ACK,
			       ane->engine + ANE_MBI_SCRATCH0 + 4 * 3);
			if (ane->rtk) {
				unsigned long hello_deadline =
					jiffies + msecs_to_jiffies(hello_wait_ms);

				dev_emerg(dev, "LEGACY hello: boot begin (%u ms)\n",
					  hello_wait_ms);
				do {
					hello_ret = apple_rtkit_boot(ane->rtk);
				} while (hello_ret == -ETIME &&
					 time_before(jiffies, hello_deadline));
				dev_emerg(dev,
					  "LEGACY hello: boot %pe running=%d crashed=%d\n",
					  ERR_PTR(hello_ret),
					  apple_rtkit_is_running(ane->rtk),
					  apple_rtkit_is_crashed(ane->rtk));
				if (!hello_ret) {
					ane->boot_done = true;
					if (start_app_eps)
						ane_rtclient_start_app_eps(ane);
				} else {
					cancel_delayed_work_sync(&ane->poll_work);
				}
			}
		} else {
			dev_warn(dev,
				 "LEGACY ack withheld (booted=%u scratch3_ack=%u chman_ok=%u)\n",
				 ane->fw ? ane->fw->booted : 0,
				 scratch3_ack, ane->chman_ok);
		}
		if (legacy_query && ane->chman_ok) {
			/* CONFIG_GET (opcode 0x03) keeps the legacy ChMan
			 * transport armed for the ioctls; the reply's
			 * word +0x08 must be nonzero (lab rule). The
			 * boot heap + 'IPC ' allocations stay HELD until
			 * reboot. */
			struct ane_legacy_buffer *command;
			int qret;

			qret = ane_rtclient_legacy_alloc(ane, SZ_16K);
			if (qret == 0) {
				command = &ane->legacy_buffers[ane->legacy_allocated - 1];
				memset(command->cpu, 0, SZ_16K);
				qret = ane_rtclient_legacy_exchange(ane,
								     command,
								     16, 0x03,
								     1, 3000);
				dev_info(dev,
					 "LEGACY CONFIG_GET words %08x %08x result=%d (DMA remains held)\n",
					 READ_ONCE(((u32 *)command->cpu)[1]),
					 READ_ONCE(((u32 *)command->cpu)[2]),
					 qret);
				if (!qret &&
				    !READ_ONCE(((u32 *)command->cpu)[2])) {
					dev_err(dev,
						"LEGACY CONFIG_GET reply word +0x08 zero\n");
					qret = -EPROTO;
				}
			}
		}
		ane->boot_done = true;
	} else {
		unsigned long deadline;
		int boot_ret;

		ane->rtk = devm_apple_rtkit_init(dev, ane, NULL, 0,
						 &ane_rtclient_rtkit_ops);
		if (IS_ERR(ane->rtk)) {
			ret = PTR_ERR(ane->rtk);
			ane->rtk = NULL;
			dev_err_probe(dev, ret, "apple_rtkit_init failed\n");
			goto err_pm_or_hold;
		}
		/* RX fallback worker first: the fw's HELLO must not be
		 * missed while the recv line is unproven. */
		schedule_delayed_work(&ane->poll_work, msecs_to_jiffies(10));
		deadline = jiffies + msecs_to_jiffies(hello_wait_ms);
		do {
			boot_ret = apple_rtkit_boot(ane->rtk);
		} while (boot_ret == -ETIME && time_before(jiffies, deadline));
		if (boot_ret) {
			dev_err(dev, "rtkit boot handshake failed: %pe\n",
				ERR_PTR(boot_ret));
			cancel_delayed_work_sync(&ane->poll_work);
			ret = boot_ret;
			goto err_pm_or_hold;
		}
		ane->boot_done = true;
		if (start_app_eps)
			ane_rtclient_start_app_eps(ane);
	}

	if (!ane->chman_ok) {
		dev_err(dev,
			"install: ChMan table not validated — refusing to register DRM device (ioctls would stall the ring)\n");
		if (!ane->held) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
		}
		return -EPROTO;
	}

	{
		struct ane_t6021_drm *adrm;
		int drmret;

		/* This kernel dropped drm_dev_init; the resource-managed
		 * alloc registers the same ABI-2 device. */
		adrm = devm_drm_dev_alloc(dev, &ane_t6021_drm_driver,
					  struct ane_t6021_drm, drm);
		if (IS_ERR(adrm)) {
			ret = dev_err_probe(dev, PTR_ERR(adrm),
					    "drm device alloc\n");
			goto err_pm_or_hold;
		}
		adrm->dev = dev;
		adrm->ane = ane;
		drmret = drm_dev_register(&adrm->drm, 0);
		if (drmret) {
			dev_err_probe(dev, drmret, "drm_dev_register\n");
			ret = drmret;
			goto err_pm_or_hold;
		}
		dev_info(dev,
			 "loaded ane_t6021 %s (DRM major %d minor %d; ABI 2; legacy_only=%u chman_ok=%u booted=%u; state %s)\n",
			 ANE_T6021_MODULE_VERSION, ANE_ABI_M2_MAJOR, 0,
			 legacy_only, ane->chman_ok,
			 ane->fw ? ane->fw->booted : 0,
			 ane->held ? "HELD" : "ready");
	}

	return 0;

err_pm_or_hold:
	if (ane->held)
		dev_warn(dev,
			 "probe failed after CPU start (%pe) — binding fenced; power/rings/IRQ HELD until reboot\n",
			 ERR_PTR(ret));
	else {
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
	}
	return ret;
}

static void ane_rtclient_remove(struct platform_device *pdev)
{
	struct ane_rtclient *ane = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ane->poll_work);

	if (ane->held)
		dev_warn(&pdev->dev,
			 "remove HELD: no teardown — reboot reclaims\n");
}

static const struct of_device_id ane_rtclient_of_match[] = {
	{ .compatible = "apple,t6021-ane" },
	{ }
};
MODULE_DEVICE_TABLE(of, ane_rtclient_of_match);

static struct platform_driver ane_rtclient_driver = {
	.driver = {
		.name = "ane_t6021",
		.of_match_table = ane_rtclient_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = ane_rtclient_probe,
	.remove = ane_rtclient_remove,
};
module_platform_driver(ane_rtclient_driver);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Apple Neural Engine (T6021/M2) installed module");
