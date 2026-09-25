// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __ANE_H__
#define __ANE_H__

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/pm_qos.h>
#include <linux/workqueue.h>

#include <drm/drm_device.h>
#include <drm/drm_mm.h>

/* Quoted so this tree's copy wins over any stray one in the kernel
 * headers; the kernel include path is searched before ccflags. */
#include "uapi/drm/ane_accel.h"

struct ane_device {
	struct drm_device drm;
	struct device *dev;

	struct device **pd_dev;
	struct device_link **pd_link;
	int pd_count;

	void __iomem *engine;

	/* The ane SET block (m1n1 ANE.ps_map): pmgr power-state words for
	 * this engine, mapped at probe on every SoC. Read-only, always:
	 * pmgr reads give recovery its ACTUAL evidence, while direct SET
	 * writes are firmware-locked and external-abort the SoC (T6001
	 * netconsole-named 2026-09-16; T8103 same mechanism). */
	phys_addr_t ps_base;
	void __iomem *ps;

	struct drm_mm mm;
	struct iommu_domain *domain;
	unsigned long shift;
	struct ane_dart {
		void __iomem *regs;
		int irq;
		u32 sid;
		bool masked;
	} darts[3];
	int dart_count;

	int irq;
	struct mutex iommu_lock;
	struct mutex engine_lock;
	struct list_head bo_list;

	/*
	 * Wedge-preserved BO ranges, tied to their still-inserted drm_mm
	 * nodes (see ane_reclaim_preserved). Registered under engine_lock.
	 */
	struct list_head preserved_list;
	bool removed;

	/*
	 * Set under engine_lock once a task manager timeout leaves the queue
	 * state unknown and DMA possibly still active. A wedged engine refuses
	 * new work while recovery (ane_tm_recover) power-cycles it; the
	 * preserve-until-reboot behaviour applies only when that reset fails.
	 */
	atomic_t wedged;

	/*
	 * Set under engine_lock for the duration of a recovery power cycle,
	 * so runtime suspend may gate an engine that the reset itself is
	 * quiescing.
	 */
	bool recovering;

	/*
	 * TM_STATUS sampled right after the first tm_enable at probe: the
	 * engine's fresh, idle-state signature. Recovery accepts a reset
	 * whose post-cycle status matches it, since the register reset
	 * value is not documented.
	 */
	u32 tm_status_fresh;
	bool tm_status_known;

	/*
	 * SoC descriptor fact: the set0/base islands hold the tm/tq
	 * register file in retention through any genpd power cycle
	 * (T6001 bisect evidence 2026-09-16: TQ_EN still reads 0x3000
	 * after a 2 s gate), so recovery must drain retained task-manager
	 * state itself instead of expecting a power-on reset to clear it.
	 * False on T8103, where the cycle is a full POR of the file.
	bool tm_retention;

	/*
	 * T8103 auto-clock-gate hack state, mirroring macOS
	 * AppleT8103PMGR::writeReg32 (mac13g 0x...9b84cd8): after ANE_SYS
	 * reaches state 0xf with ane-acg-hack set, macOS RMWs engine+0x1868a04
	 * to (old & ~0x1000) | 0x80001000; on power-down to 0 it clears
	 * bit 12. True once applied, so remove and recovery cycles restore
	 * the same endpoint state.
	 */
	bool acg_hack_applied;

	/*
	 * Engine-busy CPU cluster boost (ane_boost.c): min-frequency QoS on
	 * every cpufreq policy from the first submit until boost_idle_ms
	 * after the last one: on T8103 bandwidth-bound programs run ~2x
	 * slower while schedutil parks the idle clusters low.
	 */
	struct ane_boost {
		struct mutex lock;
		struct delayed_work off;
		struct freq_qos_request *legs;
		int nlegs;
		int held;
		unsigned long last_kick;
		bool on;
	} boost;
};

struct ane_request {
	int qid;
	u32 nid;
	u32 td_size;
	u32 td_count;
	u32 btsp_iova;
	u32 bar[ANE_TILE_COUNT];
};

/*
 * Drop the wedge pin (module refcount) without requiring a successful
 * recovery. Used by ane_drm_postclose so a wedged engine cannot leave
 * the module un-unloadable after a clean session close. Recovery stays
 * safe to call afterwards; if it returns error, the postclose path has
 * already unlocked the module count so the operator can swap the ko.
 */
void ane_wedge_clear(struct ane_device *ane);

/*
 * Reclaim wedge-preserved BO ranges (unmap, release nodes and pages).
 * Only legal once DMA is provably quiescent: after a successful
 * recovery power cycle, or at driver remove. Callers hold engine_lock.
 */
void ane_reclaim_preserved(struct ane_device *ane);

int ane_boost_init(struct ane_device *ane);
void ane_boost_exit(struct ane_device *ane);
void ane_boost_kick(struct ane_device *ane);

#define ANE_DART_MAX 3
#define ANE_DART_SCRATCH_MAX 16

struct ane_dart_scratch {
	u64 iova;
	struct page *page;
	struct drm_mm_node *node;
};

int ane_dart_init(struct ane_device *ane);
void ane_dart_mask(struct ane_device *ane);
void ane_dart_unmask(struct ane_device *ane);
bool ane_dart_faulted(struct ane_device *ane, u64 *iova, u32 *status);
int ane_dart_drain_fault(struct ane_device *ane, u64 fault_iova,
			 struct ane_dart_scratch *scratch, int *pages);
void ane_dart_release_scratch(struct ane_device *ane,
			      struct ane_dart_scratch *scratch, int pages);

#endif /* __ANE_H__ */
