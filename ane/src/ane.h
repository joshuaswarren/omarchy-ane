// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#ifndef __ANE_H__
#define __ANE_H__

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include <drm/drm_device.h>
#include <drm/drm_mm.h>

#include <uapi/drm/ane_accel.h>

struct ane_device {
	struct drm_device drm;
	struct device *dev;

	struct device **pd_dev;
	struct device_link **pd_link;
	int pd_count;

	void __iomem *engine;

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
	 * state unknown and DMA possibly still active. No documented abort/reset
	 * exists, so a wedged engine refuses new work and resource reclamation
	 * until reboot.
	 */
	atomic_t wedged;
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
