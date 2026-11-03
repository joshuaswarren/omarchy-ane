/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sim/shim.h — userspace shim so the REAL ane_h15 module source
 * (ane_h15_main.c + ane_h15_soc.c) compiles and runs unmodified in the
 * harness. Pattern and much of the shape: the PMP vehicle harness
 * (vehicles/jwm1-pmp-oot/sim/shim.h). Provides a fake MMIO bus with a
 * full read/write event record, fake DT node, fake runtime-PM counters,
 * and a capture log for every dev_* line, so the harness can assert on
 * RESULT lines, word logs, and MMIO silence.
 *
 * A sim is not silicon: this proves control flow and gate logic, never
 * hardware behavior.
 */
#ifndef SIM_SHIM_H
#define SIM_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#define GENMASK(h, l) \
	(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define BITS_PER_LONG 64
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FIELD_GET(m, v) (((v) & (m)) >> (__builtin_ctzll(m)))
#define FIELD_PREP(m, v) ((((u64)(v)) << (__builtin_ctzll(m))) & (m))
#define BIT(n) (1UL << (n))

/* ---- annotations ---- */
#define __iomem
#define __init
#define __exit
#ifndef __always_inline
#define __always_inline inline
#endif

/* ---- capture log: every dev_* line lands here (and on stdout) ---- */
#define SIM_LOG_SZ (1 << 20)
extern char sim_log[SIM_LOG_SZ];
extern size_t sim_log_len;

static inline void sim_log_reset(void)
{
	sim_log_len = 0;
	sim_log[0] = '\0';
}

static inline void rec_vlog(const char *fmt, va_list ap)
{
	va_list ap2;
	int n;

	if (sim_log_len + 1 < SIM_LOG_SZ) {
		va_copy(ap2, ap);
		n = vsnprintf(sim_log + sim_log_len, SIM_LOG_SZ - sim_log_len,
			      fmt, ap2);
		va_end(ap2);
		if (n > 0)
			sim_log_len += (size_t)n;
	}
	vprintf(fmt, ap);
}

static inline void rec_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	rec_vlog(fmt, ap);
	va_end(ap);
}

#define pr_info(...) rec_log(__VA_ARGS__)
#define pr_err(...) rec_log("ERR: " __VA_ARGS__)
#define pr_warn(...) rec_log("WARN: " __VA_ARGS__)
#define pr_crit(...) rec_log("CRIT: " __VA_ARGS__)
#define pr_emerg(...) rec_log("EMERG: " __VA_ARGS__)
#define printk pr_info

/* ---- fake DT node + device ---- */
struct device_node {
	const char *compatible;
	const void *match_data;	/* set by the harness from the REAL table */
	bool has_ane_type;
	u32 ane_type;
	int n_reg;		/* valid reg entries: 0, 1 (truncated), 2 */
	u64 reg_pa[2];
	u64 reg_size[2];
	int n_pd;		/* entries in the node's power-domains property */
	bool has_iommus;	/* the node has an iommus property (a DART client) */
};

struct device {
	struct device_node *of_node;
	const char *name;
};

struct platform_device {
	struct device dev;
	void *drvdata;
};

#define dev_info(d, ...)  do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_err(d, ...)   do { (void)(d); rec_log("ERR: " __VA_ARGS__); } while (0)
#define dev_warn(d, ...)  do { (void)(d); rec_log("WARN: " __VA_ARGS__); } while (0)
#define dev_crit(d, ...)  do { (void)(d); rec_log("CRIT: " __VA_ARGS__); } while (0)
#define dev_emerg(d, ...) do { (void)(d); rec_log("EMERG: " __VA_ARGS__); } while (0)

static inline int of_property_read_u32(const struct device_node *n,
				       const char *name, u32 *v)
{
	if (n && n->has_ane_type && !strcmp(name, "apple,ane-type")) {
		*v = n->ane_type;
		return 0;
	}
	return -EINVAL;
}

static inline const void *of_device_get_match_data(struct device *dev)
{
	return dev->of_node ? dev->of_node->match_data : NULL;
}

struct of_device_id {
	const char *compatible;
	const void *data;
};

struct driver_shim {
	const char *name;
	bool suppress_bind_attrs;
	const struct of_device_id *of_match_table;
};

struct platform_driver {
	struct driver_shim driver;
	int (*probe)(struct platform_device *pdev);
	void (*remove)(struct platform_device *pdev);
};

#define module_param(n, t, p)
#define module_param_named(n, v, t, p)
#define MODULE_PARM_DESC(n, d)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(t, x)
#define module_platform_driver(x) extern char __sim_kmod_noop
#define module_init(f) extern char __sim_mi_##f
#define module_exit(f) extern char __sim_me_##f

/* ---- fake MMIO bus with a full access record ---- */
#define SIM_MAX_REGIONS 8
#define SIM_MAX_EVENTS 8192
struct sim_region {
	u64 pa;
	u64 size;
	u32 *mem;
};
struct sim_event {
	int write;
	u64 pa;
	u32 val;
};
extern struct sim_region sim_bus[SIM_MAX_REGIONS];
extern int sim_n_regions;
extern struct sim_event sim_events[SIM_MAX_EVENTS];
extern int sim_n_events;
extern int sim_bus_faults;
/* window map/unmap counts: the harness asserts a failed probe (and
 * remove) releases exactly the windows it mapped
 */
extern int sim_n_iomap;
extern int sim_n_iounmap;

static inline struct sim_region *sim_region_find(u64 pa)
{
	int i;

	for (i = 0; i < sim_n_regions; i++)
		if (pa >= sim_bus[i].pa && pa < sim_bus[i].pa + sim_bus[i].size)
			return &sim_bus[i];
	return NULL;
}

/* Create-or-find; a new region is zeroed. of_iomap calls this. */
static inline void *sim_bus_map(u64 pa, u64 size)
{
	struct sim_region *r = sim_region_find(pa);

	if (r)
		return (void *)(uintptr_t)pa;
	if (sim_n_regions >= SIM_MAX_REGIONS) {
		fprintf(stderr, "SIM: region table full\n");
		exit(2);
	}
	r = &sim_bus[sim_n_regions++];
	r->pa = pa;
	r->size = size;
	r->mem = calloc(size / 4 + 1, 4);
	if (!r->mem) {
		fprintf(stderr, "SIM: out of memory for bus region\n");
		exit(2);
	}
	return (void *)(uintptr_t)pa;
}

static inline void sim_event(int write, u64 pa, u32 val)
{
	if (sim_n_events < SIM_MAX_EVENTS) {
		sim_events[sim_n_events].write = write;
		sim_events[sim_n_events].pa = pa;
		sim_events[sim_n_events].val = val;
	}
	sim_n_events++;
}

extern void (*sim_write_hook)(u64 pa, u32 v);
extern u32 (*sim_read_hook)(u64 pa, u32 v);

static inline u32 bus_read(u64 pa)
{
	struct sim_region *r = sim_region_find(pa);
	u32 v;

	if (!r) {
		sim_bus_faults++;
		sim_event(0, pa, 0);
		return 0xdeadbeef;
	}
	v = r->mem[(pa - r->pa) >> 2];
	if (sim_read_hook)
		v = sim_read_hook(pa, v);
	sim_event(0, pa, v);
	return v;
}

/* Optional device model: the harness sets these for the stage 4 tests only. A write hook runs after the
 * store (so it can change other registers, e.g. CPU_STATUS after a RUN write); a read hook sees the value
 * the bus would return and may change it and the model state (e.g. popping a mailbox message). */
extern void (*sim_write_hook)(u64 pa, u32 v);
extern u32 (*sim_read_hook)(u64 pa, u32 v);

static inline void bus_write(u64 pa, u32 v)
{
	struct sim_region *r = sim_region_find(pa);

	if (!r) {
		sim_bus_faults++;
		sim_event(1, pa, v);
		return;
	}
	sim_event(1, pa, v);
	r->mem[(pa - r->pa) >> 2] = v;
	if (sim_write_hook)
		sim_write_hook(pa, v);
}

#define ioremap_np(pa, size) sim_bus_map((pa), (size))
#define ioremap(pa, size) sim_bus_map((pa), (size))
static inline void iounmap(void *p)
{
	(void)p;
	sim_n_iounmap++;
}

struct resource {
	u64 start;
	u64 end;
};

static inline u64 resource_size(const struct resource *r)
{
	return r->end - r->start + 1;
}

static inline int of_address_to_resource(struct device_node *n, int idx,
					 struct resource *r)
{
	if (!n || idx < 0 || idx >= n->n_reg || !n->reg_size[idx])
		return -EINVAL;
	r->start = n->reg_pa[idx];
	r->end = n->reg_pa[idx] + n->reg_size[idx] - 1;
	return 0;
}

static inline void *of_iomap(struct device_node *n, int idx)
{
	if (!n || idx < 0 || idx >= n->n_reg || !n->reg_size[idx])
		return NULL;
	sim_n_iomap++;
	return sim_bus_map(n->reg_pa[idx], n->reg_size[idx]);
}
/* One 64-bit access is ONE event (at the low address, hook applied there): the module must not split it. */
static inline u64 bus_read64(u64 pa)
{
	struct sim_region *r = sim_region_find(pa);
	u32 lo, hi;

	if (!r || !sim_region_find(pa + 7)) {
		sim_bus_faults++;
		sim_event(0, pa, 0);
		return 0xdeadbeefdeadbeefULL;
	}
	lo = r->mem[(pa - r->pa) >> 2];
	hi = r->mem[((pa - r->pa) >> 2) + 1];
	if (sim_read_hook)
		lo = sim_read_hook(pa, lo);
	sim_event(0, pa, lo);
	return ((u64)hi << 32) | lo;
}
#define readq_relaxed(a) bus_read64((u64)(uintptr_t)(a))
/* One 64-bit store is ONE event (at the low address); the 64-bit hook sees the whole value. */
extern void (*sim_write64_hook)(u64 pa, u64 v);
static inline void bus_write64(u64 pa, u64 v)
{
	struct sim_region *r = sim_region_find(pa);

	if (!r || !sim_region_find(pa + 7)) {
		sim_bus_faults++;
		sim_event(1, pa, (u32)v);
		return;
	}
	sim_event(1, pa, (u32)v);
	r->mem[(pa - r->pa) >> 2] = (u32)v;
	r->mem[((pa - r->pa) >> 2) + 1] = (u32)(v >> 32);
	if (sim_write64_hook)
		sim_write64_hook(pa, v);
}
#define writeq_relaxed(v, a) bus_write64((u64)(uintptr_t)(a), (v))
#define readl(a) bus_read((u64)(uintptr_t)(a))
#define readl_relaxed(a) bus_read((u64)(uintptr_t)(a))
#define writel(v, a) bus_write((u64)(uintptr_t)(a), (v))

/* ---- virtual clock: 1 jiffy = 1 ms; usleep advances it so the pmgr
 * wait loop terminates against a word that never turns on ----
 */
extern unsigned long sim_jiffies;
#define jiffies sim_jiffies
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static inline bool time_after(unsigned long a, unsigned long b)
{
	return (long)(b - a) < 0;
}
static inline void writel_relaxed(u32 v, void *a)
{
	bus_write((u64)(uintptr_t)(a), v);
}
static inline void usleep_range(unsigned long lo, unsigned long hi)
{
	sim_jiffies += (lo + hi) / 2;
}
static inline void msleep(unsigned int ms) { sim_jiffies += ms; }

/* ---- runtime PM counters: the harness asserts get==put (power
 * claim released on a clean exit) ----
 */
extern int sim_pm_get;
extern int sim_pm_put;
/* when nonzero, get_sync returns this (induced power-domains failure).
 * Like the kernel, a failing get_sync still takes a usage count that a
 * pm_runtime_put_sync must drop: sim_pm_get_err counts those.
 */
extern int sim_pm_get_fail;
extern int sim_pm_get_err;
extern int sim_pm_noidle;	/* put_noidle calls: they drop a failed get_sync's count */
static inline void pm_runtime_enable(struct device *d) { (void)d; }
static inline void pm_runtime_disable(struct device *d) { (void)d; }
static inline int pm_runtime_get_sync(struct device *d)
{
	(void)d;
	if (sim_pm_get_fail) {
		sim_pm_get_err++;
		return sim_pm_get_fail;
	}
	return ++sim_pm_get;
}
static inline int pm_runtime_put_sync(struct device *d)
{
	(void)d;
	return ++sim_pm_put;
}
static inline void pm_runtime_put_noidle(struct device *d)
{
	(void)d;
	sim_pm_noidle++;
}

/* module pinning: the stage takes a reference it never drops when the core will not stop */
#define THIS_MODULE ((void *)0)
extern int sim_module_gets;
static inline void __module_get(void *m) { (void)m; sim_module_gets++; }

/* ---- ERR_PTR helpers (the real linux/err.h is an empty sim stub) ---- */
#define MAX_ERRNO 4095
static inline void *ERR_PTR(long e) { return (void *)e; }
static inline long PTR_ERR(const void *p) { return (long)p; }
static inline bool IS_ERR(const void *p)
{
	return (unsigned long)p >= (unsigned long)-MAX_ERRNO;
}
static inline bool IS_ERR_OR_NULL(const void *p) { return !p || IS_ERR(p); }


struct firmware {
	size_t size;
	const u8 *data;
};
static int sim_fw_requests, sim_fw_live, sim_dma_poison;
static inline int request_firmware(const struct firmware **out,
				  const char *name, struct device *dev)
{
	struct firmware *fw;
	FILE *file;
	long size;
	void *data;
	int read_error, close_error;

	(void)dev;
	sim_fw_requests++;
	file = fopen(name, "rb");
	if (!file)
		return -ENOENT;
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
	    fseek(file, 0, SEEK_SET)) {
		fclose(file);
		return -EIO;
	}
	fw = malloc(sizeof(*fw));
	data = malloc(size ? (size_t)size : 1);
	if (!fw || !data) {
		free(fw);
		free(data);
		fclose(file);
		return -ENOMEM;
	}
	read_error = fread(data, 1, size, file) != (size_t)size;
	close_error = fclose(file);
	if (read_error || close_error) {
		free(data);
		free(fw);
		return -EIO;
	}
	fw->size = size;
	fw->data = data;
	*out = fw;
	sim_fw_live++;
	return 0;
}
static inline void release_firmware(const struct firmware *fw)
{
	free((void *)fw->data);
	free((void *)fw);
	sim_fw_live--;
}
void ane_sha256(const void *data, u64 len, u8 out[32]);
#define sha256 ane_sha256
/* ---- DMA API for stage 7: zeroed surfaces at IOVAs far from the firmware segments ---- */
#define PAGE_SIZE 0x4000UL
#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))
#define wmb() do { } while (0)
#define dma_wmb() do { } while (0)
#define dma_rmb() do { } while (0)
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
typedef u64 dma_addr_t;
#define DMA_BIT_MASK(n) (~0ULL >> (64 - (n)))
struct sim_dma { void *va; dma_addr_t iova; size_t size; int live; int unmaps_at_free; };
extern struct sim_dma sim_dma[8];
extern int sim_n_dma, sim_dma_fail, sim_dma_mask_bits, sim_dma_scatter, sim_map_fail_at;
static inline int dma_set_mask_and_coherent(struct device *d, u64 mask)
{
	(void)d;
	sim_dma_mask_bits = 64 - __builtin_clzll(mask);
	return 0;
}
static inline void *dma_alloc_coherent(struct device *d, size_t size, dma_addr_t *iova, int gfp)
{
	(void)d; (void)gfp;
	if (sim_dma_fail == sim_n_dma + 1 || sim_n_dma >= 8)
		return NULL;
	if (posix_memalign(&sim_dma[sim_n_dma].va, 0x4000, size))
		sim_dma[sim_n_dma].va = NULL;
	if (sim_dma[sim_n_dma].va)
		memset(sim_dma[sim_n_dma].va, sim_dma_poison ? 0xa5 : 0, size);
	sim_dma[sim_n_dma].iova = 0x3ff00000000ULL - 0x4000000ULL * (sim_n_dma + 1);
	sim_dma[sim_n_dma].size = size;
	sim_dma[sim_n_dma].live = 1;
	*iova = sim_dma[sim_n_dma].iova;
	return sim_dma[sim_n_dma++].va;
}
static inline void sim_note_dma_free(struct sim_dma *d);
static inline void dma_free_coherent(struct device *d, size_t size, void *va, dma_addr_t iova)
{
	int k;

	(void)d; (void)size;
	for (k = 0; k < sim_n_dma; k++)
		if (sim_dma[k].va == va && sim_dma[k].iova == iova) {
			sim_dma[k].live = 0;
			sim_note_dma_free(&sim_dma[k]);
		}
}

/* ---- iommu API for stage 5 map_fw=1: records every call with the bus event count at that moment ---- */
#define SZ_16K 0x4000ull
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IORESOURCE_SYSTEM_RAM 1ul
#define IORES_DESC_NONE 0ul
enum { REGION_INTERSECTS, REGION_DISJOINT, REGION_MIXED };
extern int sim_region_ram;			/* what region_intersects() reports: REGION_* */
static inline int region_intersects(u64 start, size_t size, unsigned long flags, unsigned long desc)
{
	(void)start; (void)size; (void)flags; (void)desc;
	return sim_region_ram;
}
#define __IOMMU_DOMAIN_DMA_API (1U << 1)
struct iommu_domain {
	unsigned int type;
	struct { u64 aperture_start, aperture_end; bool force_aperture; } geometry;
};
static unsigned int sim_window_queries;
extern struct iommu_domain sim_default_dom;
extern int sim_default_dom_none;
static inline struct iommu_domain *iommu_get_domain_for_dev(struct device *d) { (void)d; return sim_default_dom_none ? NULL : &sim_default_dom; }
enum { SIM_IO_ALLOC = 1, SIM_IO_ATTACH, SIM_IO_MAP, SIM_IO_UNMAP, SIM_IO_DETACH, SIM_IO_FREE };
struct sim_iocall { int code; u64 iova, pa, size; int prot; int ev; };
extern struct sim_iocall sim_iocalls[64];
extern int sim_n_iocalls, sim_io_fail, sim_n_dom_live;	/* sim_io_fail: 1 alloc, 2 attach, 3 map of segment 1, 4 map of segment 0 fails; 5 reads back wrong; sim_map_fail_at fails that 1-based map call */
extern int sim_unmap_fail_at;
extern void (*sim_attach_hook)(void);			/* the DART driver programming the DART at the attach */
static inline void sim_iorec(int code, u64 iova, u64 pa, u64 size, int prot)
{
	if (sim_n_iocalls < 64)
		sim_iocalls[sim_n_iocalls] = (struct sim_iocall){ code, iova, pa, size, prot, sim_n_events };
	sim_n_iocalls++;
}
static inline void sim_note_dma_free(struct sim_dma *d)
{
	int k;

	d->unmaps_at_free = 0;
	for (k = 0; k < sim_n_iocalls && k < 64; k++)
		d->unmaps_at_free += sim_iocalls[k].code == SIM_IO_UNMAP;
}
/* Page p of a DMA allocation sits at physical page f(p). With sim_dma_scatter, whole 128 KiB blocks are swapped in pairs; f is its own inverse. */
static inline u64 sim_dma_page(u64 page)
{
	u64 block = page / 8;

	return sim_dma_scatter && block < 26 ? (block ^ 1) * 8 + page % 8 : page;
}
static inline u64 sim_dma_phys(const struct sim_dma *d, u64 off)
{
	return (u64)(uintptr_t)d->va + sim_dma_page(off / SZ_16K) * SZ_16K + off % SZ_16K;
}
static inline bool device_iommu_mapped(struct device *d) { return d->of_node && d->of_node->has_iommus; }
static inline struct iommu_domain *iommu_paging_domain_alloc(struct device *d)
{
	(void)d;
	sim_iorec(SIM_IO_ALLOC, 0, 0, 0, 0);
	if (sim_io_fail == 1)
		return ERR_PTR(-ENOMEM);
	sim_n_dom_live++;
	return calloc(1, sizeof(struct iommu_domain));
}
static inline int iommu_attach_device(struct iommu_domain *dom, struct device *d)
{
	(void)dom; (void)d;
	sim_iorec(SIM_IO_ATTACH, 0, 0, 0, 0);
	if (sim_io_fail == 2)
		return -EBUSY;
	if (sim_attach_hook)
		sim_attach_hook();
	return 0;
}
static inline int iommu_map(struct iommu_domain *dom, unsigned long iova, u64 pa, size_t size, int prot, int gfp)
{
	int maps = 0, k;

	(void)dom; (void)gfp;
	for (k = 0; k < sim_n_iocalls && k < 64; k++)
		maps += sim_iocalls[k].code == SIM_IO_MAP;
	sim_iorec(SIM_IO_MAP, iova, pa, size, prot);
	if ((sim_io_fail == 4 && maps == 0) || (sim_io_fail == 3 && maps == 1) || (sim_map_fail_at && maps + 1 == sim_map_fail_at))
		return -ENOMEM;
	return 0;
}
static inline u64 iommu_iova_to_phys(struct iommu_domain *dom, u64 iova)
{
	int k;
	sim_window_queries++;

	if (dom == &sim_default_dom)
		for (k = 0; k < sim_n_dma; k++)
			if (sim_dma[k].live && iova >= sim_dma[k].iova && iova < sim_dma[k].iova + sim_dma[k].size)
				return sim_dma_phys(&sim_dma[k], iova - sim_dma[k].iova);
	for (k = 0; k < sim_n_iocalls && k < 64; k++)
		if (sim_iocalls[k].code == SIM_IO_MAP && iova >= sim_iocalls[k].iova && iova < sim_iocalls[k].iova + sim_iocalls[k].size)
			return sim_io_fail == 5 ? 0 : sim_iocalls[k].pa + (iova - sim_iocalls[k].iova);
	return 0;
}
static inline size_t iommu_unmap(struct iommu_domain *dom, unsigned long iova, size_t size)
{
	int k, unmaps = 0;

	(void)dom;
	for (k = 0; k < sim_n_iocalls && k < 64; k++)
		unmaps += sim_iocalls[k].code == SIM_IO_UNMAP;
	sim_iorec(SIM_IO_UNMAP, iova, 0, size, 0);
	if (sim_unmap_fail_at == -(unmaps + 1))
		return 0;
	return sim_unmap_fail_at == unmaps + 1 ? size - SZ_16K : size;
}
static inline void iommu_detach_device(struct iommu_domain *dom, struct device *d) { (void)dom; (void)d; sim_iorec(SIM_IO_DETACH, 0, 0, 0, 0); }
static inline void iommu_domain_free(struct iommu_domain *dom) { sim_iorec(SIM_IO_FREE, 0, 0, 0, 0); sim_n_dom_live--; free(dom); }


/* ---- multiple power-domains: attach by id + runtime-PM device links.
 * Every attach, link, link delete and detach is counted and tracked per
 * slot, so the harness can assert that a probe (success or failure at
 * any index) and remove leave nothing attached or linked, and that no
 * slot is released twice. Like the kernel, device_link_add() returns
 * NULL on failure (not an ERR_PTR); dev_pm_domain_attach_by_id()
 * returns an ERR_PTR on failure.
 * ----
 */
#define SIM_MAX_PD 8
#define DL_FLAG_STATELESS	(1U << 0)
#define DL_FLAG_PM_RUNTIME	(1U << 1)
#define DL_FLAG_RPM_ACTIVE	(1U << 2)
struct device_link {
	int idx;
};
extern struct device sim_pd_dev[SIM_MAX_PD];
extern struct device_link sim_pd_link[SIM_MAX_PD];
/* 0 = never attached/linked, 1 = live, 2 = released */
extern int sim_pd_state[SIM_MAX_PD];
extern int sim_link_state[SIM_MAX_PD];
extern int sim_pd_attach;	/* successful attaches */
extern int sim_pd_detach;	/* detaches of a live slot */
extern int sim_link_add;	/* successful links */
extern int sim_link_del;	/* deletes of a live link */
extern int sim_pd_bad;		/* detach/del of a slot that is not live */
/* attach/link call number (0-based, counted over the whole run) that
 * fails; -1 = never
 */
extern int sim_pd_attach_fail_at;
extern int sim_link_fail_at;
extern int sim_pd_attach_calls;
extern int sim_link_calls;

static inline int of_count_phandle_with_args(const struct device_node *np,
					     const char *list_name,
					     const char *cells_name)
{
	(void)list_name;
	(void)cells_name;
	return np && np->n_pd > 0 ? np->n_pd : -ENOENT;
}

static inline struct device *dev_pm_domain_attach_by_id(struct device *dev,
							unsigned int index)
{
	int call = sim_pd_attach_calls++;

	(void)dev;
	if (call == sim_pd_attach_fail_at || index >= SIM_MAX_PD)
		return ERR_PTR(-ENODEV);
	sim_pd_state[index] = 1;
	sim_pd_attach++;
	return &sim_pd_dev[index];
}

static inline void dev_pm_domain_detach(struct device *dev, bool power_off)
{
	long idx = dev - sim_pd_dev;

	(void)power_off;
	if (idx < 0 || idx >= SIM_MAX_PD || sim_pd_state[idx] != 1) {
		sim_pd_bad++;
		return;
	}
	sim_pd_state[idx] = 2;
	sim_pd_detach++;
}

static inline struct device_link *device_link_add(struct device *consumer,
						  struct device *supplier,
						  unsigned int flags)
{
	int call = sim_link_calls++;
	long idx = supplier - sim_pd_dev;

	(void)consumer;
	(void)flags;
	if (call == sim_link_fail_at || idx < 0 || idx >= SIM_MAX_PD)
		return NULL;
	sim_link_state[idx] = 1;
	sim_link_add++;
	return &sim_pd_link[idx];
}

static inline void device_link_del(struct device_link *link)
{
	long idx = link - sim_pd_link;

	if (idx < 0 || idx >= SIM_MAX_PD || sim_link_state[idx] != 1) {
		sim_pd_bad++;
		return;
	}
	sim_link_state[idx] = 2;
	sim_link_del++;
}

/* ---- allocators / drvdata ---- */
#define GFP_KERNEL 0
static inline void *devm_kzalloc(struct device *d, size_t s, int gfp)
{
	(void)d;
	(void)gfp;
	return calloc(1, s);
}
static inline void platform_set_drvdata(struct platform_device *p, void *v)
{
	p->drvdata = v;
}
static inline void *platform_get_drvdata(struct platform_device *p)
{
	return p->drvdata;
}


/* ---- steplog shim: capture the module's step lines instead of touching a file ---- */
#define O_WRONLY 01
#define O_CREAT 0100
#define O_APPEND 02000
struct file { int fd; };
extern char sim_steplog_buf[8192];
extern int sim_steplog_len;
extern unsigned long sim_steplog_fsyncs;
extern int sim_steplog_open_fail;
extern int sim_steplog_mode;
extern unsigned short sim_step_at[2048];
extern int sim_step_n;
static inline struct file *filp_open(const char *path, int flags, int mode)
{
	(void)path; (void)flags;
	sim_steplog_mode = mode;
	if (sim_steplog_open_fail)
		return (struct file *)ERR_PTR(-ENOENT);
	return (struct file *)1;
}
static inline int filp_close(struct file *f, void *owner)
{
	(void)owner;
	return f ? 0 : -EBADF;
}
static inline ssize_t kernel_write(struct file *f, const void *buf, size_t n, void *pos)
{
	(void)f; (void)pos;
	if (n > (size_t)(sizeof(sim_steplog_buf) - 1 - sim_steplog_len))
		n = sizeof(sim_steplog_buf) - 1 - sim_steplog_len;
	memcpy(sim_steplog_buf + sim_steplog_len, buf, n);
	sim_steplog_len += n;
	sim_steplog_buf[sim_steplog_len] = 0;
	/* events seen when this line was written: proves the line precedes its access */
	if (sim_step_n >= (int)(sizeof(sim_step_at) / sizeof(sim_step_at[0])))
		sim_step_n = 0;	/* wrap: only the tail is checked for ordering */
	sim_step_at[sim_step_n++] = (unsigned short)sim_n_events;
	return n;
}
static inline int scnprintf(char *b, size_t sz, const char *fmt, ...)
{
	va_list ap; int n;
	va_start(ap, fmt); n = vsnprintf(b, sz, fmt, ap); va_end(ap);
	return n;
}
static inline int vscnprintf(char *b, size_t sz, const char *fmt, va_list ap)
{
	int n = vsnprintf(b, sz, fmt, ap);
	return n >= (int)sz ? (int)sz - 1 : n;
}
static inline size_t strscpy(char *dst, const char *src, size_t n)
{
	size_t l = strlen(src);
	if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(dst, src, c); dst[c] = 0; }
	return l;
}
static inline int vfs_fsync(struct file *f, int datasync)
{
	(void)f; (void)datasync;
	sim_steplog_fsyncs++;
	return 0;
}
static inline unsigned long long ktime_get_boottime_ns(void)
{
	extern unsigned long long sim_ktime_ns;
	return sim_ktime_ns += 1000000ULL;
}

#endif /* SIM_SHIM_H */
