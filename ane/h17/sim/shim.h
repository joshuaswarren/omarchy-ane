/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sim/shim.h: userspace shim so the real ane_h17_wake.c compiles and runs
 * unmodified in the harness. A fake MMIO bus records every access; the
 * firmware model raises CPU_STATUS.RUNNING two polls after the RUN write,
 * publishes the wake word in SCRATCH7 three polls after it, leaves one
 * mailbox message pending, and honours (or, in the stuck scenario, ignores)
 * the RUN clear; the doorbell model acknowledges a ring two polls after it
 * and publishes the wake word one poll after that. The PMGR word reads
 * 0x23ff while the domain is voted on.
 * A sim is not silicon: it proves control flow, ordering and gating of this
 * module, never T8140 behaviour.
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
typedef int8_t s8;
typedef int32_t s32;

#define __iomem
#define GFP_KERNEL 0
#define EINVAL 22
#define ENOMEM 12
#define ENODEV 19
#define EIO 5
#define EPERM 1
#define EBUSY 16

#define GENMASK(h, l) (((~0u) - (1u << (l)) + 1) & ((~0u) >> (31 - (h))))
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FIELD_GET(m, v) (((v) & (m)) >> (__builtin_ffs(m) - 1))

#define SIM_LOG_SZ (1 << 19)
extern char sim_log[SIM_LOG_SZ];
extern size_t sim_log_len;

static inline void sim_log_reset(void) { sim_log_len = 0; sim_log[0] = '\0'; }

static inline void rec_log(const char *fmt, ...)
{
	va_list ap, ap2;
	int n;

	va_start(ap, fmt);
	if (sim_log_len + 1 < SIM_LOG_SZ) {
		va_copy(ap2, ap);
		n = vsnprintf(sim_log + sim_log_len, SIM_LOG_SZ - sim_log_len, fmt, ap2);
		va_end(ap2);
		if (n > 0)
			sim_log_len += (size_t)n;
	}
	va_end(ap);
}

#define pr_err(...) rec_log(__VA_ARGS__)
#define dev_info(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_err(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_crit(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_warn(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)

struct device {
	int idx;
	void *pm_domain;
};
struct platform_device {
	struct device dev;
};

struct device_node {
	const char *name;
	const char *compatible;
	int available;
	u32 reg0;
	u32 reg1;		/* size or reg high half, sim-only */
	u32 phandle;		/* 0 = none */
	struct device_node *parent;
};

extern int sim_genpd_on;
extern int sim_genpd_disabled;
extern bool sim_j700;
extern unsigned long sim_registrations, sim_maps, sim_mmio_reads, sim_pmgr_reads;

static inline int of_device_is_available(struct device_node *n) { return n && n->available; }
static inline int of_device_is_compatible(struct device_node *n, const char *c)
{
	return n && n->compatible && strcmp(n->compatible, c) == 0;
}
static inline int of_machine_is_compatible(const char *c) { return sim_j700 && strcmp(c, "apple,j700") == 0; }
static inline void of_node_put(struct device_node *n) { (void)n; }

extern struct device_node sim_ane_node, sim_ps_node, sim_probe_node,
	sim_dart_nodes[3];
static inline struct device_node *of_find_node_by_path(const char *p)
{
	if (strcmp(p, "/soc/ane@400000000") == 0)
		return &sim_ane_node;
	if (strcmp(p, "/soc/power-management@300700000/power-controller@290") == 0)
		return &sim_ps_node;
	if (strcmp(p, "/soc/iommu@401800000") == 0)
		return &sim_dart_nodes[0];
	if (strcmp(p, "/soc/iommu@401820000") == 0)
		return &sim_dart_nodes[1];
	if (strcmp(p, "/soc/iommu@401840000") == 0)
		return &sim_dart_nodes[2];
	return NULL;
}
static inline struct device_node *of_find_compatible_node(struct device_node *from,
							  const char *type,
							  const char *compat)
{
	(void)from; (void)type;
	if (strcmp(compat, "omarchy,t8140-ane-ps-probe") == 0)
		return &sim_probe_node;
	return NULL;
}
static inline struct device_node *of_parse_phandle(struct device_node *n,
						   const char *prop, int idx)
{
	(void)prop; (void)idx;
	if (n == &sim_probe_node)
		return &sim_ps_node;
	return NULL;
}
static inline int of_property_read_u32(struct device_node *n, const char *p, u32 *v)
{
	if (n == &sim_ps_node && strcmp(p, "reg") == 0) {
		*v = 0x290;
		return 0;
	}
	return -EINVAL;
}

struct resource {
	u64 start;
	u64 size;
};
static inline u64 resource_size(struct resource *r) { return r->size; }
static inline int of_address_to_resource(struct device_node *n, int idx, struct resource *r)
{
	if (n == &sim_ane_node && idx == 0) {
		r->start = 0x400000000ull;
		r->size = 0x2000000ull;
		return 0;
	}
	return -EINVAL;
}

/* fake PMGR regmap: word at 0x290 is 0x23ff only while the domain is on;
 * the child state at 0xc008 reads 0x00 until the module writes 0xf to it,
 * after which it reads 0x2f (TARGET 0xf, ACTUAL 0xf). */
struct regmap;
extern unsigned long sim_regmap_writes;
extern u32 sim_child_word;
static inline int regmap_read(struct regmap *m, u32 off, u32 *v)
{
	(void)m;
	sim_pmgr_reads++;
	if (off == 0x290) {
		*v = sim_genpd_on ? 0x23ff : 0x2000;
		return 0;
	}
	if (off == 0xc008) {
		*v = sim_child_word;
		return 0;
	}
	*v = 0;
	return 0;
}
static inline int regmap_write(struct regmap *m, u32 off, u32 v)
{
	(void)m;
	sim_regmap_writes++;
	if (off == 0xc008 && (v & 0xf) == 0xf)
		sim_child_word = 0xff;
	return 0;
}

static inline struct regmap *syscon_node_to_regmap(struct device_node *n)
{
	(void)n;
	return (struct regmap *)1;
}

#define PTR_ERR(p) ((long)(p))
#define IS_ERR(p) ((unsigned long)(p) > ~(unsigned long)PTR_ERR_MAX)
#define PTR_ERR_MAX 0xffffffffUL
static inline void *ERR_PTR(long e) { return (void *)(long)e; }
static inline void *devm_kzalloc(struct device *d, size_t sz, int gfp)
{ (void)d; (void)gfp; return calloc(1, sz); }

/* fake genpd */
static inline int pm_runtime_enable(struct device *d) { (void)d; sim_genpd_disabled = 0; return 0; }
static inline int pm_runtime_resume_and_get(struct device *d)
{
	(void)d;
	if (sim_genpd_disabled)
		return -EINVAL;
	sim_genpd_on++;
	rec_log("sim: genpd on=%d\n", sim_genpd_on);
	return 0;
}
static inline int pm_runtime_put_sync(struct device *d)
{
	(void)d;
	sim_genpd_on--;
	rec_log("sim: genpd on=%d\n", sim_genpd_on);
	return 0;
}
static inline int pm_runtime_disable(struct device *d)
{
	(void)d;
	sim_genpd_disabled = 1;
	return 0;
}

/* fake MMIO engine + firmware model */
enum {
	H17_OFF_CPU_CONTROL = 0x1600044,
	H17_OFF_CPU_STATUS = 0x1600048,
};
struct sim_fw {
	u32 cpu_control;
	u32 cpu_status;
	u32 scratch[10];	/* 0x1880020 .. 0x1880044; 7..9 = the wake-word
				 * views of both scratch0_off alignments */
	u32 a2i;
	u32 i2a;
	int t;
	int run_t0, clear_t0;
	int run_requested, clear_requested;
	int stuck;
	int pend_msg;
	u32 recv0, recv1;
	u32 doorbell_state0;	/* engine + 0x18a4010, & 3 */
	u32 doorbell_state1;	/* engine + 0x18a4018 */
	u32 doorbell_rung;	/* last value written to engine + 0x18a4000 */
	int ring_requested, ring_t0;
	int ring_acked;		/* engine + 0x18a4008 reads back ring_val */
	u32 ring_val;
};
extern struct sim_fw sim_fw;
extern unsigned long sim_setup_writes;

static inline void sim_apply(void)
{
	if (sim_fw.run_requested && sim_fw.t >= sim_fw.run_t0 + 2)
		sim_fw.cpu_status = 0x8;
	if (sim_fw.run_requested && sim_fw.t >= sim_fw.run_t0 + 3)
		sim_fw.scratch[7] = 0x08042006;
	if (sim_fw.run_requested && sim_fw.t >= sim_fw.run_t0 + 2)
		sim_fw.pend_msg = 1;
	if (sim_fw.clear_requested && !sim_fw.stuck && sim_fw.t >= sim_fw.clear_t0 + 2)
		sim_fw.cpu_status = 0xa;
	if (sim_fw.ring_requested && sim_fw.t >= sim_fw.ring_t0 + 2)
		sim_fw.ring_acked = 1;
	if (sim_fw.ring_requested && sim_fw.t >= sim_fw.ring_t0 + 3) {
		/* published at both scratch0_off alignments' SCRATCH7 words */
		sim_fw.scratch[7] = 0x08042006;
		sim_fw.scratch[8] = 0x08042006;
		sim_fw.scratch[9] = 0x08042006;
		sim_fw.pend_msg = 1;
		sim_fw.i2a |= 1;	/* the mailbox enable rises with the wake */
	}
}

static inline void sim_msleep(unsigned int ms)
{
	(void)ms;
	sim_fw.t++;
	sim_apply();
}

#define msleep(ms) sim_msleep(ms)
#define udelay(us) do {} while (0)

extern unsigned long sim_writes_ctl;
extern void __iomem *sim_engine_base;

static inline u32 sim_read(u32 off)
{
	switch (off) {
	case 0x1600044:
		return sim_fw.cpu_control;
	case 0x1600048:
		return sim_fw.cpu_status;
	case 0x18a4000:
		return sim_fw.doorbell_rung;
	case 0x18a4008:
		return sim_fw.ring_acked ? sim_fw.ring_val : 0;
	case 0x18a4010:
		return sim_fw.doorbell_state0;
	case 0x18a4018:
		return sim_fw.doorbell_state1;
	case 0x18ac000:
		return 0;	/* IPI pending: none */
	case 0x1608110:
	case 0x196c110:
		return sim_fw.a2i;
	case 0x1608114:
	case 0x196c114:
		return sim_fw.i2a | (sim_fw.pend_msg ? 0 : 0x20000);
	case 0x1608830:
	case 0x196c830:
		sim_fw.i2a |= 0x20000; /* message popped */
		return sim_fw.recv0;
	case 0x1608838:
	case 0x196c838:
		sim_fw.pend_msg = 0;
		return sim_fw.recv1;
	default:
		if (off >= 0x1880020 && off < 0x1880020 + 10 * 4)
			return sim_fw.scratch[(off - 0x1880020) / 4];
		return 0;
	}
}

static inline void sim_write(u32 off, u32 v)
{
	if (off == 0x1600044) {
		sim_writes_ctl++;
		if ((v & 0x10) && !(sim_fw.cpu_control & 0x10)) {
			sim_fw.run_requested = 1;
			sim_fw.clear_requested = 0;
			sim_fw.run_t0 = sim_fw.t;
		} else if (!(v & 0x10) && (sim_fw.cpu_control & 0x10)) {
			sim_fw.clear_requested = 1;
			sim_fw.clear_t0 = sim_fw.t;
		}
		sim_fw.cpu_control = v;
	} else if (off == 0x18a4000) {
		sim_fw.doorbell_rung = v;
		if (v && !sim_fw.ring_requested) {
			sim_fw.ring_requested = 1;
			sim_fw.ring_val = v;
			sim_fw.ring_t0 = sim_fw.t;
		}
	} else if (off == 0x1960000 || off == 0x1974200 ||
		   off == 0x195c284 || off == 0x195c288) {
		sim_setup_writes++;
	}
}

static inline u32 readl(const void __iomem *addr)
{
	u32 off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);
	sim_mmio_reads++;
	rec_log("sim: read off=%#x\n", off);
	return sim_read(off);
}
static inline void writel(u32 v, void __iomem *addr)
{
	u32 off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);
	rec_log("sim: write off=%#x val=%#x\n", off, v);
	sim_write(off, v);
}
/* The HAL's doorbell words are 64-bit (ldr/str): the module's readq/writeq
 * reach the same fake bus. */
static inline u64 readq(const void __iomem *addr)
{
	u32 off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);

	sim_mmio_reads++;
	rec_log("sim: readq off=%#x\n", off);
	return sim_read(off);
}
static inline void writeq(u64 v, void __iomem *addr)
{
	u32 off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);

	rec_log("sim: writeq off=%#x val=%#llx\n", off, (unsigned long long)v);
	sim_write(off, (u32)v);
}
extern void __iomem *sim_engine_base;
#define SIM_MMIO_BASE 0x40000000ull
static inline void __iomem *devm_ioremap_resource(struct device *d, struct resource *r)
{
	(void)d; (void)r;
	sim_maps++;
	return (void __iomem *)(uintptr_t)SIM_MMIO_BASE;
}

struct of_device_id {
	const char *compatible;
};
struct platform_driver {
	int (*probe)(struct platform_device *);
	struct {
		const char *name;
		const struct of_device_id *of_match_table;
	} driver;
};
static struct platform_device sim_pdev = { { 0, (void *)1 } };

static inline int platform_driver_register(struct platform_driver *drv)
{
	sim_registrations++;
	return drv->probe(&sim_pdev);
}
static inline void platform_driver_unregister(struct platform_driver *drv) { (void)drv; }

#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#define __init
#define __exit
#define module_init(fn) static int (*sim_init_fn)(void) = fn
#define module_exit(fn) static void (*sim_exit_fn)(void) = fn
#define MODULE_LICENSE(lic)
#define MODULE_DESCRIPTION(desc)
#define MODULE_DEVICE_TABLE(of, table)

#endif /* SIM_SHIM_H */
