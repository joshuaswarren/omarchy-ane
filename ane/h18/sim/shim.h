/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sim/shim.h: userspace shim so the real ane_h18_main.c and ane_h18_soc.c compile and run unmodified in the
 * harness. A fake MMIO bus records every read and write; the PSD slot windows behave like the hardware
 * as pmgr-psd.c documents it (a REQUEST write sets VALID and STATUS, a STATUS read clears VALID), and the
 * ASC, IORVBAR and mailbox windows fault (return 0xdeadbeef and count a fault) unless the PSD slots they
 * depend on are running. A sim is not silicon: it proves control flow, ordering and gating of this module,
 * not any M6 behavior.
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
#include <pthread.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define __iomem

#define SIM_LOG_SZ (1 << 18)
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

#define dev_info(d, ...)  do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_err(d, ...)   do { (void)(d); rec_log("ERR: " __VA_ARGS__); } while (0)
#define dev_warn(d, ...)  do { (void)(d); rec_log("WARN: " __VA_ARGS__); } while (0)
#define dev_crit(d, ...)  do { (void)(d); rec_log("CRIT: " __VA_ARGS__); } while (0)
#define dev_emerg(d, ...) do { (void)(d); rec_log("EMERG: " __VA_ARGS__); } while (0)

/* ---- fake device tree node ---- */
#define SIM_MAX_REG 8
struct device_node {
	int n_reg;
	u64 reg_pa[SIM_MAX_REG];
	u64 reg_size[SIM_MAX_REG];
	const char *reg_name[SIM_MAX_REG];
	bool has_type;
	u32 ane_type;
	bool has_num;
	u32 mbox_num;
	bool has_power_domains;
	bool has_iommus;
	/* pmp phandle-registry node: what of_find_node_by_phandle hands back */
	u32 phandle;		/* nonzero: this node is registered under sim_phandle */
	const char *compat;	/* first compatible string, NULL = none */
	u32 reg_val;		/* the "reg" cell of_property_read_u32(np, "reg") returns */
	int refs;
};
/* the phandles the sim's live tree resolves (0 = slot empty); the t8132 node claims
 * all three slots (the ps_ane_mpm/tdbase/cpu pmgr pwrstate domains) */
#define SIM_PHANDLE_SLOTS 3
extern u32 sim_phandle[SIM_PHANDLE_SLOTS];
extern struct device_node *sim_phandle_target[SIM_PHANDLE_SLOTS];
extern int sim_pd_attached, sim_pd_false_votes;	/* the genpd claim list seam, wired in the harness */
extern struct device *sim_pdev_dev;
extern u64 sim_gate_pa[2];			/* stage=reads seams: the pmgr gate-state pages */
extern u32 sim_gate_val[2];
extern bool sim_ps_on;				/* the four ANE ps registers read fully on when set */

static inline struct device_node *of_find_node_by_phandle(u32 ph)
{
	int i;

	for (i = 0; i < SIM_PHANDLE_SLOTS; i++)
		if (ph && ph == sim_phandle[i] && sim_phandle_target[i])
			return sim_phandle_target[i];
	return NULL;
}
static inline bool of_device_is_compatible(const struct device_node *n, const char *s)
{
	return n && n->compat && !strcmp(n->compat, s);
}
static inline void of_node_put(struct device_node *n) { if (n) n->refs--; }
static inline int of_property_read_u32_index(const struct device_node *n, const char *name,
					     int index, u32 *v)
{
	if (index >= 0 && index < SIM_PHANDLE_SLOTS && !strcmp(name, "power-domains") &&
	    n->has_power_domains && n->phandle && sim_phandle[index]) {
		*v = sim_phandle[index];
		return 0;
	}
	return -EINVAL;
}


struct device { struct device_node *of_node; };
struct platform_device { struct device dev; void *drvdata; };
struct resource { u64 start; u64 end; };

static inline u64 resource_size(const struct resource *r) { return r->end - r->start + 1; }

#define IORESOURCE_MEM 0x00000200

static inline struct resource *platform_get_resource(struct platform_device *pdev, unsigned int type, unsigned int num)
{
	struct device_node *np = pdev->dev.of_node;
	static struct resource res[8];

	(void)type;
	if (!np || num >= (unsigned int)np->n_reg || !np->reg_size[num] || num >= 8)
		return NULL;
	res[num].start = np->reg_pa[num];
	res[num].end = np->reg_pa[num] + np->reg_size[num] - 1;
	return &res[num];
}

static inline int of_property_read_u32(const struct device_node *n, const char *name, u32 *v)
{
	if (!strcmp(name, "apple,ane-type") && n->has_type) { *v = n->ane_type; return 0; }
	if (!strcmp(name, "msgbox-mailbox-num") && n->has_num) { *v = n->mbox_num; return 0; }
	if (!strcmp(name, "reg") && n->phandle) { *v = n->reg_val; return 0; }
	return -EINVAL;
}

static inline bool of_property_present(const struct device_node *n, const char *name)
{
	return (!strcmp(name, "power-domains") && n->has_power_domains) || (!strcmp(name, "iommus") && n->has_iommus);
}

#define MODULE_STATE_LIVE 0
struct module { int state, refs; };
static struct module sim_owner_module;
#define THIS_MODULE (&sim_owner_module)
#define __module_get(m) ((void)((m)->refs++))
#define module_put(m) ((void)((m)->refs--))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static inline int of_property_match_string(const struct device_node *n, const char *prop, const char *s)
{
	int i;

	if (strcmp(prop, "reg-names"))
		return -EINVAL;
	for (i = 0; i < n->n_reg; i++)
		if (n->reg_name[i] && !strcmp(n->reg_name[i], s))
			return i;
	return -ENODATA;
}

static inline int of_address_to_resource(struct device_node *n, int idx, struct resource *r)
{
	if (idx < 0 || idx >= n->n_reg || !n->reg_size[idx])
		return -EINVAL;
	r->start = n->reg_pa[idx];
	r->end = n->reg_pa[idx] + n->reg_size[idx] - 1;
	return 0;
}

struct of_device_id { const char *compatible; const void *data; };
struct driver_shim { const char *name; bool suppress_bind_attrs; const struct of_device_id *of_match_table; };
struct platform_driver {
	struct driver_shim driver;
	int (*probe)(struct platform_device *pdev);
	void (*remove)(struct platform_device *pdev);
};

#define module_param(n, t, p)
#define MODULE_PARM_DESC(n, d)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define module_platform_driver(x) extern char __sim_kmod_noop

#define GFP_KERNEL 0
#define SZ_16K 0x4000UL
#define GFP_ZERO_UNUSED 0
static inline void *kzalloc(size_t n, int gfp) { (void)gfp; return calloc(1, n); }
static inline void kfree(const void *p) { free((void *)p); }
/* request_firmware: the harness hands out sim_fw_data (sim_fw_size bytes) unless sim_fw_err is set */
struct firmware { size_t size; const u8 *data; };
extern const u8 *sim_fw_data;
extern size_t sim_fw_size;
extern int sim_fw_err, sim_fw_released, sim_fw_requests;
static inline int request_firmware(const struct firmware **fw, const char *name, struct device *dev)
{
	static struct firmware f;

	(void)name; (void)dev;
	sim_fw_requests++;
	if (sim_fw_err)
		return sim_fw_err;
	f.size = sim_fw_size;
	f.data = sim_fw_data;
	*fw = &f;
	return 0;
}
static inline void release_firmware(const struct firmware *fw)
{
	(void)fw;
	sim_fw_released++;
}
#define PAGE_SIZE 0x4000UL
#define ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))
#define wmb() do { } while (0)
#define dma_wmb() do { } while (0)
#define dma_rmb() do { } while (0)
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x) *)&(x) = (v))
#ifndef BIT
#define BIT(n) (1U << (n))
#endif
typedef u64 dma_addr_t;
#define DMA_BIT_MASK(n) ((1ULL << (n)) - 1)
struct sim_dma { void *va; dma_addr_t iova; size_t size; int live; };
#define SIM_NDMA 32
extern struct sim_dma sim_dma[SIM_NDMA];
extern int sim_n_dma, sim_dma_fail, sim_dma_mask_bits;
static inline int dma_set_mask_and_coherent(struct device *d, u64 mask)
{
	(void)d;
	sim_dma_mask_bits = 64 - __builtin_clzll(mask);
	return 0;
}
static inline void *dma_alloc_coherent(struct device *d, size_t size, dma_addr_t *iova, int gfp)
{
	(void)d; (void)gfp;
	if (sim_dma_fail == sim_n_dma + 1 || sim_n_dma >= SIM_NDMA)
		return NULL;
	sim_dma[sim_n_dma].va = calloc(1, size);
	sim_dma[sim_n_dma].iova = 0x3ff00000000ULL - 0x4000000ULL * (sim_n_dma + 1);
	sim_dma[sim_n_dma].size = size;
	sim_dma[sim_n_dma].live = 1;
	*iova = sim_dma[sim_n_dma].iova;
	return sim_dma[sim_n_dma++].va;
}
static inline void dma_free_coherent(struct device *d, size_t size, void *va, dma_addr_t iova)
{
	int k;

	(void)d; (void)size;
	for (k = 0; k < sim_n_dma; k++)
		if (sim_dma[k].va == va && sim_dma[k].iova == iova)
			sim_dma[k].live = 0;
}
static inline void *devm_kzalloc(struct device *d, size_t n, int f) { (void)d; (void)f; return calloc(1, n); }
static inline void platform_set_drvdata(struct platform_device *p, void *d) { p->drvdata = d; }
static inline void *platform_get_drvdata(struct platform_device *p) { return p->drvdata; }

/* ---- fake MMIO bus ---- */
#define SIM_MAX_EVENTS 8192
struct sim_event { int write; u64 pa; u32 val; };
extern struct sim_event sim_events[SIM_MAX_EVENTS];
extern int sim_n_events;
extern int sim_faults;
extern int sim_n_iomap, sim_n_iounmap;

#define SIM_NPSD 2
#define SIM_PSD_BASE 0x303a10000ULL
/* per-slot behavior */
enum { SIM_PSD_ACK = 0, SIM_PSD_NEVER = 1, SIM_PSD_WRONG_STATE = 2, SIM_PSD_IGNORE6 = 3 };
extern int sim_psd_mode[SIM_NPSD];
extern u32 sim_psd_slot[SIM_NPSD];	/* slot numbers, set by the harness */
extern u32 sim_psd_state[SIM_NPSD];	/* current state, 6 at start */
extern u32 sim_psd_valid[SIM_NPSD];
extern u32 sim_psd_initial_valid[SIM_NPSD];
/* ASC core model for stage boot */
extern u32 sim_cpu_status, sim_cpu_ctrl, sim_run_status;
extern u64 sim_rvbar_val;
extern int sim_core_running, sim_core_stuck;
extern int sim_ctrl_writes;
#define SIM_PO_CH 600	/* the alias bank channel of ANE0 is window offset 0x10c00 = channel 536 */
#define SIM_DART_PA 0x419800000ULL
#define SIM_SCRATCH_PA 0x4198a0000ULL
#define SIM_SCRATCH_SIZE 0x1000
#define SIM_DART_SIZE 0xc000
#define SIM_PO_Q 24
extern u32 sim_po_msg[SIM_PO_CH][SIM_PO_Q][4];
extern int sim_po_n[SIM_PO_CH], sim_po_head[SIM_PO_CH];
extern int sim_start_chan;		/* channel that receives sim_start_msgs messages when RUN is set; -1 = none */
extern int sim_start_msgs;
extern int sim_start_hello;		/* RUN queues an RTKit HELLO (min 11, max 12) and a last EPMAP (base 0, bitmap 3) on the alias channel */
extern u32 sim_po_out_off, sim_alias_credits, sim_hello_min, sim_hello_max, sim_hello_ep, sim_hello_type;
/* sim_start_hello == 3: a firmware that reacts to what the host writes to TX of the alias channel (RTKit management protocol) */
extern u32 sim_tx_w[4];
extern int sim_tx_got;
extern u64 sim_tx_msgs[32];
extern int sim_n_tx_msgs, sim_idle_sent, sim_stop_needs_idle, sim_no_shutdown_reply;
extern u32 sim_iop_ack_state;
extern u32 sim_scr[8];			/* modelled SCRATCH words when sim_scr_model is set */
extern int sim_scr_model;
extern void sim_hs_react(void);
extern void sim_s3_react(u32 v);		/* the modelled firmware sees the wake word */
#define SIM_DB_PA 0x4198a4000ULL
#define SIM_PEND_PA 0x4198ac000ULL
#define SIM_ACK_PA 0x4198b0000ULL
extern int sim_ack_writes;
extern u32 sim_ack_last;
extern void sim_db_react(u32 v);	/* the modelled firmware sees a doorbell write */
extern int sim_db_writes;
extern u32 sim_pend_val;
extern int sim_credit_mode;	/* 1: every host TX message uses the only free slot, the slot returns at the next sleep */
extern u32 sim_credits_now;
extern u64 sim_asc_pa, sim_rvbar_pa, sim_mbox_pa;
extern u32 sim_asc_size, sim_rvbar_size, sim_mbox_size;

static inline void sim_fw_enqueue(int ch, u64 msg0)
{
	int slot = (sim_po_head[ch] + sim_po_n[ch]) % SIM_PO_Q;

	sim_po_msg[ch][slot][0] = (u32)msg0;
	sim_po_msg[ch][slot][1] = (u32)(msg0 >> 32);
	sim_po_msg[ch][slot][2] = 0;
	sim_po_msg[ch][slot][3] = 0;
	sim_po_n[ch]++;
}

/* the host finished a 16 byte TX message: let the modelled firmware react */
static inline void sim_fw_react(u64 msg0)
{
	unsigned int type = (msg0 >> 52) & 0xff;
	u64 state = msg0 & 0xffff;

	if (sim_n_tx_msgs < 32)
		sim_tx_msgs[sim_n_tx_msgs] = msg0;
	sim_n_tx_msgs++;
	if (type == 2) {
		sim_fw_enqueue(536, (8ULL << 52) | 3);
		sim_fw_enqueue(536, (8ULL << 52) | (1ULL << 51) | (1ULL << 32) | 1);
	} else if (type == 8 && (msg0 & (1ULL << 51))) {
		sim_fw_enqueue(536, (7ULL << 52) | sim_iop_ack_state);
	} else if (type == 0xb) {
		if (state == 0x20 || !sim_no_shutdown_reply)
			sim_fw_enqueue(536, (0xbULL << 52) | state);
	} else if (type == 6 && (state == 1 || state == 0) && !sim_no_shutdown_reply) {
		sim_fw_enqueue(536, (7ULL << 52) | state);
		sim_idle_sent = 1;	/* the modelled core accepts the RUN clear once the IOP acknowledged the shutdown state */
	}
}

static inline int sim_psd_of(u64 pa, u32 *off)
{
	int i;

	for (i = 0; i < SIM_NPSD; i++) {
		u64 b = SIM_PSD_BASE + ((u64)sim_psd_slot[i] << 8);

		if (pa >= b && pa < b + 0x100) {
			*off = (u32)(pa - b);
			return i;
		}
	}
	return -1;
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

static inline bool sim_in(u64 pa, u64 base, u32 size) { return pa >= base && pa < base + size; }

static inline u32 bus_read(u64 pa)
{
	u32 off, v = 0xdeadbeef;
	int gi, i = sim_psd_of(pa, &off);

	for (gi = 0; gi < 2; gi++) {
		if (sim_gate_pa[gi] && pa >= sim_gate_pa[gi] && pa < sim_gate_pa[gi] + 0x1000) {
			v = sim_gate_val[gi];
			sim_event(0, pa, v);
			return v;
		}
	}
	if (pa >= 0x280900000ULL && pa < 0x280901000ULL) {
		/* the pmgr1 page: the four ANE power-state registers. TARGET is bits 3:0,
		 * ACTUAL is bits 7:4 (pmgr-pwrstate.c); 0xf0 = fully on, 0 = out of state */
		v = sim_ps_on ? 0xf0 : 0;
		sim_event(0, pa, v);
		return v;
	}

	if (i >= 0) {
		if (off == 0xbc) {
			v = sim_psd_valid[i];
		} else if (off == 0x80) {
			v = sim_psd_state[i];
			sim_psd_valid[i] = 0;		/* a STATUS read clears the acknowledge */
		} else if (off == 0xa0) {
			v = 0xd1a9;
		} else {
			v = 0;
		}
	} else if (sim_in(pa, sim_asc_pa, sim_asc_size) || sim_in(pa, sim_rvbar_pa, sim_rvbar_size) ||
		   sim_in(pa, sim_mbox_pa, sim_mbox_size)) {
		/* the ASC blocks need PSD slot 0 powered (any state but 6) */
		if (sim_psd_state[0] == 6) {
			sim_faults++;
		} else if (pa == sim_asc_pa + 0x48) {
			v = sim_cpu_status;
		} else if (pa == sim_asc_pa + 0x44) {
			v = sim_cpu_ctrl;
		} else if (pa == sim_rvbar_pa) {
			v = (u32)sim_rvbar_val;
		} else if (pa == sim_rvbar_pa + 4) {
			v = (u32)(sim_rvbar_val >> 32);
		} else if (sim_in(pa, sim_mbox_pa, sim_mbox_size)) {
			u64 o = pa - sim_mbox_pa;
			u32 ch = (u32)(o / 0x80), r = (u32)(o % 0x80);

			v = 0;
			if (ch < SIM_PO_CH) {
				u32 oo = ch == 536 ? sim_po_out_off : 0;

				if (r == 0x20) {
					v = (u32)(sim_po_n[ch] > 15 ? 15 : sim_po_n[ch]);	/* a 4-bit count */
					if (ch == 536)
						v |= (sim_credit_mode ? sim_credits_now : sim_alias_credits) << 8;
				} else if (r >= oo && r < oo + 0x10 && sim_po_n[ch] > 0) {
					v = sim_po_msg[ch][sim_po_head[ch]][(r - oo) / 4];
					if (r == oo + 0xc) {		/* the last word read pops the message */
						sim_po_head[ch]++;
						sim_po_n[ch]--;
					}
				}
			}
		} else {
			v = (pa & 0xffff) | 0x10000;	/* distinguishable fake data */
		}
	} else if (sim_in(pa, SIM_SCRATCH_PA, SIM_SCRATCH_SIZE)) {
		/* the engine block page is only reachable with both ANE PSD slots on */
		if (sim_psd_state[0] == 6 || sim_psd_state[1] == 6)
			sim_faults++;
		else if (sim_scr_model && pa >= SIM_SCRATCH_PA + 0x24 && pa < SIM_SCRATCH_PA + 0x44)
			v = sim_scr[(pa - SIM_SCRATCH_PA - 0x24) / 4];
		else
			v = 0x5c000000u | (u32)(pa & 0xffff);
	} else if (sim_in(pa, SIM_PEND_PA, 0x1000)) {
		/* the IPI pending page, readable with both PSD slots on */
		if (sim_psd_state[0] == 6 || sim_psd_state[1] == 6)
			sim_faults++;
		else
			v = sim_pend_val;
	} else if (sim_in(pa, SIM_DART_PA, SIM_DART_SIZE) || sim_in(pa, SIM_DART_PA + 0x20000, SIM_DART_SIZE) || sim_in(pa, SIM_DART_PA + 0x40000, SIM_DART_SIZE) || sim_in(pa, SIM_DART_PA + 0x60000, SIM_DART_SIZE)) {
		/* the DART block is only reachable with both ANE PSD slots on */
		if (sim_psd_state[0] == 6 || sim_psd_state[1] == 6)
			sim_faults++;
		else
			v = 0xda000000u | (u32)(pa & 0xffff);
	} else {
		sim_faults++;
	}
	sim_event(0, pa, v);
	return v;
}

static inline void bus_write(u64 pa, u32 v)
{
	u32 off;
	int i = sim_psd_of(pa, &off);

	sim_event(1, pa, v);
	if (i >= 0 && off == 0x90) {
		if (sim_psd_mode[i] == SIM_PSD_ACK) {
			sim_psd_state[i] = v;
			sim_psd_valid[i] = 1;
		} else if (sim_psd_mode[i] == SIM_PSD_WRONG_STATE) {
			sim_psd_state[i] = 4;
			sim_psd_valid[i] = 1;
		} else if (sim_psd_mode[i] == SIM_PSD_IGNORE6 && v != 6) {
			sim_psd_state[i] = v;
			sim_psd_valid[i] = 1;
		}
		return;
	}
	if (i < 0 && pa == sim_asc_pa + 0x44 && sim_psd_state[0] != 6) {
		sim_ctrl_writes++;
		sim_cpu_ctrl = v;
		if (v & 0x10) {
			sim_core_running = 1;
			sim_cpu_status = sim_run_status;
			if (sim_start_chan >= 0 && sim_start_hello == 3) {
				sim_po_n[sim_start_chan] = 0;
				sim_po_head[sim_start_chan] = 0;
				sim_fw_enqueue(sim_start_chan, ((u64)sim_hello_type << 52) | ((u64)sim_hello_max << 16) | sim_hello_min);
			} else if (sim_start_chan >= 0 && sim_start_hello) {
				u64 hello = ((u64)sim_hello_type << 52) | ((u64)sim_hello_max << 16) | sim_hello_min;
				u64 epmap = (8ULL << 52) | (1ULL << 51) | 3;

				sim_po_msg[sim_start_chan][0][0] = (u32)hello; sim_po_msg[sim_start_chan][0][1] = (u32)(hello >> 32);
				sim_po_msg[sim_start_chan][0][2] = sim_hello_ep; sim_po_msg[sim_start_chan][0][3] = 0;
				sim_po_msg[sim_start_chan][1][0] = (u32)epmap; sim_po_msg[sim_start_chan][1][1] = (u32)(epmap >> 32);
				sim_po_msg[sim_start_chan][1][2] = 0; sim_po_msg[sim_start_chan][1][3] = 0;
				sim_po_n[sim_start_chan] = 2;
				sim_po_head[sim_start_chan] = 0;
			} else if (sim_start_chan >= 0) {
				int k;

				for (k = 0; k < sim_start_msgs && sim_po_n[sim_start_chan] < SIM_PO_Q; k++) {
					int slot = (sim_po_head[sim_start_chan] + sim_po_n[sim_start_chan]) % SIM_PO_Q;

					sim_po_msg[sim_start_chan][slot][0] = 0x1000 + k;
					sim_po_msg[sim_start_chan][slot][1] = 0;
					sim_po_msg[sim_start_chan][slot][2] = 0x100;
					sim_po_msg[sim_start_chan][slot][3] = 0;
					sim_po_n[sim_start_chan]++;
				}
			}
		} else if (!sim_core_stuck && !(sim_stop_needs_idle && !sim_idle_sent)) {
			sim_core_running = 0;
			sim_cpu_status = 0xa;
		}
		return;
	}
	if (i < 0 && sim_in(pa, sim_mbox_pa, sim_mbox_size) && sim_psd_state[0] != 6) {
		/* an inbox write with the ANE slot on is a legal bus access; a modelled firmware reads TX of the alias channel */
		if (sim_start_hello == 3 && pa >= sim_mbox_pa + 0x10c10 && pa < sim_mbox_pa + 0x10c20) {
			sim_tx_w[(pa - sim_mbox_pa - 0x10c10) / 4] = v;
			if (pa == sim_mbox_pa + 0x10c1c) {
				sim_credits_now = 0;
				sim_fw_react(((u64)sim_tx_w[1] << 32) | sim_tx_w[0]);
			}
		}
		return;
	}
	if (i < 0 && sim_scr_model && sim_in(pa, SIM_SCRATCH_PA + 0x24, 0x20) && sim_psd_state[0] != 6 && sim_psd_state[1] != 6) {
		sim_scr[(pa - SIM_SCRATCH_PA - 0x24) / 4] = v;
		if (pa == SIM_SCRATCH_PA + 0x40)
			sim_hs_react();
		if (pa == SIM_SCRATCH_PA + 0x30)
			sim_s3_react(v);
		return;
	}
	if (i < 0 && sim_in(pa, SIM_ACK_PA, 0x1000) && sim_psd_state[0] != 6 && sim_psd_state[1] != 6) {
		sim_ack_writes++;
		sim_ack_last = v;
		return;
	}
	if (i < 0 && sim_in(pa, SIM_DB_PA, 0x1000) && sim_psd_state[0] != 6 && sim_psd_state[1] != 6) {
		sim_db_writes++;
		if (pa == SIM_DB_PA)
			sim_db_react(v);
		return;
	}
	if (i < 0)
		sim_faults++;
}

/* sim_find_node: the harness registers the node; of_iomap returns the physical address as the "mapping" */
extern int sim_iomap_fail_at;	/* fail the Nth of_iomap/ioremap call (0 = never) */

static inline void *of_iomap(struct device_node *n, int idx)
{
	if (!n || idx < 0 || idx >= n->n_reg || !n->reg_size[idx])
		return NULL;
	sim_n_iomap++;
	if (sim_iomap_fail_at && sim_n_iomap == sim_iomap_fail_at)
		return NULL;
	return (void *)(uintptr_t)n->reg_pa[idx];
}
static inline void *ioremap(u64 pa, u32 size)
{
	(void)size;
	sim_n_iomap++;
	if (sim_iomap_fail_at && sim_n_iomap == sim_iomap_fail_at)
		return NULL;
	return (void *)(uintptr_t)pa;
}
static inline void iounmap(void *p) { (void)p; sim_n_iounmap++; }

#define readl(a) bus_read((u64)(uintptr_t)(a))
#define writel(v, a) bus_write((u64)(uintptr_t)(a), (v))

/* ---- runtime PM and iommu stubs: calls are recorded, ordered against the MMIO events by event index ---- */
extern int sim_rpm_enabled, sim_rpm_gets, sim_rpm_puts, sim_rpm_get_fail;
static inline int devm_pm_runtime_enable(struct device *d) { (void)d; sim_rpm_enabled++; return 0; }
static inline int pm_runtime_get_sync(struct device *d)
{
	if (sim_pd_attached && d == sim_pdev_dev)
		sim_pd_false_votes++;	/* no auto domain with 2+ cells: this casts no vote */
	sim_rpm_gets++;
	return sim_rpm_get_fail ? -EIO : 0;
}
static inline void pm_runtime_put_sync(struct device *d) { (void)d; sim_rpm_puts++; }
static inline int pm_runtime_put_noidle(struct device *d) { (void)d; return 0; }
static inline int pm_runtime_resume_and_get(struct device *d)
{
	(void)d;
	sim_event(2, 0xe0, 0);
	if (sim_rpm_get_fail)
		return -ETIMEDOUT;
	sim_rpm_gets++;
	return 0;
}
static inline int pm_runtime_put(struct device *d) { (void)d; sim_rpm_puts++; return 0; }

extern int sim_count_error;	/* force of_count_phandle_with_args to return -EINVAL */
static inline int of_count_phandle_with_args(const struct device_node *n,
					     const char *name, const char *cells)
{
	int i, cnt = 0;

	(void)name; (void)cells;
	if (!n || !n->has_power_domains)
		return -ENOENT;
	if (sim_count_error)
		return -EINVAL;
	for (i = 0; i < SIM_PHANDLE_SLOTS; i++)
		cnt += sim_phandle[i] != 0;
	return cnt;
}

/* devm_pm_domain_attach_list seam: with >= 2 cells the driver core attaches NO
 * domain, so a pm_runtime_get_sync on the device itself is the false-vote trap */
#define PD_FLAG_DEV_LINK_ON 1
struct dev_pm_domain_attach_data { u32 pd_flags; };
struct dev_pm_domain_list { int num_pds; };
extern int sim_pd_attach_fail;	/* force devm_pm_domain_attach_list to fail with -EIO */
extern struct dev_pm_domain_list sim_pd_list;
static inline int devm_pm_domain_attach_list(struct device *dev,
					     const struct dev_pm_domain_attach_data *data,
					     struct dev_pm_domain_list **list)
{
	(void)dev; (void)data;
	if (sim_pd_attached)
		return -EBUSY;	/* devm double-attach */
	if (sim_pd_attach_fail) {
		sim_pd_attach_fail = 0;
		return -EIO;
	}
	if (of_count_phandle_with_args(dev->of_node, "power-domains", "#power-domain-cells") < 2)
		return -EINVAL;
	*list = &sim_pd_list;
	sim_pd_list.num_pds = sim_pd_attached = of_count_phandle_with_args(dev->of_node, "power-domains", "#power-domain-cells");
	return sim_pd_attached;
}

/* ---- the reads step control: mutex, user-copy helpers, debugfs ---- */
struct mutex { pthread_mutex_t value; };
static inline void mutex_lock(struct mutex *m) { pthread_mutex_lock(&m->value); }
static inline void mutex_unlock(struct mutex *m) { pthread_mutex_unlock(&m->value); }
/* the kernel's DEFINE_MUTEX does not add a storage class; callers write `static DEFINE_MUTEX(...)` */
#define DEFINE_MUTEX(n) struct mutex n = { .value = PTHREAD_MUTEX_INITIALIZER }
static inline char *strim(char *s)
{
	char *end;
	size_t len;

	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	len = strlen(s);
	if (!len)
		return s;
	end = s + len - 1;
	while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
		*end-- = '\0';
	return s;
}
static inline ssize_t simple_write_to_buffer(void *to, size_t avail, loff_t *ppos,
					     const char *from, size_t count)
{
	size_t cp = count < avail ? count : avail;

	if (!cp)
		return -EINVAL;
	memcpy(to, from, cp);
	*ppos += cp;
	return (ssize_t)cp;
}
#define __user
struct inode { void *i_private; };
struct file { void *private_data; };
struct file_operations {
	struct module *owner;
	ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
};
struct dentry { void *data; const struct file_operations *ops; bool live; };
extern int sim_rd_ready;
static struct dentry sim_rd_entry;	/* the reads step control, driven by the harness */
static inline struct dentry *debugfs_create_file(const char *name, int mode, void *parent, void *data, const struct file_operations *ops)
{
	(void)parent;
	if (strcmp(name, "ane_h18_rd"))
		abort();
	if (mode != 0200)
		abort();
	sim_rd_entry = (struct dentry){ .data = data, .ops = ops, .live = true };
	sim_rd_ready = 1;
	return &sim_rd_entry;
}
static inline void debugfs_remove(struct dentry *entry) { if (entry) entry->live = false; }
#define IS_ERR_OR_NULL(p) (!(p) || IS_ERR(p))

#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define __IOMMU_DOMAIN_DMA_API (1U << 1)
struct iommu_domain { unsigned int type; };
extern struct iommu_domain sim_default_dom;
extern int sim_default_dom_none;
static inline struct iommu_domain *iommu_get_domain_for_dev(struct device *d) { (void)d; return sim_default_dom_none ? NULL : &sim_default_dom; }
enum { SIM_IO_ALLOC = 1, SIM_IO_ATTACH, SIM_IO_MAP, SIM_IO_UNMAP, SIM_IO_DETACH, SIM_IO_FREE };
struct sim_iocall { int code; u64 iova, pa, size; int prot; int ev; };
extern struct sim_iocall sim_iocalls[64];
extern int sim_n_iocalls, sim_io_fail;		/* sim_io_fail: 1 alloc fails, 2 attach fails, 3 the second map fails */
extern int sim_n_dom_live;
static inline void sim_iorec(int code, u64 iova, u64 pa, u64 size, int prot)
{
	if (sim_n_iocalls < 64)
		sim_iocalls[sim_n_iocalls] = (struct sim_iocall){ code, iova, pa, size, prot, sim_n_events };
	sim_n_iocalls++;
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
	return sim_io_fail == 2 ? -EBUSY : 0;
}
static inline int iommu_map(struct iommu_domain *dom, unsigned long iova, u64 pa, size_t size, int prot, int gfp)
{
	(void)dom; (void)gfp;
	sim_iorec(SIM_IO_MAP, iova, pa, size, prot);
	if (sim_io_fail == 3) {
		int maps = 0, k;

		for (k = 0; k < sim_n_iocalls && k < 64; k++)
			maps += sim_iocalls[k].code == SIM_IO_MAP;
		if (maps == 2)
			return -ENOMEM;
	}
	return 0;
}
static inline size_t iommu_unmap(struct iommu_domain *dom, unsigned long iova, size_t size)
{
	(void)dom;
	sim_iorec(SIM_IO_UNMAP, iova, 0, size, 0);
	return size;
}
static inline void iommu_detach_device(struct iommu_domain *dom, struct device *d) { (void)dom; (void)d; sim_iorec(SIM_IO_DETACH, 0, 0, 0, 0); }
static inline void iommu_domain_free(struct iommu_domain *dom) { sim_iorec(SIM_IO_FREE, 0, 0, 0, 0); sim_n_dom_live--; free(dom); }

/* ---- virtual clock: 1 jiffy = 1 ms ---- */
extern unsigned long sim_jiffies;
#define jiffies sim_jiffies
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static inline bool time_after(unsigned long a, unsigned long b) { return (long)(b - a) < 0; }
extern void sim_usleep_hook(void);
static inline void usleep_range(unsigned long lo, unsigned long hi) { sim_jiffies += (lo + hi) / 2000 + 1; sim_credits_now = 1; sim_usleep_hook(); }

#define ERANGE_UNUSED 0

#endif /* SIM_SHIM_H */
