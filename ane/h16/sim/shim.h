/* SPDX-License-Identifier: GPL-2.0 */
/* ane/h16/sim/shim.h -- userspace shim so the REAL ane_h16 module source
 * compiles and runs unmodified against a fake MMIO bus, a fake boot ADT,
 * a fake DART, and a firmware model (see sim_harness.c). Same design as
 * the PMP vehicle harness (vehicles/jwm1-pmp-oot/sim/shim.h).
 *
 * Known cosmetic gap: "%pOF" (ane_h16_adt_open) renders as pointer+"OF"
 * here instead of the kernel's node path; nothing keys on that line.
 */
#ifndef ANE_H16_SIM_SHIM_H
#define ANE_H16_SIM_SHIM_H

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef EPROBE_DEFER
#define EPROBE_DEFER 517
#endif

/* kernel int-ll64 widths: u64 is always long long, so the module's
 * %#llx format strings typecheck the same as in the kernel build
 */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned long long dma_addr_t;

/* ---- bit helpers ---- */
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (31 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define BIT(b) (1U << (b))
#define BIT_ULL(b) (1ULL << (b))
#define FIELD_PREP(m, v) ((((u64)(v)) << (__builtin_ctzll(m))) & (m))
#define FIELD_GET(m, v) ((((v) & (m)) >> (__builtin_ctzll(m))))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define SZ_4K 0x1000
#define SZ_16K 0x4000
#define SZ_1M 0x100000

/* ---- err/alloc ---- */
#define GFP_KERNEL 0
#define MEMREMAP_WB 0
extern int sim_snapshots, sim_snapshot_attempts, sim_snapshot_fail;
static inline void *kvzalloc(size_t size, int flags)
{
	void *p;
	(void)flags;
	if (++sim_snapshot_attempts == sim_snapshot_fail)
		return NULL;
	p = calloc(1, size);
	if (p)
		sim_snapshots++;
	return p;
}
static inline void kvfree(void *p)
{
	if (p) {
		sim_snapshots--;
		free(p);
	}
}
#define __iomem
#define __maybe_unused __attribute__((unused))
#define IS_ERR(p) ((unsigned long)(p) > (unsigned long)-4096L)
#define PTR_ERR(p) ((long)(p))
#define ERR_PTR(e) ((void *)(long)(e))

/* ---- fake MMIO bus ---- */
struct sim_region {
	u64 pa;
	u64 size;
	u32 *backing;
	const char *name;
};
struct sim_event {
	u64 pa;
	u32 val;
	bool write;
};
#define SIM_MAX_EVENTS (1 << 16)
extern struct sim_region sim_regions[8];
extern int sim_n_regions;
extern struct sim_event sim_events[SIM_MAX_EVENTS];
extern int sim_n_events;
extern bool sim_events_overflow;
extern int sim_ioremaps;
extern void (*sim_fw_read_hook)(u64 pa);
extern void (*sim_fw_write_hook)(u64 pa, u32 val);

static inline u32 sim_bus_read(u64 pa)
{
	int i;

	if (sim_fw_read_hook)
		sim_fw_read_hook(pa);
	for (i = 0; i < sim_n_regions; i++) {
		struct sim_region *r = &sim_regions[i];

		if (pa >= r->pa && pa + 4 <= r->pa + r->size) {
			if (sim_n_events < SIM_MAX_EVENTS) {
				sim_events[sim_n_events].pa = pa;
				sim_events[sim_n_events].write = false;
				sim_n_events++;
			} else {
				sim_events_overflow = true;
			}
			return r->backing[(pa - r->pa) / 4];
		}
	}
	fprintf(stderr, "SIM-BUG: read of unmapped pa %#llx\n",
		(unsigned long long)pa);
	abort();
}

static inline void sim_bus_write(u64 pa, u32 v)
{
	int i;

	for (i = 0; i < sim_n_regions; i++) {
		struct sim_region *r = &sim_regions[i];

		if (pa >= r->pa && pa + 4 <= r->pa + r->size) {
			if (sim_n_events < SIM_MAX_EVENTS) {
				sim_events[sim_n_events].pa = pa;
				sim_events[sim_n_events].val = v;
				sim_events[sim_n_events].write = true;
				sim_n_events++;
			} else {
				sim_events_overflow = true;
			}
			r->backing[(pa - r->pa) / 4] = v;
			if (sim_fw_write_hook)
				sim_fw_write_hook(pa, v);
			return;
		}
	}
	fprintf(stderr, "SIM-BUG: write of unmapped pa %#llx\n",
		(unsigned long long)pa);
	abort();
}

#define readl(a) sim_bus_read((u64)(uintptr_t)(a))
#define writel(v, a) sim_bus_write((u64)(uintptr_t)(a), (u32)(v))
#define readl_relaxed(a) readl(a)
#define writel_relaxed(v, a) writel(v, a)
#define readq(a) ((u64)sim_bus_read((u64)(uintptr_t)(a)) | \
		  ((u64)sim_bus_read((u64)(uintptr_t)(a) + 4) << 32))
#define readq_relaxed(a) readq(a)
#define writeq(v, a) (sim_bus_write((u64)(uintptr_t)(a), (u32)(u64)(v)), \
		      sim_bus_write((u64)(uintptr_t)(a) + 4, \
				    (u32)((u64)(v) >> 32)))
#define writeq_relaxed(v, a) writeq(v, a)
/* the sim bus is program-ordered; the barrier only marks the kext's
 * SCRATCH7/CPU_CONTROL ordering point for readers of the code
 */
#define wmb() do { } while (0)
extern int sim_dma_wmbs, sim_dma_rmbs;
#define dma_wmb() do { sim_dma_wmbs++; } while (0)
#define dma_rmb() do { sim_dma_rmbs++; } while (0)

#define readl_poll_timeout(addr, val, cond, sleep_us, timeout_us) ({     \
	unsigned long __tmo = jiffies + msecs_to_jiffies((timeout_us) / 1000); \
	int __ret = 0;							\
	for (;;) {							\
		(val) = readl(addr);					\
		if (cond)						\
			break;						\
		if (time_after(jiffies, __tmo)) {			\
			__ret = -ETIMEDOUT;				\
			break;						\
		}							\
		usleep_range(sleep_us, sleep_us);			\
	}								\
	__ret;								\
})

static inline void udelay(unsigned long us)
{
	(void)us;
}

/* ---- virtual time: only usleep_range advances it, so a spin without
 * sleep freezes the clock and every bounded wait hits its deadline ----
 */
extern unsigned long sim_now_ms;
#define jiffies sim_now_ms
#define msecs_to_jiffies(ms) (ms)
#define time_after(a, b) ((long)(b) - (long)(a) < 0)
#define time_after_eq(a, b) ((long)(a) - (long)(b) >= 0)
static inline void usleep_range(unsigned long a, unsigned long b)
{
	sim_now_ms += (b + 999) / 1000;
	(void)a;
}

/* ---- logging: every dev_* line is printed AND stored for assertions ---- */
#define SIM_LOG_MAX 8192
#define __printf(a, b) __attribute__((format(printf, a, b)))
extern char *sim_log[SIM_LOG_MAX];
extern int sim_n_log;
void sim_log_line(const char *tag, const char *fmt, ...) __printf(2, 3);
#define dev_info(dev, ...) sim_log_line("info", __VA_ARGS__)
#define dev_warn(dev, ...) sim_log_line("warn", __VA_ARGS__)
#define dev_err(dev, ...) sim_log_line("ERR ", __VA_ARGS__)
#define dev_crit(dev, ...) sim_log_line("CRIT", __VA_ARGS__)
#define dev_emerg(dev, ...) sim_log_line("EMRG", __VA_ARGS__)
#define pr_crit(...) sim_log_line("CRIT", __VA_ARGS__)

/* ---- byte order ---- */
#define le32_to_cpup(p) ((u32)((const u8 *)(p))[0] | \
			((u32)((const u8 *)(p))[1] << 8) | \
			((u32)((const u8 *)(p))[2] << 16) | \
			((u32)((const u8 *)(p))[3] << 24))
#define le64_to_cpu(x) ((u64)le32_to_cpup((const u8 *)&(x)) | \
			((u64)le32_to_cpup((const u8 *)&(x) + 4) << 32))

/* ---- fake device tree ---- */
struct sim_prop {
	const char *name;
	const void *val;
	int len;
};
struct device_node {
	const char *name;
	const char *label;
	struct sim_prop props[8];
	int nprops;
	u64 reg[2][2];		/* {start, size} per index */
	int nreg;
	struct device_node *children[4];
	int nchildren;
};
struct resource {
	u64 start;
	u64 end;
};
static inline u64 resource_size(const struct resource *r)
{
	return r->end - r->start + 1;
}
static inline const void *of_get_property(struct device_node *np,
					  const char *name, int *lenp)
{
	int i;

	for (i = 0; i < np->nprops; i++) {
		if (!strcmp(np->props[i].name, name)) {
			if (lenp)
				*lenp = np->props[i].len;
			return np->props[i].val;
		}
	}
	return NULL;
}
static inline int of_property_read_u32(struct device_node *np,
				       const char *name, u32 *out)
{
	const void *v = of_get_property(np, name, NULL);

	if (!v)
		return -EINVAL;
	*out = le32_to_cpup(v);
	return 0;
}
static inline int of_property_count_u32_elems(struct device_node *np,
					      const char *name)
{
	const void *v = of_get_property(np, name, NULL);
	int len = 0;

	if (!v)
		return -EINVAL;
	(void)of_get_property(np, name, &len);
	return len / 4;
}
static inline int of_address_to_resource(struct device_node *np, int idx,
					 struct resource *res)
{
	if (idx < 0 || idx >= np->nreg)
		return -ENOENT;
	res->start = np->reg[idx][0];
	res->end = np->reg[idx][0] + np->reg[idx][1] - 1;
	return 0;
}
extern struct device_node *sim_dt_nodes[8];
extern int sim_n_dt_nodes;
static inline struct device_node *of_find_node_by_name(struct device_node *from,
						       const char *name)
{
	int i;

	(void)from;
	for (i = 0; i < sim_n_dt_nodes; i++)
		if (!strcmp(sim_dt_nodes[i]->name, name))
			return sim_dt_nodes[i];
	return NULL;
}
static inline struct device_node *of_get_next_child(struct device_node *np,
						    struct device_node *prev)
{
	int i, start = 0;

	if (prev)
		for (i = 0; i < np->nchildren; i++)
			if (np->children[i] == prev)
				start = i + 1;
	return start < np->nchildren ? np->children[start] : NULL;
}
#define for_each_child_of_node(parent, child)			\
	for ((child) = of_get_next_child((parent), NULL); (child); \
	     (child) = of_get_next_child((parent), (child)))
static inline void of_node_put(struct device_node *np)
{
	(void)np;
}

/* ---- System RAM model for preload ownership admission ---- */
#define IORESOURCE_SYSTEM_RAM	0x00020000
#define IORES_DESC_NONE		0
enum ios_region { REGION_DISJOINT, REGION_INTERSECTS, REGION_MIXED };
struct sim_ram_range { u64 start, end; };
extern struct sim_ram_range sim_sys_ram[4];extern int sim_n_sys_ram;
static inline int region_intersects(u64 start, size_t size, unsigned long flags,
				    int desc)
{
	int i;

	(void)flags;
	(void)desc;
	for (i = 0; i < sim_n_sys_ram; i++)
		if (start < sim_sys_ram[i].end &&
		    sim_sys_ram[i].start < start + size)
			return REGION_INTERSECTS;
	return REGION_DISJOINT;
}

/* ---- platform device / driver ---- */
struct device {
	struct device_node *of_node;
};
extern const void *sim_match_data;
struct platform_device {
	struct device dev;
	const char *name;
	void *drvdata;
};
struct of_device_id {
	const char *compatible;
	const void *data;
};
struct device_driver {
	const char *name;
	bool suppress_bind_attrs;
	const struct of_device_id *of_match_table;
};
struct platform_driver {
	struct device_driver driver;
	int (*probe)(struct platform_device *pdev);
	void (*remove)(struct platform_device *pdev);
};
static inline const void *of_device_get_match_data(struct device *dev)
{
	(void)dev;
	return sim_match_data;
}
static inline void platform_set_drvdata(struct platform_device *pdev, void *d)
{
	pdev->drvdata = d;
}
static inline void *platform_get_drvdata(struct platform_device *pdev)
{
	return pdev->drvdata;
}

/* ---- devm / ioremap ---- */
/* Fake bus addresses: the real t8132 engine and pmgr window bases. */
#define SIM_ENGINE_PA 0x500000000ull	/* engine window 0x2000000 bytes */
#define SIM_PMGR_PA 0x380700000ull	/* pmgr window 0x18000 bytes */
#define SIM_PWGATE_PA 0x300724000ull	/* H17 pwgate window 0x4000 bytes */
extern bool sim_pwgate_mapped;
extern int sim_rpm_gets, sim_rpm_puts;
extern struct iommu_domain *sim_dart;	/* NULL = device has no domain */
#define IORESOURCE_MEM 0x00000200
static inline struct resource *platform_get_resource_byname(
	struct platform_device *dev, unsigned int flags, const char *name)
{
	struct device_node *np = dev->dev.of_node;
	struct resource *r = calloc(1, sizeof(*r));
	int idx = !strcmp(name, "engine") ? 0 :
		  !strcmp(name, "pmgr") ? 1 :
		  !strcmp(name, "pwgate") ? 2 : -1;

	(void)flags;
	if (idx < 0 || idx >= np->nreg) {
		free(r);
		return NULL;
	}
	r->start = np->reg[idx][0];
	r->end = np->reg[idx][0] + np->reg[idx][1] - 1;
	return r;
}
static inline void __iomem *devm_ioremap(struct device *dev, u64 pa, u64 size)
{
	int i;

	(void)dev;
	sim_ioremaps++;
	for (i = 0; i < sim_n_regions; i++)
		if (sim_regions[i].pa == pa &&
		    sim_regions[i].size <= size)
			return (void *)(uintptr_t)pa;
	return NULL;
}
struct sim_phys {
	u64 pa;
	u64 size;
	void *host;
};
extern struct sim_phys sim_phys_maps[8];
extern int sim_n_phys_maps;
static inline void *devm_kzalloc(struct device *dev, size_t size, int gfp)
{
	(void)dev;
	(void)gfp;
	return calloc(1, size);
}
static inline void *devm_memremap(struct device *dev, u64 pa, size_t size,
				  int flags)
{
	int i;

	(void)dev;
	(void)flags;
	for (i = 0; i < sim_n_phys_maps; i++)
		if (pa >= sim_phys_maps[i].pa &&
		    pa + size <= sim_phys_maps[i].pa + sim_phys_maps[i].size)
			return sim_phys_maps[i].host;
	return ERR_PTR(-ENODEV);
}
static inline void devm_memunmap(struct device *dev, void *p)
{
	(void)dev;
	(void)p;
}
/* of_iomap() maps without requesting the region, which is why the module uses it: the pmgr driver already owns part of the pmgr window. */
extern int sim_iounmaps;
static inline void *of_iomap(struct device_node *np, int idx)
{
	if (idx < 0 || idx >= np->nreg)
		return NULL;
	sim_ioremaps++;
	return (void *)(uintptr_t)np->reg[idx][0];
}
static inline void iounmap(void *p)
{
	(void)p;
	sim_iounmaps++;
}
/* reg-names is a list of NUL separated strings. */
static inline int of_property_match_string(struct device_node *np,
					   const char *name, const char *s)
{
	int len = 0, off = 0, idx = 0;
	const char *v = of_get_property(np, name, &len);

	if (!v)
		return -EINVAL;
	while (off < len) {
		if (!strcmp(v + off, s))
			return idx;
		off += strlen(v + off) + 1;
		idx++;
	}
	return -ENODATA;
}

/* ---- power-domain attach (multi-domain devices attach by index) ---- */
#define DL_FLAG_STATELESS	0x1
#define DL_FLAG_PM_RUNTIME	0x2
#define DL_FLAG_RPM_ACTIVE	0x4
struct device_link { int idx; };
extern int sim_pd_count, sim_pd_fail_at, sim_pd_link_fail_at;
extern int sim_pd_attached, sim_pd_detached, sim_pd_links, sim_pd_unlinked;
static inline int of_count_phandle_with_args(const struct device_node *np,
					     const char *list,
					     const char *cells)
{
	(void)np;
	(void)list;
	(void)cells;
	return sim_pd_count;
}
extern struct device sim_pd_devs[8];
extern struct device_link sim_pd_link_objs[8];
static inline struct device *dev_pm_domain_attach_by_id(struct device *dev,
							 unsigned int i)
{
	(void)dev;
	if ((int)i == sim_pd_fail_at)
		return ERR_PTR(-EPROBE_DEFER);
	sim_pd_attached++;
	return &sim_pd_devs[i];
}
static inline void dev_pm_domain_detach(struct device *dev, bool power_off)
{
	(void)dev;
	(void)power_off;
	sim_pd_detached++;
}
static inline struct device_link *device_link_add(struct device *c,
						  struct device *s, u32 flags)
{
	int i = (int)(s - sim_pd_devs);

	(void)c;
	(void)flags;
	if (i == sim_pd_link_fail_at)
		return NULL;
	sim_pd_links++;
	return &sim_pd_link_objs[i];
}
static inline void device_link_del(struct device_link *l)
{
	(void)l;
	sim_pd_unlinked++;
}
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))

/* ---- runtime pm (genpd stand-in) ---- */
extern int sim_rpm_gets, sim_rpm_puts, sim_rpm_get_rc;
static inline void pm_runtime_enable(struct device *dev) { (void)dev; }
static inline void pm_runtime_disable(struct device *dev) { (void)dev; }
static inline int pm_runtime_get_sync(struct device *dev)
{
	(void)dev;
	sim_rpm_gets++;
	return sim_rpm_get_rc;
}
static inline void pm_runtime_put_sync(struct device *dev)
{
	(void)dev;
	sim_rpm_puts++;
}
static inline void pm_runtime_put_noidle(struct device *dev)
{
	(void)dev;
	sim_rpm_puts++;
}
static inline void pm_runtime_get_noresume(struct device *dev) { (void)dev; }

/* ---- DMA ---- */
extern int sim_module_refs;
#define THIS_MODULE ((void *)1)
static inline void __module_get(void *module)
{
	(void)module;
	sim_module_refs++;
}

extern int sim_dma_outstanding;
extern int sim_dma_allocs, sim_dma_frees;
extern int sim_dma_fail_at, sim_dma_bad_at;
extern u64 sim_resv_length;
static int sim_coherent_map(size_t size, dma_addr_t *dma);
static void sim_coherent_unmap(dma_addr_t dma, size_t size);
#define DMA_BIT_MASK(n) (~0ULL >> (64 - (n)))
static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{
	(void)dev;
	(void)mask;
	return 0;
}
static inline void *dma_alloc_coherent(struct device *dev, size_t size,
				       dma_addr_t *dma, int gfp)
{
	void *p = NULL;

	(void)dev;
	(void)gfp;
	if (sim_dma_fail_at && sim_dma_allocs + 1 == sim_dma_fail_at)
		return NULL;
	/* the module asserts 16 KiB alignment on the staged buffer
	 */
	if (posix_memalign(&p, SZ_16K, (size + SZ_16K - 1) & ~(SZ_16K - 1)))
		return NULL;
	memset(p, 0, size);
	if (sim_coherent_map(size, dma)) {
		free(p);
		return NULL;
	}
	sim_dma_outstanding++;
	sim_dma_allocs++;
	return p;
}
static inline void dma_free_coherent(struct device *dev, size_t size,
				     void *cpu, dma_addr_t dma)
{
	(void)dev;
	(void)size;
	(void)dma;
	sim_coherent_unmap(dma, size);
	sim_dma_outstanding--;
	sim_dma_frees++;
	free(cpu);
}
static inline u64 virt_to_phys(void *p)
{
	return (u64)(uintptr_t)p;
}

/* ---- fake DART (iommu) ---- */
struct iommu_domain {
	struct {
		u64 iova;
		u64 pa;
		u64 len;
	} map[4096];
	int n;
	bool short_unmap;
	bool dma_api;
	bool alias_reserved;
};
#define IOMMU_READ 1
#define IOMMU_WRITE 2
extern struct iommu_domain *sim_dart;	/* NULL = device has no domain */
static inline bool iommu_is_dma_domain(struct iommu_domain *d)
{
	return d->dma_api;
}

struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD(n) struct list_head n = { &(n), &(n) }
#define list_for_each_entry(pos, head, member) \
	for (struct list_head *_node = (head)->next; \
	     _node != (head) && ((pos) = (void *)((char *)_node - \
	         offsetof(__typeof__(*(pos)), member)), 1); _node = _node->next)
#define IOMMU_RESV_RESERVED 1
struct iommu_resv_region {
	u64 start, length;
	int type;
	struct list_head list;
};
static inline void iommu_get_resv_regions(struct device *dev, struct list_head *h)
{
	static struct iommu_resv_region region;
	(void)dev;
	if (sim_dart && sim_dart->alias_reserved) {
		region.start = 0x10000000000ULL;
		region.length = sim_resv_length ? sim_resv_length : 0x4000000;
		region.type = IOMMU_RESV_RESERVED;
		region.list.next = region.list.prev = h;
		h->next = h->prev = &region.list;
	}
}
static inline void iommu_put_resv_regions(struct device *dev, struct list_head *h)
{
	(void)dev;
	h->next = h->prev = h;
}
static inline struct iommu_domain *iommu_get_domain_for_dev(struct device *dev)
{
	(void)dev;
	return sim_dart;
}
static inline int iommu_map(struct iommu_domain *d, u64 iova, u64 pa,
			    size_t len, int prot, int gfp)
{
	(void)prot;
	(void)gfp;
	if (d->n >= (int)ARRAY_SIZE(d->map))
		return -ENOMEM;
	d->map[d->n].iova = iova;
	d->map[d->n].pa = pa;
	d->map[d->n].len = len;
	d->n++;
	return 0;
}
static inline u64 iommu_iova_to_phys(struct iommu_domain *d, u64 iova)
{
	int i;

	for (i = 0; i < d->n; i++)
		if (iova >= d->map[i].iova &&
		    iova < d->map[i].iova + d->map[i].len)
			return d->map[i].pa + (iova - d->map[i].iova);
	return 0;
}
static inline size_t iommu_unmap(struct iommu_domain *d, u64 iova, size_t size)
{
	int i;
	size_t removed = 0;

	if (d->short_unmap)
		return 0;

	for (i = 0; i < d->n; i++)
		if (d->map[i].iova >= iova &&
		    d->map[i].iova + d->map[i].len <= iova + size) {
			removed += d->map[i].len;
			d->map[i].len = 0;
		}
	return removed;
}

/* CPU address, DMA handle and backing PA deliberately all differ. */
#define SIM_DMA_SOURCE_IOVA 0x20000000000ULL
#define SIM_DMA_SOURCE_PA 0x30000000000ULL
static int sim_coherent_map(size_t size, dma_addr_t *dma)
{
	size_t off;
	if (!sim_dart)
		return -ENODEV;
	*dma = SIM_DMA_SOURCE_IOVA + (u64)sim_dma_allocs * 0x2000000;
	if (sim_dma_bad_at && sim_dma_allocs + 1 == sim_dma_bad_at)
		*dma = 0x50000000000ULL;
	for (off = 0; off < size; off += SZ_16K)
		if (iommu_map(sim_dart, *dma + off,
			      SIM_DMA_SOURCE_PA + (u64)sim_dma_allocs * 0x2000000 + off,
			      SZ_16K, IOMMU_READ | IOMMU_WRITE, 0))
			return -ENOMEM;
	return 0;
}
static void sim_coherent_unmap(dma_addr_t dma, size_t size)
{
	iommu_unmap(sim_dart, dma, size);
}

/* ---- firmware ---- */
struct firmware {
	size_t size;
	const u8 *data;
};
extern const struct firmware *sim_fw_file;	/* NULL = nothing to serve */
extern const char *sim_fw_name;
static inline int request_firmware(const struct firmware **fw,
				   const char *name, struct device *dev)
{
	(void)dev;
	if (!sim_fw_file || strcmp(name, sim_fw_name))
		return -ENOENT;
	*fw = sim_fw_file;
	return 0;
}
static inline void release_firmware(const struct firmware *fw)
{
	(void)fw;
}

/* ---- sha256 (compact, public-domain shape; self-tested in the
 * harness against the FIPS 180-2 "abc" vector before any pin runs) ----
 */
#define SHA256_DIGEST_SIZE 32
void sha256(const u8 *data, size_t len, u8 *out);

/* ---- bit iteration ---- */
static inline unsigned long sim_find_bit(const unsigned long *addr,
					 unsigned long size,
					 unsigned long start)
{
	unsigned long i;

	for (i = start; i < size; i++)
		if ((*addr >> i) & 1UL)
			return i;
	return size;
}
#define find_first_bit(addr, size) sim_find_bit((addr), (size), 0)
#define find_next_bit(addr, size, off) sim_find_bit((addr), (size), (off))
#define for_each_set_bit(bit, addr, size)				\
	for ((bit) = find_first_bit((addr), (size)); (bit) < (size);	\
	     (bit) = find_next_bit((addr), (size), (bit) + 1))

/* ---- module plumbing ---- */
#define module_param(n, t, p)
#define MODULE_PARM_DESC(n, d)
#define module_init(f)
#define module_exit(f)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(t, x)
#define __init __maybe_unused
#define __exit __maybe_unused

#endif /* ANE_H16_SIM_SHIM_H */
