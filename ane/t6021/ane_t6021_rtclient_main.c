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
#include <crypto/sha2.h>

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
#define ANE_TM_TD_WINDOW		0x20400
#define ANE_TM_TD_WINDOW_SIZE		0x440
#define ANE_TM_TD_COUNT_OFF		0x58
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

#define ANE_LEGACY_ALLOCS 8192
#define ANE_LEGACY_BYTES SZ_512M

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
	struct ane_legacy_buffer *legacy_buffers;
	u32 legacy_allocated;
	size_t legacy_bytes;
	u32 legacy_malloc_cursor;
	/* Last-committed-TD word after the previous completed call. */
	u32 td_seen;
	u32 legacy_cmd_cursor[ANE_T6021_CHMAN_COUNT];
	/* One reusable 16 KiB command buffer for every host command
	 * (CONFIG_GET + the three ioctls). Protocol-legal to reuse: an
	 * exchange returns 0 only after the IO slot flipped back to
	 * host-owned with a zero status (the lab's own completion
	 * predicate), so the firmware has fully consumed the previous
	 * command. The legacy_buffers table stays for fw MALLOC replies,
	 * which the firmware may reference forever (held until reboot). */
	struct ane_legacy_buffer *cmd_buf;
};

/* Per-open BO ownership (drm_file->driver_priv). Handles live in the
 * fd's list; closing the fd drops its handles (postclose). The
 * coherent buffers themselves are only freed while no firmware is
 * staged — once a CPU may be running, every DMA surface is HELD until
 * reboot (the lab rule: the firmware never sees a freed address). */
struct ane_t6021_fd {
	struct list_head bos;
};

/* BOs are owned per open file (ane_t6021_fd); bo_lock guards the
/* Per-file handle counter and every per-fd list against same-fd concurrent
 * ioctls. Coherent buffers are held until reboot once firmware is
 * staged (the firmware never sees a freed address).
 *
 * Cap raised 2026-09-30: the 256 KiB Qwen-class matvec weights seen by the
 * H14 compiler are bounded by `reduction * columns * 2`, which reaches
 * 20 MiB at (K,N)=(2048,5120). The same cap serves the Qwen4-attention
 * (K,N)=(4096,4096) constant at 32 MiB and any H14 softmax/reduction
 * with an 8 MiB table. The cap is one BO; total-BO-bytes are capped
 * separately by an atomic counter under ane_t6021_bo_lock.
 *   ANE_T6021_BO_MAX       — per-BO size, the IOVA is at most 1 GiB.
 *   ANE_T6021_BO_TOTAL_MAX — total coherent BO bytes across all fds,
 *                            enforced at alloc and released at drop.
 * The 16 KiB alignment check is unchanged: every DMA site assumes it. */
#define ANE_T6021_BO_MAX		SZ_1G
#define ANE_T6021_BO_TOTAL_MAX		(2UL * SZ_1G)
#define ANE_T6021_BO_HASH_CHUNK		SZ_1M

struct ane_t6021_bo {
	struct list_head node;
	struct ane_t6021_fd *owner;
	u32 handle;
	void *cpu;			/* coherent mapping */
	dma_addr_t dma;
	size_t size;
};

static DEFINE_MUTEX(ane_t6021_bo_lock);
static u32 ane_t6021_next_handle = 1;
static atomic64_t ane_t6021_bo_total_bytes = ATOMIC64_INIT(0);

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

/* Time to let the output writes land after the completion signals.
 * Measured 2026-09-29 on some boots: the fw ack, the TD counter and the
 * TQ words all report done ~0.13 ms before the output reaches DRAM (the
 * output read as zeros in about 1 of 5 calls). No signal for "output
 * landed" is known, so the wait is a fixed margin of about 8x the lag. */
static unsigned int call_settle_us = 1000;
module_param(call_settle_us, uint, 0644);
MODULE_PARM_DESC(call_settle_us,
		 "Microseconds to wait after a CALL completes so its output lands (default 1000, 0 = none)");

/* Completion wait for one PROCEDURE_CALL. The firmware ack alone is not
 * completion: measured 2026-09-29, an ack can arrive before the engine
 * has written the output (exec returned after 0.16 ms, output landed
 * 0.12 ms later; the same call then read all zeros in ~1 of 5 runs after
 * an idle gap). All eight TQ status words read 0x81 before a TD starts,
 * so idle alone proves nothing either. The proof is: the last-committed
 * TD word (TM +0x20458, counts up by 0x10000 per completed TD)
 * moved off the value seen after the previous call, AND all eight TQ
 * words read idle.
 *
 * Every TM read is gated on the seven pmgr PS words reading 0x3ff (a TM
 * read while the compute domains are off hangs the SoC); while they do
 * not, the call has not started or finished and the loop keeps waiting.
 * Only single words are read. Returns 0 when complete, -ETIMEDOUT else. */
static int ane_rtclient_call_wait(struct ane_rtclient *ane,
				  unsigned int timeout_ms)
{
	void __iomem *tm = ioremap_np(ANE_TM_BASE + ANE_TM_TD_WINDOW,
				      ANE_TM_TD_WINDOW_SIZE);
	void __iomem *pm = ioremap_np(0x28e084000ull, 0x40);
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	int ret = -ETIMEDOUT;

	if (!tm || !pm) {
		ret = -ENOMEM;
		goto out;
	}
	while (time_before(jiffies, deadline)) {
		unsigned int i, q;
		u32 cnt;

		for (i = 0; i <= 0x30; i += 8)
			if ((readl(pm + i) & 0x3ff) != 0x3ff)
				break;
		if (i <= 0x30) {
			usleep_range(100, 200);
			continue;
		}
		cnt = readl(tm + ANE_TM_TD_COUNT_OFF);
		if (cnt != ane->td_seen) {
			for (q = 0; q < ANE_T6021_CHMAN_COUNT; q++)
				if (readl(tm + ANE_TM_TQ_STATUS_OFF -
					  ANE_TM_TD_WINDOW +
					  q * ANE_TM_TQ_STATUS_STRIDE) !=
				    ANE_TM_TQ_STATUS_IDLE)
					break;
			if (q == ANE_T6021_CHMAN_COUNT) {
				ane->td_seen = cnt;
				ret = 0;
				break;
			}
		}
		usleep_range(50, 100);
	}
out:
	if (tm)
		iounmap(tm);
	if (pm)
		iounmap(pm);
	return ret;
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
		dev_dbg(ane->dev, "T2H ch=%s slot=%u hdr=%016llx len=%#llx\n",
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

static int ane_rtclient_command(struct ane_rtclient *ane,
						     struct ane_legacy_buffer *command,
						     size_t length, u16 opcode,
						     unsigned int channel,
						     unsigned int timeout_ms)
{
	int ret;

	ret = ane_rtclient_legacy_exchange(ane, command, length, opcode,
					   channel, timeout_ms);
	if (ret) {
		dev_info(ane->dev, "EXCH op=%#x failed %d (fw allocs %u, %zu bytes)\n",
			 opcode, ret, ane->legacy_allocated, ane->legacy_bytes);
		atomic_set(&ane_t6021_quarantined, 1);
		return ret;
	}
	if (opcode == CSNE_CMD_PROCEDURE_CALL) {
		ret = ane_rtclient_call_wait(ane, timeout_ms);
		if (!ret && call_settle_us)
			usleep_range(call_settle_us, call_settle_us + 100);
		if (ret) {
			dev_info(ane->dev, "call completion wait failed %d\n",
				 ret);
			atomic_set(&ane_t6021_quarantined, 1);
			return ret;
		}
	}
	/* The fw talks back on the target-to-host rings (fwlog, perf);
	 * hand those slots back so the rings never fill (the sequencer
	 * did this per step; same ack, channels 4 and 6). */
	ane_rtclient_drain_t2h(ane, 4);
	ane_rtclient_drain_t2h(ane, 6);
	/* BOs are dma_alloc_coherent memory mapped write-combined for the
	 * CPU, so no cache maintenance is needed on either side. */
	return 0;
}

/* ---- LOAD / CREATE / CALL wrappers ---- */

/* The firmware never frees a program or a process, and its program table
 * holds 256 entries: measured 2026-09-29, the 257th LOAD_PROGRAM on one
 * boot answered with a protocol error and quarantined the device. Two
 * loads of byte-identical sections therefore share one firmware program
 * and one process. The key is SHA-256 over every section's id, size and
 * bytes, so a client can only reach a program whose bytes it also
 * supplied. Protected by ane_t6021_fw_lock. */
#define ANE_T6021_MAX_PROGRAMS 250

struct ane_t6021_prog {
	u8 digest[SHA256_DIGEST_SIZE];
	u32 prog_id;
	u32 proc_id;		/* U32_MAX until a process exists */
};

static struct ane_t6021_prog ane_t6021_progs[ANE_T6021_MAX_PROGRAMS];
static unsigned int ane_t6021_nprogs;

static struct ane_t6021_prog *ane_t6021_prog_find(const u8 *digest)
{
	unsigned int i;

	for (i = 0; i < ane_t6021_nprogs; i++)
		if (!memcmp(ane_t6021_progs[i].digest, digest,
			    SHA256_DIGEST_SIZE))
			return &ane_t6021_progs[i];
	return NULL;
}

static struct ane_t6021_prog *ane_t6021_prog_by_id(u32 prog_id)
{
	unsigned int i;

	for (i = 0; i < ane_t6021_nprogs; i++)
		if (ane_t6021_progs[i].prog_id == prog_id)
			return &ane_t6021_progs[i];
	return NULL;
}


/* Build a LOAD_PROGRAM (0x200) message in a kernel-owned buffer. The
 * section bytes live in the BOs the user supplied (section_ptr), the
 * generic binds (bufferId, bo_handle, size) patch the generic section
 * at iova +0x20 / +0x28 once the BO is copied. Returns 0 with
 * *prog_id set on success. */
static int ane_rtclient_load_program(struct ane_rtclient *ane,
				     struct drm_file *file,
				     const struct drm_ane_prog_load *user,
				     __u32 *prog_id)
{
	struct drm_ane_section *sections;
	struct drm_ane_generic_bind *binds;
	struct ane_legacy_buffer *command;
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_prog *cached;
	u8 digest[SHA256_DIGEST_SIZE];
	size_t binds_size;
	int ret, i, j;

	if (user->section_count < 1 ||
	    user->section_count > ANE_T6021_LOAD_SEC_COUNT ||
	    user->generic_count > ANE_M2_MAX_BINDS || !fd)
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

	/* Lab contract: the record slot IS the section identity (slot =
	 * id - 1; tdprop id 7 lands at slot 6 and slot 5 stays empty,
	 * exactly the proven add-path image). Reject out-of-range and
	 * duplicate ids before building the wire image. */
	for (i = 0; i < user->section_count; i++) {
		if (sections[i].id < 1 ||
		    sections[i].id > ANE_T6021_LOAD_SEC_COUNT) {
			ret = -EINVAL;
			goto out;
		}
		for (j = i + 1; j < user->section_count; j++) {
			if (sections[i].id == sections[j].id) {
				ret = -EINVAL;
				goto out;
			}
		}
	}

	/* Identical sections share one firmware program (see the table).
	 * A constant section can reach 20 MiB (the H14 (K,N)=(2048,5120)
	 * matvec), so feed the hash in ANE_T6021_BO_HASH_CHUNK-sized slices
	 * under bo_lock: a single sha256_update of a 20 MiB buffer would
	 * rely on a 20 MiB stack argument list and has no upper bound on
	 * the chunk that the BO could supply. */
	{
		struct sha256_ctx sha;
		void *scratch;

		sha256_init(&sha);
		scratch = kvmalloc(ANE_T6021_BO_HASH_CHUNK, GFP_KERNEL);
		if (!scratch) {
			ret = -ENOMEM;
			goto out;
		}
		for (i = 0; i < user->section_count; i++) {
			struct ane_t6021_bo *bo = NULL, *b;
			u64 hdr[2] = { sections[i].id, sections[i].size };
			size_t left;

			mutex_lock(&ane_t6021_bo_lock);
			list_for_each_entry(b, &fd->bos, node) {
				if (b->handle == sections[i].bo_handle) {
					bo = b;
					break;
				}
			}
			if (!bo || sections[i].size > bo->size ||
			    sections[i].offset > bo->size - sections[i].size) {
				mutex_unlock(&ane_t6021_bo_lock);
				kvfree(scratch);
				ret = -EINVAL;
				goto out;
			}
			sha256_update(&sha, (const u8 *)hdr, sizeof(hdr));
			left = sections[i].size;
			while (left) {
				size_t n = min(left, (size_t)ANE_T6021_BO_HASH_CHUNK);

				memcpy(scratch, (const u8 *)bo->cpu +
				       sections[i].offset +
				       (sections[i].size - left), n);
				sha256_update(&sha, scratch, n);
				left -= n;
			}
			mutex_unlock(&ane_t6021_bo_lock);
		}
		kvfree(scratch);
		sha256_final(&sha, digest);
	}
	cached = ane_t6021_prog_find(digest);
	if (cached) {
		*prog_id = cached->prog_id;
		ret = 0;
		goto out;
	}
	if (ane_t6021_nprogs == ANE_T6021_MAX_PROGRAMS) {
		ret = -ENOSPC;
		goto out;
	}

	command = ane->cmd_buf;
	if (!command) {
		ret = -ENODEV;
		goto out;
	}
	memset(command->cpu, 0, SZ_16K);
	/* One 0x30-byte section record per supplied section, placed at
	 * its IDENTITY slot: slot = id - 1 (tdprop id 7 lands at slot
	 * 6, slot 5 stays empty — exactly the proven add-path image:
	 * six populated records, ids 1,2,3,4,5 then 7). Unsupplied
	 * slots stay zero-filled (flags = 0). flags bit0 = 1, id at
	 * +0x04, iova at +0x18, size at +0x20. */
	for (i = 0; i < user->section_count; i++) {
		struct ane_t6021_bo *bo = NULL, *b;
		u64 slot_base;
		u8 *cmd = command->cpu;

		mutex_lock(&ane_t6021_bo_lock);
		list_for_each_entry(b, &fd->bos, node) {
			if (b->handle == sections[i].bo_handle) {
				bo = b;
				break;
			}
		}
		mutex_unlock(&ane_t6021_bo_lock);
		if (!bo || sections[i].size > bo->size ||
		    sections[i].offset > bo->size - sections[i].size) {
			ret = -EINVAL;
			goto out;
		}
		slot_base = 0x8 + (u64)(sections[i].id - 1) * 0x30;
		*(u32 *)(cmd + slot_base + 0x00) = cpu_to_le32(1);
		*(u32 *)(cmd + slot_base + 0x04) =
			cpu_to_le32(sections[i].id);
		*(u64 *)(cmd + slot_base + 0x18) =
			cpu_to_le64(bo->dma + sections[i].offset);
		*(u64 *)(cmd + slot_base + 0x20) =
			cpu_to_le64(sections[i].size);
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
	ret = ane_rtclient_command(ane, command,
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
		memcpy(ane_t6021_progs[ane_t6021_nprogs].digest, digest,
		       SHA256_DIGEST_SIZE);
		ane_t6021_progs[ane_t6021_nprogs].prog_id = *prog_id;
		ane_t6021_progs[ane_t6021_nprogs].proc_id = U32_MAX;
		ane_t6021_nprogs++;
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
	struct ane_t6021_prog *prog = ane_t6021_prog_by_id(prog_id);
	int ret;

	if (!prog)
		return -ENOENT;
	if (prog->proc_id != U32_MAX) {
		*proc_id = prog->proc_id;
		return 0;
	}
	command = ane->cmd_buf;
	if (!command)
		return -ENODEV;
	memset(command->cpu, 0, SZ_16K);
	{
		u8 *cmd = command->cpu;

		*(u32 *)(cmd + 0x08) = cpu_to_le32(prog_id);
		*(u32 *)(cmd + 0x0c) = cpu_to_le32(U32_MAX);
	}
	ret = ane_rtclient_command(ane, command, 0x10,
						       CSNE_CMD_CREATE_PROCESS,
						       1, 3000);
	if (!ret) {
		u8 *cmd = command->cpu;

		*proc_id = le32_to_cpu(*(u32 *)(cmd + 0x0c));
		if (*proc_id == U32_MAX)
			ret = -EPROTO;
		else
			prog->proc_id = *proc_id;
	}
	return ret;
}

static int ane_rtclient_procedure_call(struct ane_rtclient *ane,
				       struct drm_file *file,
				       const struct drm_ane_exec *user)
{
	struct drm_ane_exec_io *ios;
	struct ane_legacy_buffer *command;
	struct ane_t6021_fd *fd = file->driver_priv;
	size_t ios_size;
	size_t cmd_size;
	int ret, i;

	if (user->count < 1 || user->count > ANE_M2_MAX_BINDS ||
	    user->priority < 2 || user->priority > 7 || !fd)
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
	command = ane->cmd_buf;
	if (!command) {
		mutex_unlock(&ane_t6021_fw_lock);
		kfree(ios);
		return -ENODEV;
	}
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

			mutex_lock(&ane_t6021_bo_lock);
			list_for_each_entry(b, &fd->bos, node) {
				if (b->handle == ios[i].bo_handle) {
					bo = b;
					break;
				}
			}
			mutex_unlock(&ane_t6021_bo_lock);
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
		/* Drain the CPU write buffers so the input BOs the user
		 * filled through its uncached mapping are in DRAM before
		 * the fw starts reading them. */
		wmb();
		ret = ane_rtclient_command(ane, command,
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
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_bo *bo;
	struct ane_rtclient *ane;

	if (args->size == 0 || args->size > ANE_T6021_BO_MAX || !fd)
		return -EINVAL;
	/* Global coherent-memory accounting. Each BO is 16 KiB-aligned and
	 * stays mapped until reboot once firmware is staged, so the bound
	 * here is a hard cap on the firmware's visible DMA surface area. */
	if (atomic64_add_return(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes) >
	    ANE_T6021_BO_TOTAL_MAX) {
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
		return -ENOSPC;
	}
	ane = to_ane_t6021_drm(drm)->ane;
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo) {
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
		return -ENOMEM;
	}
	bo->cpu = dma_alloc_coherent(drm->dev, args->size, &bo->dma,
				     GFP_KERNEL);
	if (!bo->cpu) {
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
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
		atomic64_sub(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes);
		kfree(bo);
		return -ERANGE;
	}
	bo->size = args->size;
	bo->owner = fd;
	mutex_lock(&ane_t6021_bo_lock);
	bo->handle = ane_t6021_next_handle++;
	if (bo->handle == 0)
		bo->handle = ane_t6021_next_handle++;
	list_add_tail(&bo->node, &fd->bos);
	mutex_unlock(&ane_t6021_bo_lock);
	args->handle = bo->handle;
	/* mmap offset = the handle; libane mmaps the fd at exactly this
	 * offset and ane_t6021_mmap resolves the BO from vm_pgoff. */
	args->offset = (u64)bo->handle << PAGE_SHIFT;
	return 0;
}

/* Drop one handle owned by this fd. The coherent buffer is freed only
 * while no firmware is staged; once a CPU may be running, every DMA
 * surface is HELD until reboot (lab rule: the firmware never sees a
 * freed address; libane's IOVA-lifetime-v1 comment describes this).
 * The global bytes counter is debited only when the memory is really
 * freed; a HELD surface stays counted, so the 2 GiB cap bounds the
 * memory that short-lived contexts can pin until reboot. */
static void ane_t6021_bo_drop(struct drm_device *drm, struct ane_t6021_bo *bo)
{
	struct ane_rtclient *ane = to_ane_t6021_drm(drm)->ane;

	list_del(&bo->node);
	if (bo->cpu && !ane->fw) {
		atomic64_sub(PAGE_ALIGN(bo->size), &ane_t6021_bo_total_bytes);
		dma_free_coherent(drm->dev, bo->size, bo->cpu, bo->dma);
	}
	kfree(bo);
}

static int ane_t6021_bo_free_ioctl(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct drm_ane_bo_free *args = data;
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_bo *bo = NULL, *b;

	if (!fd)
		return -ENODEV;
	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry(b, &fd->bos, node) {
		if (b->handle == args->handle) {
			bo = b;
			break;
		}
	}
	if (bo)
		ane_t6021_bo_drop(drm, bo);
	mutex_unlock(&ane_t6021_bo_lock);
	return bo ? 0 : -ENOENT;
}

/* Resolve the BO a user mmap names (BO_INIT returned offset = handle
 * << PAGE_SHIFT) and map the coherent buffer cacheably — the device
 * half coheres through the DART (IOMMU_CACHE), so reads after EXEC
 * observe the firmware's writes without extra sync. */
static int ane_t6021_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *file = filp->private_data;
	struct ane_t6021_fd *fd = file->driver_priv;
	struct drm_device *drm = file->minor->dev;
	struct ane_t6021_bo *bo = NULL, *b;
	size_t size = vma->vm_end - vma->vm_start;
	u32 handle;
	int ret;

	if (!fd)
		return -ENODEV;
	/* vm_pgoff is the BO_INIT offset in pages: handle << PAGE_SHIFT
	 * >> PAGE_SHIFT == handle. */
	handle = (u32)vma->vm_pgoff;
	if (!handle)
		return -EINVAL;
	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry(b, &fd->bos, node) {
		if (b->handle == handle && b->owner == fd) {
			bo = b;
			break;
		}
	}
	if (!bo || size > PAGE_ALIGN(bo->size)) {
		mutex_unlock(&ane_t6021_bo_lock);
		return -ENOENT;
	}
	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(drm->dev, vma, bo->cpu, bo->dma, size);
	mutex_unlock(&ane_t6021_bo_lock);
	return ret;
}

static int ane_t6021_open(struct drm_device *drm, struct drm_file *file)
{
	struct ane_t6021_fd *fd;

	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;
	INIT_LIST_HEAD(&fd->bos);
	file->driver_priv = fd;
	return 0;
}

static void ane_t6021_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct ane_t6021_fd *fd = file->driver_priv;
	struct ane_t6021_bo *bo, *tmp;

	if (!fd)
		return;
	mutex_lock(&ane_t6021_bo_lock);
	list_for_each_entry_safe(bo, tmp, &fd->bos, node)
		ane_t6021_bo_drop(drm, bo);
	mutex_unlock(&ane_t6021_bo_lock);
	kfree(fd);
	file->driver_priv = NULL;
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
	ret = ane_rtclient_load_program(adrm->ane, file, args,
					&args->prog_id_out);
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

	return ane_rtclient_procedure_call(adrm->ane, file, args);
}

static const struct drm_ioctl_desc ane_t6021_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ANE_BO_INIT, ane_t6021_bo_init_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_BO_FREE, ane_t6021_bo_free_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_SUBMIT, ane_t6021_submit_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_PROG_LOAD, ane_t6021_prog_load_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_PROC_CREATE, ane_t6021_proc_create_ioctl, 0),
	DRM_IOCTL_DEF_DRV(ANE_EXEC, ane_t6021_exec_ioctl, 0),
};

/* Driver fops: the accel-core entry points plus our BO mmap (the core
 * default maps only GEM objects; this driver keeps its own BO table,
 * so .mmap resolves the handle from BO_INIT's returned offset). */
static const struct file_operations ane_t6021_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = accel_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = ane_t6021_mmap,
};

/* Version reported through DRM_IOCTL_VERSION: ABI 2 (T6021).
 * DRIVER_COMPUTE_ACCEL puts the node at /dev/accel/accelN. */
static const struct drm_driver ane_t6021_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_COMPUTE_ACCEL,
	.open = ane_t6021_open,
	.postclose = ane_t6021_postclose,
	.ioctls = ane_t6021_ioctls,
	.num_ioctls = ARRAY_SIZE(ane_t6021_ioctls),
	.fops = &ane_t6021_fops,
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
	ane->legacy_buffers = devm_kcalloc(dev, ANE_LEGACY_ALLOCS,
					   sizeof(*ane->legacy_buffers),
					   GFP_KERNEL);
	if (!ane->legacy_buffers)
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
		int cfg_err = 0;

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
		if (ane->chman_ok) {
			/* One reusable command buffer for the transport
			 * (CONFIG_GET + every ioctl); reuse is legal
			 * because each exchange completes with the slot
			 * host-owned again. The 128-entry table stays
			 * for fw MALLOC replies only. */
			if (ane_rtclient_legacy_alloc(ane, SZ_16K) == 0)
				ane->cmd_buf =
					&ane->legacy_buffers[ane->legacy_allocated - 1];
			else
				cfg_err = -ENOMEM;
		}
		if (!cfg_err && legacy_query && ane->chman_ok) {
			/* CONFIG_GET (opcode 0x03) keeps the legacy ChMan
			 * transport armed for the ioctls; the reply's
			 * word +0x08 must be nonzero (lab rule). The
			 * boot heap + 'IPC ' allocations stay HELD until
			 * reboot. */
			struct ane_legacy_buffer *command = ane->cmd_buf;
			int qret;

			if (command) {
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
				if (qret)
					cfg_err = qret;
				else if (!READ_ONCE(((u32 *)command->cpu)[2])) {
					dev_err(dev,
						"LEGACY CONFIG_GET reply word +0x08 zero\n");
					cfg_err = -EPROTO;
				}
			} else {
				cfg_err = -ENOMEM;
			}
		}
		ane->boot_done = true;
		if (cfg_err) {
			dev_err(dev,
				"install: CONFIG_GET failed (%d) — refusing to register DRM device (ioctls would run on an unproven ring)\n",
				cfg_err);
			if (!ane->held) {
				pm_runtime_put_sync_suspend(dev);
				pm_runtime_disable(dev);
			}
			return cfg_err;
		}
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
