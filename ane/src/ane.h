// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __ANE_H__
#define __ANE_H__

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mutex.h>

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

	int irq;
	struct mutex iommu_lock;
	struct mutex engine_lock;
	struct list_head bo_list;
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
};

struct ane_request {
	int qid;
	u32 nid;
	u32 td_size;
	u32 td_count;
	u32 btsp_iova;
	u32 bar[ANE_TILE_COUNT];
};

#endif /* __ANE_H__ */
