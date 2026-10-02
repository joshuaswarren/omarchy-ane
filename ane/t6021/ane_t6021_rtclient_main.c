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
 *     command; completion = the legacy exchange's reply PLUS the
 *     firmware's finish event on the IO_T2H ring. EXEC also returns the
 *     fw's target-to-host slots (the sequencer's per-step drain).
 *
 * Compiled defaults are the load-run.sh parameter list — a bare
 * `insmod ane_t6021.ko` is the proven add-path configuration on
 * boot 3ab812a3 (fw_load=1 fw_start=1 fw_start_dapf=0 legacy_only=1
 * legacy_query=1 scratch3_ack=1 poll_rx=1 ...) — except hello_wait_ms,
 * 1000 in that list and 0 here (see its definition).
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
 *  - Firmware alias: the staged copy (own memory, default) or, with the
 *    lab option fw_alias_reserved=1, the iBoot-reserved SEG0/SEGi phys
 *    is aliased at the latched RVBAR entry; the subsequent kernel writes
 *    into the entry region translate through dart-ane0 instead of
 *    faulting.
 */

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kref.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/seq_file.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <linux/util_macros.h>
#include <linux/vmalloc.h>
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

/*
 * ane_stats sysfs attribute wrapper. The counters are fetched through
 * the real drvdata type (struct ane_rtclient *, counters at
 * ->fw->stats_ctrs) and formatted by the typed accessor
 * ane_stats_emit() (ane/include/ane_stats.h), which never sees the
 * device pointer. ane_timeline_fops lives in ane/ane_stats_show.c.
 */
static ssize_t ane_t6021_stats_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct ane_rtclient *ane = dev_get_drvdata(dev);

	return ane_stats_emit(buf, &ane->fw->stats_ctrs);
}
static DEVICE_ATTR_RO(ane_stats);

extern const struct file_operations ane_timeline_fops;

/* Doorbell (IPI) block, engine + 0x1844000: set +0, pending +0x8000,
 * ack +0xc000 (kext aneInterruptHandler reads +0x184c000 and writes
 * +0x1850000 with no SoC branch, receipts/2026-10-01-t8112-ane). */
#define ANE_IPI_OFF			0x1844000

/* CPU_STATUS bits (m1n1 ASCRegs shape) */
#define ANE_ASC_CPU_STATUS_RUNNING	BIT(0)
#define ANE_ASC_CPU_STATUS_STOPPED	BIT(1)

/* Boot-time module parameters — defaults are the proven add-path
 * parameter list (boot 3ab812a3 / load-run.sh), except fw_alias_reserved,
 * which defaults to own memory (receipts/2026-10-01-t602x-independent,
 * "Boot B"). fw_load, fw_extra_ram and fw_alias_reserved live in
 * ane_t6021_fwload.c and boot_prevent_nap in ane_t6021_boot.c (single
 * registration each). Remaining knobs are overridable from sysfs for
 * bisection only. */
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

/* 0 (default) skips RTKit in legacy mode, so the ANE mailbox never
 * starts. Measured 2026-09-30 (receipts/2026-09-30-t6021-stock-mailbox):
 * the 13.5 firmware sent no HELLO on any recorded boot (-ETIME after
 * 1000 ms), and starting the mailbox enables AIC2 884, a level line
 * that then fired ~700,000 times/s (one CPU of hardirq time). A
 * firmware that speaks RTKit needs hello_wait_ms=1000. */
static unsigned int hello_wait_ms;
module_param(hello_wait_ms, uint, 0444);
MODULE_PARM_DESC(hello_wait_ms,
		 "RTKit HELLO wait in legacy mode; 0 (default) skips RTKit and leaves the mailbox stopped. A firmware that speaks RTKit needs 1000.");

/*
 * Stats module parameter (ane_stats / ane_timeline producer side).
 * Default 1: producer enabled, sysfs/debugfs files exposed, counters
 * update on the hot path. stats=0 makes the hot path one predictable
 * branch and skips file creation entirely.
 *
 * Identical name and shape to ane.ko's stats parameter so a reader
 * always knows which knob the producer exposes, regardless of which
 * kernel-side driver happened to bind on this box.
 */
static bool stats = true;
module_param(stats, bool, 0444);
MODULE_PARM_DESC(stats,
		 "Enable ane_stats sysfs and ane_timeline debugfs (default 1; 0 = hot path is a single predictable branch and no files are created)");

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
	const struct ane_t602x_soc *soc;
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
 * coherent buffers themselves live as long as something references
 * them: a user mapping, or the firmware (see ane_t6021_bo_release). */
struct ane_t6021_fd {
	struct list_head bos;
};

/* Per-file handle counter and every per-fd list against same-fd concurrent
 * ioctls. A BO whose IOVA reached the firmware (fw_ref) never goes back to
 * the kernel: a program section is held until reboot, an io BO is parked
 * for reuse (see ane_t6021_bo_release). Every other BO frees at its last
 * reference.
 *
 * Per-BO cap raised 2026-09-30: the 256 KiB Qwen-class matvec weights seen
 * by the H14 compiler are bounded by `reduction * columns * 2`, which
 * reaches 20 MiB at (K,N)=(2048,5120). The same cap serves the
 * Qwen4-attention (K,N)=(4096,4096) constant at 32 MiB and any H14
 * softmax/reduction with an 8 MiB table. Total BO bytes across all fds are
 * capped separately (bo_total_max_mb) by an atomic counter, enforced at
 * alloc and released when the memory is really freed. Program sections stay
 * held until reboot, and the 38 Qwen programs alone hold about 2.6 GiB.
 * A cap of 0 refuses every BO_INIT with -ENOSPC; there is no unlimited
 * value. Every BO also needs IOVA below 4 GiB (32-bit DMA mask), so
 * allocations fail with -ENOMEM near that bound whatever the cap.
 * The 16 KiB alignment check is unchanged: every DMA site assumes it. */
#define ANE_T6021_BO_MAX		SZ_1G
#define ANE_T6021_BO_HASH_CHUNK		SZ_1M

struct ane_t6021_bo {
	struct list_head node;
	struct ane_t6021_fd *owner;
	u32 handle;
	struct kref refcount;		/* handle + user mappings */
	void *cpu;			/* coherent mapping */
	dma_addr_t dma;
	size_t size;
	bool fw_ref;			/* the firmware received this IOVA */
	bool fw_program;		/* a loaded program keeps this IOVA */
	struct device *dev;
};

static DEFINE_MUTEX(ane_t6021_bo_lock);
static u32 ane_t6021_next_handle = 1;
static atomic64_t ane_t6021_bo_total_bytes = ATOMIC64_INIT(0);

static unsigned int bo_total_max_mb = 12288;
module_param(bo_total_max_mb, uint, 0444);
MODULE_PARM_DESC(bo_total_max_mb,
		 "Cap on the BO bytes held at one time, in MiB (default 12288)");

static int ane_t6021_bo_total_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%lld\n",
			  (long long)atomic64_read(&ane_t6021_bo_total_bytes));
}

static int ane_t6021_bo_total_set(const char *val,
				  const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops ane_t6021_bo_total_ops = {
	.set = ane_t6021_bo_total_set,
	.get = ane_t6021_bo_total_get,
};
module_param_cb(bo_total_bytes, &ane_t6021_bo_total_ops, NULL, 0444);
MODULE_PARM_DESC(bo_total_bytes,
		 "Read only: the BO bytes counted against bo_total_max_mb now");

/* Mark the device quarantined: a timed-out command left the firmware
 * queue state unknown. The only safe next step is to refuse further
 * ioctls until a reboot reclaims the surfaces (wedged-pin rule). */
static atomic_t ane_t6021_quarantined = ATOMIC_INIT(0);

/* Parked io BOs: their last user is gone, the IOVA stays mapped and the
 * bytes stay counted. BO_INIT of the same page-aligned size takes one,
 * so held memory stays at the peak of concurrent io BOs instead of
 * growing with every process until the BO cap refuses BO_INIT. */
static LIST_HEAD(ane_t6021_bo_pool);
static DEFINE_SPINLOCK(ane_t6021_bo_pool_lock);

/* Final put: the last handle or mapping is gone. The firmware never sees
 * a freed IOVA (lab rule), so a fw_ref BO is never freed: a program
 * section stays held, because a cached firmware program keeps reading
 * it; an io BO goes to the pool, unless a quarantined firmware may still
 * write it. Every other BO frees here, so the BO cap bounds only the
 * memory the firmware may touch. */
static void ane_t6021_bo_release(struct kref *ref)
{
	struct ane_t6021_bo *bo = container_of(ref, struct ane_t6021_bo,
					       refcount);

	if (!bo->fw_ref) {
		atomic64_sub(PAGE_ALIGN(bo->size), &ane_t6021_bo_total_bytes);
		dma_free_coherent(bo->dev, bo->size, bo->cpu, bo->dma);
		kfree(bo);
	} else if (bo->fw_program || atomic_read(&ane_t6021_quarantined)) {
		kfree(bo);
	} else {
		spin_lock(&ane_t6021_bo_pool_lock);
		list_add(&bo->node, &ane_t6021_bo_pool);
		spin_unlock(&ane_t6021_bo_pool_lock);
	}
}

/* A parked io BO of SIZE's page-aligned size, or NULL. */
static struct ane_t6021_bo *ane_t6021_bo_pool_take(size_t size)
{
	struct ane_t6021_bo *bo;

	if (atomic_read(&ane_t6021_quarantined))
		return NULL;
	spin_lock(&ane_t6021_bo_pool_lock);
	list_for_each_entry(bo, &ane_t6021_bo_pool, node) {
		if (PAGE_ALIGN(bo->size) == PAGE_ALIGN(size)) {
			list_del(&bo->node);
			spin_unlock(&ane_t6021_bo_pool_lock);
			return bo;
		}
	}
	spin_unlock(&ane_t6021_bo_pool_lock);
	return NULL;
}

/* A user mapping holds its BO's memory until it is torn down: open
 * (fork, mremap split) takes a reference, close drops it. A BO the
 * user freed while mapped stays allocated until the last mapping
 * goes away. */
static void ane_t6021_vm_open(struct vm_area_struct *vma)
{
	kref_get(&((struct ane_t6021_bo *)vma->vm_private_data)->refcount);
}

static void ane_t6021_vm_close(struct vm_area_struct *vma)
{
	kref_put(&((struct ane_t6021_bo *)vma->vm_private_data)->refcount,
		 ane_t6021_bo_release);
}

static const struct vm_operations_struct ane_t6021_vm_ops = {
	.open = ane_t6021_vm_open,
	.close = ane_t6021_vm_close,
};

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
	void __iomem *ipi = ane->engine + ANE_IPI_OFF;
	unsigned int cursor = ane->legacy_malloc_cursor;
	unsigned long deadline;
	u32 *reply;
	int result = -ETIMEDOUT;

	if (!ane->held || !ane->chman_ok ||
	    ane_t6021_chman_check(a->boot_ipc, a->boot_ipc_iova))
		return -EPROTO;
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
	return result;
}

/* Time to let the output writes land after the completion signal.
 * Measured 2026-09-29 on some boots: the fw ack, the TD counter and the
 * TQ words all report done ~0.13 ms before the output reaches DRAM (the
 * output read as zeros in about 1 of 5 calls). The finish event arrives
 * at the same time for short programs. No signal for "output landed" is
 * known, so the wait is a fixed margin of about 8x the lag. */
static unsigned int call_settle_us = 1000;
module_param(call_settle_us, uint, 0644);
MODULE_PARM_DESC(call_settle_us,
		 "Microseconds to wait after a CALL completes so its output lands (default 1000, 0 = none)");

/* trace_td: a read-only timeline of each CALL for performance work. Off
 * by default; switch it at runtime with
 * /sys/module/ane_t6021/parameters/trace_td (no device needed). Off, the
 * CALL path is unchanged. On, the completion wait polls every 20-40 us
 * and also reads the last-committed-TD word. It reads that word only
 * while the seven ANE pmgr PS words read 0x3ff. No register is written.
 *
 * Provenance (omarchy-ane commits):
 * - The TD word: engine + ane_t602x_soc.trace_td_off; on T602x TM
 *   0x285c00000 + TD window 0x20400 + 0x58, the word the CALL wait polled
 *   from be2cf130d761ed675ad86f12b21ac1d53d906e1d until
 *   3a942d6cf6278526fbc02bf0c4743c5c1b276cdb. 3a942d6 measured its
 *   layout: the call's nid in bits 23:16 (+1 per call), the index of the
 *   last task taken in bits 15:0 (receipts/2026-09-30-t6021-call-wait).
 *   Not known on T8112, so trace_td records nothing there.
 * - The seven ANE power-state words at ane_t602x_soc.pmu_pa + ps_off
 *   (T602x: DT power-domains ane_sys_mpm 0x4000 .. ane_set4 0x4030 of
 *   the pmgr at 0x28e080000). The guard "PS words 0x3ff before any TM
 *   read; a TM read while the compute domains are off hangs the SoC" is
 *   ane_rtclient_pm_pwrstate_ok in 27e996a6de544a803a71d7a5c4ed11d828d4d049,
 *   and the CALL wait applied it until 3a942d6.
 *
 * Each record has a ktime_get_ns() stamp: CALL before the exchange, ACK
 * when the firmware acked it, TD for each new TD word value (word = the
 * value), EVENT for each IO_T2H event of the CALL (word = its state),
 * GATE when a PS word reads other than 0x3ff (word = its offset), DONE
 * when the wait ends (word = the TD samples taken). The buffer is
 * allocated at the first switch-on and kept until unload; each switch-on
 * empties it, and records past its end are counted in `dropped`. Read
 * it, while no CALL runs, from debugfs ane_t6021/trace_td (0400). All
 * trace state is protected by ane_t6021_fw_lock. */
#define ANE_TRACE_MAGIC		0x31445441	/* "ATD1" */
#define ANE_TRACE_RECS		(1U << 18)
#define ANE_PMGR_PS_LAST_OFF	0x30

enum {
	ANE_TR_CALL = 1,
	ANE_TR_ACK,
	ANE_TR_TD,
	ANE_TR_EVENT,
	ANE_TR_GATE,
	ANE_TR_DONE,
};

struct ane_t6021_trace_rec {
	u64 t_ns;
	u32 word;
	u16 kind;
	u16 call;
};

struct ane_t6021_trace {
	u32 magic;
	u32 rec_size;
	u32 capacity;
	u32 n;
	u32 dropped;
	u32 calls;
	u64 reserved;
	struct ane_t6021_trace_rec r[];
};

static bool trace_td;
static bool ane_t6021_tracing;	/* the running CALL is traced */
static struct ane_t6021_trace *ane_t6021_trace;
static struct debugfs_blob_wrapper ane_t6021_trace_blob;
static struct dentry *ane_t6021_trace_dir;

static void ane_t6021_trace_add(u16 kind, u32 word)
{
	struct ane_t6021_trace *t = ane_t6021_trace;

	if (t->n == t->capacity) {
		t->dropped++;
		return;
	}
	t->r[t->n++] = (struct ane_t6021_trace_rec){
		.t_ns = ktime_get_ns(), .word = word, .kind = kind,
		.call = t->calls,
	};
}

static int ane_t6021_trace_set(const char *val, const struct kernel_param *kp)
{
	size_t size = struct_size_t(struct ane_t6021_trace, r, ANE_TRACE_RECS);
	bool on;
	int ret;

	ret = kstrtobool(val, &on);
	if (ret)
		return ret;
	mutex_lock(&ane_t6021_fw_lock);
	if (on && !ane_t6021_trace) {
		ane_t6021_trace = vzalloc(size);
		if (!ane_t6021_trace) {
			ret = -ENOMEM;
			goto out;
		}
		ane_t6021_trace->magic = ANE_TRACE_MAGIC;
		ane_t6021_trace->rec_size = sizeof(struct ane_t6021_trace_rec);
		ane_t6021_trace->capacity = ANE_TRACE_RECS;
		ane_t6021_trace_blob.data = ane_t6021_trace;
		ane_t6021_trace_blob.size = size;
		ane_t6021_trace_dir = debugfs_create_dir("ane_t6021", NULL);
		debugfs_create_blob("trace_td", 0400, ane_t6021_trace_dir,
				    &ane_t6021_trace_blob);
	}
	if (on && !trace_td) {
		ane_t6021_trace->n = 0;
		ane_t6021_trace->dropped = 0;
		ane_t6021_trace->calls = 0;
	}
	trace_td = on;
out:
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

static const struct kernel_param_ops ane_t6021_trace_ops = {
	.set = ane_t6021_trace_set,
	.get = param_get_bool,
};
module_param_cb(trace_td, &ane_t6021_trace_ops, &trace_td, 0644);
MODULE_PARM_DESC(trace_td,
		 "Record a read-only per-CALL TD-word timeline in debugfs ane_t6021/trace_td (default 0; T602x only)");

static void ane_t6021_trace_free(void)
{
	debugfs_remove_recursive(ane_t6021_trace_dir);
	vfree(ane_t6021_trace);
}

/* The CALL cookie (CALL +0x20); the firmware returns it in the call's
 * IO_T2H events. */
#define ANE_CALL_COOKIE		0xADD0

/* IO_T2H event of a PROCEDURE_CALL, 0x28 bytes (measured 2026-09-30,
 * receipts/2026-09-30-t6021-call-wait): u32 sequence, u32 0x300, u64
 * cookie, u32 program id, u32 process id, u32 0, u32 state. The firmware
 * posts two per call: state 0 about 0.2-0.5 ms after the ack whatever the
 * program length (a 3,597-task program takes its last task 252 ms later;
 * receipts/2026-10-01-t6021-trace-td), and state 1 when the procedure has
 * finished. */
#define ANE_T2H_CALL_COOKIE_OFF		0x08
#define ANE_T2H_CALL_STATE_OFF		0x1c
#define ANE_T2H_CALL_FINISHED		1

/* The CPU address of LEN bytes at the firmware IOVA, or NULL. The firmware
 * places T2H payloads in memory the host gave it: a SHAREDMALLOC buffer or
 * the 'IPC ' surface. */
static const void *ane_rtclient_fw_cpu(struct ane_rtclient *ane, u64 iova,
				       size_t len)
{
	u32 i;

	for (i = 0; i < ane->legacy_allocated; i++) {
		const struct ane_legacy_buffer *b = &ane->legacy_buffers[i];

		if (iova >= b->dma && iova - b->dma + len <= b->size)
			return b->cpu + (iova - b->dma);
	}
	if (iova >= ane->fw->boot_ipc_iova &&
	    iova - ane->fw->boot_ipc_iova + len <= ane->fw->boot_ipc_size)
		return ane->fw->boot_ipc + (iova - ane->fw->boot_ipc_iova);
	return NULL;
}

/* Return every firmware-owned slot on a target-to-host ring to the fw
 * (legacy_t2h_ack=1, the lab default): the fw publishes by writing the
 * DMA address with bit0 clear; the host returns the slot by setting
 * bit0 and ringing the channel's doorbell bit, as the allocation ring
 * does. The sequencer drained channels 4/6 after every step; the
 * ioctls drain them after every completed exchange. Returns true when
 * an IO_T2H slot held the finish event of a CALL. */
static bool ane_rtclient_drain_t2h(struct ane_rtclient *ane,
				   unsigned int channel)
{
	const struct ane_t6021_chman_static *c = &ane_t6021_chman_layout[channel];
	unsigned int n = 0, slot_i = ane->legacy_cmd_cursor[channel];
	void __iomem *ipi = ane->engine + ANE_IPI_OFF;
	bool finished = false;

	if (!ane->fw || !ane->fw->boot_ipc)
		return false;
	while (n < c->size) {
		u64 *slot = ane->fw->boot_ipc + c->off + (size_t)slot_i * 64;
		u64 hdr = READ_ONCE(slot[0]), len = READ_ONCE(slot[1]);

		if (hdr & 1)
			break;
		dma_rmb();
		dev_dbg(ane->dev, "T2H ch=%s slot=%u hdr=%016llx len=%#llx\n",
			 c->name, slot_i, hdr, len);
		if (channel == 6 && len >= ANE_T2H_CALL_STATE_OFF + 4) {
			const u8 *ev = ane_rtclient_fw_cpu(ane, hdr,
							   ANE_T2H_CALL_STATE_OFF + 4);

			if (ev &&
			    get_unaligned_le64(ev + ANE_T2H_CALL_COOKIE_OFF) ==
			    ANE_CALL_COOKIE) {
				u32 state = get_unaligned_le32(ev +
							       ANE_T2H_CALL_STATE_OFF);

				if (ane_t6021_tracing)
					ane_t6021_trace_add(ANE_TR_EVENT, state);
				if (state == ANE_T2H_CALL_FINISHED)
					finished = true;
			}
		}
		n++;
		WRITE_ONCE(slot[0], hdr | 1);
		dma_wmb();
		writel(BIT(c->bit), ipi);
		slot_i = (slot_i + 1) % c->size;
		ane->legacy_cmd_cursor[channel] = slot_i;
	}
	return finished;
}

/* The completion wait with trace_td on: the same finish-event test with a
 * 20-40 us poll, and one TD-word sample per poll under the PS-word guard
 * (see trace_td). */
static int ane_rtclient_call_wait_traced(struct ane_rtclient *ane,
					 unsigned long deadline)
{
	void __iomem *td = ane->engine + ane->soc->trace_td_off;
	void __iomem *ps = ioremap_np(ane->soc->pmu_pa + ane->soc->ps_off,
				      ANE_PMGR_PS_LAST_OFF + 4);
	unsigned int i, gate = ANE_PMGR_PS_LAST_OFF + 8;
	u32 last = U32_MAX, samples = 0;
	int ret = -ETIMEDOUT;

	do {
		if (ane_rtclient_drain_t2h(ane, 6)) {
			ret = 0;
			break;
		}
		if (ps) {
			for (i = 0; i <= ANE_PMGR_PS_LAST_OFF; i += 8)
				if ((readl(ps + i) & 0x3ff) != 0x3ff)
					break;
			if (i > ANE_PMGR_PS_LAST_OFF) {
				u32 w = readl(td);

				samples++;
				if (w != last)
					ane_t6021_trace_add(ANE_TR_TD, w);
				last = w;
			} else if (i != gate) {
				ane_t6021_trace_add(ANE_TR_GATE, i);
			}
			gate = i;
		}
		usleep_range(20, 40);
	} while (time_before(jiffies, deadline));
	if (ret && ane_rtclient_drain_t2h(ane, 6))
		ret = 0;
	ane_t6021_trace_add(ANE_TR_DONE, samples);
	if (ps)
		iounmap(ps);
	return ret;
}

/* Completion wait for one PROCEDURE_CALL: the finish event on IO_T2H
 * (channel 6). The ack, the eight TQ status words and the last-committed
 * TD word (TM +0x20458: the call's nid in bits 23:16 and the index of the
 * last task taken in bits 15:0) all report done once the last task has
 * been dispatched, not when it has run: measured 2026-09-30, Qwen program
 * 20 (20 tasks) showed its last task index 0.22 ms after the ack, posted
 * its finish event 3.5 ms after the ack, and a caller that returned at
 * the first signal read an all-zero output. Returns 0 when the event
 * arrived, -ETIMEDOUT else. */
static int ane_rtclient_call_wait(struct ane_rtclient *ane,
				  unsigned int timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);

	if (ane_t6021_tracing)
		return ane_rtclient_call_wait_traced(ane, deadline);

	do {
		if (ane_rtclient_drain_t2h(ane, 6))
			return 0;
		usleep_range(50, 100);
	} while (time_before(jiffies, deadline));
	return ane_rtclient_drain_t2h(ane, 6) ? 0 : -ETIMEDOUT;
}

static int ane_rtclient_command(struct ane_rtclient *ane,
						     struct ane_legacy_buffer *command,
						     size_t length, u16 opcode,
						     unsigned int channel,
						     unsigned int timeout_ms)
{
	int ret;

	ane_t6021_tracing = opcode == CSNE_CMD_PROCEDURE_CALL && trace_td &&
			    ane->soc->trace_td_off;
	if (ane_t6021_tracing) {
		ane_t6021_trace->calls++;
		ane_t6021_trace_add(ANE_TR_CALL, 0);
	}
	/* Producer contract hot path (ane_stats.h): record submit at
	 * command enqueue, completion after the call returns, one
	 * begin/complete pair per engine submission. Only
	 * CSNE_CMD_PROCEDURE_CALL is engine work; the control-plane
	 * exchanges that ride this function (LOAD_PROGRAM,
	 * CREATE_PROCESS, CH_PROPERTY_WRITE, CONFIG_GET) are not
	 * counted, so jobs matches the engine calls the workload made.
	 * The T6021 path can run concurrently (MBI per-channel rings),
	 * so overlapping calls share a busy period and busy_ns is the
	 * union of the submit-to-completion windows. tmst is 0 (no
	 * host TM on T6021; documented in the file header line).
	 * tasks = 1 (one call per submission); rc = ret. */
	uint64_t stats_ticket = 0;
	uint64_t stats_submit_ns = 0;
	bool stats_call = stats && opcode == CSNE_CMD_PROCEDURE_CALL;

	if (stats_call) {
		stats_submit_ns = ktime_get_ns();
		stats_ticket = ane_stats_begin(&ane->fw->stats_ctrs,
					       &ane->fw->stats_ring,
					       stats_submit_ns, 1u);
	}
	ret = ane_rtclient_legacy_exchange(ane, command, length, opcode,
					   channel, timeout_ms);
	if (ret) {
		ane_t6021_tracing = false;
		if (stats_call)
			ane_stats_complete(&ane->fw->stats_ctrs,
					   &ane->fw->stats_ring, stats_ticket,
					   ktime_get_ns(),
					   (uint32_t)ret, 0ull);
		dev_info(ane->dev, "EXCH op=%#x failed %d (fw allocs %u, %zu bytes)\n",
			 opcode, ret, ane->legacy_allocated, ane->legacy_bytes);
		atomic_set(&ane_t6021_quarantined, 1);
		return ret;
	}
	if (opcode == CSNE_CMD_PROCEDURE_CALL) {
		if (ane_t6021_tracing)
			ane_t6021_trace_add(ANE_TR_ACK, 0);
		ret = ane_rtclient_call_wait(ane, timeout_ms);
		ane_t6021_tracing = false;
		if (!ret && call_settle_us)
			usleep_range(call_settle_us, call_settle_us + 100);
		if (ret) {
			if (stats_call)
				ane_stats_complete(&ane->fw->stats_ctrs,
						   &ane->fw->stats_ring,
						   stats_ticket,
						   ktime_get_ns(),
						   (uint32_t)ret, 0ull);
			dev_info(ane->dev, "call completion wait failed %d\n",
				 ret);
			atomic_set(&ane_t6021_quarantined, 1);
			return ret;
		}
	}
	if (stats_call)
		ane_stats_complete(&ane->fw->stats_ctrs,
				   &ane->fw->stats_ring, stats_ticket,
				   ktime_get_ns(), 0u, 0ull);
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
		u64 slot_base, iova;
		u8 *cmd = command->cpu;

		mutex_lock(&ane_t6021_bo_lock);
		list_for_each_entry(b, &fd->bos, node) {
			if (b->handle == sections[i].bo_handle) {
				bo = b;
				break;
			}
		}
		if (bo && sections[i].size <= bo->size &&
		    sections[i].offset <= bo->size - sections[i].size) {
			/* The IOVA below is about to be published to the
			 * firmware, so mark the BO before the exchange:
			 * the fw may read the section any time after the
			 * doorbell rings, including during a timeout.
			 * Under bo_lock the handle reference cannot go
			 * away, so no kref is needed here. */
			bo->fw_ref = true;
			bo->fw_program = true;
			iova = bo->dma + sections[i].offset;
		} else {
			bo = NULL;
		}
		mutex_unlock(&ane_t6021_bo_lock);
		if (!bo) {
			ret = -EINVAL;
			goto out;
		}
		slot_base = 0x8 + (u64)(sections[i].id - 1) * 0x30;
		*(u32 *)(cmd + slot_base + 0x00) = cpu_to_le32(1);
		*(u32 *)(cmd + slot_base + 0x04) =
			cpu_to_le32(sections[i].id);
		*(u64 *)(cmd + slot_base + 0x18) = cpu_to_le64(iova);
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
		*(u64 *)(cmd + 0x20) = cpu_to_le64(ANE_CALL_COOKIE);
		*(u32 *)(cmd + 0x28) = cpu_to_le32(user->count);
		for (i = 0; i < user->count; i++) {
			struct ane_t6021_bo *bo = NULL, *b;
			u64 slot_base = 0x60 + (u64)i * 0x30;
			u64 iova;

			mutex_lock(&ane_t6021_bo_lock);
			list_for_each_entry(b, &fd->bos, node) {
				if (b->handle == ios[i].bo_handle) {
					bo = b;
					break;
				}
			}
			if (!bo || ios[i].size > bo->size) {
				mutex_unlock(&ane_t6021_bo_lock);
				ret = -EINVAL;
				goto unlock;
			}
			/* The IOVA below is about to be published to the
			 * firmware, so mark the BO before the exchange.
			 * Under bo_lock the handle reference cannot go
			 * away, so no kref is needed here. */
			bo->fw_ref = true;
			iova = bo->dma;
			mutex_unlock(&ane_t6021_bo_lock);
			*(u32 *)(cmd + slot_base + 0x00) = cpu_to_le32(1);
			*(u32 *)(cmd + slot_base + 0x04) =
				cpu_to_le32(ios[i].buffer_id);
			*(u32 *)(cmd + slot_base + 0x08) =
				cpu_to_le32(ios[i].type);
			*(u64 *)(cmd + slot_base + 0x18) =
				cpu_to_le64(iova);
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
	/* A parked io BO is already mapped and counted; its old contents
	 * belong to another process, so it is zeroed like a new one. */
	bo = ane_t6021_bo_pool_take(args->size);
	if (bo) {
		memset(bo->cpu, 0, PAGE_ALIGN(args->size));
		goto publish;
	}
	/* Global coherent-memory accounting. Each BO is 16 KiB-aligned;
	 * a BO whose IOVA reaches the firmware is never freed (held or
	 * pooled), so this bound caps the memory that outlives its
	 * users. */
	if (atomic64_add_return(PAGE_ALIGN(args->size), &ane_t6021_bo_total_bytes) >
	    (s64)bo_total_max_mb << 20) {
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
publish:
	bo->size = args->size;
	bo->owner = fd;
	bo->dev = drm->dev;
	kref_init(&bo->refcount); /* the handle holds this reference */
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

/* Drop one handle owned by this fd. The kref may keep the memory
 * alive past this call — a user mapping still holds a reference — so
 * the final ane_t6021_bo_release makes the free-or-hold decision. */
static void ane_t6021_bo_drop(struct ane_t6021_bo *bo)
{
	list_del(&bo->node);
	kref_put(&bo->refcount, ane_t6021_bo_release);
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
		ane_t6021_bo_drop(bo);
	mutex_unlock(&ane_t6021_bo_lock);
	return bo ? 0 : -ENOENT;
}

/* Resolve the BO a user mmap names (BO_INIT returned offset = handle
 * << PAGE_SHIFT) and map the coherent buffer cacheably — the device
 * half coheres through the DART (IOMMU_CACHE), so reads after EXEC
 * observe the firmware's writes without extra sync. The mapping takes
 * one BO reference: it survives BO_FREE until the vma is gone. */
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
			kref_get(&b->refcount);
			bo = b;
			break;
		}
	}
	if (!bo || size > PAGE_ALIGN(bo->size)) {
		mutex_unlock(&ane_t6021_bo_lock);
		if (bo)
			kref_put(&bo->refcount, ane_t6021_bo_release);
		return -ENOENT;
	}
	vma->vm_pgoff = 0;
	ret = dma_mmap_coherent(drm->dev, vma, bo->cpu, bo->dma, size);
	mutex_unlock(&ane_t6021_bo_lock);
	if (ret) {
		kref_put(&bo->refcount, ane_t6021_bo_release);
		return ret;
	}
	/* The vma owns the lookup reference: close (and open on fork)
	 * go through ane_t6021_vm_ops. */
	vma->vm_private_data = bo;
	vma->vm_ops = &ane_t6021_vm_ops;
	return 0;
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
		ane_t6021_bo_drop(bo);
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

/* Firmware perf mode: CSNE_CMD_CH_PROPERTY_WRITE (0x1f), channel 0,
 * property 0x10aa, value 1. macOS sends it once during power-on (H13
 * kext evidence in the omarchy-ane perf-mode receipt); the selene 13.5
 * fw routes it to CAneEngineExeLoop::setPerfMode. It is a runtime
 * switch (write 1 to /sys/module/ane_t6021/parameters/fw_perf_mode) so
 * its effect on call time can be measured on one boot. Only 1 is
 * accepted: no other value is known to be safe. */
static struct ane_rtclient *ane_t6021_perf_ane;
static bool fw_perf_mode;

static int ane_t6021_perf_mode_set(const char *val,
				   const struct kernel_param *kp)
{
	struct ane_rtclient *ane = READ_ONCE(ane_t6021_perf_ane);
	struct ane_legacy_buffer *command;
	bool on;
	int ret;

	ret = kstrtobool(val, &on);
	if (ret)
		return ret;
	if (!on || fw_perf_mode)
		return on ? 0 : -EINVAL;
	if (!ane)
		return -ENODEV;
	mutex_lock(&ane_t6021_fw_lock);
	if (atomic_read(&ane_t6021_quarantined)) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ETIMEDOUT;
	}
	command = ane->cmd_buf;
	if (!command) {
		mutex_unlock(&ane_t6021_fw_lock);
		return -ENODEV;
	}
	memset(command->cpu, 0, SZ_16K);
	*(u32 *)((u8 *)command->cpu + 0x08) = cpu_to_le32(0);
	*(u32 *)((u8 *)command->cpu + 0x0c) = cpu_to_le32(0x10aa);
	*(u32 *)((u8 *)command->cpu + 0x10) = cpu_to_le32(1);
	ret = ane_rtclient_command(ane, command, 0x14, 0x001f, 1, 3000);
	if (!ret) {
		fw_perf_mode = true;
		dev_info(ane->dev, "fw perf mode set (property 0x10aa = 1)\n");
	}
	mutex_unlock(&ane_t6021_fw_lock);
	return ret;
}

static const struct kernel_param_ops ane_t6021_perf_mode_ops = {
	.set = ane_t6021_perf_mode_set,
	.get = param_get_bool,
};
module_param_cb(fw_perf_mode, &ane_t6021_perf_mode_ops, &fw_perf_mode, 0644);
MODULE_PARM_DESC(fw_perf_mode,
		 "Write 1 once to send CH_PROPERTY_WRITE 0x10aa = 1 (fw perf mode); reads back whether it was sent");

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

	if (!ane_t6021_fwload_placement_ok(dev)) {
		dev_err(dev,
			"fw_alias_reserved=1, but no no-map /reserved-memory node covers the iBoot firmware windows (this m1n1 does not reserve them); refusing before power access, ANE off\n");
		return -ENODEV;
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
	ane->soc = of_device_get_match_data(dev);
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
	ps_cpu = readl(ane->pmgr + ane->soc->ps_cpu_off);
	dev_emerg(dev, "ane_cpu ACTUAL = 0x%x\n", ps_cpu);
	if (FIELD_GET(ANE_PS_ACTUAL, ps_cpu) != ANE_PS_ON) {
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		return -EPROBE_DEFER;
	}
	/* T8112: the kext (type 0x70) opens PWGATE (bits 29:28 = 0) before
	 * the ps words. This driver only reads it: an engine read behind a
	 * closed gate is untested, so it refuses first. */
	if (ane->soc->pwgate_off) {
		void __iomem *set = devm_of_iomap(dev, dev->of_node, 2, NULL);
		u32 gate = IS_ERR(set) ? U32_MAX :
			   readl(set + ane->soc->pwgate_off);

		dev_emerg(dev, "PWGATE = 0x%x\n", gate);
		if (gate & GENMASK(29, 28)) {
			pm_runtime_put_sync_suspend(dev);
			pm_runtime_disable(dev);
			return dev_err_probe(dev, -ENODEV,
					     "PWGATE closed; refusing before any engine read\n");
		}
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
			/* hello_wait_ms > 0 only: init rtkit and arm the RX
			 * poll worker BEFORE writing the ack, so a HELLO
			 * after the ack is not missed. The 13.5 fw sent none
			 * on any recorded boot. */
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
		cancel_delayed_work_sync(&ane->poll_work);
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
		WRITE_ONCE(ane_t6021_perf_ane, ane);
		drmret = drm_dev_register(&adrm->drm, 0);
		if (drmret) {
			dev_err_probe(dev, drmret, "drm_dev_register\n");
			ret = drmret;
			goto err_pm_or_hold;
		}
		dev_info(dev,
			 "loaded ane_t6021 %s (DRM major %d minor %d; ABI 2; legacy_only=%u chman_ok=%u booted=%u; state %s; BO cap %u MiB)\n",
			 ANE_T6021_MODULE_VERSION, ANE_ABI_M2_MAJOR, 0,
			 legacy_only, ane->chman_ok,
			 ane->fw ? ane->fw->booted : 0,
			 ane->held ? "HELD" : "ready", bo_total_max_mb);
	}

	if (ane->fw && stats) {
		ane->fw->stats_ring_n = 1u << ANE_STATS_RING_ORDER_DEFAULT;
		ane->fw->stats_slots = kcalloc(ane->fw->stats_ring_n,
					       sizeof(*ane->fw->stats_slots),
					       GFP_KERNEL);
		if (ane->fw->stats_slots) {
			struct dentry *root;

			ane_stats_counters_init(&ane->fw->stats_ctrs,
					       &ane->fw->stats_ring,
					       ANE_STATS_RING_ORDER_DEFAULT);
			/* counters_init zeroes the ring including the
			 * slots pointer; attach the preallocated array
			 * after it (the order ane_drv.c uses). */
			ane->fw->stats_ring.slots = ane->fw->stats_slots;
			/* Sysfs ane_stats: per-device file in the
			 * module's existing sysfs group (the same group
			 * that exposes wedged/reset today). */
			ret = device_create_file(dev, &dev_attr_ane_stats);
			if (ret)
				dev_warn(dev,
					 "ane_stats sysfs create failed %d\n",
					 ret);
			root = debugfs_create_dir("ane_t6021", NULL);
			if (!IS_ERR(root)) {
				ane->fw->stats_debugfs = root;
				debugfs_create_file("ane_timeline", 0444,
						    root,
						    &ane->fw->stats_ring,
						    &ane_timeline_fops);
			}
		}
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
	if (READ_ONCE(ane_t6021_perf_ane) == ane)
		WRITE_ONCE(ane_t6021_perf_ane, NULL);

	cancel_delayed_work_sync(&ane->poll_work);

	if (ane->held)
		dev_warn(&pdev->dev,
			 "remove HELD: no teardown — reboot reclaims\n");
}

static const struct of_device_id ane_rtclient_of_match[] = {
	{ .compatible = "apple,t6020-ane", .data = &ane_t6020_soc },
	{ .compatible = "apple,t6021-ane", .data = &ane_t6021_soc },
	{ .compatible = "apple,t6022-ane", .data = &ane_t6022_soc },
	{ .compatible = "apple,t8112-ane", .data = &ane_t8112_soc },
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

static int __init ane_rtclient_init(void)
{
	int ret = platform_driver_register(&ane_rtclient_driver);

	if (ret)
		ane_t6021_trace_free();
	return ret;
}
module_init(ane_rtclient_init);

static void __exit ane_rtclient_exit(void)
{
	platform_driver_unregister(&ane_rtclient_driver);
	ane_t6021_trace_free();
}
module_exit(ane_rtclient_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION(ANE_T6021_MODULE_VERSION);
MODULE_DESCRIPTION("Apple Neural Engine (T6021/M2) installed module");
