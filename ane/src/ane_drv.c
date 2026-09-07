// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#include <linux/atomic.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>

#include "ane.h"
#include "ane_tm.h"

#define CMD_BUF_BDX 0
#define KRN_BUF_BDX 1

struct ane_bo {
	struct drm_gem_object base;
	struct list_head node;
	struct drm_mm_node *mm;
	u32 npages;
	struct page **pages;
	dma_addr_t iova;
};

#define to_bo(gem) (container_of(gem, struct ane_bo, base))

static struct ane_bo *bo_lookup(struct drm_file *file, u32 handle)
{
	struct drm_gem_object *gem = drm_gem_object_lookup(file, handle);
	if (!gem)
		return NULL;
	return to_bo(gem);
}

/*
 * Buffers are mapped and unmapped only through the kernel-owned IOMMU
 * domain the device was attached to by its providers. apple-dart
 * programs every DART in the device's "iommus" list (TTBRs, stream
 * setup, invalidation and fault IRQs are all provider-owned);
 * iommu_unmap() flushes through the provider before returning, so pages
 * may be reclaimed once it succeeds.
 */
static int ane_iommu_map_pages(struct ane_device *ane, struct ane_bo *bo)
{
	int err;

	lockdep_assert_held(&ane->engine_lock);
	if (bo->mm)
		return -EBUSY;

	bo->mm = kzalloc(sizeof(*bo->mm), GFP_KERNEL);
	if (!bo->mm)
		return -ENOMEM;

	mutex_lock(&ane->iommu_lock);

	/* reserve area from ANE address space */
	err = drm_mm_insert_node_generic(&ane->mm, bo->mm,
					 bo->npages << ane->shift,
					 1UL << ane->shift, 0, 0);
	if (err < 0) {
		dev_err(ane->dev, "out of ANE space: %d\n", err);
		goto unlock;
	}

	bo->iova = bo->mm->start;

	/* map into ANE address space */
	for (u32 i = 0; i < bo->npages; i++) {
		dma_addr_t iova = bo->iova + (i << ane->shift);
		err = iommu_map(ane->domain, iova, page_to_phys(bo->pages[i]),
				1UL << ane->shift, IOMMU_READ | IOMMU_WRITE,
				GFP_KERNEL);
		if (err < 0) {
			dev_err(ane->dev, "iommu_map failed at 0x%llx", iova);
			while (i-- > 0) {
				iommu_unmap(ane->domain,
					    bo->iova + (i << ane->shift),
					    1UL << ane->shift);
			}
			drm_mm_remove_node(bo->mm);
			bo->iova = 0;
			break;
		}
	}

	mutex_unlock(&ane->iommu_lock);

	if (err < 0) {
		kfree(bo->mm);
		bo->mm = NULL;
		return err;
	}

	return 0;

unlock:
	mutex_unlock(&ane->iommu_lock);
	kfree(bo->mm);
	bo->mm = NULL;
	return err;
}

static void ane_iommu_unmap_pages(struct ane_device *ane, struct ane_bo *bo)
{
	struct drm_mm_node *mm = bo->mm;

	lockdep_assert_held(&ane->engine_lock);

	if (!mm)
		return;

	list_del_init(&bo->node);
	mutex_lock(&ane->iommu_lock);
	for (u32 i = 0; i < bo->npages; i++) {
		dma_addr_t iova = bo->iova + (i << ane->shift);
		iommu_unmap(ane->domain, iova, 1UL << ane->shift);
	}
	drm_mm_remove_node(mm);
	bo->mm = NULL;
	bo->iova = 0;
	mutex_unlock(&ane->iommu_lock);

	kfree(mm);
}

static vm_fault_t ane_gem_vm_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *gem = vma->vm_private_data;
	struct ane_bo *bo = to_bo(gem);
	struct page *page;
	pgoff_t offset;

	if (!bo->pages)
		return VM_FAULT_SIGBUS;

	offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
	if (offset >= bo->npages)
		return VM_FAULT_SIGBUS;
	page = bo->pages[offset];

	return vmf_insert_page(vma, vmf->address, page);
}

static const struct vm_operations_struct drm_gem_ane_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
	.fault = ane_gem_vm_fault,
};

/*
 * Sole destruction path for buffer objects: every reference (handles,
 * mmap vmas, lookups) funnels here. Nothing is reclaimed while the
 * engine is wedged, since DMA may still be reading the IOVA.
 */
static void ane_gem_free_object(struct drm_gem_object *gem)
{
	struct ane_device *ane = gem->dev->dev_private;
	struct ane_bo *bo = to_bo(gem);

	mutex_lock(&ane->engine_lock);
	if (bo->mm) {
		if (atomic_read(&ane->wedged)) {
			/* Fail closed: leak the mapping and the pages so no
			 * IOVA is torn down or page reused underneath active
			 * DMA. Reboot reclaims them. */
			dev_err(ane->dev, "wedged: preserving bo mapping\n");
			list_del_init(&bo->node);
			bo->mm = NULL;
			bo->pages = NULL;
		} else {
			ane_iommu_unmap_pages(ane, bo);
		}
	}
	mutex_unlock(&ane->engine_lock);

	if (bo->pages) {
		drm_gem_put_pages(gem, bo->pages, true, true);
		bo->pages = NULL;
	}
	drm_gem_object_release(gem);
	kfree(bo);
}

static const struct drm_gem_object_funcs ane_gem_object_funcs = {
	.free = ane_gem_free_object,
	.vm_ops = &drm_gem_ane_vm_ops,
};

static int ane_bo_init(struct drm_device *drm, void *data,
		       struct drm_file *file)
{
	struct ane_device *ane = drm->dev_private;
	struct drm_ane_bo_init *args = data;
	struct drm_gem_object *gem;
	struct ane_bo *bo;
	int err;

	if (args->pad || !args->size)
		return -EINVAL;

	bo = kzalloc(sizeof(struct ane_bo), GFP_KERNEL);
	if (!bo)
		return -ENOMEM;
	INIT_LIST_HEAD(&bo->node);

	gem = &bo->base;
	gem->funcs = &ane_gem_object_funcs;
	err = drm_gem_object_init(drm, gem, round_up(args->size, PAGE_SIZE));
	if (err < 0) {
		drm_gem_private_object_fini(gem);
		goto free;
	}

	err = drm_gem_create_mmap_offset(gem);
	if (err < 0)
		goto put;

	args->offset = drm_vma_node_offset_addr(&gem->vma_node);

	bo->npages = gem->size >> PAGE_SHIFT;
	bo->pages = drm_gem_get_pages(gem);
	if (IS_ERR(bo->pages)) {
		err = PTR_ERR(bo->pages);
		bo->pages = NULL;
		goto put;
	}

	mutex_lock(&ane->engine_lock);
	if (ane->removed) {
		err = -ENODEV;
		goto unlock_put;
	}
	if (atomic_read(&ane->wedged)) {
		dev_err_ratelimited(ane->dev, "wedged: refusing bo init\n");
		err = -ENODEV;
		goto unlock_put;
	}

	err = ane_iommu_map_pages(ane, bo);
	if (err < 0)
		goto unlock_put;
	list_add_tail(&bo->node, &ane->bo_list);

	err = drm_gem_handle_create(file, gem, &args->handle);
	mutex_unlock(&ane->engine_lock);
	drm_gem_object_put(gem); /* handle (or error path) owns it now */
	return err;

unlock_put:
	mutex_unlock(&ane->engine_lock);
put:
	drm_gem_object_put(gem); /* routes through ane_gem_free_object() */
	return err;
free:
	kfree(bo);
	return err;
}

static int ane_bo_free(struct drm_device *drm, void *data,
		       struct drm_file *file)
{
	struct ane_device *ane = drm->dev_private;
	struct drm_ane_bo_free *args = data;
	struct ane_bo *bo;

	if (args->pad)
		return -EINVAL;

	mutex_lock(&ane->engine_lock);
	if (ane->removed) {
		mutex_unlock(&ane->engine_lock);
		return -ENODEV;
	}
	if (atomic_read(&ane->wedged)) {
		dev_err_ratelimited(ane->dev, "wedged: refusing bo free\n");
		mutex_unlock(&ane->engine_lock);
		return -ENODEV;
	}

	bo = bo_lookup(file, args->handle);
	mutex_unlock(&ane->engine_lock);
	if (!bo)
		return -EINVAL;

	/* Keep reference drops outside engine_lock: the final put enters
	 * ane_gem_free_object(), which owns that lock while unmapping. */
	drm_gem_handle_delete(file, args->handle);
	drm_gem_object_put(&bo->base);
	return 0;
}

static int ane_submit(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct ane_device *ane = drm->dev_private;
	struct drm_ane_submit *args = data;
	struct drm_gem_object *gem[ANE_TILE_COUNT] = { 0 };
	struct drm_gem_object *btsp = NULL;
	struct ane_bo *bo;
	struct ane_request req;
	int err;

	memset(&req, 0, sizeof(req));

	if (args->pad || !args->tsk_size || !args->td_count ||
	    args->td_count > 0xffff || args->td_size < 4 ||
	    args->td_size > 0x40000 || (args->td_size & 3) ||
	    !args->handles[CMD_BUF_BDX] || args->handles[KRN_BUF_BDX] ||
	    !args->btsp_handle) {
		return -EINVAL;
	}

	/* Fail closed: never queue work behind a wedged engine. */
	if (atomic_read(&ane->wedged)) {
		dev_err_ratelimited(ane->dev, "wedged: refusing submit\n");
		return -ECANCELED;
	}

	req.qid = 4;
	req.nid = ANE_FIFO_NID;
	req.td_size = args->td_size;
	req.td_count = args->td_count;

	for (int bdx = 0; bdx < ANE_TILE_COUNT; bdx++) {
		if (!args->handles[bdx])
			continue;
		bo = bo_lookup(file, args->handles[bdx]);
		if (!bo) {
			err = -EINVAL;
			goto put;
		}
		gem[bdx] = &bo->base;
		if (!bo->iova ||
		    ((bdx == CMD_BUF_BDX) &&
		     (args->tsk_size >= (bo->npages << ane->shift)))) {
			err = -EINVAL;
			goto put;
		}
		req.bar[bdx] = lower_32_bits(bo->iova);
	}

	/*
	 * The microcode and weights are packed @ 16 gran for bank aligned
	 * access. Since this isn't page aligned, we represent the two as one
	 * buffer and calculate the delimiter (where the weights would start).
	 */
	req.bar[KRN_BUF_BDX] =
		req.bar[CMD_BUF_BDX] + round_up(args->tsk_size, ANE_CMD_GRAN);

	bo = bo_lookup(file, args->btsp_handle);
	if (!bo) {
		err = -EINVAL;
		goto put;
	}
	btsp = &bo->base;
	if (!bo->iova || args->td_size > bo->base.size) {
		err = -EINVAL;
		goto put;
	}
	req.btsp_iova = lower_32_bits(bo->iova);

	/*
	 * Lookup references are held across the synchronous execute and
	 * dropped only after ane_tm_execute() confirmed completion, so a
	 * concurrent BO_FREE cannot unmap a buffer the engine reads.
	 */
	mutex_lock(&ane->engine_lock);
	if (ane->removed) {
		err = -ENODEV;
		goto unlock;
	}
	if (atomic_read(&ane->wedged)) {
		dev_err_ratelimited(ane->dev, "wedged: refusing submit\n");
		err = -ECANCELED;
		goto unlock;
	}

	err = ane_tm_enqueue(ane, &req);
	if (err < 0)
		goto unlock;

	err = ane_tm_execute(ane, &req);

unlock:
	mutex_unlock(&ane->engine_lock);
put:
	drm_gem_object_put(btsp);
	for (int bdx = 0; bdx < ANE_TILE_COUNT; bdx++) {
		if (gem[bdx])
			drm_gem_object_put(gem[bdx]);
	}
	return err;
}

static const struct drm_ioctl_desc ane_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(ANE_BO_INIT, ane_bo_init, 0),
	DRM_IOCTL_DEF_DRV(ANE_BO_FREE, ane_bo_free, 0),
	DRM_IOCTL_DEF_DRV(ANE_SUBMIT, ane_submit, 0),
};

static int ane_drm_open(struct drm_device *drm, struct drm_file *file)
{
	struct ane_device *ane = drm->dev_private;
	int err;

	/* Bring up power while the file context is created. A failed
	 * resume propagates; nothing is released that was not acquired. */
	err = pm_runtime_resume_and_get(ane->dev);
	if (err < 0)
		return err;

	pm_runtime_put(ane->dev);
	return 0;
}

static void ane_drm_postclose(struct drm_device *drm, struct drm_file *file)
{
	/* GEM release runs before postclose. Normal remove pre-unmaps every
	 * live BO before supplier detach; wedged cleanup preserves mappings. */
}

static long ane_drm_unlocked_ioctl(struct file *file, unsigned int cmd,
				   unsigned long arg)
{
	struct drm_file *filp = file->private_data;
	struct drm_device *drm = filp->minor->dev;
	struct ane_device *ane = drm->dev_private;
	long err;

	err = pm_runtime_resume_and_get(ane->dev);
	if (err < 0)
		return err;

	err = drm_ioctl(file, cmd, arg);

	pm_runtime_put(ane->dev);

	return err;
}

static int ane_drm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct drm_gem_object *gem;
	struct ane_bo *bo;
	int err;

	err = drm_gem_mmap(file, vma);
	if (err < 0)
		return err;

	/*
	 * Set vm_pgoff (used as a fake buffer offset by DRM) to 0 and map the
	 * whole buffer from the start.
	 */
	vma->vm_pgoff = 0;
	gem = vma->vm_private_data;
	bo = to_bo(gem);

	if (vma_pages(vma) == 0)
		return -ENXIO;

	/*
	 * We allocated a struct page table, so clear
	 * VM_PFNMAP flag that was set by drm_gem_mmap_obj()/drm_gem_mmap().
	 */
	vm_flags_mod(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP, VM_PFNMAP);

	vma->vm_page_prot =
		pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	return vm_map_pages(vma, bo->pages, bo->npages);
}

static const struct file_operations ane_drm_fops = {
	.owner = THIS_MODULE,
	.fop_flags = FOP_UNSIGNED_OFFSET,
	.open = accel_open,
	.release = drm_release,
	.unlocked_ioctl = ane_drm_unlocked_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = ane_drm_mmap,
};

static const struct drm_driver ane_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_COMPUTE_ACCEL,
	.open = ane_drm_open,
	.postclose = ane_drm_postclose,
	.ioctls = ane_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(ane_drm_ioctls),
	.fops = &ane_drm_fops,
	.major = ANE_ABI_MAJOR,
	.name = "ane",
	.desc = "Apple Neural Engine driver",
};

static int ane_iommu_domain_init(struct ane_device *ane)
{
	u64 min_iova, limit;

	struct iommu_domain *domain = iommu_get_domain_for_dev(ane->dev);
	if (!domain)
		return -EPROBE_DEFER;

	if (!(domain->pgsize_bitmap & PAGE_SIZE) ||
	    !domain->geometry.force_aperture)
		return -EINVAL;
	min_iova = max_t(u64, domain->geometry.aperture_start, PAGE_SIZE);
	limit = round_down(min_t(u64, domain->geometry.aperture_end, U32_MAX) + 1,
			   PAGE_SIZE);
	if (min_iova >= limit)
		return -EINVAL;
	min_iova = round_up(min_iova, PAGE_SIZE);
	if (min_iova >= limit)
		return -EINVAL;
	ane->domain = domain;
	ane->shift = PAGE_SHIFT;
	drm_mm_init(&ane->mm, min_iova, limit - min_iova);

	return 0;
}

static void ane_detach_genpd(struct ane_device *ane)
{
	if (ane->pd_count <= 1)
		return;

	for (int i = ane->pd_count - 1; i >= 0; i--) {
		if (ane->pd_link[i])
			device_link_del(ane->pd_link[i]);
		if (!IS_ERR_OR_NULL(ane->pd_dev[i]))
			dev_pm_domain_detach(ane->pd_dev[i], true);
	}
}

static int ane_attach_genpd(struct ane_device *ane)
{
	struct device *dev = ane->dev;

	ane->pd_count = of_count_phandle_with_args(
		dev->of_node, "power-domains", "#power-domain-cells");
	if (ane->pd_count < 1)
		return ane->pd_count < 0 ? ane->pd_count : -EINVAL;
	if (ane->pd_count == 1)
		return dev->pm_domain ? 0 : -EPROBE_DEFER;

	ane->pd_dev = devm_kcalloc(dev, ane->pd_count, sizeof(*ane->pd_dev),
				   GFP_KERNEL);
	if (!ane->pd_dev)
		return -ENOMEM;

	ane->pd_link = devm_kcalloc(dev, ane->pd_count, sizeof(*ane->pd_link),
				    GFP_KERNEL);
	if (!ane->pd_link)
		return -ENOMEM;

	for (int i = 0; i < ane->pd_count; i++) {
		ane->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR(ane->pd_dev[i])) {
			ane_detach_genpd(ane);
			return PTR_ERR(ane->pd_dev[i]);
		}

		ane->pd_link[i] =
			device_link_add(dev, ane->pd_dev[i],
					DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
						DL_FLAG_RPM_ACTIVE);
		if (!ane->pd_link[i]) {
			ane_detach_genpd(ane);
			return -EINVAL;
		}
	}

	return 0;
}

static int ane_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ane_device *ane;
	struct drm_device *drm;
	int err;

	ane = devm_drm_dev_alloc(dev, &ane_drm_driver, struct ane_device, drm);
	if (IS_ERR(ane))
		return PTR_ERR(ane);

	platform_set_drvdata(pdev, ane);
	ane->dev = dev;

	drm = &ane->drm;
	drm->dev_private = ane;

	atomic_set(&ane->wedged, 0);

	/* Managed power first: genpd links hold the ANE/DART supplier
	 * topology awake before any register is touched. */
	err = ane_attach_genpd(ane);
	if (err < 0) {
		dev_err(dev, "failed to attach power domains\n");
		return err;
	}

	ane->irq = platform_get_irq_byname(pdev, "ane");
	if (ane->irq < 0) {
		err = ane->irq;
		goto detach_genpd;
	}
	/* Polled completion design: the engine IRQ is validated but never
	 * requested here. The DART interrupt belongs to the DART driver
	 * and is never fetched, masked or unmasked from this driver. */

	/* Mapping only; no register access happens while unpowered. */
	ane->engine = devm_platform_ioremap_resource_byname(pdev, "engine");
	if (IS_ERR(ane->engine)) {
		err = PTR_ERR(ane->engine);
		goto detach_genpd;
	}

	mutex_init(&ane->iommu_lock);
	mutex_init(&ane->engine_lock);
	INIT_LIST_HEAD(&ane->bo_list);

	/*
	 * Kernel-owned IOMMU domain. Defers until every "iommus" provider
	 * has attached. There is deliberately no fallback to direct DART
	 * programming: without providers this driver must not bind.
	 */
	err = ane_iommu_domain_init(ane);
	if (err < 0)
		goto detach_genpd;

	/* Managed runtime PM from here on. The device stays powered for the
	 * whole qualification lifetime: autosuspend stays disabled and this
	 * reference is held until remove balances it. */
	pm_runtime_get_noresume(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	/* First MMIO strictly after a checked managed resume. */
	err = pm_runtime_resume_and_get(dev);
	if (err < 0)
		goto disable_pm;

	ane_tm_enable(ane);

	pm_runtime_put(dev);

	err = drm_dev_register(drm, 0);
	if (err < 0)
		goto disable_pm;

	dev_info(dev, "loaded ane\n");

	return 0;

disable_pm:
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev); /* balances the probe-time noresume get */
	drm_mm_takedown(&ane->mm);
detach_genpd:
	ane_detach_genpd(ane);
	return err;
}

static void ane_platform_remove(struct platform_device *pdev)
{
	struct ane_device *ane = platform_get_drvdata(pdev);
	struct ane_bo *bo, *tmp;

	mutex_lock(&ane->engine_lock);
	ane->removed = true;
	drm_dev_unplug(&ane->drm);

	if (atomic_read(&ane->wedged)) {
		dev_err(ane->dev,
			"forced removal of a wedged device is unsafe; reboot required\n");
		mutex_unlock(&ane->engine_lock);
		return;
	}

	list_for_each_entry_safe(bo, tmp, &ane->bo_list, node)
		ane_iommu_unmap_pages(ane, bo);
	drm_mm_takedown(&ane->mm);

	ane_detach_genpd(ane);

	pm_runtime_disable(ane->dev);
	pm_runtime_put_noidle(ane->dev);
	mutex_unlock(&ane->engine_lock);
}

static int __maybe_unused ane_runtime_suspend(struct device *dev)
{
	struct ane_device *ane = dev_get_drvdata(dev);

	/* Veto gating while the engine may be DMA-active: there is no
	 * documented abort/reset to establish quiescence first. */
	if (atomic_read(&ane->wedged))
		return -EBUSY;

	return 0;
}

static int __maybe_unused ane_runtime_resume(struct device *dev)
{
	struct ane_device *ane = dev_get_drvdata(dev);

	/* After a power gate the task manager must be re-enabled; every
	 * other translation is owned by the IOMMU providers. */
	ane_tm_enable(ane);

	return 0;
}

// clang-format off
static const struct dev_pm_ops ane_pm_ops = {
	SET_RUNTIME_PM_OPS(ane_runtime_suspend, ane_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};
// clang-format on

static const struct of_device_id ane_of_match[] = {
	{ .compatible = "apple,t8103-ane" },
	{ .compatible = "apple,t6000-ane" },
	{}
};

MODULE_DEVICE_TABLE(of, ane_of_match);

static struct platform_driver ane_platform_driver = {
    .probe  = ane_platform_probe,
    .remove = ane_platform_remove,
    .driver =
	{
	    .name	    = "ane",
	    .suppress_bind_attrs = true,
	    .pm             = pm_ptr(&ane_pm_ops),
	    .of_match_table = ane_of_match,
	},
};

module_platform_driver(ane_platform_driver);

MODULE_AUTHOR("Eileen Yoon <eyn@gmx.com>");
MODULE_DESCRIPTION("Apple Neural Engine driver");
MODULE_VERSION("f2a3e5e+lifecycle6");
MODULE_LICENSE("Dual MIT/GPL");
