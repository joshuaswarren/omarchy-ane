/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sim/shim.h: userspace shim so the real ane_h17_wake.c compiles and
 * runs unmodified. The gate is 0, so only the rehearsal and refusal
 * paths execute here; the engine paths exist to prove they compile and
 * are exercised by the gate-removal mutant. A sim is not silicon: it
 * proves control flow, ordering and gating of this module, never T8140
 * behaviour.
 */
#ifndef SIM_SHIM_H
#define SIM_SHIM_H

#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int32_t s32;
typedef int64_t loff_t;

#define __iomem
#define GFP_KERNEL 0
#define EINVAL 22
#define ENOMEM 12
#define ENODEV 19
#define EIO 5
#define EPERM 1
#define EBUSY 16
#define EOPNOTSUPP 95
#define O_APPEND 0x400
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_WRONLY 1
#define O_SYNC 0x101000

#define GENMASK(h, l) (((~0u) - (1u << (l)) + 1) & ((~0u) >> (31 - (h))))
#define BIT(n) (1u << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FIELD_GET(m, v) (((v) & (m)) >> (__builtin_ffs(m) - 1))
#define WRITE_ONCE(x, v) (x = (v))
#define READ_ONCE(x) (x)

#define SIM_LOG_SZ (1 << 19)
extern char sim_log[SIM_LOG_SZ];
extern size_t sim_log_len;
extern bool sim_j700;

static inline void rec_log(const char *fmt, ...)
{
	va_list ap;
	int n;
	size_t left = SIM_LOG_SZ - sim_log_len;

	if (left <= 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(sim_log + sim_log_len, left, fmt, ap);
	va_end(ap);
	if (n > 0) {
		sim_log_len += (size_t)n;
		if (sim_log_len > SIM_LOG_SZ - 1)
			sim_log_len = SIM_LOG_SZ - 1;
	}
}

static inline bool of_machine_is_compatible(const char *name)
{
	return sim_j700 && strcmp(name, "apple,j700") == 0;
}

#define pr_err(...) rec_log(__VA_ARGS__)
#define pr_info(...) rec_log(__VA_ARGS__)
#define dev_info(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_err(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_warn(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_crit(d, ...) do { (void)(d); rec_log(__VA_ARGS__); } while (0)

extern int sim_panic_count;
extern jmp_buf sim_panic_jb;
extern unsigned long sim_registrations, sim_maps;
#define panic(...) do { rec_log(__VA_ARGS__); sim_panic_count++; \
			longjmp(sim_panic_jb, 1); } while (0)

/* kernel marker file */
struct file;
extern unsigned long sim_marker_opens, sim_marker_writes, sim_marker_closed;
static inline struct file *filp_open(const char *path, int flags, int mode)
{
	(void)path; (void)flags; (void)mode;
	sim_marker_opens++;
	return (struct file *)1;
}
static inline ssize_t kernel_write(struct file *f, const char *buf, size_t len,
				   loff_t *pos)
{
	(void)f; (void)pos;
	sim_marker_writes++;
	rec_log("sim: marker %.*s", (int)(len ? len - 1 : 0), buf);
	return (ssize_t)len;
}
static inline int filp_close(struct file *f, void *cred)
{
	(void)f; (void)cred;
	sim_marker_closed++;
	return 0;
}

/* kernel timers */
struct timer_list {
	int armed;
	unsigned long expires;
};
#define TIMER_PINNED 0
extern unsigned long sim_timer_armed, sim_timer_disarmed, sim_timer_mods;
unsigned long sim_jiffies;
#define jiffies sim_jiffies
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static inline void timer_setup(struct timer_list *t,
			       void (*fn)(struct timer_list *), int flags)
{
	(void)fn; (void)flags;
	t->armed = 0;
}
static inline void add_timer_on(struct timer_list *t, int cpu)
{
	(void)cpu;
	t->armed = 1;
	sim_timer_armed++;
}
static inline int mod_timer(struct timer_list *t, unsigned long expires)
{
	(void)expires;
	if (!t->armed)
		sim_timer_mods++;
	return 0;
}
static inline int timer_delete_sync(struct timer_list *t)
{
	int was = t->armed;

	t->armed = 0;
	if (was)
		sim_timer_disarmed++;
	return was;
}

/* cpus */
#define nr_cpu_ids 4U
extern int sim_processor_id;
static inline int smp_processor_id(void) { return sim_processor_id; }
static inline int num_online_cpus(void) { return nr_cpu_ids; }
typedef struct { unsigned int bits; } cpumask_t;
typedef cpumask_t cpumask;
#define cpu_online_mask ((const cpumask *)0)
static inline unsigned int cpumask_next(unsigned int n, const cpumask *s)
{
	(void)s;
	return n + 1 < nr_cpu_ids ? n + 1 : nr_cpu_ids;
}
static inline unsigned int cpumask_first(const cpumask *s)
{
	(void)s;
	return 0;
}
static inline bool cpu_online(unsigned int cpu) { return cpu < nr_cpu_ids; }

/* kthreads + completion: the shim runs the thread body synchronously at
 * wake, so a gate-1 mutant exercises the whole engine phase inline. */
struct task_struct;
extern int (*sim_kthread_fn)(void *);
extern void *sim_kthread_data;
static inline struct task_struct *kthread_create_on_cpu(
	int (*fn)(void *), void *data, int cpu, const char *name)
{
	(void)cpu; (void)name;
	sim_kthread_fn = fn;
	sim_kthread_data = data;
	return (struct task_struct *)fn;
}
static inline int wake_up_process(struct task_struct *t)
{
	(void)t;
	return sim_kthread_fn(sim_kthread_data);
}
struct completion {
	int done;
};
static inline void init_completion(struct completion *c) { c->done = 0; }
static inline void complete(struct completion *c) { c->done = 1; }
static inline void wait_for_completion(struct completion *c) { (void)c; }

/* device tree */
extern u32 sim_wdt_regs[12];
struct device_node {
	const char *name;
	const char *compatible;
	int available;
	struct device_node *parent;
};
static inline int of_device_is_available(struct device_node *n)
{
	return n && n->available;
}
static inline int of_device_is_compatible(struct device_node *n, const char *c)
{
	return n && n->compatible && strcmp(n->compatible, c) == 0;
}
static inline void of_node_put(struct device_node *n) { (void)n; }
extern struct device_node sim_ane_node, sim_ps_node, sim_probe_node,
	sim_dart_nodes[3], sim_wdt_node;
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
static inline struct device_node *of_find_compatible_node(
	struct device_node *from, const char *type, const char *compat)
{
	(void)from; (void)type;
	if (strcmp(compat, "omarchy,t8140-ane-ps-probe") == 0)
		return &sim_probe_node;
	if (strcmp(compat, "apple,t8140-wdt") == 0)
		return &sim_wdt_node;
	return NULL;
}
struct resource {
	u64 start;
	u64 size;
};
static inline u64 resource_size(struct resource *r) { return r->size; }
static inline int of_address_to_resource(struct device_node *n, int idx,
					 struct resource *r)
{
	(void)idx;
	if (n == &sim_ane_node) {
		r->start = 0x400000000ull;
		r->size = 0x2000000ull;
		return 0;
	}
	return -EINVAL;
}
static inline void __iomem *of_iomap(struct device_node *np, int index)
{
	(void)np; (void)index;
	return (void __iomem *)sim_wdt_regs;
}
static inline void iounmap(void __iomem *addr) { (void)addr; }

/* regmap (PMGR) */
struct regmap;
extern int sim_genpd_on, sim_genpd_disabled;
extern unsigned long sim_regmap_writes, sim_pmgr_reads;
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

/* genpd */
static inline int pm_runtime_enable(void *dev)
{
	(void)dev;
	sim_genpd_disabled = 0;
	return 0;
}
static inline int pm_runtime_resume_and_get(void *dev)
{
	(void)dev;
	if (sim_genpd_disabled)
		return -EINVAL;
	sim_genpd_on++;
	rec_log("sim: genpd on=%d\n", sim_genpd_on);
	return 0;
}
static inline int pm_runtime_put_sync(void *dev)
{
	(void)dev;
	sim_genpd_on--;
	rec_log("sim: genpd on=%d\n", sim_genpd_on);
	return 0;
}
static inline void pm_runtime_disable(void *dev)
{
	(void)dev;
	sim_genpd_disabled = 1;
}

/* fake MMIO engine + watchdog registers */
extern void __iomem *sim_engine_base;
extern unsigned long sim_mmio_reads, sim_mmio_writes;
static inline bool sim_is_wdt(const void __iomem *addr)
{
	return (uintptr_t)addr >= (uintptr_t)sim_wdt_regs &&
	       (uintptr_t)addr < (uintptr_t)sim_wdt_regs + sizeof(sim_wdt_regs);
}
static inline void *__iomem devm_ioremap_resource(void *dev, struct resource *r)
{
	(void)dev; (void)r;
	return sim_engine_base;
}
static inline u32 sim_read(u32 off)
{
	(void)off;
	return 0;
}
static inline void sim_write(u32 off, u32 v)
{
	(void)off; (void)v;
}
static inline u32 readl(const void __iomem *addr)
{
	u32 off;

	if (sim_is_wdt(addr))
		return *(const volatile u32 *)addr;
	off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);
	sim_mmio_reads++;
	rec_log("sim: read off=%#x\n", off);
	return sim_read(off);
}
static inline void writel(u32 v, void __iomem *addr)
{
	u32 off;

	if (sim_is_wdt(addr)) {
		*(volatile u32 *)addr = v;
		return;
	}
	off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);
	sim_mmio_writes++;
	rec_log("sim: write off=%#x val=%#x\n", off, v);
	sim_write(off, v);
}
static inline u64 readq(const void __iomem *addr)
{
	u32 off;

	if (sim_is_wdt(addr))
		return *(const volatile u64 *)addr;
	off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);
	sim_mmio_reads++;
	rec_log("sim: readq off=%#x\n", off);
	return sim_read(off);
}
static inline void writeq(u64 v, void __iomem *addr)
{
	u32 off;

	if (sim_is_wdt(addr)) {
		*(volatile u64 *)addr = v;
		return;
	}
	off = (u32)((uintptr_t)addr - (uintptr_t)sim_engine_base);
	sim_mmio_writes++;
	rec_log("sim: writeq off=%#x val=%#llx\n", off, (unsigned long long)v);
	sim_write(off, (u32)v);
}
static inline u32 readl_relaxed(const void __iomem *addr) { return readl(addr); }
static inline void writel_relaxed(u32 v, void __iomem *addr) { writel(v, addr); }
static inline void msleep(unsigned int ms)
{
	(void)ms;
	sim_wdt_regs[4]++;	/* WD1 CUR_TIME ticks */
}

#define PTR_ERR(p) ((long)(p))
#define IS_ERR(p) ((unsigned long)(p) > ~(unsigned long)0xffffffffUL)
static inline void *ERR_PTR(long e) { return (void *)(long)e; }
static inline void *devm_kzalloc(void *d, size_t sz, int gfp)
{
	(void)d; (void)gfp;
	return calloc(1, sz);
}

struct device {
	int idx;
	void *pm_domain;
};
struct platform_device {
	struct device dev;
};
static struct platform_device sim_pdev = { { 0, (void *)1 } };

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
static inline int platform_driver_register(struct platform_driver *drv)
{
	sim_registrations++;
	return drv->probe(&sim_pdev);
}
static inline void platform_driver_unregister(struct platform_driver *drv)
{
	(void)drv;
}
extern unsigned long sim_registrations, sim_maps;

#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#define __init
#define __exit
#define module_init(fn) static int (*sim_init_fn)(void) = fn
#define module_exit(fn) static void (*sim_exit_fn)(void) = fn
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(of, table)
#endif /* SIM_SHIM_H */
