// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sim/sim_harness.c: drives the real ane_h18 probe through every stage against sim/shim.h.
 *   sim_h18 pos                positive controls (exit 0 iff all pass)
 *   sim_h18 neg-psd-never      a slot that never acknowledges: FAIL, undo, no ASC read
 *   sim_h18 neg-psd-wrong      a slot that acknowledges the wrong state: FAIL, no ASC read
 *   sim_h18 neg-dt             tampered windows or properties: refused before any bus access
 *   sim_h18 neg-args           wrong optin, unknown stage, bad psd_state or psd_slots: no bus access
 * A negative control exits 0 only when the module fails the induced fault correctly.
 */
#include "shim.h"

char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
struct sim_event sim_events[SIM_MAX_EVENTS];
int sim_n_events, sim_faults, sim_n_iomap, sim_n_iounmap;
int sim_psd_mode[SIM_NPSD];
u32 sim_psd_slot[SIM_NPSD] = { 30, 31 };
u32 sim_psd_state[SIM_NPSD];
u32 sim_psd_valid[SIM_NPSD];
u32 sim_psd_initial_valid[SIM_NPSD];
u64 sim_asc_pa = 0x419600000ULL, sim_rvbar_pa = 0x419050000ULL, sim_mbox_pa = 0x316000000ULL;
u32 sim_asc_size = 0x74000, sim_rvbar_size = 0x4000, sim_mbox_size = 0x1c000;
unsigned long sim_jiffies;
u32 sim_cpu_status = 0xa, sim_cpu_ctrl, sim_run_status = 0x9;
u64 sim_rvbar_val = 0x10000000001ULL;
int sim_core_running, sim_core_stuck, sim_ctrl_writes;
int sim_sleeping;
u32 sim_po_msg[SIM_PO_CH][SIM_PO_Q][4];
int sim_po_n[SIM_PO_CH], sim_po_head[SIM_PO_CH];
u32 sim_tx_w[4];
int sim_tx_got;
u64 sim_tx_msgs[32];
int sim_n_tx_msgs, sim_idle_sent, sim_stop_needs_idle, sim_no_shutdown_reply;
u32 sim_iop_ack_state = 0x20;
int sim_credit_mode;
u32 sim_scr[8];
int sim_scr_model;
struct sim_dma sim_dma[SIM_NDMA];
int sim_n_dma, sim_dma_fail, sim_dma_mask_bits;
struct iommu_domain sim_default_dom = { __IOMMU_DOMAIN_DMA_API };
int sim_default_dom_none;
int sim_hs_mode;	/* 0 the firmware answers the wake word, 1 it never answers */
int sim_hs_block_ok, sim_hs_wakes;
static int sim_ipc_i = 1;
int sim_db_writes, sim_hs_table, sim_s3_mode, sim_cmd_mode, sim_cmds_seen, sim_table_bad, sim_chan_req;
u32 sim_pend_val;
int sim_ack_writes, sim_cmd_blk_in_pool, sim_posted, sim_term_tick;
u32 sim_ack_last;
u32 sim_cmd_ops[16];
u32 sim_prop[8][3];	/* CH_PROPERTY_WRITE blocks seen: arg at +8, id at +0xc, value at +0x10 */
int sim_nprops;
u32 sim_prop_refuse;	/* a property id the modelled firmware refuses (ack 1), 0 = none */
u32 sim_credits_now = 1;
int sim_rpm_enabled, sim_rpm_gets, sim_rpm_puts, sim_rpm_get_fail;
int sim_rpm_get_fail_at = -1;	/* PG53: fail the second runtime PM get (the recovery psd_up), -1 = never */
u32 sim_phandle[SIM_PHANDLE_SLOTS];			/* slots 0.. = the claimed ps domains, a PMP report entry in a slot is a refusal case only */
struct device_node *sim_phandle_target[SIM_PHANDLE_SLOTS];
int sim_pd_attached, sim_pd_false_votes, sim_pd_attach_fail, sim_count_error;
struct dev_pm_domain_list sim_pd_list;
struct device *sim_pdev_dev;
int sim_rd_ready, sim_res_ready;	/* the module published the reads / resident controls */
int (*sim_fw_hook)(const char *name, const u8 **data, size_t *size);
u64 sim_gate_pa[2];			/* stage=reads seams: the pmgr gate-state pages */
u32 sim_gate_val[2];
bool sim_ps_on;				/* the four ANE ps registers read fully on when set */
int sim_iomap_fail_at;
struct sim_iocall sim_iocalls[SIM_NIOCALLS];
int sim_n_iocalls, sim_io_fail, sim_n_dom_live;
struct sim_iomap sim_iomaps[SIM_NIOMAPS];
int sim_n_iomaps;
unsigned long sim_op_seq, sim_last_dmafree_seq;
u64 sim_unmap_short_iova;
static u8 sim_owned_fw[1572864];	/* the owned_fw trial payload: zeroed, matching the zeroed sim pin (sha256 stub returns zeros) */
int sim_start_chan = -1, sim_start_msgs, sim_start_hello;
u32 sim_po_out_off, sim_alias_credits = 1, sim_hello_min = 11, sim_hello_max = 12, sim_hello_ep, sim_hello_type = 1;

#define ane_t8152_soc sim_soc_t8152_orig	/* the const row keeps its data; the probe sees the writable copy below */
#include "../ane_h18_soc.c"
static struct ane_h18_soc ane_t8152_soc_shim;	/* reset_all() copies the row in: a pin may shrink dart_size to refuse a DART0 word */
#define ane_t8152_soc ane_t8152_soc_shim
#include "../ane_h18_main.c"
/* no #undef: harness code above and below both mean the writable copy */



/* ---- hs_prog: the program file the harness hands to request_firmware, and a firmware that answers the program commands ---- */
const u8 *sim_fw_data;
size_t sim_fw_size;
int sim_fw_err, sim_fw_released, sim_fw_requests;
static u8 sim_blob[0x10000];
static size_t sim_blob_len;
int sim_prog_mode, sim_prog_n, sim_prog_loaded, sim_prog_created, sim_t2h_back, sim_out_written, sim_t2h_posted;
u32 sim_prog_ops[64];
u32 sim_prog_ids[64];
u64 sim_load_sec2_iova, sim_load_sec2_size;
u8 sim_load_sec2_b1;
int sim_pub_wait, sim_premapped;
u64 sim_ktime_calls;
u64 sim_map_fail_iova;

/* Multi-program firmware model: tracks N concurrently resident programs with distinct prog_id/proc_id,
 * verifies every PROCEDURE_CALL against still-live section surfaces, and supports targeted fault injection. */
struct sim_fw_prog {
	bool live;
	bool has_proc;
	u32 prog_id;
	u32 proc_id;
	u32 out_tag;
	unsigned int nsec;
	u64 sec_iova[9];
	u64 sec_size[9];
	u64 sec2_iova;
	u64 sec2_size;
};
static struct sim_fw_prog sim_fw_progs[32];
static u32 sim_fw_next_prog, sim_fw_next_proc;
static u16 sim_res_fail_op;		/* nonzero: return sim_res_fail_ack on this op (filtered by sim_res_fail_prog if nonzero) */
static u32 sim_res_fail_prog;
static u16 sim_res_fail_ack;
static u32 sim_res_spoof_prog, sim_res_spoof_proc;	/* nonzero: stamp wrong resident prog/proc into IO_T2H events */
static u32 sim_res_dup_prog;				/* nonzero: force next LOAD_PROGRAM to return this duplicate prog_id */
static int sim_res_malformed_create;			/* nonzero: next CREATE_PROCESS leaves proc_id at 0xffffffff */
int sim_cfu_fail_at, sim_cfu_calls;
int sim_ctu_fail_at, sim_ctu_calls;
int sim_kvmalloc_fail_at, sim_kvmalloc_calls, sim_kv_live;
static int sim_res_io_echo;				/* nonzero: firmware echoes 512B id5^0x5a into id4 + checks zero pre-state */
static u8 sim_res_io_last_in[0x4000];
static size_t sim_res_io_last_in_sz;
static int sim_res_io_out_was_zero;

static struct sim_fw_prog *sim_fw_find(u32 prog_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sim_fw_progs); i++)
		if (sim_fw_progs[i].live && sim_fw_progs[i].prog_id == prog_id)
			return &sim_fw_progs[i];
	return NULL;
}

static void sim_fw_sync_flags(void)
{
	unsigned int i;

	sim_prog_loaded = 0;
	sim_prog_created = 0;
	for (i = 0; i < ARRAY_SIZE(sim_fw_progs); i++) {
		if (sim_fw_progs[i].live) {
			sim_prog_loaded = 1;
			if (sim_fw_progs[i].has_proc)
				sim_prog_created = 1;
		}
	}
}

/* the file: sections 1 to 5 and 7 (the add program's six), buffers 5 and 6 as inputs and 4 as the output, 0x4000 bytes each; input 5 starts with 0xaa bytes */
static void make_blob(void)
{
	static const u32 sids[6] = { 1, 2, 3, 4, 5, 7 };
	static const u32 bids[3] = { 5, 6, 4 };
	static const u32 bdir[3] = { 0, 0, 1 };
	u32 magic = ANE_H18_PROG_MAGIC, version = 1, nsec = 6, nio = 3, i, zero = 0;
	u64 pos = 0x1000, fsize, size, off, dsz, doff;

	memset(sim_blob, 0, sizeof(sim_blob));
	memcpy(sim_blob, &magic, 4);
	memcpy(sim_blob + 4, &version, 4);
	memcpy(sim_blob + 8, &nsec, 4);
	memcpy(sim_blob + 12, &nio, 4);
	for (i = 0; i < nsec; i++) {
		u8 *r = sim_blob + 0x20 + i * 0x20;

		size = 0x100 * (i + 1);
		off = pos;
		memcpy(r, &sids[i], 4);
		memcpy(r + 4, &zero, 4);
		memcpy(r + 8, &size, 8);
		memcpy(r + 16, &off, 8);
		memset(sim_blob + off, 0x10 + i, size);
		pos += size;
	}
	for (i = 0; i < nio; i++) {
		u8 *r = sim_blob + 0x20 + (nsec + i) * 0x20;

		size = 0x4000;
		dsz = i < 2 ? 0x40 : 0;
		doff = i < 2 ? pos : 0;
		memcpy(r, &bids[i], 4);
		memcpy(r + 4, &bdir[i], 4);
		memcpy(r + 8, &size, 8);
		memcpy(r + 16, &doff, 8);
		memcpy(r + 24, &dsz, 8);
		if (dsz) {
			memset(sim_blob + doff, i ? 0xbb : 0xaa, dsz);
			pos += dsz;
		}
	}
	fsize = pos;
	memcpy(sim_blob + 16, &fsize, 8);
	sim_blob_len = fsize;
	sim_fw_data = sim_blob;
	sim_fw_size = fsize;
}

/* the prog_iova_kern trial file: the section with id 2 is exactly 0x4000 bytes, so nothing sits behind its first 0x4000 */
static void make_blob_kern(void)
{
	static const u64 pos[6] = { 0x1000, 0x1100, 0x5100, 0x5400, 0x5800, 0x5d00 };
	static const u64 size[6] = { 0x100, 0x4000, 0x300, 0x400, 0x500, 0x600 };
	u64 fsize = 0x6380, i;

	make_blob();
	for (i = 0; i < 6; i++) {
		u8 *r = sim_blob + 0x20 + i * 0x20;

		memcpy(r + 8, &size[i], 8);
		memcpy(r + 16, &pos[i], 8);
		memset(sim_blob + pos[i], 0x10 + i, size[i]);
	}
	for (i = 0; i < 2; i++) {
		u64 doff = 0x6300 + 0x40 * i;
		u8 *r = sim_blob + 0x20 + 6 * 0x20 + i * 0x20;

		memcpy(r + 16, &doff, 8);
		memset(sim_blob + doff, i ? 0xbb : 0xaa, 0x40);
	}
	memcpy(sim_blob + 16, &fsize, 8);
	sim_blob_len = fsize;
	sim_fw_size = fsize;
}

/* H18G weighted trial file: section 2 has 0x4000 bytes of __TEXT/__const (0x11) followed by 0x4000 bytes of __KERN_0 weights (0x11 at +0, 0x5a at +1..),
 * and section 3 fits before prog_text_const=0x280. */
static void make_blob_kern_weights(void)
{
	static const u64 pos[6] = { 0x1000, 0x1100, 0x9100, 0x9300, 0x9700, 0x9c00 };
	static const u64 size[6] = { 0x100, 0x8000, 0x200, 0x400, 0x500, 0x600 };
	u64 fsize = 0xa280, i;

	make_blob();
	for (i = 0; i < 6; i++) {
		u8 *r = sim_blob + 0x20 + i * 0x20;

		memcpy(r + 8, &size[i], 8);
		memcpy(r + 16, &pos[i], 8);
		memset(sim_blob + pos[i], 0x10 + i, size[i]);
	}
	/* Mark the weights tail behind the 0x4000 const window so LOAD_PROGRAM slot 1 proves it received pg_kern, not pg_sec[s2]. */
	memset(sim_blob + pos[1] + 0x4000 + 1, 0x5a, 0x4000 - 1);
	for (i = 0; i < 2; i++) {
		u64 doff = 0xa200 + 0x40 * i;
		u8 *r = sim_blob + 0x20 + 6 * 0x20 + i * 0x20;

		memcpy(r + 16, &doff, 8);
		memset(sim_blob + doff, i ? 0xbb : 0xaa, 0x40);
	}
	memcpy(sim_blob + 16, &fsize, 8);
	sim_blob_len = fsize;
	sim_fw_size = fsize;
}

static int sim_buf_ok(u64 iova, u64 size)
{
	int d;

	for (d = 0; d < sim_n_dma; d++)
		if (sim_dma[d].live && iova >= sim_dma[d].iova && size <= sim_dma[d].size && iova - sim_dma[d].iova <= sim_dma[d].size - size)
			return 1;
	if (sim_alias_dma(iova))	/* a fixed alias resolves through the fake iommu */
		return sim_buf_ok(sim_alias_dma(iova), size);
	return 0;
}

static u8 *sim_buf_va(u64 iova)
{
	int d;

	for (d = 0; d < sim_n_dma; d++)
		if (sim_dma[d].live && iova >= sim_dma[d].iova && iova < sim_dma[d].iova + sim_dma[d].size)
			return (u8 *)sim_dma[d].va + (iova - sim_dma[d].iova);
	if (sim_alias_dma(iova))
		return sim_buf_va(sim_alias_dma(iova));
	return NULL;
}

/* The firmware's side of LOAD_PROGRAM, CREATE_PROCESS, PROCEDURE_CALL, TERMINATE_PROCESS and DESTROY_PROGRAM. Returns the acknowledgement for +6: 0 ok, 3 refused by
 * the mode, 4 an id it does not know, 5 a program with a live process, 7 an address that is not in a buffer the host allocated. A call posts two IO_T2H events. */
static u16 sim_prog_cmd(u16 op, u8 *blk, u64 cmd_len)
{
	u32 prog = 0, proc = 0, v;
	u64 cookie, a, s;
	int i, n;

	if (sim_prog_n < (int)ARRAY_SIZE(sim_prog_ops))
		sim_prog_ops[sim_prog_n] = op;
	sim_prog_n++;
	memcpy(&prog, blk + 8, 4);
	memcpy(&proc, blk + 0xc, 4);
	if (sim_prog_n <= (int)ARRAY_SIZE(sim_prog_ids))
		sim_prog_ids[sim_prog_n - 1] = prog;
	if (sim_res_fail_op == op && (!sim_res_fail_prog || sim_res_fail_prog == prog))
		return sim_res_fail_ack ? sim_res_fail_ack : 3;
	switch (op) {
	case 0x20d: {
		u32 a1, ty, f54, f58, f60;
		u64 f68, pool, sec;

		if (sim_prog_mode == 7)
			return 3;
		if (sim_prog_mode == 8) {	/* answers with another command's id */
			u16 wrong = 0x20e;

			memcpy(blk + 4, &wrong, 2);
			return 0;
		}
		memcpy(&a1, blk + 8, 4);
		memcpy(&ty, blk + 0x10, 4);
		memcpy(&f54, blk + 0x54, 4);
		memcpy(&f58, blk + 0x58, 4);
		memcpy(&f60, blk + 0x60, 4);
		memcpy(&f68, blk + 0x68, 8);
		memcpy(&pool, blk + 0x70, 8);
		memcpy(&sec, blk + 0x78, 8);
		if (a1 != 1 || ty != 2 || f54 != 1 || !(f58 & 1) || f60 != 4 || f68 != 1 || !sim_buf_ok(pool, 0x3898) || sec != 0x4000)
			return 7;
		sim_premapped = 1;
		return 0;
	}
	case 0x200: {
		struct sim_fw_prog *fp = NULL;
		unsigned int slot;

		if (sim_prog_mode == 1)
			return 3;
		if (cmd_len != 0x1c0)
			return 7;
		if (sim_res_dup_prog) {
			v = sim_res_dup_prog;
			sim_res_dup_prog = 0;
			memcpy(blk + 0x1b8, &v, 4);
			if (sim_prog_n <= (int)ARRAY_SIZE(sim_prog_ids))
				sim_prog_ids[sim_prog_n - 1] = v;
			return 0;
		}
		for (slot = 0; slot < ARRAY_SIZE(sim_fw_progs); slot++)
			if (!sim_fw_progs[slot].live) {
				fp = &sim_fw_progs[slot];
				break;
			}
		if (!fp)
			return 3;
		memset(fp, 0, sizeof(*fp));
		n = 0;
		for (i = 1; i <= 9; i++) {
			const u8 *r = blk + 8 + (i - 1) * 0x30;
			u32 flags, id;

			memcpy(&flags, r, 4);
			memcpy(&id, r + 4, 4);
			if (!flags)
				continue;
			memcpy(&a, r + 0x18, 8);
			memcpy(&s, r + 0x20, 8);
			if (id != (u32)i || !sim_buf_ok(a, s))
				return 7;
			if (sim_buf_va(a)[0] != 0x10 + (id == 7 ? 5 : id - 1))	/* the section bytes of the file are in the surface */
				return 7;
			if (id == 2) {
				sim_load_sec2_iova = a;
				sim_load_sec2_size = s;
				sim_load_sec2_b1 = s > 1 ? sim_buf_va(a)[1] : 0;
				fp->sec2_iova = a;
				fp->sec2_size = s;
			}
			fp->sec_iova[n] = a;
			fp->sec_size[n] = s;
			n++;
		}
		if (!n)
			return 7;
		fp->nsec = (unsigned int)n;
		v = 0x5a + sim_fw_next_prog++;
		fp->prog_id = v;
		fp->out_tag = 0x11223344u + slot;
		fp->live = true;
		if (sim_prog_mode != 4) {
			memcpy(blk + 0x1b8, &v, 4);
			if (sim_prog_n <= (int)ARRAY_SIZE(sim_prog_ids))
				sim_prog_ids[sim_prog_n - 1] = v;
		}
		sim_fw_sync_flags();
		return 0;
	}
	case 0x202: {
		struct sim_fw_prog *fp = sim_fw_find(prog);

		if (sim_prog_mode == 2)
			return 3;
		if (sim_prog_mode == 6)
			return 0xffff;
		if (cmd_len != 0x10)
			return 7;
		if (!fp)
			return 4;
		if (sim_res_malformed_create) {
			sim_res_malformed_create = 0;
			return 0;	/* leaves proc_id at 0xffffffff */
		}
		v = 0x77 + sim_fw_next_proc++;
		memcpy(blk + 0xc, &v, 4);
		fp->proc_id = v;
		fp->has_proc = true;
		sim_fw_sync_flags();
		return 0;
	}
	case 0x204: {
		struct sim_fw_prog *fp = sim_fw_find(prog);
		u32 cnt, prio;
		u64 *ring = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xb000);
		u8 *pool = sim_dma[0].va;
		unsigned int si;

		if (!fp || !fp->has_proc || fp->proc_id != proc)
			return 4;
		/* Every section surface of the called resident program must still be live and mapped. */
		for (si = 0; si < fp->nsec; si++)
			if (!sim_buf_ok(fp->sec_iova[si], fp->sec_size[si]))
				return 7;
		memcpy(&prio, blk + 0x18, 4);
		memcpy(&cookie, blk + 0x20, 8);
		memcpy(&cnt, blk + 0x28, 4);
		if (prio != 2 || cookie != 0xADD0 || !cnt || cnt > 8)
			return 7;
		{
			const u8 *in5_va = NULL;
			size_t in5_sz = 0;

			for (i = 0; i < (int)cnt; i++) {
				const u8 *r = blk + 0x60 + i * 0x30;
				u32 flags, type, bid;
				u8 *o;

				memcpy(&flags, r, 4);
				memcpy(&bid, r + 4, 4);
				memcpy(&type, r + 8, 4);
				memcpy(&a, r + 0x18, 8);
				memcpy(&s, r + 0x20, 8);
				if (flags != 1 || !sim_buf_ok(a, s))
					return 7;
				if (sim_res_io_echo) {
					if (bid != 4 && bid != 5 && bid != 6)
						return 7;
					if (bid == 5 && type == 0) {
						in5_va = sim_buf_va(a);
						in5_sz = (size_t)s;
						sim_res_io_last_in_sz = in5_sz < sizeof(sim_res_io_last_in) ? in5_sz : sizeof(sim_res_io_last_in);
						memcpy(sim_res_io_last_in, in5_va, sim_res_io_last_in_sz);
					}
					if (type == 1 && (o = sim_buf_va(a))) {
						size_t k;

						sim_res_io_out_was_zero = 1;
						for (k = 0; k < (size_t)s; k++)
							if (o[k] != 0) {
								sim_res_io_out_was_zero = 0;
								break;
							}
						if (in5_va) {
							size_t n = 512;

							if (n > in5_sz)
								n = in5_sz;
							if (n > (size_t)s)
								n = (size_t)s;
							for (k = 0; k < n; k++)
								o[k] = (u8)(in5_va[k] ^ 0x5aU);
						}
						sim_out_written = 1;
					}
				} else {
					if ((bid == 5 && sim_buf_va(a)[0] != 0xaa) || (bid == 6 && sim_buf_va(a)[0] != 0xbb) || (bid != 4 && bid != 5 && bid != 6))	/* the input data of the file is in the surface */
						return 7;
					if (type == 1 && (o = sim_buf_va(a)) && (sim_prog_mode != 12 || !sim_out_written)) {	/* mode 12: only the first call writes the output */
						v = fp->out_tag;
						memcpy(o, &v, 4);
						memcpy(o + 4, &v, 4);
						sim_out_written = 1;
					}
				}
			}
		}
		if (sim_prog_mode == 3)
			return 0;
		int base = sim_t2h_posted;

		for (i = 0; i < 2; i++) {
			int sl = (base + i) % 0x40;
			u8 *ev = pool + 0x1000 + sl * 0x40;
			u32 seq = (u32)(base + i + 1), k = 0x300, st = (u32)i;

			u32 ev_prog = sim_res_spoof_prog ? sim_res_spoof_prog : prog;
			u32 ev_proc = sim_res_spoof_proc ? sim_res_spoof_proc : proc;

			memset(ev, 0, 0x28);
			memcpy(ev, &seq, 4);
			memcpy(ev + 4, &k, 4);
			memcpy(ev + 8, &cookie, 8);
			memcpy(ev + 0x10, &ev_prog, 4);
			memcpy(ev + 0x14, &ev_proc, 4);
			memcpy(ev + 0x1c, &st, 4);
			ring[sl * 8 + 1] = 0x28;
			if (sim_prog_mode == 5) {	/* the length first, the address (the publication) three sleeps later */
				sim_pub_wait = 3;
				continue;
			}
			if (sim_prog_mode == 10) {	/* the same, but the publication waits five yields with no sleep in between: only the timed spin's cond_resched runs */
				sim_pub_wait = 5;
				continue;
			}
			ring[sl * 8] = sim_dma[0].iova + 0x1000 + sl * 0x40;
			sim_t2h_posted++;
		}
		return 0;
	}
	case 0x203: {
		struct sim_fw_prog *fp = sim_fw_find(prog);

		if (cmd_len != 0x10)
			return 7;
		if (!fp || !fp->has_proc || fp->proc_id != proc)
			return 4;
		fp->has_proc = false;
		sim_fw_sync_flags();
		return 0;
	}
	case 0x201: {
		struct sim_fw_prog *fp = sim_fw_find(prog);

		if (sim_prog_mode == 11)	/* PG52: the firmware never answers the unload */
			return 0xffff;
		if (cmd_len != 0x0c)
			return 7;
		if (!fp)
			return 4;
		if (fp->has_proc)
			return 5;
		fp->live = false;
		sim_fw_sync_flags();
		return 0;
	}
	}
	return 4;
}

/* The modelled firmware sees the wake word: it checks the published init block against the layout of the T6021 header and, in mode 0, answers with a
 * table address inside the IPC surface and READY. sim_hs_block_ok is set only when every field the decode names is right. */
void sim_hs_react(void)
{
	u64 pub = ((u64)sim_scr[1] << 32) | sim_scr[0], f;
	int k, found = -1;
	const u8 *b;

	sim_hs_wakes++;
	for (k = 0; k < sim_n_dma; k++)
		if (sim_dma[k].iova == pub)
			found = k;
	if (found < 0 || sim_n_dma < 2)
		return;
	b = sim_dma[found].va;
#define F8(off) (memcpy(&f, b + (off), 8), f)
#define F4(off) (memcpy(&f, b + (off), 4), f & 0xffffffffu)
	{
		/* with owned_fw staged the image is sim_dma[0] and every handshake surface shifts one slot */
		int sh = 0, pool, ipc, heap;

		for (k = 0; k < sim_n_dma; k++)
			sh += sim_dma[k].size == 0x370000u;
		pool = sh ? 1 : 0;
		ipc = pool + 1;
		heap = pool + 2;
		f = 0;
		sim_hs_block_ok = F8(0x00) == ane_t8152_soc.fw_seg[0].iova && F8(0x08) == sim_dma[ipc].iova && F8(0x10) == hs_cfg_size && F8(0x18) == 0x10000000ULL - hs_cfg_size &&
				  F8(0x20) == (sim_n_dma > 2 ? sim_dma[heap].iova : 0) && F8(0x28) == 0xf00000 && F8(0x50) == 0xc441 && F8(0x58) == pub && F8(0x60) == 0x40000 && F4(0x68) == 0x40 && F4(0x12c) == 4 &&
				  sim_dma[pool].size == 0x40000 && sim_dma[ipc].size == 0x10000 && sim_dma[heap].size == 0xf00000;
		sim_ipc_i = ipc;
	}
	if (sim_hs_mode == 2)
		sim_scr[7] = ANE_H18_HS_READY;	/* READY again, but no table address: the block address stays in SCRATCH0 and SCRATCH1 */
	if (sim_hs_mode == 0 && sim_hs_table) {
		/* a table like the board's: eight 0x100 byte entries at the start of the IPC surface, the M6 ring offsets */
		static const struct { const char *n; u32 type, bit; u64 slots, off; } ch[8] = {
			{ "TERMINAL", 2, 0, 0x200, 0x800 }, { "IO", 0, 1, 0x10, 0x8800 }, { "DEBUG", 0, 2, 8, 0x8c00 }, { "BUF_H2T", 0, 3, 0x40, 0x8e00 },
			{ "BUF_T2H", 1, 4, 0x40, 0x9e00 }, { "SHAREDMALLOC", 1, 5, 8, 0xae00 }, { "IO_T2H", 1, 6, 0x40, 0xb000 }, { "DATA_CHAIN_H2T", 0, 7, 0x10, 0xc000 },
		};
		u8 *t = sim_dma[sim_ipc_i].va;
		int c;

		memset(t, 0, 8 * 0x100);
		for (c = 0; c < 8; c++) {
			u64 ring = sim_dma[sim_ipc_i].iova + ch[c].off + (sim_table_bad == c + 1 ? 0x1000000 : 0);

			if (c == 2 && sim_table_bad == 100)
				ring = sim_dma[sim_ipc_i].iova + 0x80;	/* inside the table entries */
			if (c == 2 && sim_table_bad == 102)
				ring = 0xffffffffffffff00ULL;	/* ring + slots * 0x40 wraps past 2^64 */
			if (c == 2 && sim_table_bad == 101)
				ring = sim_dma[sim_ipc_i].iova + 0x8800;	/* the IO ring */

			strcpy((char *)t + c * 0x100, ch[c].n);
			memcpy(t + c * 0x100 + 0x40, &ch[c].type, 4);
			memcpy(t + c * 0x100 + 0x44, &ch[c].bit, 4);
			memcpy(t + c * 0x100 + 0x48, &ch[c].slots, 8);
			memcpy(t + c * 0x100 + 0x50, &ring, 8);
		}
		if (sim_chan_req) {
			u64 *sm = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xae00);	/* the SHAREDMALLOC ring: a request nobody answered */

			memcpy((u8 *)sim_dma[sim_ipc_i].va + 0x800, "ANE fw says hi", 14);	/* the firmware console */
			if (sim_chan_req == 1) {
				memcpy((u8 *)sim_dma[sim_ipc_i].va + 0xf000, "fw text\n\x01", 9);	/* a console record that points to text: slot 2, flag 2, length 9, timestamp 0x77 */
				((u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0x800))[2 * 8] = (sim_dma[sim_ipc_i].iova + 0xf000) | 2;
				((u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0x800))[2 * 8 + 1] = 9;
				((u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0x800))[2 * 8 + 3] = 0x77;
				memset((u8 *)sim_dma[sim_ipc_i].va + 0xf100, 'a', 0x101);	/* the limit: slot 3 has length 0x100 (printed, ends in 'yz'), slot 4 has 0x101 (counted, not printed) */
				((u8 *)sim_dma[sim_ipc_i].va)[0xf100 + 0xfe] = 'y';
				((u8 *)sim_dma[sim_ipc_i].va)[0xf100 + 0xff] = 'z';
				((u8 *)sim_dma[sim_ipc_i].va)[0xf100 + 0x100] = 'w';
				for (int q = 3; q <= 4; q++) {
					((u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0x800))[q * 8] = (sim_dma[sim_ipc_i].iova + 0xf100) | 2;
					((u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0x800))[q * 8 + 1] = q == 3 ? 0x100 : 0x101;
					((u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0x800))[q * 8 + 3] = 0x75 + q;
				}
			}
			sm[0] = 0x1000;	/* a header that is an iova, with a size: no request has this shape */
			sm[1] = 0x40;
			sm[2] = 0;
			sim_pend_val = 0x20;
		}
		sim_scr[0] = (u32)sim_dma[sim_ipc_i].iova;
		sim_scr[1] = (u32)(sim_dma[sim_ipc_i].iova >> 32);
		sim_scr[7] = ANE_H18_HS_READY;
	} else if (sim_hs_mode == 0) {
		u32 mark[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };

		memcpy((u8 *)sim_dma[sim_ipc_i].va + 0x100, mark, sizeof(mark));
		sim_scr[0] = (u32)(sim_dma[sim_ipc_i].iova + 0x100);
		sim_scr[1] = (u32)((sim_dma[sim_ipc_i].iova + 0x100) >> 32);
		sim_scr[7] = ANE_H18_HS_READY;
	}
}


/* A firmware console that prints on every poll (chan_req 4): the terminal ring changes at each sleep once the table exists. */
void sim_usleep_hook(void)
{
	if (sim_pub_wait && !--sim_pub_wait && sim_n_dma > 1) {	/* publish the two IO_T2H events of a call in mode 5 */
		u64 *ring = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xb000);
		int q;

		for (q = 0; q < 2; q++) {
			ring[q * 8] = sim_dma[0].iova + 0x1000 + q * 0x40;
			sim_t2h_posted++;
		}
	}
	if (sim_chan_req == 4 && sim_n_dma > 1 && sim_scr_model && sim_scr[3] == 0 && sim_hs_table) {
		u32 *t = (u32 *)((u8 *)sim_dma[sim_ipc_i].va + 0x800);

		t[0] = ++sim_term_tick;
		for (int j = 1; j < 6; j++)	/* six nonzero 16 byte lines in all, to see the cap of a change snapshot */
			t[4 * j] = 0x100 + j;
	}
}

/* SCRATCH3 = READY from the host: the modelled firmware clears it (mode 0) or never does (mode 1). */
void sim_s3_react(u32 v)
{
	if (v == ANE_H18_HS_READY && sim_s3_mode == 0)
		sim_scr[3] = 0;
	if (v == ANE_H18_HS_READY && (sim_chan_req == 2 || sim_chan_req == 3) && sim_n_dma > 1) {
		u64 *sm = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xae00);

		sm[0] = 0;	/* a MALLOC of 0x8000 bytes, mode 0: the firmware takes no command before it is answered */
		sm[1] = 0x8000;
		sm[2] = 0;
	}
}

/* A doorbell write with a command on the IO ring (ring at IPC + 0x8800, 0x10 slots): the firmware takes the block named by a slot header with bit 0 clear,
 * answers it in mode 0 (CONFIG_GET gets 0x16e3600 at +8), gives it a failing acknowledgement in mode 2, and never answers in mode 1. */
void sim_db_react(u32 v)
{
	u64 *ring;
	int k, d;

	if (v == BIT(5) && sim_chan_req == 3 && !sim_posted && sim_n_dma > 3) {
		u64 *sm = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xae00);

		if (sm[0] & 1) {	/* the first MALLOC was answered: the firmware gives that buffer back and asks for the same size again */
			sim_posted = 1;
			sm[8] = sm[0] & ~1ULL;
			sm[9] = 0;
			sm[10] = 0;
			sm[16] = 0;
			sm[17] = 0x8000;
			sm[18] = 0;
		}
		return;
	}
	if (v == BIT(6) && sim_n_dma > 1) {
		u64 *r6 = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xb000);
		int q, nback = 0;

		for (q = 0; q < 0x40; q++)
			if (r6[q * 8] & 1)
				nback++;
		sim_t2h_back = nback;
		return;
	}
	if (v != BIT(1) || sim_n_dma < 3)
		return;
	if (sim_chan_req == 2 && !(((u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xae00))[0] & 1))
		return;	/* the firmware still waits for its memory */
	ring = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0x8800);
	for (k = 0; k < 0x10; k++) {
		u64 hdr = ring[k * 8];
		u8 *blk = NULL;
		u16 op;

		if (hdr & 1)
			continue;
		for (d = 0; d < sim_n_dma; d++)
			if (hdr >= sim_dma[d].iova && hdr < sim_dma[d].iova + sim_dma[d].size)
				blk = (u8 *)sim_dma[d].va + (hdr - sim_dma[d].iova);
		if (!blk)
			return;
		memcpy(&op, blk + 4, 2);
		if (d <= sim_n_dma && blk >= (u8 *)sim_dma[0].va + 0x400 && blk < (u8 *)sim_dma[0].va + 0x4400)
			sim_cmd_blk_in_pool = 1;
		if (sim_cmds_seen < 16)
			sim_cmd_ops[sim_cmds_seen] = op;
		sim_cmds_seen++;
		if (sim_cmd_mode == 1)
			return;
		if (sim_cmd_mode == 2) {
			u16 bad = 2;

			memcpy(blk + 6, &bad, 2);
		}
		if ((op >= 0x200 && op <= 0x204) || op == 0x20d) {
			u16 ack = sim_prog_cmd(op, blk, ring[k * 8 + 1]);

			if (ack == 0xffff)	/* the modelled firmware does not answer this one */
				return;
			memcpy(blk + 6, &ack, 2);
		}
		if (op == ANE_H18_CSNE_CH_PROPERTY_WRITE && sim_nprops < 8) {
			memcpy(sim_prop[sim_nprops], blk + 8, 4);
			memcpy(sim_prop[sim_nprops] + 1, blk + 0xc, 8);
			if (sim_prop[sim_nprops][1] == sim_prop_refuse && sim_prop_refuse) {
				u16 one = 1;

				memcpy(blk + 6, &one, 2);
			}
			sim_nprops++;
		}
		if (op == ANE_H18_CSNE_RESOURCE_INFO_GET) {
			u32 a = 0xabcd0020, b = 0xabcd0050;

			memcpy(blk + 0x20, &a, 4);
			memcpy(blk + 0x50, &b, 4);
		}
		if (op == ANE_H18_CSNE_CONFIG_GET) {
			u32 rate = 0x16e3600;

			memcpy(blk + 8, &rate, 4);
		}
		ring[k * 8 + 2] = 0;
		ring[k * 8] = hdr | 1;
		if (sim_chan_req == 2 && op == ANE_H18_CSNE_CONFIG_GET) {
			u64 *sm = (u64 *)((u8 *)sim_dma[sim_ipc_i].va + 0xae00);

			sm[8] = sm[0] & ~1ULL;	/* the firmware gives its buffer back: a FREE in slot 1 */
			sm[9] = 0;
			sm[10] = 0;
		}
		return;
	}
}

static int fails;
static struct platform_device pdev;
static struct device_node node;

static void chk(int ok, const char *name)
{
	printf("  %-74s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok)
		fails++;
}

static int sim_in_dart(u64 pa)
{
	return pa >= SIM_DART_PA && pa < SIM_DART_PA + SIM_DART_SIZE;
}

static int log_count(const char *needle)
{
	const char *p = sim_log;
	int n = 0;

	while ((p = strstr(p, needle))) {
		n++;
		p++;
	}
	return n;
}

static int log_pos(const char *needle)
{
	const char *p = strstr(sim_log, needle);

	return p ? (int)(p - sim_log) : -1;
}

static bool result(const char *stg, const char *verdict, const char *reason)
{
	char head[80], tail[80];

	/* the soc sits between stage and verdict, and the reads tests run t8132 while the
	 * older ones run t8152: match the two ends separately */
	snprintf(head, sizeof(head), "ane_h18 RESULT stage=%s soc=", stg);
	snprintf(tail, sizeof(tail), "verdict=%s reason=%s", verdict, reason);
	return strstr(sim_log, head) != NULL && strstr(sim_log, tail) != NULL;
}

static int writes(void)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		n += sim_events[i].write;
	return n;
}

static int events_in(u64 base, u32 size, int write)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write == write && sim_events[i].pa >= base && sim_events[i].pa < base + size)
			n++;
	return n;
}

static u64 psd_pa(int i) { return SIM_PSD_BASE + ((u64)sim_psd_slot[i] << 8); }

static void reset_all(void)
{
	int i;

	sim_n_events = sim_faults = sim_n_iomap = sim_n_iounmap = 0;
	sim_jiffies = 0;
	sim_log_reset();
	for (i = 0; i < SIM_NPSD; i++) {
		sim_psd_mode[i] = SIM_PSD_ACK;
		sim_psd_state[i] = 6;
		sim_psd_valid[i] = sim_psd_initial_valid[i] = 0;
	}
	optin = "t8152";
	stage = "dt";
	confirm_power = false;
	psd_state = 0;
	psd_slots = 1;
	keep_on = false;
	keep_alive = false;
	wake_sleeping = false;
	read_mbox = false;
	read_ctrl = false;
	read_dart = false;
	alias_mbox = false;
	rtkit_hello = false;
	read_scratch = false;
	rtkit_start = false;
	rtkit_stop = 0;
	sim_n_tx_msgs = sim_idle_sent = sim_stop_needs_idle = sim_no_shutdown_reply = 0;
	sim_iop_ack_state = 0x20;
	sim_credit_mode = 0;
	sim_credits_now = 1;
	handshake = false;
	hs_cfg_size = 0x380000;
	hs_wait_ms = 50;
	sim_scr_model = sim_n_dma = sim_dma_fail = sim_default_dom_none = sim_hs_mode = sim_hs_block_ok = sim_hs_wakes = 0;
	hs_chan = hs_cmds = hs_start = hs_prog = hs_premap = false;
	hs_props = 0;
	sim_nprops = 0;
	sim_prop_refuse = 0;
	dart_inst = 0;
	prog_out_words = 0;
	hs_prog_calls = 1;
	hs_prog_reuse = false;
	premap_second = 0;
	sim_premapped = sim_pub_wait = sim_prog_mode = sim_prog_n = sim_prog_loaded = sim_prog_created = sim_t2h_back = sim_out_written = sim_t2h_posted = 0;
	memset(sim_prog_ops, 0, sizeof(sim_prog_ops));
	memset(sim_prog_ids, 0, sizeof(sim_prog_ids));
	memset(sim_fw_progs, 0, sizeof(sim_fw_progs));
	sim_fw_next_prog = sim_fw_next_proc = 0;
	sim_res_fail_op = 0;
	sim_res_fail_prog = 0;
	sim_res_fail_ack = 0;
	sim_res_spoof_prog = 0;
	sim_res_spoof_proc = 0;
	sim_res_dup_prog = 0;
	sim_res_malformed_create = 0;
	sim_cfu_fail_at = sim_cfu_calls = 0;
	sim_ctu_fail_at = sim_ctu_calls = 0;
	sim_kvmalloc_fail_at = sim_kvmalloc_calls = sim_kv_live = 0;
	sim_res_io_echo = 0;
	sim_res_io_last_in_sz = 0;
	sim_res_io_out_was_zero = 0;
	memset(sim_res_io_last_in, 0, sizeof(sim_res_io_last_in));
	sim_map_fail_iova = 0;
	sim_fw_hook = NULL;
	sim_fw_err = sim_fw_released = sim_fw_requests = 0;
	make_blob();
	prog_text_const = 0;
	prog_iova_kern = 0;
	prog_kern_slot = false;
	sim_load_sec2_iova = 0;
	sim_load_sec2_size = 0;
	sim_load_sec2_b1 = 0;
	hs_settle_ms = 10;
	sim_unmap_short_iova = 0;
	sim_owner_module.refs = 0;
	sim_db_writes = sim_hs_table = sim_s3_mode = sim_cmd_mode = sim_cmds_seen = sim_table_bad = sim_chan_req = 0;
	sim_pend_val = 0;
	sim_ack_writes = 0;
	sim_cmd_blk_in_pool = sim_posted = sim_term_tick = 0;
	sim_ack_last = 0;
	memset(sim_dma, 0, sizeof(sim_dma));
	map_fw = false;
	sim_rpm_enabled = sim_rpm_gets = sim_rpm_puts = sim_rpm_get_fail = 0;
	sim_rpm_get_fail_at = -1;
	ane_t8152_soc_shim = sim_soc_t8152_orig;	/* every probe sees the untouched row unless a pin shrank it */
	sim_n_iocalls = sim_io_fail = sim_n_dom_live = 0;
	sim_n_iomaps = 0;
	sim_start_hello = 0;
	sim_po_out_off = 0;
	sim_alias_credits = 1;
	sim_hello_min = 11;
	sim_hello_max = 12;
	sim_hello_ep = 0;
	sim_hello_type = 1;
	confirm_boot = false;
	boot_wait_ms = 300;
	sim_cpu_status = 0xa;
	sim_cpu_ctrl = 0;
	sim_run_status = 0x9;
	sim_rvbar_val = 0x10000000001ULL;
	sim_core_running = sim_core_stuck = sim_ctrl_writes = 0;
	sim_sleeping = 0;
	memset(sim_po_msg, 0, sizeof(sim_po_msg));
	memset(sim_po_n, 0, sizeof(sim_po_n));
	memset(sim_po_head, 0, sizeof(sim_po_head));
	sim_start_chan = -1;
	sim_start_msgs = 0;
	psd_timeout_ms = 1000;
	memset(sim_phandle, 0, sizeof(sim_phandle));
	memset(sim_phandle_target, 0, sizeof(sim_phandle_target));
	sim_pd_attached = sim_pd_false_votes = sim_pd_attach_fail = sim_count_error = 0;
	sim_pd_list.num_pds = 0;
	sim_pdev_dev = &pdev.dev;
	sim_rd_ready = 0;
	sim_res_ready = 0;
	sim_res_entry.live = false;
	sim_res_dir_entry.live = false;
	sim_ps_on = false;
	sim_iomap_fail_at = 0;
	allow_reads = false;
	confirm_power = false;
	pmgr_gate_pa = 0;
	pmgr_gate_mask = 0;
	pmgr_gate2_pa = 0;
	pmgr_gate2_mask = 0;
	pmgr_ps_pa = 0;
	pmgr_ps_mask = 0;
	memset(&pdev, 0, sizeof(pdev));
	memset(&node, 0, sizeof(node));
	node.n_reg = 5;
	node.reg_pa[0] = 0x419600000ULL; node.reg_size[0] = 0x74000; node.reg_name[0] = "asc";
	node.reg_pa[1] = 0x419050000ULL; node.reg_size[1] = 0x4000; node.reg_name[1] = "rvbar";
	node.reg_pa[2] = 0x316000000ULL; node.reg_size[2] = 0x1c000; node.reg_name[2] = "mbox";
	node.reg_pa[3] = psd_pa(0); node.reg_size[3] = 0x100; node.reg_name[3] = "psd0";
	node.reg_pa[4] = psd_pa(1); node.reg_size[4] = 0x100; node.reg_name[4] = "psd1";
	node.has_type = node.has_num = true;
	node.ane_type = 784;
	node.mbox_num = 12;
	pdev.dev.of_node = &node;
}

static int probe_now(void) { return ane_h18_probe(&pdev); }

static void pos_dt(void)
{
	int ret;

	reset_all();
	stage = "dt";
	ret = probe_now();
	chk(ret == 0, "P1 dt: probe returns 0");
	chk(result("dt", "PASS", "dt-windows-match"), "P1 dt: RESULT PASS dt-windows-match");
	chk(sim_n_events == 0 && sim_faults == 0, "P1 dt: zero bus accesses");
	chk(sim_n_iomap == 0, "P1 dt: nothing mapped");
}

static void pos_psd(void)
{
	int ret;

	reset_all();
	stage = "psd";
	ret = probe_now();
	chk(ret == 0, "P2 psd (idle slots): probe returns 0");
	chk(result("psd", "PASS", "psd-read-only"), "P2 psd: RESULT PASS psd-read-only");
	chk(writes() == 0, "P2 psd: zero bus writes");
	chk(sim_faults == 0, "P2 psd: zero faults");
	chk(events_in(psd_pa(0), 0x100, 0) == 1 && events_in(psd_pa(1), 0x100, 0) == 1,
	    "P2 psd: one read per slot (VALID only, it is not valid)");
	chk(events_in(sim_asc_pa, sim_asc_size, 0) + events_in(sim_rvbar_pa, sim_rvbar_size, 0) + events_in(sim_mbox_pa, sim_mbox_size, 0) == 0,
	    "P2 psd: no ASC, IORVBAR or mailbox read");
	chk(log_count("not-valid") == 2, "P2 psd: both slots reported not-valid");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2, "P2 psd: two windows mapped and released on remove");

	reset_all();
	stage = "psd";
	sim_psd_valid[0] = sim_psd_state[0] ? 1 : 1;
	sim_psd_state[0] = 0;
	ret = probe_now();
	chk(ret == 0 && events_in(psd_pa(0), 0x100, 0) == 2 && log_count("state=P0-run") == 1,
	    "P2b psd: a valid slot also reads STATUS and reports P0-run");
	chk(writes() == 0, "P2b psd: still zero writes");
}

static void pos_refuse(void)
{
	int ret;

	reset_all();
	stage = "power";
	ret = probe_now();
	chk(ret == -EPERM && result("power", "REFUSED", "confirm_power=1 required"), "P3 power without confirm_power: REFUSED, -EPERM");
	chk(writes() == 0 && events_in(sim_asc_pa, sim_asc_size, 0) == 0, "P3 power without confirm_power: no write and no ASC read");
	chk(sim_n_iomap == sim_n_iounmap, "P3 power without confirm_power: windows released");
}

/* the sequence of writes as "pa:val" tokens */
static void write_seq(char *out, size_t n)
{
	int i;

	out[0] = 0;
	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write)
			snprintf(out + strlen(out), n - strlen(out), "%llx=%u ", (unsigned long long)sim_events[i].pa, sim_events[i].val);
}

static int first_asc_read(void)
{
	int i;

	for (i = 0; i < sim_n_events; i++)
		if (!sim_events[i].write && (sim_in(sim_events[i].pa, sim_asc_pa, sim_asc_size) || sim_in(sim_events[i].pa, sim_rvbar_pa, sim_rvbar_size) ||
					     sim_in(sim_events[i].pa, sim_mbox_pa, sim_mbox_size)))
			return i;
	return -1;
}

static int last_write(void)
{
	int i, l = -1;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write)
			l = i;
	return l;
}

static int first_write(void)
{
	int i;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write)
			return i;
	return -1;
}

static void pos_power(void)
{
	char seq[256], want[256];
	int ret, i;

	/* one slot, default state 0 */
	reset_all();
	stage = "power";
	confirm_power = true;
	ret = probe_now();
	chk(ret == 0 && result("power", "PASS", "psd-acked+asc-read"), "P4 power slot 30 only: PASS");
	write_seq(seq, sizeof(seq));
	snprintf(want, sizeof(want), "%llx=0 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(0) + 0x90);
	chk(!strcmp(seq, want), "P4 power: writes are exactly REQUEST=0 on slot 30, then REQUEST=6 on slot 30");
	chk(events_in(psd_pa(1), 0x100, 1) == 0 && events_in(psd_pa(1), 0x100, 0) == 0, "P4 power: slot 31 never touched");
	chk(sim_faults == 0, "P4 power: zero faults (no ASC read before the acknowledge)");
	chk(first_asc_read() > first_write() && first_asc_read() < last_write(), "P4 power: ASC reads sit after the first write and before the rail-gate write");
	chk(events_in(sim_asc_pa, sim_asc_size, 0) == 2 && events_in(sim_rvbar_pa, sim_rvbar_size, 0) == 2, "P4 power: two ASC reads, two IORVBAR reads");
	chk(events_in(sim_mbox_pa, sim_mbox_size, 0) == 0, "P4 power: no mailbox read without read_mbox");
	chk(events_in(sim_asc_pa + 0x44, 4, 0) == 0 && events_in(sim_asc_pa, sim_asc_size, 1) == 0, "P4 power: CPU_CONTROL not read by default, ASC window never written");
	chk(log_count("CRIT: ane_h18 write") == writes(), "P4 power: every write was logged before it");
	chk(log_count("CRIT: ane_h18 read CPU_STATUS") == 1 && log_count("CRIT: ane_h18 read ASC_STATUS") == 1 &&
	    log_count("CRIT: ane_h18 read IORVBAR_LO") == 1 && log_count("CRIT: ane_h18 read IORVBAR_HI") == 1,
	    "P4 power: every read was logged");
	chk(log_pos("CRIT: ane_h18 write psd0 slot=30 REQUEST=0") < log_pos("state 0 acknowledged"), "P4 power: write logged before the acknowledge line");
	chk(sim_psd_state[0] == 6, "P4 power: slot 30 ends at state 6");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == sim_n_iounmap, "P4 power: all windows released on remove");

	/* two slots */
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = 2;
	ret = probe_now();
	write_seq(seq, sizeof(seq));
	snprintf(want, sizeof(want), "%llx=0 %llx=0 %llx=6 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(1) + 0x90,
		 (unsigned long long)psd_pa(1) + 0x90, (unsigned long long)psd_pa(0) + 0x90);
	chk(ret == 0 && !strcmp(seq, want), "P5 power two slots: 30 on, 31 on, then 31 off, 30 off");

	/* keep_on */
	reset_all();
	stage = "power";
	confirm_power = true;
	keep_on = true;
	ret = probe_now();
	chk(ret == 0 && writes() == 1 && sim_psd_state[0] == 0, "P6 keep_on: one write, the slot stays at state 0");

	/* read_ctrl: CPU_CONTROL is read once, still never written */
	reset_all();
	stage = "power";
	confirm_power = true;
	read_ctrl = true;
	ret = probe_now();
	chk(ret == 0 && events_in(sim_asc_pa + 0x44, 4, 0) == 1 && events_in(sim_asc_pa, sim_asc_size, 1) == 0 && log_count("CRIT: ane_h18 read CPU_CONTROL pa=0x419600044") == 1,
	    "P8 read_ctrl: CPU_CONTROL read once, logged before, ASC window never written");

	/* read_mbox: the three candidate channels, exact addresses */
	reset_all();
	stage = "power";
	confirm_power = true;
	read_mbox = true;
	ret = probe_now();
	chk(ret == 0 && events_in(sim_mbox_pa, sim_mbox_size, 0) == 3, "P7 read_mbox: three mailbox reads");
	for (i = 0; i < 3; i++) {
		static const unsigned int ch[3] = { 12, 24, 25 };
		bool found = false;
		int k;

		for (k = 0; k < sim_n_events; k++)
			if (!sim_events[k].write && sim_events[k].pa == sim_mbox_pa + ch[i] * 0x80 + 0x20)
				found = true;
		char nm[64];

		snprintf(nm, sizeof(nm), "P7 read_mbox: channel %u status at 0x%llx", ch[i], (unsigned long long)(sim_mbox_pa + ch[i] * 0x80 + 0x20));
		chk(found, nm);
	}
}

static int neg_psd_failure(int mode, int slots)
{
	int ret, bad = 0;
	char seq[256], want[256];

	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = slots;
	if (slots == 2) {
		sim_psd_mode[1] = mode;		/* slot 31 fails, slot 30 acks */
	} else {
		sim_psd_mode[0] = mode;
	}
	ret = probe_now();
	if (ret != -ETIMEDOUT) {
		printf("    expected -ETIMEDOUT, got %d\n", ret);
		bad = 1;
	}
	if (!result("power", "FAIL", "psd-not-acknowledged")) {
		printf("    expected RESULT FAIL psd-not-acknowledged\n");
		bad = 1;
	}
	if (first_asc_read() >= 0) {
		printf("    an ASC, IORVBAR or mailbox read happened without an acknowledge\n");
		bad = 1;
	}
	if (sim_faults) {
		printf("    bus faults: %d\n", sim_faults);
		bad = 1;
	}
	write_seq(seq, sizeof(seq));
	if (slots == 2)
		snprintf(want, sizeof(want), "%llx=0 %llx=0 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(1) + 0x90,
			 (unsigned long long)psd_pa(0) + 0x90);
	else
		snprintf(want, sizeof(want), "%llx=0 ", (unsigned long long)psd_pa(0) + 0x90);
	if (strcmp(seq, want)) {
		printf("    write sequence '%s', want '%s'\n", seq, want);
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    a failed probe leaked a window\n");
		bad = 1;
	}
	if (sim_jiffies < psd_timeout_ms) {
		printf("    the wait ended before the timeout (%lu ms)\n", sim_jiffies);
		bad = 1;
	}
	return bad;
}

/* a slot that acknowledges the run state and ignores the request for state 6: the stage must fail, never report PASS */
static int neg_undo6(void)
{
	int bad = 0, ret;

	reset_all();
	stage = "power";
	confirm_power = true;
	psd_state = 0;
	psd_slots = 1;
	sim_psd_mode[0] = SIM_PSD_IGNORE6;
	ret = probe_now();
	if (ret == 0 || !result("power", "FAIL", "psd-not-returned-to-state-6") || log_count("verdict=PASS") != 0) {
		printf("    a slot that ignores state 6: ret %d, no FAIL psd-not-returned-to-state-6 or a PASS line\n", ret);
		bad = 1;
	}
	if (log_count("did not return to state 6") != 1 || sim_psd_state[0] != 0) {
		printf("    the warning line or the slot state is not as expected (state %u)\n", sim_psd_state[0]);
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    leaked a window\n");
		bad = 1;
	}
	return bad;
}

static int neg_never(void)
{
	int bad = neg_psd_failure(SIM_PSD_NEVER, 1) | neg_psd_failure(SIM_PSD_NEVER, 2) | neg_undo6();

	printf("%s\n", bad ? "  neg-psd-never: harness FAILED the module (BAD)" : "  neg-psd-never: module fails as required");
	return bad;
}

static int neg_wrong(void)
{
	int bad = neg_psd_failure(SIM_PSD_WRONG_STATE, 1) | neg_psd_failure(SIM_PSD_WRONG_STATE, 2);

	printf("%s\n", bad ? "  neg-psd-wrong: harness FAILED the module (BAD)" : "  neg-psd-wrong: module fails as required");
	return bad;
}

static int neg_dt(void)
{
	int c, ret, bad = 0;

	for (c = 0; c < 8; c++) {
		const char *what = "";

		reset_all();
		stage = "power";
		confirm_power = true;
		switch (c) {
		case 0: node.reg_pa[0] += 0x1000000; what = "asc base moved"; break;
		case 1: node.reg_size[1] -= 0x1000; what = "rvbar shorter"; break;
		case 2: node.reg_pa[3] = psd_pa(0) + 0x100; what = "psd0 on the next slot"; break;
		case 3: node.reg_pa[4] = 0x303a15200ULL; what = "psd1 on a GPU slot"; break;
		case 4: node.mbox_num = 0; what = "mailbox number 0 (the first overlay's value)"; break;
		case 5: node.ane_type = 1; what = "wrong ane-type"; break;
		case 6: node.reg_name[2] = "mailbox"; what = "reg-names mbox missing"; break;
		case 7: node.has_num = false; what = "no mailbox number"; break;
		}
		ret = probe_now();
		if (ret == 0 || !strstr(sim_log, "verdict=FAIL reason=dt-window-mismatch")) {
			printf("    %s: probe returned %d without a dt-window-mismatch FAIL\n", what, ret);
			bad = 1;
		}
		if (sim_n_events || sim_n_iomap) {
			printf("    %s: bus access or mapping before the check (events %d, iomap %d)\n", what, sim_n_events, sim_n_iomap);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-dt: harness FAILED the module (BAD)" : "  neg-dt: module refuses as required");
	return bad;
}


static void boot_setup(void)
{
	reset_all();
	stage = "boot";
	owned_fw = false;
	fw_path = NULL;
	sim_fw_data = sim_blob;
	sim_fw_size = sim_blob_len;
	confirm_power = true;
	confirm_boot = true;
	psd_slots = 2;
	psd_state = 1;
}

static int ctrl_write_events(void) { return events_in(sim_asc_pa + 0x44, 4, 1); }

/* kernel-powered node: power-domains and iommus in the node, both PSD slots already on (the kernel PSD driver did it) */
static void kboot_setup(void)
{
	boot_setup();
	node.has_power_domains = true;
	node.has_iommus = true;
	sim_psd_state[0] = sim_psd_state[1] = 1;
	map_fw = true;
}

static int psd_write_events(void)
{
	int k, n = 0;

	for (k = 0; k < sim_n_events; k++)
		if (sim_events[k].write == 1 && sim_events[k].pa >= SIM_PSD_BASE && sim_events[k].pa < SIM_PSD_BASE + 0x40000)
			n++;
	return n;
}

/* real bus writes only: the write==2 events are the shim's runtime-PM markers, not bus accesses */
static int bus_write_events(void)
{
	int k, n = 0;

	for (k = 0; k < sim_n_events; k++)
		n += sim_events[k].write == 1;
	return n;
}

static int iocalls_of(int code)
{
	int k, n = 0;

	for (k = 0; k < sim_n_iocalls && k < 64; k++)
		n += sim_iocalls[k].code == code;
	return n;
}

static int first_run_event(void)
{
	int k, run = -1;

	for (k = 0; k < sim_n_events; k++)
		if (sim_events[k].write == 1 && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
			run = k;
	return run;
}


/* the program file reader on its own: every limit and every overflow is refused, the good file is accepted */
static void pos_prog_parse(void)
{
	static u8 b[0x8000];
	struct ane_h18_pdesc pd;
	const char *why;
	size_t n;
	u64 big = ~0ULL, v;
	u32 two = 2;
	int r_good, k;
	struct { const char *what; size_t off; const void *val; size_t len; } mut[] = {
		{ "a section size of zero", 0x20 + 8, &(u64){ 0 }, 8 },
		{ "a section over the buffer limit", 0x20 + 8, &(u64){ ANE_H18_PROG_MAX_BUF + 1 }, 8 },
		{ "a section offset near 2^64", 0x20 + 16, &big, 8 },
		{ "a section size that runs past the end", 0x20 + 8, &(u64){ 0x7ffff }, 8 },
		{ "a section offset that is not 16 byte aligned", 0x20 + 16, &(u64){ 0x1008 + 4 }, 8 },
		{ "a section offset inside the tables", 0x20 + 16, &(u64){ 0x10 }, 8 },
		{ "a section id of zero", 0x20, &(u64){ 0 }, 4 },
		{ "two sections with one id", 0x20 + 0x20, &(u32){ 1 }, 4 },
		{ "nine buffers", 12, &(u32){ 9 }, 4 },
		{ "no sections", 8, &(u32){ 0 }, 4 },
		{ "ten sections", 8, &(u32){ 10 }, 4 },
		{ "a buffer direction of 3", 0x20 + 6 * 0x20 + 4, &(u32){ 3 }, 4 },
		{ "initial data larger than its buffer", 0x20 + 6 * 0x20 + 24, &(u64){ 0x4001 }, 8 },
		{ "initial data offset near 2^64", 0x20 + 6 * 0x20 + 16, &big, 8 },
		{ "a buffer of zero bytes", 0x20 + 6 * 0x20 + 8, &(u64){ 0 }, 8 },
		{ "two buffers with one id", 0x20 + 7 * 0x20, &(u32){ 5 }, 4 },
		{ "a wrong size in the header", 16, &(u64){ 0x1234 }, 8 },
		{ "version 2", 4, &two, 4 },
	};

	make_blob();
	memcpy(b, sim_blob, sim_blob_len);
	n = sim_blob_len;
	r_good = ane_h18_prog_parse(b, n, &pd, &why);
	chk(r_good == 0 && pd.nsec == 6 && pd.nio == 3 && pd.sec[5].id == 7 && pd.io[2].dir == 1 && pd.io[0].dsize == 0x40, "PG12 the good program file is accepted with its six sections and three buffers");
	for (k = 0; k < (int)(sizeof(mut) / sizeof(mut[0])); k++) {
		char name[120];

		memcpy(b, sim_blob, sim_blob_len);
		memcpy(b + mut[k].off, mut[k].val, mut[k].len);
		snprintf(name, sizeof(name), "PG13 refused: %s", mut[k].what);
		chk(ane_h18_prog_parse(b, n, &pd, &why) == -EINVAL, name);
	}
	/* the same files, truncated: the header or the tables do not fit */
	memcpy(b, sim_blob, sim_blob_len);
	chk(ane_h18_prog_parse(b, 0x10, &pd, &why) == -EINVAL && ane_h18_prog_parse(b, 0x100, &pd, &why) == -EINVAL, "PG14 a file shorter than its header or tables is refused");
	/* total over the limit: buffers are not bounded by the file (zero filled), so three of 64 MiB plus the sections are over 192 MiB; two of 64 MiB are fine */
	memcpy(b, sim_blob, sim_blob_len);
	v = ANE_H18_PROG_MAX_BUF;
	memcpy(b + 0x20 + 6 * 0x20 + 8, &v, 8);
	memcpy(b + 0x20 + 7 * 0x20 + 8, &v, 8);
	chk(ane_h18_prog_parse(b, n, &pd, &why) == 0, "PG15 two buffers of 64 MiB with the sections are within the byte limit");
	memcpy(b + 0x20 + 8 * 0x20 + 8, &v, 8);
	chk(ane_h18_prog_parse(b, n, &pd, &why) == -EINVAL, "PG15 three buffers of 64 MiB with the sections are over the byte limit and refused");
}

static void pos_boot(void)
{
	char seq[512], want[512];
	int ret, i;

	/* happy path: the core starts, reports running, posts two messages on channel 25, stops on request */
	boot_setup();
	sim_start_chan = 25;
	sim_start_msgs = 2;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "B1 boot: PASS core-ran-and-stopped");
	write_seq(seq, sizeof(seq));
	snprintf(want, sizeof(want), "%llx=1 %llx=1 %llx=16 %llx=0 %llx=6 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(1) + 0x90,
		 (unsigned long long)sim_asc_pa + 0x44, (unsigned long long)sim_asc_pa + 0x44, (unsigned long long)psd_pa(1) + 0x90, (unsigned long long)psd_pa(0) + 0x90);
	if (strcmp(seq, want))
		printf("    got  %s\n    want %s\n", seq, want);
	chk(!strcmp(seq, want), "B1 boot: writes are exactly PSD 30 and 31 on, CONTROL=16 (0x10), CONTROL=0, PSD 31 and 30 off");
	chk(sim_faults == 0, "B1 boot: zero faults");
	chk(log_count("ane_h18 msg ch25 msg0=0x1000 msg1=0x100") == 1 && log_count("ane_h18 msg ch25 msg0=0x1001 msg1=0x100") == 1, "B1 boot: both messages logged with their raw words");
	chk(log_count("running_seen=1 messages=2") == 1 && sim_core_running == 0 && sim_cpu_status == 0xa, "B1 boot: running seen, two messages, core stopped at the end");
	{
		int first_run = -1, k;

		for (k = 0; k < sim_n_events; k++)
			if (sim_events[k].write && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
				first_run = k;
		chk(first_run > 0 && first_run > first_asc_read(), "B1 boot: the RUN write comes after the preflight reads");
	}
	chk(log_count("CRIT: ane_h18 write CPU_CONTROL=0x10") == 1 && log_count("CRIT: ane_h18 write CPU_CONTROL=0 ") == 1, "B1 boot: both CPU_CONTROL writes logged before");
	chk(events_in(sim_rvbar_pa, sim_rvbar_size, 1) == 0 && events_in(sim_mbox_pa, sim_mbox_size, 1) == 0, "B1 boot: IORVBAR and the mailbox are never written");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == sim_n_iounmap, "B1 boot: windows released on remove after a clean stop");

	/* message cap */
	boot_setup();
	sim_start_chan = 25;
	sim_start_msgs = 20;
	ret = probe_now();
	if (log_count("ane_h18 msg ch25") != ANE_H18_PO_MAXMSG)
		printf("    msg lines: %d\n", log_count("ane_h18 msg ch25"));
	chk(ret == 0 && log_count("ane_h18 msg ch25") == ANE_H18_PO_MAXMSG, "B2 boot: at most 8 messages are read");

	/* messages on the other candidate channels are seen too */
	boot_setup();
	sim_start_chan = 12;
	sim_start_msgs = 1;
	ret = probe_now();
	chk(ret == 0 && log_count("ane_h18 msg ch12") == 1, "B3 boot: a message on candidate channel 12 is read");

	/* read_dart: twelve reads of the DART block, after both PSD acknowledges and before the RUN write, never a write */
	boot_setup();
	read_dart = true;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "D1 boot read_dart: PASS");
	chk(events_in(SIM_DART_PA, SIM_DART_SIZE, 0) == ANE_H18_DART_NREAD && events_in(SIM_DART_PA, SIM_DART_SIZE, 1) == 0, "D1 boot read_dart: twelve DART reads, no DART write");
	chk(sim_faults == 0, "D1 boot read_dart: zero faults (the DART block was read with both slots on)");
	{
		int first_dart = -1, last_dart = -1, run = -1, psd_ack2 = -1, k;

		for (k = 0; k < sim_n_events; k++) {
			if (sim_in(sim_events[k].pa, SIM_DART_PA, SIM_DART_SIZE)) {
				if (first_dart < 0)
					first_dart = k;
				last_dart = k;
			}
			if (sim_events[k].write && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
				run = k;
			if (sim_events[k].write && sim_events[k].pa == psd_pa(1) + 0x90 && sim_events[k].val == 1)
				psd_ack2 = k;
		}
		chk(first_dart > psd_ack2 && psd_ack2 >= 0 && last_dart < run && run > 0, "D1 boot read_dart: DART reads sit after PSD slot 31 on and before the RUN write");
	}
	chk(log_count("CRIT: ane_h18 read DART0_0 pa=0x419800000") == 1 && log_count("CRIT: ane_h18 read DART0_0x1438 pa=0x419801438") == 1, "D1 boot read_dart: each read is logged before it is done");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == sim_n_iounmap, "D1 boot read_dart: the DART mapping is released on remove");
	/* without read_dart the DART block is never touched, and no window is mapped for it */
	boot_setup();
	ret = probe_now();
	chk(ret == 0 && events_in(SIM_DART_PA, SIM_DART_SIZE, 0) == 0 && events_in(SIM_DART_PA, SIM_DART_SIZE, 1) == 0, "D2 boot: no DART access without read_dart");
	/* alias bank: the status word at window + 0x10c20 is read only with alias_mbox, and a message there is logged raw */
	boot_setup();
	ret = probe_now();
	chk(ret == 0 && events_in(sim_mbox_pa + ANE_H18_PO_ALIAS_OFF, 0x1000, 0) == 0, "A1 boot: the alias bank is not read without alias_mbox");
	boot_setup();
	alias_mbox = true;
	sim_start_chan = 536;
	sim_start_msgs = 1;
	ret = probe_now();
	chk(ret == 0 && log_count("ane_h18 msg alias24 rx=0x1000,0,0x100,0") == 1, "A2 boot alias_mbox: a message on the alias channel is read and logged raw");
	{
		int k, st = 0;

		for (k = 0; k < sim_n_events; k++)
			if (!sim_events[k].write && sim_events[k].pa == sim_mbox_pa + 0x10c20)
				st++;
		chk(st >= 1 && events_in(sim_mbox_pa, sim_mbox_size, 1) == 0, "A2 boot alias_mbox: the status word at window + 0x10c20 is read, the mailbox is never written");
	}

	/* RTKit exchange: HELLO answered with version 12 and the last EPMAP answered with the same flag, in TX (+0x10) only; RX is +0x00 */
	{
		int k, hello_w = 0, epmap_w = 0, wrong_half = 0;

		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		sim_start_chan = 536;
		sim_start_hello = 1;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "H1 boot rtkit_hello: PASS");
		for (k = 0; k < sim_n_events; k++) {
			u64 pa = sim_events[k].pa;

			if (!sim_events[k].write || !sim_in(pa, sim_mbox_pa, sim_mbox_size))
				continue;
			if (pa >= sim_mbox_pa + 0x10c00 && pa < sim_mbox_pa + 0x10c10)
				wrong_half++;
			if (pa == sim_mbox_pa + 0x10c10 && sim_events[k].val == 0x000c000c)
				hello_w++;
			if (pa == sim_mbox_pa + 0x10c14 && sim_events[k].val == 0x00200000)
				hello_w++;
			if (pa == sim_mbox_pa + 0x10c14 && sim_events[k].val == 0x00880000)
				epmap_w++;
		}
		chk(hello_w == 2 && epmap_w == 1 && wrong_half == 0, "H1 boot rtkit_hello: HELLO_REPLY version 12 and the EPMAP reply go to TX (+0x10) only, never to RX");
		chk(events_in(sim_mbox_pa, sim_mbox_size, 1) == 8 && log_count("CRIT: ane_h18 write TX of alias24 pa=0x316010c10") == 2, "H1 boot rtkit_hello: two replies of four words, each logged before");
		if (log_count("ane_h18 rtkit rx type=") != 2 || sim_faults)
			printf("    rx lines %d, faults %d\n", log_count("ane_h18 rtkit rx type="), sim_faults);
		chk(log_count("ane_h18 rtkit rx type=") == 2 && sim_faults == 0, "H1 boot rtkit_hello: both messages decoded and logged, zero faults");
	}
	/* rtkit_start: the whole management boot against a firmware that reacts to what the host writes */
	{
		static const u64 want_start[6] = { 0x200000000c000cULL, 0x80000000000001ULL, 0x88000100000000ULL, 0x50000100000002ULL, 0xb0000000000020ULL, 0x50002000000002ULL };
		int k, same;

		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		sim_start_chan = 536;
		sim_start_hello = 3;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "R1 boot rtkit_start: PASS");
		same = sim_n_tx_msgs == 6;
		for (k = 0; k < 6 && k < sim_n_tx_msgs; k++)
			same &= sim_tx_msgs[k] == want_start[k];
		if (!same)
			for (k = 0; k < sim_n_tx_msgs && k < 32; k++)
				printf("    tx[%d] = %#llx\n", k, (unsigned long long)sim_tx_msgs[k]);
		chk(same, "R1 boot rtkit_start: HELLO_REPLY, two EPMAP replies, STARTEP ep 1, AP power ON, STARTEP ep 0x20, in that order and nothing else");
		chk(sim_faults == 0 && log_count("rtkit endpoint 0x20 discovered") == 1 && log_count("AP power state acknowledged: 0xb0000000000020") + log_count("AP power state acknowledged: 0x20") == 1, "R1 boot rtkit_start: endpoint 0x20 discovered, the AP power acknowledgement seen, zero faults");
		/* one TX slot that comes back one poll later: the same six messages, in the same order, none lost (a queue, not a drop) */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_credit_mode = 1;
		ret = probe_now();
		same = sim_n_tx_msgs == 6;
		for (k = 0; k < 6 && k < sim_n_tx_msgs; k++)
			same &= sim_tx_msgs[k] == want_start[k];
		if (!same)
			for (k = 0; k < sim_n_tx_msgs && k < 32; k++)
				printf("    tx[%d] = %#llx\n", k, (unsigned long long)sim_tx_msgs[k]);
		chk(ret == 0 && same, "Q1 boot rtkit_start with one TX slot returning each poll: the six messages go out in order, a message that finds no slot is queued and sent later");
		/* an IOP acknowledgement for another state does not get AP power ON */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_iop_ack_state = 0x10;
		ret = probe_now();
		chk(ret == 0 && sim_n_tx_msgs == 4, "R2 boot rtkit_start: an IOP acknowledgement for state 0x10 gets no AP power ON and no application endpoint start");
		/* without rtkit_start only HELLO and the maps are answered */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		sim_start_chan = 536;
		sim_start_hello = 3;
		ret = probe_now();
		chk(ret == 0 && sim_n_tx_msgs == 3, "R3 boot rtkit_hello alone: three replies, no STARTEP, no power state message");
		/* rtkit_stop: AP OFF, IOP OFF, IOP idle, and only then does the modelled core accept the RUN clear */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 1;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_stop_needs_idle = 1;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped") && sim_n_tx_msgs == 8 && sim_tx_msgs[6] == 0xb0000000000010ULL && sim_tx_msgs[7] == 0x60000000000001ULL,
		    "R4 boot rtkit_stop: AP power QUIESCED then IOP power SLEEP follow the boot messages and the core stops after the IOP acknowledgement");
		chk(log_count("rtkit shutdown: ap_acked=1 iop_acked=1 iop_idle=0") == 1, "R4 boot rtkit_stop: both acknowledgements seen");
		/* rtkit_stop=2: OFF then OFF, the pair the board stopped with */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 2;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_stop_needs_idle = 1;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped") && sim_n_tx_msgs == 8 && sim_tx_msgs[6] == 0xb0000000000000ULL && sim_tx_msgs[7] == 0x60000000000000ULL,
		    "R6 boot rtkit_stop=2: AP power OFF then IOP power OFF, the core stops after the IOP acknowledgement");
		/* a firmware that ignores the shutdown while the core still accepts the RUN clear: the stage must not report PASS */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 1;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_no_shutdown_reply = 1;
		ret = probe_now();
		chk(ret != 0 && result("boot", "FAIL", "rtkit-shutdown-incomplete") && log_count("verdict=PASS") == 0, "R8 boot rtkit_stop: an unanswered shutdown is FAIL rtkit-shutdown-incomplete even when the core stopped");
		/* rtkit_stop=3 is refused */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 3;
		ret = probe_now();
		chk(ret == -EINVAL && !writes(), "R7 boot rtkit_stop=3 is refused before any write");
		/* a firmware that never answers the shutdown: no PASS, the slots stay, the core is reported as not stopped */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 1;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_stop_needs_idle = 1;
		sim_no_shutdown_reply = 1;
		ret = probe_now();
		chk(ret != 0 && result("boot", "FAIL", "core-not-stopped") && log_count("ap_acked=0 iop_acked=0") == 1 && sim_n_tx_msgs == 7, "R5 boot rtkit_stop: no shutdown answer gives FAIL core-not-stopped, AP QUIESCED sent once, no IOP SLEEP");
	}
	/* read_scratch: three reads of eight words, read only, with both slots on; none without the option */
	boot_setup();
	alias_mbox = true;
	rtkit_hello = true;
	rtkit_start = true;
	read_scratch = true;
	sim_start_chan = 536;
	sim_start_hello = 3;
	ret = probe_now();
	chk(ret == 0 && events_in(SIM_SCRATCH_PA, SIM_SCRATCH_SIZE, 0) == 3 * ANE_H18_SCRATCH_N && events_in(SIM_SCRATCH_PA, SIM_SCRATCH_SIZE, 1) == 0 && sim_faults == 0,
	    "S1 boot read_scratch: three phases of eight SCRATCH reads, no write, zero faults");
	chk(log_count("CRIT: ane_h18 read SCRATCH0_pre pa=0x4198a0024") == 1 && log_count("CRIT: ane_h18 read SCRATCH7_apon pa=0x4198a0040") == 1 && log_count("CRIT: ane_h18 read SCRATCH0_end pa=0x4198a0024") == 1,
	    "S1 boot read_scratch: the addresses engine + 0x18a0024 to +0x18a0040 are logged before each read, in the phases pre, apon and end");
	{
		int k, first_scratch = -1, run = -1;

		for (k = 0; k < sim_n_events; k++) {
			if (first_scratch < 0 && sim_in(sim_events[k].pa, SIM_SCRATCH_PA, SIM_SCRATCH_SIZE))
				first_scratch = k;
			if (sim_events[k].write == 1 && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
				run = k;
		}
		chk(first_scratch > 0 && first_scratch < run, "S1 boot read_scratch: the first SCRATCH read comes before the RUN write");
	}
	boot_setup();
	alias_mbox = true;
	rtkit_hello = true;
	sim_start_chan = 536;
	sim_start_hello = 3;
	ret = probe_now();
	chk(ret == 0 && events_in(SIM_SCRATCH_PA, SIM_SCRATCH_SIZE, 0) == 0, "S2 boot: no SCRATCH access without read_scratch");
	/* handshake: the surfaces, the init block, SCRATCH0, SCRATCH1, the wake word, the table address */
	{
		static const u32 scr0[8] = { 8, 0xc440, 0x100, 0xf00000, 0, 0x100090, 0, ANE_H18_HS_READY };
		int pass, k, w0 = -1, w1 = -1, w7 = -1, run = -1, shut = -1, live;
		int nsc, nst;

		for (pass = 0; pass < 11; pass++) {
			kboot_setup();
			alias_mbox = true;
			rtkit_hello = true;
			rtkit_start = true;
			rtkit_stop = pass == 2 ? 0 : 1;
			read_scratch = true;
			handshake = true;
			sim_start_chan = 536;
			sim_start_hello = 3;
			sim_scr_model = 1;
			memcpy(sim_scr, scr0, sizeof(scr0));
			if (pass == 1) sim_hs_mode = 1;
			if (pass == 3) sim_scr[0] = 0x21;
			if (pass == 4) sim_dma_fail = 3;
			if (pass == 5) sim_default_dom_none = 1;
			if (pass == 6) sim_scr[3] = 0x10000000;
			if (pass == 7) sim_scr[1] = 0xffffffff;
			if (pass == 8) sim_hs_mode = 2;
			if (pass == 9) sim_scr[7] = 0;
			if (pass == 10) { sim_cpu_status = 0xe; sim_sleeping = 1; wake_sleeping = true; }
			ret = probe_now();
			nsc = events_in(SIM_SCRATCH_PA, SIM_SCRATCH_SIZE, 1);
			for (live = 0, k = 0; k < sim_n_dma; k++)
				live += sim_dma[k].live;
			nst = log_count("ane_h18 rtkit shutdown: ap_acked=1 iop_acked=1");
			if (pass == 10) {
				int woke = 0;

				for (k = 0; k < sim_n_tx_msgs && k < 32; k++)
					woke |= sim_tx_msgs[k] == ((6ULL << 52) | 0x20);
				chk(ret == 0 && result("boot", "PASS", "core-handshook-and-stopped") && woke && !sim_sleeping && sim_faults == 0,
				    "HS10 wake_sleeping=1 with the core in the post-shutdown state (0xe): RUN, then IOP power ON (0x20) wakes it, the handshake and the stop pass");
			}
			if (pass == 0) {
				for (k = 0; k < sim_n_events; k++) {
					if (sim_events[k].write == 1 && sim_events[k].pa == SIM_SCRATCH_PA + 0x24 && w0 < 0) w0 = k;
					if (sim_events[k].write == 1 && sim_events[k].pa == SIM_SCRATCH_PA + 0x28 && w1 < 0) w1 = k;
					if (sim_events[k].write == 1 && sim_events[k].pa == SIM_SCRATCH_PA + 0x40 && w7 < 0) w7 = k;
					if (sim_events[k].write == 1 && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10) run = k;
					if (sim_events[k].write == 1 && sim_events[k].pa == sim_mbox_pa + 0x10c1c) shut = k;
				}
				chk(ret == 0 && result("boot", "PASS", "core-handshook-and-stopped") && sim_faults == 0, "HS1 handshake=1: PASS, the firmware's table address is seen, core stopped, no fault");
				chk(sim_hs_wakes == 1 && sim_hs_block_ok, "HS1 handshake=1: the init block holds firmware iova, IPC and HEAP iovas, config size, SCRATCH3 and ordinal, pool iova, count 0x40, template word 4");
				chk(nsc == 3 && sim_scr[7] == ANE_H18_HS_READY && w0 > run && w1 > w0 && w7 > w1, "HS1 handshake=1: exactly three SCRATCH writes, S0 then S1 then S7, all after RUN");
				chk(sim_n_dma == 3 && live == 0 && sim_dma_mask_bits == 42, "HS1 handshake=1: three surfaces (pool, IPC, HEAP) from the DMA API with a 42 bit mask, all freed after the stop");
				chk(sim_n_iocalls >= 2 && sim_iocalls[0].code == SIM_IO_MAP && sim_n_dom_live == 0 && log_count("no attach") == 1 && log_count("attach the ANE to a new DART domain") == 0,
				    "HS1 handshake=1: the firmware segments go into the default domain, no domain allocated or attached");
				chk(nst == 1 && log_pos("ane_h18 handshake result 0") > 0 && log_pos("ane_h18 handshake result 0") < log_pos("ane_h18 rtkit shutdown:"), "HS1 handshake=1: the handshake runs before the shutdown, which still completes");
				chk(log_count("ane_h18 hs table +0: 11111111 22222222 33333333 44444444") == 1 && log_count("is in a surface of 0x10000 bytes") == 1 && log_count("ane_h18 hs table +") == 1 && log_count("ane_h18 hs table: 127 all-zero lines of 16 bytes skipped") == 1, "HS1 handshake=1: the table dump starts at the address the firmware published");
				chk(shut > w7, "HS1 handshake=1: the shutdown message comes after the wake word");
			}
			if (pass == 1)
				chk(ret == -ETIMEDOUT && result("boot", "FAIL", "handshake-no-table-address") && nsc == 3 && live == 0 && nst == 1, "HS2 handshake=1: no answer gives FAIL handshake-no-table-address, the core still stopped cleanly and the surfaces freed");
			if (pass == 2)
				chk(ret == -EINVAL && result("boot", "REFUSED", "handshake=1 needs map_fw=1, rtkit_start=1, rtkit_stop=1 or 2, read_scratch=1, hs_wait_ms 10 to 5000, hs_cfg_size 1 to 0x10000000") && writes() == 0 && sim_n_dma == 0, "HS3 handshake=1 without rtkit_stop is refused before any write");
			if (pass == 3)
				chk(ret != 0 && result("boot", "FAIL", "handshake-not-done") && nsc == 0 && sim_n_dma == 0 && nst == 1, "HS4 handshake=1: SCRATCH0 at 0x21 or above: no allocation, no SCRATCH write, clean stop, FAIL");
			if (pass == 4)
				chk(ret != 0 && result("boot", "FAIL", "handshake-not-done") && nsc == 0 && live == 0 && sim_n_dma == 2 && nst == 1, "HS5 handshake=1: the HEAP allocation fails: surfaces freed, no SCRATCH write, clean stop, FAIL");
			if (pass == 5)
				chk(ret != 0 && result("boot", "FAIL", "fw-map-failed") && nsc == 0 && sim_n_dma == 0 && events_in(sim_asc_pa + 0x44, 4, 1) == 0, "HS6 handshake=1 without a default DMA domain: FAIL before RUN");
			if (pass == 8)
				chk(ret == -ETIMEDOUT && result("boot", "FAIL", "handshake-no-table-address") && nsc == 3 && live == 0, "HS8 handshake=1: READY again without a new table address is not an answer");
			if (pass == 9)
				chk(ret != 0 && result("boot", "FAIL", "handshake-not-done") && nsc == 0 && sim_n_dma == 0 && nst == 1, "HS9 handshake=1: SCRATCH7 not yet READY: no allocation, no SCRATCH write, clean stop, FAIL");
			if (pass == 6 || pass == 7)
				chk(ret != 0 && result("boot", "FAIL", "handshake-not-done") && nsc == 0 && sim_n_dma == 0 && nst == 1, pass == 6 ? "HS7 handshake=1: a HEAP request above the limit is refused before any allocation" : "HS7 handshake=1: an IPC request above the limit is refused before any allocation");
		}
	}
	/* channel step: the table check, the H2T ownership, SCRATCH3, then the two commands */
	{
		static const u32 scr0[8] = { 8, 0xc440, 0x100, 0xf00000, 0, 0x100090, 0, ANE_H18_HS_READY };
		int pass, k, w3 = -1, db = -1, nsc3, nh2t, ok;
		u8 *ipc;

		for (pass = 0; pass < 62; pass++) {
			kboot_setup();
			alias_mbox = true;
			rtkit_hello = true;
			rtkit_start = true;
			rtkit_stop = 1;
			read_scratch = true;
			handshake = true;
			hs_chan = pass != 5;
			hs_cmds = pass >= 1 && pass != 5;
			if (pass == 5) hs_cmds = true;
			sim_start_chan = 536;
			sim_start_hello = 3;
			sim_scr_model = 1;
			sim_hs_table = 1;
			memcpy(sim_scr, scr0, sizeof(scr0));
			if (pass == 2) sim_s3_mode = 1;
			if (pass == 3) sim_cmd_mode = 1;
			if (pass == 4) sim_cmd_mode = 2;
			if (pass == 6) sim_table_bad = 2;
			if (pass == 7) sim_table_bad = 4;
			if (pass == 8) sim_table_bad = 100;
			if (pass == 9) sim_table_bad = 101;
			if (pass == 10) sim_table_bad = 102;
			if (pass == 11) { sim_chan_req = 1; sim_cmd_mode = 1; }
			if (pass == 12) sim_chan_req = 2;
			if (pass == 13) { sim_chan_req = 3; }
			if (pass == 14) { sim_chan_req = 4; sim_cmd_mode = 1; }
			if (pass == 13) hs_cmds = false;
			if (pass == 15) hs_start = true;
			if (pass == 16) { hs_start = true; hs_cmds = false; }
			if (pass >= 17) { hs_start = true; hs_prog = true; }
			if (pass == 18) hs_start = false;
			if (pass == 19) sim_blob[0] ^= 0xff;
			if (pass == 20) sim_fw_err = -2;
			if (pass == 21) sim_prog_mode = 1;
			if (pass == 22) sim_prog_mode = 3;
			if (pass == 23) sim_prog_mode = 2;
			if (pass == 24) sim_prog_mode = 4;
			if (pass == 25) sim_prog_mode = 5;
			if (pass == 26) sim_prog_mode = 6;
			if (pass == 27) read_dart = true;
			if (pass == 28) { read_dart = true; hs_premap = true; dart_inst = 0xf; }
			if (pass == 29) { hs_premap = true; sim_prog_mode = 7; }
			if (pass == 30) { hs_prog = false; hs_premap = true; }
			if (pass == 31) dart_inst = 0x2;
			if (pass == 32) { read_dart = true; dart_inst = 0x10; }
			if (pass == 33) { hs_premap = true; sim_prog_mode = 8; }
			if (pass == 34) hs_props = 1;
			if (pass == 35) { hs_props = 1; sim_prop_refuse = 0x10aa; }
			if (pass == 36) { hs_start = false; hs_prog = false; hs_props = 1; }
			if (pass == 37) hs_props = 2;
			if (pass == 38) hs_props = 3;
			if (pass == 39) { prog_out_words = 20; hs_prog_calls = 2; }
			if (pass == 40) { hs_prog = false; prog_out_words = 16; }
			if (pass == 41) hs_prog_calls = 1001;
			if (pass == 42) { prog_out_words = 16; hs_prog_calls = 1; sim_prog_mode = 10; }
			if (pass == 43) { prog_iova_bufs = 0x40000000; prog_iova_text = 0x50000000; }
			if (pass == 44) prog_iova_bufs = 0xffffffffffff0000ull;
			if (pass == 45) { prog_iova_bufs = 0x40000000; prog_iova_text = 0x50000000; sim_io_fail = 4; }
			if (pass == 46) { owned_fw = true; fw_path = "pin.bin"; }
			if (pass == 47) {
				owned_fw = true; fw_path = "pin.bin"; hs_prog = false;
				sim_fw_data = sim_owned_fw; sim_fw_size = sizeof(sim_owned_fw);
			}
			if (pass == 48) hs_settle_ms = 1001;
			if (pass == 49) { make_blob_kern(); prog_iova_kern = 0x30010000; }
			if (pass == 50) {
				owned_fw = true; fw_path = "pin.bin"; hs_prog = false;
				sim_fw_data = sim_owned_fw; sim_fw_size = sizeof(sim_owned_fw);
				node.has_iommus = false;
			}
			if (pass == 51) { prog_iova_bufs = 0x40000000; prog_iova_text = 0x50000000; sim_unmap_short_iova = 0x40000000; }
			if (pass == 52) sim_prog_mode = 11;
			if (pass == 53) { sim_prog_mode = 6; sim_rpm_get_fail_at = 1; }
			if (pass == 54) {
				owned_fw = true; fw_path = "pin.bin"; hs_prog = false;
				sim_fw_data = sim_owned_fw; sim_fw_size = sizeof(sim_owned_fw);
				read_dart = true;
				ane_t8152_soc_shim.dart_size = 0x1438;	/* the last DART0 word sits outside its window: the read is refused */
			}
			if (pass == 55) sim_core_stuck = 1;
			if (pass == 56) { hs_prog_calls = 3; hs_prog_reuse = true; prog_out_words = 20; sim_prog_mode = 12; }
			if (pass == 57) { hs_prog = false; hs_prog_reuse = true; }
			if (pass == 58) {
				make_blob_kern_weights();
				prog_iova_bufs = 0x30000000;
				prog_iova_text = 0x3000c000;
				prog_text_const = 0x280;
				prog_iova_kern = 0x30020000;
				prog_kern_slot = true;
			}
			if (pass == 59) {
				make_blob_kern_weights();
				prog_iova_bufs = 0x30000000;
				prog_iova_text = 0x3000c000;
				prog_text_const = 0x280;
				prog_iova_kern = 0x30020000;
				prog_kern_slot = false;
			}
			if (pass == 60) { prog_iova_kern = 0; prog_kern_slot = true; }
			if (pass == 61) { hs_prog = false; prog_iova_kern = 0x30010000; prog_kern_slot = true; }
			ret = probe_now();
			ipc = sim_n_dma > 1 ? sim_dma[1].va : NULL;
			nsc3 = 0;
			for (w3 = -1, db = -1, k = 0; k < sim_n_events; k++) {
				if (sim_events[k].write == 1 && sim_events[k].pa == SIM_SCRATCH_PA + 0x30) { nsc3++; w3 = k; }
				if (sim_events[k].write == 1 && sim_events[k].pa == SIM_DB_PA && db < 0) db = k;
			}
			(void)ipc;
			nh2t = log_count("hs chan ");
			ok = result("boot", "PASS", "core-handshook-and-stopped");

			if (pass >= 17) {
				int live = 0, kk;

				for (kk = 0; kk < sim_n_dma; kk++)
					live += sim_dma[kk].live;
				if (pass == 17) {
					chk(ret == 0 && ok && sim_cmds_seen == 9 && sim_cmd_ops[4] == 0x200 && sim_cmd_ops[5] == 0x202 && sim_cmd_ops[6] == 0x204 && sim_cmd_ops[7] == 0x203 && sim_cmd_ops[8] == 0x201 &&
					    !sim_prog_loaded && !sim_prog_created,
					    "PG1 hs_prog=1: LOAD_PROGRAM, CREATE_PROCESS, PROCEDURE_CALL, TERMINATE_PROCESS, DESTROY_PROGRAM in that order after the four start commands, the firmware holds nothing afterwards");
					chk(log_count("DART exception state (before LOAD_PROGRAM) not read: read_dart=0") == 1 && log_count("DART exception state (after the call) not read: read_dart=0") == 1 && log_count("LOAD_PROGRAM program id 0x5a") == 1 && log_count("CREATE_PROCESS process id 0x77") == 1 && sim_out_written &&
					    log_count("buffer 4 (output, 0x4000 bytes): 2 nonzero words, first 11223344 11223344") == 1 && log_count("buffer 5 (input, 0x4000 bytes): 16 nonzero words, first aaaaaaaa") == 1,
					    "PG2 the ids come back from the replies, the sections and buffers reach the firmware (it checked every IOVA), the output buffer is read back and logged");
					chk(log_count("PROCEDURE_CALL finished after") == 1 && log_count("IO_T2H slot 0 header=") == 1 && log_count("IO_T2H slot 1 header=") == 1 && sim_t2h_posted == 2 && sim_t2h_back == 2 && log_count("write doorbell BIT(6)") == 2,
					    "PG3 both IO_T2H events are read, logged and given back with the doorbell of that ring");
					chk(sim_n_dma >= 3 + 6 + 3 && live == 0 && sim_fw_requests == 1 && sim_fw_released == 1 && sim_prog_n == 5 && log_count("sequence result 0") == 1,
					    "PG4 nine surfaces for the program, all freed after the core stopped, the file requested once and released once");
				}
				if (pass == 18)
					chk(ret == -EINVAL && result("boot", "REFUSED", "hs_chan=1 needs handshake=1, hs_cmds=1 needs hs_chan=1, hs_start=1 needs hs_cmds=1 and hs_prog=1 needs hs_start=1") && writes() == 0 && sim_fw_requests == 0, "PG5 hs_prog=1 without hs_start=1 is refused before any write and before the file is requested");
				if (pass == 19)
					chk(ret == -EINVAL && result("boot", "REFUSED", "hs_prog=1: the program file is not valid") && writes() == 0 && sim_fw_released == 1 && sim_n_dma == 0, "PG6 a file with a wrong magic is refused before any write or allocation, and released");
				if (pass == 20)
					chk(ret == -ENOENT && result("boot", "REFUSED", "hs_prog=1: the program file could not be loaded") && writes() == 0 && sim_fw_released == 0, "PG7 a file that cannot be loaded is refused before any write");
				if (pass == 21)
					chk(ret == -EIO && result("boot", "FAIL", "handshake-channel-step-failed") && sim_prog_n == 1 && sim_prog_ops[0] == 0x200 && log_count("rtkit shutdown:") == 1 && live == 0,
					    "PG8 LOAD_PROGRAM refused by the firmware (acknowledgement 3): FAIL, no further program command, the stop still runs, surfaces freed");
				if (pass == 22)
					chk(ret == -EIO && sim_prog_n == 5 && sim_prog_ops[2] == 0x204 && sim_prog_ops[3] == 0x203 && sim_prog_ops[4] == 0x201 && !sim_prog_loaded && log_count("gave no finish event") == 1 && live == 0 && sim_t2h_posted == 0,
					    "PG9 a call without a finish event: FAIL, the unload is tried anyway and answered so nothing stays resident, the stop still runs");
				if (pass == 23)
					chk(ret == -EIO && sim_prog_n == 3 && sim_prog_ops[1] == 0x202 && sim_prog_ops[2] == 0x201 && !sim_prog_loaded, "PG10 CREATE_PROCESS refused: the program is destroyed, no call");
				if (pass == 28) {
					int dr1 = 0, dw1 = 0;

					for (kk = 0; kk < sim_n_events; kk++)
						if (sim_events[kk].pa >= SIM_DART_PA + 0x20000 && sim_events[kk].pa < SIM_DART_PA + 0x20000 * 4 && (sim_events[kk].pa & 0x1ffff) < SIM_DART_SIZE) {
							if (sim_events[kk].write)
								dw1++;
							else
								dr1++;
						}
					chk(ret == 0 && ok && sim_prog_n == 6 && sim_prog_ops[0] == 0x20d && sim_prog_ops[1] == 0x200 && sim_premapped && log_count("PREMAP_BUFFER reply +0:") == 1 && log_count("PREMAP_BUFFER pool iova=") == 1,
					    "PG19 hs_premap=1: PREMAP_BUFFER (0x20d, type 2, one pool of the request size, the pool size as second value) goes before LOAD_PROGRAM and is answered, the program runs after it");
					chk(dw1 == 0 && sim_faults == 0 && dr1 == 3 * 2 * (5 + 5 * 5) && log_count("DART 1 before LOAD_PROGRAM stream 11: status 0xda0082c0 +4 0xda0082c4 addr 0xda0082ccda0082c8 +0x10 0xda0082d0") == 1 && log_count("DART 3 after the call stream 15: status") == 1 && log_count("DART 0 after the call +0xc00 = ") == 1,
					    "PG20 dart_inst=0xf: instances 1 to 3 are mapped and read, before LOAD_PROGRAM and after the call (five registers and the records of streams 0, 10, 11, 13 and 15 each), no write");
				}
				if (pass == 29)
					chk(ret == 0 && ok && sim_prog_n == 6 && sim_prog_ops[0] == 0x20d && sim_prog_ops[1] == 0x200 && !sim_premapped && log_count("PREMAP_BUFFER was refused, going on with LOAD_PROGRAM") == 1,
					    "PG21 PREMAP_BUFFER refused by the firmware (acknowledgement 3): logged, the program runs anyway");
				if (pass == 34)
					chk(ret == 0 && ok && sim_cmds_seen == 14 && sim_cmd_ops[3] == 0x1f && sim_cmd_ops[4] == 0x22 && sim_cmd_ops[5] == 0x1f && sim_cmd_ops[8] == 0x1f && sim_cmd_ops[9] == 0x200 && sim_nprops == 5 &&
					    sim_prop[0][1] == 0x10a4 && sim_prop[0][2] == 1 && sim_prop[1][1] == 0x10a6 && sim_prop[1][2] == 1 && sim_prop[2][1] == 0xa1 && sim_prop[2][2] == 0 && sim_prop[3][1] == 0x10aa && sim_prop[3][2] == 1 &&
					    sim_prop[4][1] == 0x1209 && sim_prop[4][2] == 1 && sim_prop[0][0] == 0 && sim_prop[4][0] == 0,
					    "PR1 hs_props=1: PRINT_ENABLE, START, CONFIG_GET, property 0x10a4, RESOURCE_INFO_GET, properties 0x10a6, 0xa1, 0x10aa, 0x1209 (id at +0xc, value at +0x10), then the program");
				if (pass == 35)
					chk(ret == 0 && sim_cmds_seen == 14 && sim_nprops == 5 && log_count("the firmware refused PROPERTY 0x10aa") == 1 && sim_cmd_ops[9] == 0x200,
					    "PR2 a refused property write is logged and the sequence goes on to the program");
				if (pass == 37)
					chk(ret == 0 && sim_nprops == 5 && sim_prop[2][1] == 0xa1 && sim_prop[2][2] == 1 && sim_prop[0][2] == 1 && sim_prop[3][2] == 1, "PR4 hs_props=2 sends 0xa1 = 1 and the other four as hs_props=1");
				if (pass == 39) {
					const char *l1 = strstr(sim_log, "call 1/2 finished in "), *l2 = strstr(sim_log, "call 2/2 finished in ");
					unsigned long long n1 = l1 ? strtoull(l1 + strlen("call 1/2 finished in "), NULL, 10) : 0, n2 = l2 ? strtoull(l2 + strlen("call 2/2 finished in "), NULL, 10) : 0;

					chk(ret == 0 && ok && sim_prog_n == 10 && sim_t2h_posted == 4 && sim_t2h_back >= 2 && log_count("PROCEDURE_CALL finished after") == 2 && log_count("write doorbell BIT(6)") == 4 && log_count("sequence result 0") == 2 && live == 0,
					    "PO1 hs_prog_calls=2 prog_out_words=20: the sequence runs twice, each pass loads, calls once and unloads, four IO_T2H events read and given back");
					chk(n1 > 0 && n2 > 0 && log_pos("call 1/2 finished in ") < log_pos("call 2/2 finished in ") && log_count("call 1/2 finished in ") == 2 && log_count("call 2/2 finished in ") == 2,
					    "PO2 each call logs 'call N/M finished in X ns' with a nonzero X, in order, plus the ms line");
					chk(log_count("out buf 4 +0x0: 11223344 11223344 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000\n") == 2 &&
					    log_count("out buf 4 +0x40: 00000000 00000000 00000000 00000000\n") == 2 && log_count("out buf 4 +0x50") == 0 && log_count("out buf 5") == 0 && log_count("out buf 6") == 0,
					    "PO3 after each call the first 20 words of the output buffer only are logged, 16 words a line, the last line short, no input buffer");
				}
				if (pass == 42) {
					const char *m1 = strstr(sim_log, "call 1/1 finished in ");
					unsigned long long nn = m1 ? strtoull(m1 + strlen("call 1/1 finished in "), NULL, 10) : 0;

					chk(ret == 0 && ok && nn > 0 && log_count("call 1/1 finished in ") == 2 && log_count("no latency") == 0 &&
					    log_count("gave no finish event") == 0 && sim_t2h_posted == 2 && sim_t2h_back == 2 && live == 0,
					    "PO5 prog_out_words=16 with the finish event arriving five yields after the ack, no sleep in between: the timed spin stamps a real ns latency, no no-latency line");
				}
				if (pass == 40 || pass == 41)
					chk(ret == -EINVAL && result("boot", "REFUSED", "prog_out_words needs hs_prog=1; hs_prog_calls is 1 to 1000 and needs hs_prog=1 above 1") && writes() == 0 && sim_fw_requests == 0,
					    pass == 40 ? "PO4 prog_out_words without hs_prog=1 is refused before any write" : "PO4 hs_prog_calls above 1000 is refused before any write");
				if (pass == 43) {
					int mfix = 0, ufix = 0, kk;

					for (kk = 0; kk < sim_n_iocalls; kk++) {
						mfix += sim_iocalls[kk].code == SIM_IO_MAP && (sim_iocalls[kk].iova == 0x40000000 || sim_iocalls[kk].iova == 0x50000000);
						ufix += sim_iocalls[kk].code == SIM_IO_UNMAP && (sim_iocalls[kk].iova == 0x40000000 || sim_iocalls[kk].iova == 0x50000000);
					}
					chk(ret == 0 && ok && mfix >= 2 && ufix >= 2 && live == 0,
					    "PG43 prog_iova_bufs/text: the section 3 and buffer aliases map at the fixed IOVAs, the call finishes and every alias unmaps at free");
				}
				if (pass == 44)
					chk(ret == -EINVAL && log_count("is not 16 KiB aligned or is not below 0xe0000000") == 1,
					    "PG44 a prog_iova_bufs base at or past 0xe0000000 (or one whose buffers overflow it) is refused by the fixed-IOVA bound");
				if (pass == 45) {
					int rb = 0, kk;

					for (kk = 0; kk < sim_n_iocalls; kk++)
						rb += sim_iocalls[kk].code == SIM_IO_UNMAP && sim_iocalls[kk].iova == 0x50000000 && sim_iocalls[kk].size == 0x4000;
					chk(ret == -ENOMEM && rb >= 1 && log_count("map of page") == 1,
					    "PG45 a failed alias page map rolls back the pages of that buffer already mapped and fails the sequence");
				}
				if (pass == 46)
					chk(ret == -EINVAL && result("boot", "REFUSED", "owned_fw=1 is a firmware-residency trial that submits no program; drop hs_prog=1 or owned_fw=1") && writes() == 0 && sim_fw_requests == 0,
					    "PG46 owned_fw=1 together with hs_prog=1 is refused before any write or file load");
				if (pass == 47) {
					int kk;
					unsigned long seq_unmap_owned = 0, seq_free_hs = 0;

					for (kk = 0; kk < sim_n_iocalls; kk++)
						if (sim_iocalls[kk].code == SIM_IO_UNMAP && (sim_iocalls[kk].iova == 0x10000000000ull || sim_iocalls[kk].iova == 0x100000c4000ull))
							seq_unmap_owned = seq_unmap_owned ? seq_unmap_owned : sim_iocalls[kk].seq;
					for (kk = 0; kk < sim_n_dma; kk++)
						if (!sim_dma[kk].live && sim_dma[kk].size != 0x370000u)
							seq_free_hs = sim_dma[kk].free_seq > seq_free_hs ? sim_dma[kk].free_seq : seq_free_hs;
					chk(ret == 0 && ok && seq_unmap_owned && seq_free_hs && seq_free_hs < seq_unmap_owned && log_count("(owned image)") == 2,
					    "PG47 owned_fw=1: the pinned payload stages, both segments map at the fixed TEXT/DATA IOVAs (owned image), and the handshake surfaces free before the owned ranges unmap");
				}
				if (pass == 48)
					chk(ret == -EINVAL && result("boot", "REFUSED", "prog_out_words needs hs_prog=1; hs_prog_calls is 1 to 1000 and needs hs_prog=1 above 1; hs_settle_ms is 0 to 1000") && writes() == 0 && sim_fw_requests == 0 && sim_n_dma == 0 && first_run_event() < 0,
					    "PG48 hs_settle_ms=1001 with hs_prog=1 is refused before any write, file load or allocation");
				if (pass == 49) {
					int kern_maps = 0;

					for (kk = 0; kk < sim_n_iocalls; kk++)
						kern_maps += sim_iocalls[kk].code == SIM_IO_MAP && sim_iocalls[kk].iova == 0x30010000;
					chk(ret == -EINVAL && result("boot", "FAIL", "handshake-not-done") && log_count("prog_iova_kern is set but section 2") == 1 && kern_maps == 0 && sim_prog_n == 0 && live == 0,
					    "PG49 prog_iova_kern with a section 2 of exactly 0x4000 bytes: the empty tail is logged, the boot FAILs, no kern alias maps and nothing leaks");
				}
				if (pass == 50) {
					int live_img = 0;

					for (kk = 0; kk < sim_n_dma; kk++)
						live_img += sim_dma[kk].live && sim_dma[kk].size == ANE_H18_FW_IMAGE_SIZE;
					chk(ret == -ENODEV && result("boot", "FAIL", "owned-map-failed") && bus_write_events() == 0 && first_run_event() < 0 &&
					    log_count("the node has no iommus") == 1 && log_count("owned firmware staged bytes=") == 1 && live_img == 0 && sim_owner_module.refs == 0,
					    "PG50 owned_fw=1 with no iommus in the node: the early map refusal is FAIL owned-map-failed, the staged image goes back, nothing pinned");
				}
				if (pass == 51) {
					u64 dma_pa = 0;
					int leaked = 0, live_other = 0;

					for (kk = 0; kk < sim_n_iomaps; kk++)
						if (sim_iomaps[kk].iova == 0x40000000)
							dma_pa = sim_iomaps[kk].pa;
					for (kk = 0; kk < sim_n_dma; kk++)
						if (sim_dma[kk].live) {
							if (sim_dma[kk].iova == dma_pa)
								leaked++;
							else
								live_other++;
						}
					chk(ret == 0 && ok && log_count("the pages are leaked, not freed") == 1 && leaked == 1 && live_other == 0 && sim_owner_module.refs == 1,
					    "PG51 a short unmap of the 0x40000000 alias keeps that buffer allocated (leaked, never freed) and pins the module, the rest of the run frees clean");
				}
				if (pass == 52)
					chk(ret == -EIO && result("boot", "FAIL", "rtkit-shutdown-incomplete") && sim_prog_n == 5 && sim_prog_ops[4] == 0x201 &&
					    log_count("DESTROY_PROGRAM was not answered") == 1 && log_count("did not answer the unload") == 1 &&
					    log_count("the ANE domain is back up") == 1 && sim_rpm_gets == 2 && sim_rpm_puts >= 1 &&
					    log_count("did not stop after RUN was cleared") == 0,
					    "PG52 an unanswered DESTROY_PROGRAM power-cycles the domain (runtime PM put then get) and the recovery cleans the program: the stop still runs, the boot never reports core-not-stopped");
				if (pass == 53) {
					int gi, touched = 0;

					for (gi = sim_n_events - 1; gi >= 0; gi--)
						if (sim_events[gi].write == 2 && sim_events[gi].pa == 0xe0)
							break;
					for (kk = gi + 1; kk < sim_n_events; kk++)
						if (!(sim_events[kk].write == 2 && sim_events[kk].pa == 0xe0))
							touched++;
					chk(ret == -EIO && result("boot", "FAIL", "domain-down-after-recover") && result("recover", "FAIL", "kernel-psd-power-on-failed") &&
					    sim_owner_module.refs == 1 && log_count("the ANE domain did not come back up") == 1 &&
					    gi >= 0 && touched == 0 && sim_rpm_gets == 1 && sim_rpm_puts == 1,
					    "PG53 a psd_up failure during the recovery leaves domain_down set: the boot FAILs domain-down-after-recover, the module is pinned, and no MMIO access of any ANE window (not even the post-failure snapshot's IPI pending read) happens after the failed get");
				}
				if (pass == 54) {
					int live_img = 0;

					for (kk = 0; kk < sim_n_dma; kk++)
						live_img += sim_dma[kk].live && sim_dma[kk].size == ANE_H18_FW_IMAGE_SIZE;
					chk(ret == -EPERM && result("boot", "FAIL", "dart-read-refused") && log_count("refusing read DART0_0x1438 +") == 1 &&
					    events_in(SIM_DART_PA, SIM_DART_SIZE, 0) == 11 && log_count("owned firmware staged bytes=") == 1 &&
					    live_img == 0 && sim_n_iomap == sim_n_iounmap,
					    "PG54 a DART0 read refusal with owned_fw=1 frees the staged owned image (nothing leaks, no window left mapped) and FAILs dart-read-refused");
				}
				if (pass == 56) {
					static const u32 want[7] = { 0x200, 0x202, 0x204, 0x204, 0x204, 0x203, 0x201 };
					int same = sim_prog_n == 7, q;

					for (q = 0; same && q < 7; q++)
						same = sim_prog_ops[q] == want[q];
					chk(ret == 0 && ok && same && sim_t2h_posted == 6 && log_count("LOAD_PROGRAM program id 0x5a") == 1 && log_count("CREATE_PROCESS process id 0x77") == 1 &&
					    log_count("sequence result 0") == 1 && live == 0,
					    "PU1 hs_prog_reuse=1 hs_prog_calls=3: LOAD_PROGRAM and CREATE_PROCESS once, three PROCEDURE_CALLs, one TERMINATE_PROCESS and DESTROY_PROGRAM, nothing leaks");
					chk(log_count("call 1/3 finished in ") == 2 && log_count("call 2/3 finished in ") == 2 && log_count("call 3/3 finished in ") == 2 &&
					    log_pos("call 1/3 finished in ") < log_pos("call 2/3 finished in ") && log_pos("call 2/3 finished in ") < log_pos("call 3/3 finished in "),
					    "PU2 each reused call logs its ns and ms latency lines, in order");
					{
						const char *d1 = strstr(sim_log, "out buf 4 call 1/3 digest "), *d2 = strstr(sim_log, "out buf 4 call 2/3 digest "), *d3 = strstr(sim_log, "out buf 4 call 3/3 digest ");
						size_t dl = strlen("out buf 4 call 1/3 digest ");

						chk(log_count("out buf 4 +0x0: 11223344 11223344 00000000") == 1 && log_count("out buf 4 +0x0: 00000000 00000000 00000000") == 1 && log_count("out buf 4 +0x40") == 2,
						    "PU3 under reuse only the first and the last call dump words: call 1 holds its result, call 3 (left unwritten by the model) reads zeros, so the buffer was cleared");
						chk(d1 && d2 && d3 && log_count(" digest ") == 3 && !strncmp(d2 + dl, d3 + dl, 16) && strncmp(d1 + dl, d2 + dl, 16),
						    "PU5 every reused call logs one digest of its whole output buffer: calls 2 and 3 (zeros) match each other and differ from call 1");
					}
				}
				if (pass == 57)
					chk(ret == -EINVAL && result("boot", "REFUSED", "prog_out_words needs hs_prog=1; hs_prog_calls is 1 to 1000 and needs hs_prog=1 above 1; hs_settle_ms is 0 to 1000; hs_prog_reuse needs hs_prog=1") && writes() == 0 && sim_fw_requests == 0,
					    "PU4 hs_prog_reuse=1 without hs_prog=1 is refused before any write");
				if (pass == 58) {
					int kmap = 0, kunmap = 0;

					for (kk = 0; kk < sim_n_iocalls; kk++) {
						kmap += sim_iocalls[kk].code == SIM_IO_MAP && sim_iocalls[kk].iova == 0x30020000;
						kunmap += sim_iocalls[kk].code == SIM_IO_UNMAP && sim_iocalls[kk].iova == 0x30020000;
					}
					chk(ret == 0 && ok && sim_load_sec2_iova == 0x30020000 && sim_load_sec2_size == 0x4000 && sim_load_sec2_b1 == 0x5a &&
					    kmap == 1 && kunmap == 1 && live == 0,
					    "KS1 prog_kern_slot=1 with prog_iova_kern=0x30020000: LOAD_PROGRAM slot 1 (sec 2) points at pg_kern.iova with size-0x4000 and the weights tail behind the const window, then unmaps clean");
				}
				if (pass == 59)
					chk(ret == 0 && ok && sim_load_sec2_iova != 0x30020000 && sim_load_sec2_size == 0x8000 && sim_load_sec2_b1 == 0x11 && live == 0,
					    "KS2 prog_kern_slot=0 with prog_iova_kern=0x30020000: LOAD_PROGRAM slot 1 keeps pg_sec[s2] and full section 2 size");
				if (pass == 60)
					chk(ret == -EINVAL && result("boot", "REFUSED", "prog_kern_slot=1 needs hs_prog=1 and prog_iova_kern") && writes() == 0 && sim_fw_requests == 0 && sim_n_dma == 0,
					    "KS3 prog_kern_slot=1 without prog_iova_kern is refused before any write, file load or allocation");
				if (pass == 61)
					chk(ret == -EINVAL && result("boot", "REFUSED", "prog_kern_slot=1 needs hs_prog=1 and prog_iova_kern") && writes() == 0 && sim_fw_requests == 0 && sim_n_dma == 0,
					    "KS4 prog_kern_slot=1 without hs_prog=1 is refused before any write, file load or allocation");
				if (pass == 55)
					chk(ret == -EBUSY && result("boot", "FAIL", "core-not-stopped") && sim_owner_module.refs == 1 &&
					    log_count("reboot required") >= 1 && sim_rpm_puts == 0 && sim_n_iounmap == 0,
					    "PG55 a core that never reports STOPPED after RUN was cleared pins the module: the surfaces and the power reference stay, reboot required");
				if (pass == 38)
					chk(ret == -EINVAL && result("boot", "REFUSED", "hs_props is 0, 1 or 2 and needs hs_start=1") && writes() == 0, "PR5 hs_props=3 is refused before any write");
				if (pass == 36)
					chk(ret == -EINVAL && result("boot", "REFUSED", "hs_props is 0, 1 or 2 and needs hs_start=1") && writes() == 0, "PR3 hs_props=1 without hs_start=1 is refused before any write");
				if (pass == 33)
					chk(ret == -EIO && sim_prog_n == 1 && sim_prog_ops[0] == 0x20d && log_count("PREMAP_BUFFER was refused") == 0 && log_count("sequence result -71") == 1 && live == 0,
					    "PG23 a malformed PREMAP_BUFFER answer (wrong id) is a protocol FAIL, not a refusal: no LOAD_PROGRAM follows");
				if (pass == 30)
					chk(ret == -EINVAL && result("boot", "REFUSED", "hs_premap=1 needs hs_prog=1; dart_inst needs hs_prog=1 and read_dart=1 and is a mask of instances 0 to 3") && writes() == 0, "PG22 hs_premap=1 without hs_prog=1 is refused before any write");
				if (pass == 31)
					chk(ret == -EINVAL && result("boot", "REFUSED", "hs_premap=1 needs hs_prog=1; dart_inst needs hs_prog=1 and read_dart=1 and is a mask of instances 0 to 3") && writes() == 0, "PG22 dart_inst without read_dart=1 is refused before any write");
				if (pass == 32)
					chk(ret == -EINVAL && result("boot", "REFUSED", "hs_premap=1 needs hs_prog=1; dart_inst needs hs_prog=1 and read_dart=1 and is a mask of instances 0 to 3") && writes() == 0, "PG22 dart_inst with a bit above instance 3 is refused before any write");
				if (pass == 27) {
					int dw = 0, dr = 0;

					for (kk = 0; kk < sim_n_events; kk++)
						if (sim_in_dart(sim_events[kk].pa)) {
							if (sim_events[kk].write)
								dw++;
							else
								dr++;
						}
					chk(ret == 0 && ok && dw == 0 && sim_faults == 0 && log_count("DART exc before LOAD_PROGRAM stream ") == 8 && log_count("DART exc after the call stream ") == 8 &&
					    log_count("DART exc before LOAD_PROGRAM bitmap word 7 (+0x401c)") == 1 && log_count("DART exc after the call +0x1c0 = ") == 1 && dr >= 2 * (5 + 8 + 8 * 9),
					    "PG18 read_dart=1 with hs_prog: the DART exception state (five words, eight bitmap words, at most eight stream records) is read before LOAD_PROGRAM and after the call, never written");
				}
				if (pass == 26)
					chk(ret == -EIO && sim_prog_n == 2 && sim_prog_ops[0] == 0x200 && sim_prog_ops[1] == 0x202 && sim_prog_loaded && live == 0 &&
					    log_count("did not answer the unload") == 1 && log_count("RESULT stage=boot soc=t8152 verdict=FAIL") == 1,
					    "PG17 CREATE_PROCESS not answered: FAIL, no destroy can go out (the timed-out slot is not ours) and the domain is power-cycled, the stop still runs");
				if (pass == 25)
					chk(ret == 0 && ok && sim_t2h_posted == 2 && sim_t2h_back == 2 && log_count("PROCEDURE_CALL finished after") == 1 && log_count("is not in a buffer the firmware was given") == 0 && log_count("IO_T2H slot 0 header=0x") == 1,
					    "PG16 an IO_T2H slot whose length is written before its address is not consumed until the address is there: both events are read after publication, the call finishes");
				if (pass == 24)
					chk(ret == -EIO && sim_prog_n == 1 && log_count("did not write a program id") == 1, "PG11 a LOAD_PROGRAM reply without a program id is a FAIL, nothing is created");
			}
			if (pass == 0) {
				chk(ret == 0 && ok && nsc3 == 1 && sim_db_writes == 0 && sim_ack_writes == 0 && nh2t == 8 && log_count("chan snapshot after SCRATCH3: IPI pending = 0\n") == 1 && log_count("chan snapshot after SCRATCH3: TERMINAL +") == 0 && log_pos("read IPI pending pa=0x4198ac000") > 0 && log_pos("read IPI pending pa=0x4198ac000") < log_pos("IPI pending = 0\n"), "CH1 hs_chan=1: table of 8 rings checked, SCRATCH3 written once, firmware cleared it, no doorbell, snapshot with the pending address logged before its read");
				chk(log_count("SCRATCH3 reads 0 after") == 1 && sim_n_dma == 3 && log_count("write doorbell") == 0, "CH1 hs_chan=1: no command surface allocated, no doorbell write");
			}
			if (pass == 1) {
				chk(ret == 0 && ok && nsc3 == 1 && sim_db_writes == 2 && sim_cmds_seen == 2 && sim_cmd_ops[0] == 4 && sim_cmd_ops[1] == 3, "CH2 hs_cmds=1: PRINT_ENABLE then CONFIG_GET on the IO ring, two doorbell writes");
				chk(log_count("CONFIG_GET reply u32 at +8 = 0x16e3600") == 1 && log_count("ack=0\n") == 2 && w3 > 0 && db > w3, "CH2 hs_cmds=1: the reply is read, both acknowledged with 0, the doorbell comes after SCRATCH3");
				chk(sim_n_dma == 3 && log_count("hs surface CMD inside POOL at +0x400 iova=") == 1 && sim_cmd_blk_in_pool, "CH2 hs_cmds=1: the command block lives inside the pool surface at +0x400 (no fourth surface), as the vendor driver does");
			}
			if (pass == 2)
				chk(ret == -EIO && result("boot", "FAIL", "handshake-channel-step-failed") && nsc3 == 1 && sim_db_writes == 0 && sim_cmds_seen == 0 && log_count("rtkit shutdown:") == 1, "CH3 SCRATCH3 never cleared: FAIL, no command, the stop still runs");
			if (pass == 3)
				chk(ret == -EIO && result("boot", "FAIL", "handshake-channel-step-failed") && sim_cmds_seen == 1 && sim_db_writes == 1 && log_count("rtkit shutdown:") == 1, "CH4 no answer to PRINT_ENABLE: FAIL after one command, the stop still runs");
			if (pass == 4)
				chk(ret == -EIO && result("boot", "FAIL", "handshake-channel-step-failed") && sim_cmds_seen == 1, "CH5 a failing acknowledgement (+6 = 2) is a FAIL, CONFIG_GET is not sent");
			if (pass == 5)
				chk(ret == -EINVAL && result("boot", "REFUSED", "hs_chan=1 needs handshake=1, hs_cmds=1 needs hs_chan=1, hs_start=1 needs hs_cmds=1 and hs_prog=1 needs hs_start=1") && writes() == 0, "CH6 hs_cmds=1 without hs_chan=1 is refused before any write");
			if (pass == 13) {
				u64 *sm = sim_n_dma > 1 ? (u64 *)((u8 *)sim_dma[1].va + 0xae00) : NULL;

				chk(ret == 0 && ok && sm && sm[8] == 1 && (sm[16] & 1) && sm[18] == 1 && log_count("ordinal 1\n") == 2 && sim_n_dma == 5, "CH12 a freed MALLOC slot is reused: the second request of the same size gets ordinal 1 again, not 2");
			}
			if (pass == 15)
				chk(ret == 0 && ok && sim_cmds_seen == 4 && sim_cmd_ops[0] == 4 && sim_cmd_ops[1] == 0 && sim_cmd_ops[2] == 3 && sim_cmd_ops[3] == 0x22 && log_count("RESOURCE_INFO_GET reply +0x20: abcd0020") == 1 && log_count("RESOURCE_INFO_GET reply +0x50: abcd0050") == 1,
				    "CH14 hs_start=1: PRINT_ENABLE, START, CONFIG_GET, RESOURCE_INFO_GET in the kext's order, the 100 byte reply logged in 16 byte lines");
			if (pass == 16)
				chk(ret == -EINVAL && result("boot", "REFUSED", "hs_chan=1 needs handshake=1, hs_cmds=1 needs hs_chan=1, hs_start=1 needs hs_cmds=1 and hs_prog=1 needs hs_start=1") && writes() == 0, "CH15 hs_start=1 without hs_cmds=1 is refused before any write");
			if (pass == 14)
				chk(log_count("chan snapshot ring change: TERMINAL has 6 nonzero lines, 2 not printed") >= 1 && log_count("chan snapshot ring change: TERMINAL +0x30 = ") >= 1 && log_count("chan snapshot ring change: TERMINAL +0x40 = ") == 0 && log_count("chan snapshot ring change: TERMINAL text slot") == 0 && log_count("chan snapshot after SCRATCH3: TERMINAL text slot 0: ") == 1 && log_count("chan snapshot after SCRATCH3: TERMINAL has 6 nonzero lines, 0 not printed") == 1,
				    "CH16 a change snapshot prints 4 nonzero lines of a ring and no terminal text, the explicit snapshot prints all 6 and the text");
			if (pass == 14)
				chk(log_count("chan snapshot ring change: IPI pending") == 11, "CH13 a console that changes at every poll gives at most 6 change snapshots per wait (6 + 5 in this run, 10 + 5 without the cap), not one per check");
			if (pass == 12) {
				u64 *sm = sim_n_dma > 1 ? (u64 *)((u8 *)sim_dma[1].va + 0xae00) : NULL;
				int fw = -1, lv = 0;

				for (k = 0; k < sim_n_dma; k++) {
					lv += sim_dma[k].live;
					if (sm && sim_dma[k].size == 0x8000)
						fw = k;
				}
				chk(ret == 0 && ok && sim_cmds_seen == 2 && sm && (sm[0] & 1) && sm[2] == 1 && fw >= 0, "CH11 a firmware MALLOC on the SHAREDMALLOC ring is answered (header = dma | 1, ordinal 1): the command that waited for it completes");
				chk(sm && sm[8] == 1 && sm[9] == 0 && fw >= 0 && !sim_dma[fw].live && log_count("fw FREE of iova") == 1 && lv == 0, "CH11 the firmware's later FREE is answered (header 1), the buffer it returned is freed, nothing is left allocated after the stop");
				chk(sim_db_writes == 4 && log_count("write doorbell BIT(5) pa=0x4198a4000 to answer the MALLOC") == 1 && log_count("write doorbell BIT(5) pa=0x4198a4000 to answer the FREE") == 1, "CH11 two command doorbells and two SHAREDMALLOC doorbells (bit 5), each address logged before the write");
			}
			if (pass == 11)
				chk(ret == -EIO && log_count("chan snapshot after SCRATCH3: IPI pending = 0x20") == 1 && log_count("chan snapshot after the failed PRINT_ENABLE: IPI pending = 0x20") == 1 &&
				    log_count("chan snapshot after SCRATCH3: SHAREDMALLOC +0 = 0x1000 0x40") == 1 && log_count("after SCRATCH3: TERMINAL text slot 0: ANE fw says hi") == 1 && log_count("after the failed PRINT_ENABLE: TERMINAL text slot 0: ANE fw says hi") == 1 && sim_ack_writes > 0 && sim_ack_last == 0x20 && log_count("write IPI ack=0x20") > 0 && log_count("chan snapshot after the failed PRINT_ENABLE: IO slot 0 = ") == 1 &&
			    log_count("fw log after the failed PRINT_ENABLE: 2 ts 0x77: fw text .\n") == 1 && log_count("fw log after the failed PRINT_ENABLE: 2 raw 9 bytes: 667720746578740a01\n") == 1 && log_count("fw log after the failed PRINT_ENABLE: 3 ts 0x78: aaaa") == 1 && log_count("yz\n") == 1 && log_count("fw log after the failed PRINT_ENABLE: 4 ts") == 0 && log_count("fw log after the failed PRINT_ENABLE: 2 records printed, 2 not") == 1,
				    "CH10 a request nobody answered on the SHAREDMALLOC ring, the IO slot and the console text a record points to show up after the failed command (the record that holds inline text is counted, not printed)");
			if (pass == 10)
				chk(ret == -EPROTO && result("boot", "FAIL", "handshake-channel-step-failed") && nsc3 == 0 && sim_db_writes == 0 && sim_cmds_seen == 0, "CH9 a ring address near 2^64 that wraps the old bound check: FAIL, nothing written");
			if (pass == 8 || pass == 9)
				chk(ret == -EPROTO && result("boot", "FAIL", "handshake-channel-step-failed") && nsc3 == 0 && sim_db_writes == 0 && sim_cmds_seen == 0 && log_count("overlaps") == 1, pass == 8 ? "CH8 a ring that overlaps the table entries: FAIL, nothing written" : "CH8 a ring that overlaps another ring: FAIL, nothing written");
			if (pass == 6 || pass == 7)
				chk(ret == -EPROTO && result("boot", "FAIL", "handshake-channel-step-failed") && nsc3 == 0 && sim_db_writes == 0 && sim_cmds_seen == 0, pass == 6 ? "CH7 a ring outside the surface: FAIL, nothing written (no SCRATCH3, no ring slot, no doorbell)" : "CH7 another ring outside the surface: FAIL, nothing written");
		}
	}
	/* without rtkit_hello the messages are only logged */
	boot_setup();
	alias_mbox = true;
	sim_start_chan = 536;
	sim_start_hello = 1;
	ret = probe_now();
	chk(ret == 0 && events_in(sim_mbox_pa, sim_mbox_size, 1) == 0 && log_count("ane_h18 rtkit rx type=") == 2, "H2 boot alias_mbox without rtkit_hello: messages logged, nothing written");
	/* unknown type, another endpoint, firmware versions without 12, no inbox credit: no reply */
	for (i = 0; i < 4; i++) {
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		sim_start_chan = 536;
		sim_start_hello = 1;
		if (i == 0) sim_hello_type = 3;
		if (i == 1) sim_hello_ep = 1;
		if (i == 2) { sim_hello_min = 13; sim_hello_max = 14; }
		if (i == 3) sim_alias_credits = 0;
		ret = probe_now();
		chk(ret == 0 && events_in(sim_mbox_pa, sim_mbox_size, 1) == (i == 3 ? 0 : 4), i == 0 ? "H3 boot rtkit_hello: an unknown type gets no reply (only the EPMAP is answered)" : i == 1 ? "H3 boot rtkit_hello: a message for another endpoint gets no reply (only the EPMAP is answered)" : i == 2 ? "H3 boot rtkit_hello: firmware versions 13 to 14 get no HELLO reply (only the EPMAP is answered)" : "H3 boot rtkit_hello: no inbox credit, nothing written");
	}
	/* status read before RUN */
	boot_setup();
	alias_mbox = true;
	ret = probe_now();
	{
		int first_alias = -1, run = -1, k;

		for (k = 0; k < sim_n_events; k++) {
			if (first_alias < 0 && !sim_events[k].write && sim_events[k].pa == sim_mbox_pa + 0x10c20)
				first_alias = k;
			if (sim_events[k].write && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
				run = k;
		}
		chk(ret == 0 && first_alias > 0 && first_alias < run && log_count("ane_h18 alias status before RUN") == 1, "H4 boot alias_mbox: the alias STATUS word is read and logged before the RUN write");
	}

	/* kernel-powered node with map_fw: no PSD request, a runtime PM reference, then attach, two maps, the DART read again, RUN, and a clean undo */
	kboot_setup();
	read_dart = true;
	alias_mbox = true;
	rtkit_hello = true;
	sim_start_chan = 536;
	sim_start_hello = 1;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "K1 boot map_fw (kernel-powered): PASS");
	chk(psd_write_events() == 0 && sim_rpm_enabled == 1 && sim_rpm_gets == 1 && sim_rpm_puts == 1, "K1 boot map_fw: no PSD write, one runtime PM get and put");
	chk(iocalls_of(SIM_IO_ALLOC) == 1 && iocalls_of(SIM_IO_ATTACH) == 1 && iocalls_of(SIM_IO_MAP) == 2 && iocalls_of(SIM_IO_UNMAP) == 2 &&
	    iocalls_of(SIM_IO_DETACH) == 1 && iocalls_of(SIM_IO_FREE) == 1 && sim_n_dom_live == 0, "K1 boot map_fw: alloc, attach, two maps, then two unmaps, detach and free after the clean stop");
	{
		int maps = 0, ok = 1, k, order_ok = 1, last_map = -1, attach = -1;

		for (k = 0; k < sim_n_iocalls && k < 64; k++) {
			if (sim_iocalls[k].code == SIM_IO_ATTACH)
				attach = sim_iocalls[k].ev;
			if (sim_iocalls[k].code != SIM_IO_MAP)
				continue;
			last_map = sim_iocalls[k].ev;
			if (maps == 0)
				ok &= sim_iocalls[k].iova == 0x10000000000ULL && sim_iocalls[k].pa == 0x100014e8000ULL && sim_iocalls[k].size == 0xc4000 && sim_iocalls[k].prot == IOMMU_READ;
			if (maps == 1)
				ok &= sim_iocalls[k].iova == 0x100000c4000ULL && sim_iocalls[k].pa == 0x100030c4000ULL && sim_iocalls[k].size == 0x2ac000 && sim_iocalls[k].prot == (IOMMU_READ | IOMMU_WRITE);
			maps++;
		}
		chk(maps == 2 && ok, "K1 boot map_fw: TEXT maps 0x100014e8000 at 0x10000000000 read only, DATA 0x100030c4000 at 0x100000c4000 read and write");
		order_ok = attach > 0 && last_map >= attach && first_run_event() > last_map;
		chk(order_ok, "K1 boot map_fw: the attach and both maps come before the RUN write");
	}
	chk(events_in(SIM_DART_PA, SIM_DART_SIZE, 0) == 2 * ANE_H18_DART_NREAD && log_count("CRIT: ane_h18 read DARTPOST_0 pa=0x419800000") == 1, "K1 boot map_fw: the DART words are read before and after the map");
	chk(sim_faults == 0 && log_count("ane_h18 rtkit rx type=") == 2, "K1 boot map_fw: zero faults, HELLO and EPMAP decoded");
	/* kernel-powered node without map_fw: still no PSD request */
	kboot_setup();
	map_fw = false;
	ret = probe_now();
	chk(ret == 0 && psd_write_events() == 0 && iocalls_of(SIM_IO_ALLOC) == 0 && sim_rpm_gets == 1 && sim_rpm_puts == 1, "K2 boot kernel-powered without map_fw: PASS, no PSD write, no iommu call");

	/* the core never reports running: PASS with its own reason, still stopped at the end */
	boot_setup();
	sim_run_status = 0x2a;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-never-reported-running"), "B4 boot: no RUNNING bit gives PASS core-never-reported-running");
	(void)i;
}

static int neg_boot(void)
{
	int bad = 0, ret;
	char seq[256];

	/* refusals: no write of any kind */
	boot_setup();
	confirm_boot = false;
	ret = probe_now();
	if (ret != -EPERM || writes() || !result("boot", "REFUSED", "confirm_power=1 and confirm_boot=1 required")) { printf("    no confirm_boot\n"); bad = 1; }
	boot_setup();
	confirm_power = false;
	ret = probe_now();
	if (ret != -EPERM || writes()) { printf("    boot without confirm_power\n"); bad = 1; }
	boot_setup();
	boot_wait_ms = 5;
	ret = probe_now();
	if (ret != -EINVAL || writes()) { printf("    boot_wait_ms 5\n"); bad = 1; }

	/* rtkit_start needs rtkit_hello, rtkit_stop needs rtkit_start: refused before any write */
	boot_setup();
	alias_mbox = true;
	rtkit_start = true;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("boot", "REFUSED", "rtkit_start=1 needs rtkit_hello=1")) { printf("    rtkit_start without rtkit_hello\n"); bad = 1; }
	boot_setup();
	alias_mbox = true;
	rtkit_hello = true;
	rtkit_stop = 1;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("boot", "REFUSED", "rtkit_stop=1 or 2 needs rtkit_start=1")) { printf("    rtkit_stop without rtkit_start\n"); bad = 1; }
	/* rtkit_hello without alias_mbox is refused before any write */
	boot_setup();
	rtkit_hello = true;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("boot", "REFUSED", "rtkit_hello=1 needs alias_mbox=1")) { printf("    rtkit_hello without alias_mbox\n"); bad = 1; }
	/* read_dart needs both slots: with one slot it is refused before any write or DART read */
	boot_setup();
	read_dart = true;
	psd_slots = 1;
	ret = probe_now();
	if (ret != -EINVAL || writes() || events_in(SIM_DART_PA, SIM_DART_SIZE, 0) || !result("boot", "REFUSED", "read_dart=1 needs psd_slots=2 (the DART block is read only with both ANE slots on)")) { printf("    read_dart with one slot\n"); bad = 1; }
	/* a preflight stop never reads the DART */
	boot_setup();
	read_dart = true;
	sim_rvbar_val = 0x10000000000ULL;
	ret = probe_now();
	if (ret != -EBUSY || events_in(SIM_DART_PA, SIM_DART_SIZE, 0) || ctrl_write_events()) { printf("    DART read after a failed preflight\n"); bad = 1; }

	/* preflight: each deviation stops before the RUN write and puts the slots back */
	{
		int c;

		for (c = 0; c < 5; c++) {
			const char *what = "";

			boot_setup();
			switch (c) {
			case 0: sim_rvbar_val = 0x10000000000ULL; what = "IORVBAR without the valid bit"; break;
			case 1: sim_rvbar_val = 0x20000000001ULL; what = "IORVBAR at another address"; break;
			case 2: sim_cpu_status = 0x9; what = "the core already reports RUNNING"; break;
			case 3: sim_cpu_ctrl = 0x10; what = "CPU_CONTROL.RUN already set"; break;
			case 4: sim_cpu_status = 0xe; what = "the core is in the post-shutdown state (bit 2, as a previous RTKit shutdown leaves it): RUN alone does not restart it"; break;
			}
			ret = probe_now();
			write_seq(seq, sizeof(seq));
			if (ret != -EBUSY || ctrl_write_events() || !result("boot", "FAIL", "preflight-not-as-iboot-left-it") || sim_psd_state[0] != 6 || sim_psd_state[1] != 6) {
				printf("    %s: ret %d, control writes %d, writes '%s'\n", what, ret, ctrl_write_events(), seq);
				bad = 1;
			}
			if (sim_n_iomap != sim_n_iounmap) { printf("    %s: leaked a window\n", what); bad = 1; }
		}
	}
	/* a slot that does not acknowledge: no ASC access, no CONTROL write */
	boot_setup();
	sim_psd_mode[1] = SIM_PSD_NEVER;
	ret = probe_now();
	if (ret != -ETIMEDOUT || ctrl_write_events() || first_asc_read() >= 0 || sim_psd_state[0] != 6) { printf("    PSD never acknowledges\n"); bad = 1; }

	/* map_fw: refusals and failures end before the RUN write and undo what was done */
	boot_setup();
	map_fw = true;
	ret = probe_now();
	if (ret != -EINVAL || writes() || sim_n_iocalls || !result("boot", "REFUSED", "map_fw=1 needs power-domains in the node (the kernel's PSD driver powers the slots for the DART)")) { printf("    map_fw without power-domains\n"); bad = 1; }
	{
		int c;

		for (c = 0; c < 5; c++) {
			const char *what = "";

			kboot_setup();
			switch (c) {
			case 0: node.has_iommus = false; what = "no iommus in the node"; break;
			case 1: sim_io_fail = 1; what = "domain alloc fails"; break;
			case 2: sim_io_fail = 2; what = "attach fails"; break;
			case 3: sim_io_fail = 3; what = "the second map fails"; break;
			case 4: sim_rpm_get_fail = 1; what = "the runtime PM get fails"; break;
			}
			ret = probe_now();
			if (ret == 0 || first_run_event() >= 0 || psd_write_events() || sim_n_dom_live != 0 || !result("boot", "FAIL", c == 4 ? "kernel-psd-power-on-failed" : "fw-map-failed")) {
				printf("    %s: ret %d, run event %d, psd writes %d, live domains %d\n", what, ret, first_run_event(), psd_write_events(), sim_n_dom_live);
				bad = 1;
			}
			if (c == 3 && (iocalls_of(SIM_IO_MAP) != 2 || iocalls_of(SIM_IO_UNMAP) != 1 || iocalls_of(SIM_IO_DETACH) != 1 || iocalls_of(SIM_IO_FREE) != 1)) { printf("    %s: undo incomplete\n", what); bad = 1; }
			if (c == 2 && (iocalls_of(SIM_IO_MAP) != 0 || iocalls_of(SIM_IO_FREE) != 1)) { printf("    %s: map after a failed attach or no free\n", what); bad = 1; }
			if (c == 4 && (sim_n_iocalls || events_in(sim_asc_pa, sim_asc_size, 0))) { printf("    %s: touched the ASC or the iommu\n", what); bad = 1; }
			if (c != 4 && sim_rpm_puts != 1) { printf("    %s: runtime PM reference not released\n", what); bad = 1; }
			if (sim_n_iomap != sim_n_iounmap) { printf("    %s: leaked a window\n", what); bad = 1; }
		}
	}
	/* a stuck core with map_fw: the mapping and the power reference stay, nothing is undone under a core that may run */
	kboot_setup();
	sim_core_stuck = 1;
	ret = probe_now();
	if (ret == 0 || !result("boot", "FAIL", "core-not-stopped") || iocalls_of(SIM_IO_UNMAP) || iocalls_of(SIM_IO_DETACH) || iocalls_of(SIM_IO_FREE) || sim_n_dom_live != 1 || sim_rpm_puts != 0) {
		printf("    stuck core with map_fw: ret %d unmaps %d live %d puts %d\n", ret, iocalls_of(SIM_IO_UNMAP), sim_n_dom_live, sim_rpm_puts);
		bad = 1;
	}

	/* a core that does not stop: FAIL, slots stay on, the module keeps its mappings and refuses to claim a clean state */
	boot_setup();
	sim_core_stuck = 1;
	ret = probe_now();
	write_seq(seq, sizeof(seq));
	if (ret == 0 || !result("boot", "FAIL", "core-not-stopped") || sim_psd_state[0] == 6 || sim_psd_state[1] == 6 || log_count("reboot required") < 1 || sim_n_iounmap != 0) {
		printf("    stuck core: ret %d writes '%s' psd %u %u iounmap %d\n", ret, seq, sim_psd_state[0], sim_psd_state[1], sim_n_iounmap);
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-boot: harness FAILED the module (BAD)" : "  neg-boot: module refuses and fails as required");
	return bad;
}

static int neg_args(void)
{
	int bad = 0, ret;

	reset_all();
	optin = NULL;
	ret = probe_now();
	if (ret != -EPERM || sim_n_events || sim_n_iomap) { printf("    unset optin\n"); bad = 1; }
	reset_all();
	optin = "t8142";
	ret = probe_now();
	if (ret != -EPERM || sim_n_events || sim_n_iomap) { printf("    wrong optin\n"); bad = 1; }
	reset_all();
	stage = "run";
	ret = probe_now();
	if (ret != -EINVAL || sim_n_events || sim_n_iomap) { printf("    stage=run must not exist\n"); bad = 1; }
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_state = 6;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("power", "REFUSED", "psd_state must be 0 to 5 and psd_slots 1 or 2")) { printf("    psd_state 6\n"); bad = 1; }
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = 3;
	ret = probe_now();
	if (ret != -EINVAL || writes()) { printf("    psd_slots 3\n"); bad = 1; }
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = 0;
	ret = probe_now();
	if (ret != -EINVAL || writes()) { printf("    psd_slots 0\n"); bad = 1; }
	printf("%s\n", bad ? "  neg-args: harness FAILED the module (BAD)" : "  neg-args: module refuses as required");
	return bad;
}

/* ---- stage=reads: the step machine on the t8132 row (pmgr domains only, no PMP vote) ---- */

static struct device_node sim_cpu_node;		/* ps_ane_cpu, the third power-domains cell */
static struct device_node sim_pmp_entry;	/* a t6050 PMP report entry: must never be a cell */

static void set_t8132_node(void)
{
	static struct device_node sim_mpm_node, sim_tdbase_node;

	memset(&node, 0, sizeof(node));
	node.n_reg = 1;
	node.reg_pa[0] = 0x5080000000ULL; node.reg_size[0] = 0x2000000; node.reg_name[0] = "ane";
	node.has_type = true;
	node.ane_type = 546;
	node.has_power_domains = true;
	node.phandle = 0xa1;
	sim_pd_attached = 0;	/* the claims attach only through devm_pm_domain_attach_list */		/* the shim returns the per-slot phandle for each power-domains index */
	node.compat = NULL;
	pdev.dev.of_node = &node;
	/* the three claimed ANE pmgr domains: ps_ane_mpm 0xa2, ps_ane_tdbase 0xa3, ps_ane_cpu 0xa4 */
	memset(&sim_mpm_node, 0, sizeof(sim_mpm_node));
	sim_mpm_node.compat = "apple,t6050-pmgr-pwrstate";
	sim_mpm_node.phandle = 0xa2;
	sim_mpm_node.refs = 1;
	sim_phandle[0] = 0xa2;
	sim_phandle_target[0] = &sim_mpm_node;
	memset(&sim_tdbase_node, 0, sizeof(sim_tdbase_node));
	sim_tdbase_node.compat = "apple,t6050-pmgr-pwrstate";
	sim_tdbase_node.phandle = 0xa3;
	sim_tdbase_node.refs = 1;
	sim_phandle[1] = 0xa3;
	sim_phandle_target[1] = &sim_tdbase_node;
	memset(&sim_cpu_node, 0, sizeof(sim_cpu_node));
	sim_cpu_node.compat = "apple,t6050-pmgr-pwrstate";
	sim_cpu_node.phandle = 0xa4;
	sim_cpu_node.refs = 1;
	sim_phandle[2] = 0xa4;
	sim_phandle_target[2] = &sim_cpu_node;
	memset(&sim_pmp_entry, 0, sizeof(sim_pmp_entry));
	sim_pmp_entry.compat = "apple,t6050-pmp-v2-report-entry";
	sim_pmp_entry.reg_val = 0x10;
	sim_pmp_entry.phandle = 0xa1;
	sim_pmp_entry.refs = 1;
}

static int rd_next(ssize_t *rc)
{
	struct file file = { 0 };
	loff_t position = 0;

	if (!sim_rd_entry.ops || !sim_rd_entry.ops->write)
		return -1;
	*rc = sim_rd_entry.ops->write(&file, "next\n", 5, &position);
	return 0;
}

/* A malformed power-domains cell count (of_count error) must refuse the reads
 * stage outright: -EINVAL, FAIL verdict, no vote cast, no attach, no maps. */
static int pos_reads_single(void)
{
	int ret;

	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_ps_on = true;
	set_t8132_node();
	sim_count_error = 1;	/* of_count_phandle_with_args returns -EINVAL */
	ret = probe_now();
	chk(ret == -EINVAL, "S1 single: malformed cell count refuses the arm with -EINVAL");
	chk(result("reads", "FAIL", "pd-count-malformed"), "S1 single: RESULT FAIL pd-count-malformed");
	chk(sim_rpm_gets == 0, "S1 single: no device vote cast on the refusal");
	chk(sim_pd_attached == 0, "S1 single: no list attach on the refusal");
	chk(sim_n_iomap == 0 && sim_n_iounmap == 0, "S1 single: no engine window mapped on the refusal");
	printf("pos-reads-single: %s\n", fails ? "FAILED" : "PASS");
	return fails;
}

static int pos_reads(void)
{
	struct file file = { 0 };
	loff_t position = 0;
	ssize_t operated;
	int ret, i, reads_before;

	reset_all();
	optin = "t8132";
	stage = "reads";
	allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL;
	pmgr_gate_mask = 0x3f;
	pmgr_gate2_pa = 0x2922d0000ULL;
	pmgr_gate2_mask = 0x1;
	pmgr_ps_pa = 0x280900000ULL;
	pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_gate_pa[1] = pmgr_gate2_pa; sim_gate_val[1] = 0x1;
	sim_ps_on = true;
	set_t8132_node();

	ret = probe_now();
	chk(ret == 0, "R1 reads: probe returns 0");
	chk(result("reads", "PASS", "reads-armed-step-via-ane_h18_rd"), "R1 reads: RESULT PASS reads-armed");
	chk(sim_rd_ready, "R1 reads: rd control published");
	chk(sim_rpm_gets == 0 && sim_rpm_puts == 0, "R1 reads: no device runtime PM get on the multi-domain path (the claims hold through links)");
	/* two gate reads at the first step: both gate families */
	reads_before = sim_n_events;
	ret = rd_next(&operated);
	chk(ret == 0 && operated == 5, "R1 reads: first next accepted");
	chk(result("reads", "PASS", "reads-done") == false, "R1 reads: not done after one step");
	for (i = 2; i <= (int)ARRAY_SIZE(ane_h18_rd_list); i++) {
		ret = rd_next(&operated);
		chk(ret == 0 && operated == 5, "R1 reads: next accepted");
	}
	chk(result("reads", "PASS", "reads-done") == false, "R1 reads: every listed register read, not done yet");
	ret = rd_next(&operated);	/* the write after the last register reports done */
	chk(ret == 0 && operated == 5, "R1 reads: done write accepted");
	chk(result("reads", "PASS", "reads-done"), "R1 reads: RESULT PASS reads-done after all listed reads");
	reads_before = sim_n_events;
	ret = rd_next(&operated);
	chk(ret == 0, "R1 reads: next after done is a no-op that returns count");
	chk(sim_n_events == reads_before, "R1 reads: no register re-read after done");
	/* reads only: no bus write of any kind across the whole sequence */
	for (i = 0; i < sim_n_events; i++)
		chk(!sim_events[i].write, "R1 reads: zero bus writes (event index above)");
	/* removal: control first, then the vote, then the maps; the multi-domain claims
	 * hold through devm links, so there is no device put and none was faked */
	ane_h18_unmap(ane_h18_rd_dev);
	chk(sim_pd_attached == 3 && sim_pd_list.num_pds == 3, "R1 reads: all three claims attached");
	chk(sim_pd_false_votes == 0, "R1 reads: no pm_runtime_get_sync on the multi-domain device (no false vote)");
	chk(sim_rpm_puts == 0, "R1 reads: removal puts nothing on the multi-domain path (devm links hold)");
	chk(!ane_h18_rd_dev, "R1 reads: rd device cleared");

	/* R2: the default vote-free configuration - the four ps registers alone, no extra gate
	 * register: it arms, reads, and never maps or reads a gate page */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == 0, "R2 reads: arms with the ps registers only");
	chk(result("reads", "PASS", "reads-armed-step-via-ane_h18_rd"), "R2 reads: RESULT PASS reads-armed");
	chk(sim_n_iomap == 2, "R2 reads: only the engine window and the ps page are mapped");
	chk(sim_pd_attached == 3 && sim_rpm_gets == 0, "R2 reads: the three pmgr claims attach, no device vote");
	ret = rd_next(&operated);
	chk(ret == 0 && operated == 5, "R2 reads: first step accepted");
	chk(!result("reads", "REFUSED", "pmgr-ps-off"), "R2 reads: no refusal with the ps registers on");
	sim_ps_on = false;
	ret = rd_next(&operated);
	chk(ret == 0 && operated < 0, "R2 reads: step refused once a ps register leaves state");
	chk(result("reads", "REFUSED", "pmgr-ps-off"), "R2 reads: REFUSED pmgr-ps-off with no gate register configured");
	ane_h18_unmap(ane_h18_rd_dev);

	/* R3: stage=dt on the incomplete t8132 row reports the windows unchecked:
	 * no window check ran, so the checked verdict would be a false PASS */
	reset_all();
	optin = "t8132"; stage = "dt";
	set_t8132_node();
	ret = probe_now();
	chk(ret == 0 && sim_n_iomap == 0, "R3 dt: incomplete row maps nothing in stage=dt");
	chk(result("dt", "PASS", "dt-windows-reported-unchecked"), "R3 dt: RESULT PASS dt-windows-reported-unchecked");
	printf("pos-reads: %s\n", fails ? "FAILED" : "PASS");
	return fails;
}

static int neg_reads(void)
{
	int ret;
	ssize_t operated;

	/* N1: no allow_reads */
	reset_all();
	optin = "t8132"; stage = "reads";
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EPERM, "N1 reads: refused without allow_reads (rc above)");

	/* N2: the second extra gate register without the first */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate2_pa = 0x2922d0000ULL; pmgr_gate2_mask = 0x1;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EINVAL, "N2 reads: gate2 without gate refused (rc above)");
	chk(result("reads", "FAIL", "gates-unverified-pass-pmgr-gate-pa-and-mask"), "N2 reads: FAIL gates-unverified");
	chk(sim_rpm_gets == 0 && sim_pd_attached == 0, "N2 reads: no power claim attached for a bad gate config");

	/* N3: bare node (no power-domains) */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	memset(&node, 0, sizeof(node));
	node.n_reg = 1;
	node.reg_pa[0] = 0x5080000000ULL; node.reg_size[0] = 0x2000000;
	node.has_type = true; node.ane_type = 546;
	node.has_power_domains = false;
	pdev.dev.of_node = &node;
	ret = probe_now();
	chk(ret == -EPERM, "N3 reads: bare node refused");
	chk(strstr(sim_log, "stage=reads requires power-domains") != NULL, "N3 reads: bare-node refusal names the missing provider");

	/* N4: phandle resolves to a foreign node (the base dtb shifted) */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_ps_on = true;
	set_t8132_node();
	sim_cpu_node.compat = "apple,t6050-pmgr-OTHER";	/* shifted tree: wrong compatible */
	ret = probe_now();
	chk(ret == -EINVAL, "N4 reads: foreign phandle target refused (rc above)");
	chk(result("reads", "FAIL", "ps-domain-mismatch"), "N4 reads: FAIL ps-domain-mismatch");
	chk(sim_rpm_gets == 0 && sim_pd_attached == 0, "N4 reads: no power claim on a foreign domain");

	/* N5: phandle dangles (the final dtb predates the ps_ane_cpu pin) */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_ps_on = true;
	set_t8132_node();
	sim_phandle_target[2] = NULL;	/* the ps_ane_cpu phandle dangles in the live tree */
	ret = probe_now();
	chk(ret == -EINVAL, "N5 reads: dangling phandle refused (rc above)");
	chk(result("reads", "FAIL", "ps-domain-mismatch"), "N5 reads: FAIL ps-domain-mismatch");
	chk(sim_rpm_gets == 0 && sim_pd_attached == 0, "N5 reads: no power claim without a target");

	/* N6: the PMP report entry as the last cell (the old vote-last layout) = refuse */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_ps_on = true;
	set_t8132_node();
	sim_phandle[2] = 0xa1; sim_phandle_target[2] = &sim_pmp_entry;
	ret = probe_now();
	chk(ret == -EINVAL, "N6 reads: PMP report entry cell refused (rc above)");
	chk(result("reads", "FAIL", "ps-domain-mismatch"), "N6 reads: FAIL ps-domain-mismatch");
	chk(sim_rpm_gets == 0 && sim_pd_attached == 0, "N6 reads: no PMP vote and no attach");

	/* N7: gates fall off between arm and step: REFUSED, no readl */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_gate2_pa = 0x2922d0000ULL; pmgr_gate2_mask = 0x1;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_gate_pa[1] = pmgr_gate2_pa; sim_gate_val[1] = 0x1;
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == 0, "N7 reads: armed");
	sim_gate_val[0] = 0;	/* the power gates dropped */
	ret = rd_next(&operated);
	chk(ret == 0 && operated < 0, "N7 reads: step refused when the gate dropped");
	chk(result("reads", "REFUSED", "pmgr-gates-off"), "N7 reads: REFUSED pmgr-gates-off");
	/* still no writes even in the refused state */
	sim_n_events = 0;
	rd_next(&operated);
	for (ret = 0; ret < sim_n_events; ret++)
		chk(!sim_events[ret].write, "N7 reads: refused state does no writes");

	/* N8: second gate family fails the mask */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_gate2_pa = 0x2922d0000ULL; pmgr_gate2_mask = 0x1;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_gate_pa[1] = pmgr_gate2_pa; sim_gate_val[1] = 0;	/* clock gates off */
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == 0, "N8 reads: armed");
	ret = rd_next(&operated);
	chk(ret == 0 && operated < 0, "N8 reads: step refused when gate2 lacks its mask");
	chk(result("reads", "REFUSED", "pmgr-gates-off"), "N8 reads: REFUSED pmgr-gates-off");

	/* N9: the claims attach fails at arm: no maps leak, no control */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_ps_on = true;
	sim_pd_attach_fail = 1;
	set_t8132_node();
	ret = probe_now();
	chk(ret < 0, "N9 reads: attach failure refuses the arm (rc above)");
	chk(result("reads", "FAIL", "pd-attach-failed"), "N9 reads: FAIL pd-attach-failed");
	chk(!sim_rd_ready, "N9 reads: no control published");
	chk(sim_n_iounmap == 1, "N9 reads: engine window unmapped on attach failure");

	/* N10: t8132 refuses psd/power/boot outright */
	reset_all();
	optin = "t8132"; stage = "psd";
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EINVAL, "N10 t8132: psd refused on the incomplete row");
	reset_all();
	optin = "t8132"; stage = "boot";
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EINVAL, "N10 t8132: boot refused on the incomplete row");

	/* N11: the ps page params missing = fail closed */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EINVAL, "N11 reads: missing ps params refused");
	chk(result("reads", "FAIL", "ps-unverified-pass-pmgr-ps-pa"), "N11 reads: FAIL ps-unverified");
	chk(sim_rpm_gets == 0, "N11 reads: no vote cast without the ps check");

	/* N12: a ps register not fully on = REFUSED at the step, no readl */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_gate2_pa = 0x2922d0000ULL; pmgr_gate2_mask = 0x1;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_gate_pa[1] = pmgr_gate2_pa; sim_gate_val[1] = 0x1;
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == 0, "N12 reads: armed");
	sim_ps_on = false;	/* a domain dropped out of state */
	ret = rd_next(&operated);
	chk(ret == 0 && operated < 0, "N12 reads: step refused when a ps register left state");
	chk(result("reads", "REFUSED", "pmgr-ps-off"), "N12 reads: REFUSED pmgr-ps-off");

	/* N13: gate2_pa with a zero mask = refuse at arm (a zero mask passes trivially) */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_gate2_pa = 0x2922d0000ULL; pmgr_gate2_mask = 0;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EINVAL, "N13 reads: gate2 zero-mask refused");
	chk(result("reads", "FAIL", "gates-unverified-pass-pmgr-gate-pa-and-mask"), "N13 reads: FAIL gates-unverified");
	chk(sim_rpm_gets == 0, "N13 reads: no vote cast for a zero gate2 mask");

	/* N14: an unaligned pmgr_ps_pa = refuse at arm */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_ps_pa = 0x280900123ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EINVAL, "N14 reads: unaligned ps page refused");
	chk(result("reads", "FAIL", "ps-unverified-pass-pmgr-ps-pa"), "N14 reads: FAIL ps-unverified");
	chk(sim_rpm_gets == 0, "N14 reads: no vote cast for an unaligned ps page");

	/* N15: the ps ioremap failing must unmap gate2 AND gate, release the vote, leave no leak */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_gate2_pa = 0x2922d0000ULL; pmgr_gate2_mask = 0x1;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_gate_pa[1] = pmgr_gate2_pa; sim_gate_val[1] = 0x1;
	sim_ps_on = true;
	set_t8132_node();
	{
		int maps_before = sim_n_iomap, unmaps_before = sim_n_iounmap;

		/* engine of_iomap + gate + gate2 succeed; the 4th map (ps) fails */
		sim_iomap_fail_at = 4;
		ret = probe_now();
		chk(ret != 0, "N15 reads: ps ioremap failure fails the arm");
		chk(sim_n_iomap == maps_before + 4, "N15 reads: four maps attempted, the fourth failed");
		chk(sim_n_iounmap == unmaps_before + 3, "N15 reads: gate2, gate and engine all unmapped");
		chk(sim_rpm_puts == 0, "N15 reads: no device put on the multi-domain path (devm holds)");
	}

	/* N16: a claimed ps sub-block cell that does not resolve to a pmgr pwrstate = refuse */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_gate_pa = 0x2922c0000ULL; pmgr_gate_mask = 0x3f;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_gate_pa[0] = pmgr_gate_pa; sim_gate_val[0] = 0x3f;
	sim_ps_on = true;
	set_t8132_node();
	sim_phandle_target[0] = NULL;	/* the claimed ps_ane_mpm phandle dangles in the live tree */
	ret = probe_now();
	chk(ret == -EINVAL, "N16 reads: dangling ps-domain claim refused");
	chk(result("reads", "FAIL", "ps-domain-mismatch"), "N16 reads: FAIL ps-domain-mismatch");
	chk(sim_rpm_gets == 0, "N16 reads: no vote cast for a dangling ps-domain claim");

	/* N17: the PMP report entry as the first cell = refuse before any claim attaches */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0xf;
	sim_ps_on = true;
	set_t8132_node();
	sim_phandle[0] = 0xa1; sim_phandle_target[0] = &sim_pmp_entry;
	ret = probe_now();
	chk(ret == -EINVAL, "N17 reads: PMP cell first refused");
	chk(result("reads", "FAIL", "ps-domain-mismatch"), "N17 reads: FAIL ps-domain-mismatch");
	chk(sim_rpm_gets == 0 && sim_pd_attached == 0, "N17 reads: no vote and no attach when a PMP cell is present");


	/* N19: stage=reads on the t8152 row = refuse: reads is the t8132 row's probe */
	reset_all();
	optin = "t8152"; stage = "reads"; allow_reads = true;
	ret = probe_now();
	chk(ret == -EINVAL, "N19 reads: t8152 row refuses stage=reads (rc above)");
	chk(sim_n_iomap == 0 && sim_rpm_gets == 0 && sim_pd_attached == 0, "N19 reads: nothing mapped or claimed on the refusal");

	/* N18: a pmgr_ps_mask other than the on-identity 0xf = refuse at arm */
	reset_all();
	optin = "t8132"; stage = "reads"; allow_reads = true;
	pmgr_ps_pa = 0x280900000ULL; pmgr_ps_mask = 0x7;
	sim_ps_on = true;
	set_t8132_node();
	ret = probe_now();
	chk(ret == -EINVAL, "N18 reads: pmgr_ps_mask other than 0xf refused");
	chk(result("reads", "FAIL", "ps-mask-not-0xf"), "N18 reads: FAIL ps-mask-not-0xf");
	chk(sim_rpm_gets == 0 && sim_pd_attached == 0, "N18 reads: no vote and no attach for a foreign mask");

	printf("neg-reads: %s\n", fails ? "FAILED" : "PASS");
	return fails;
}

/* ---- H18 resident binding lifetime tests (RP1..RP7): N resident programs, switch without reload, free in any order ---- */
static u8 sim_blob_plain[0x10000];
static size_t sim_blob_plain_len;
static u8 sim_blob_plain_io[0x10000];
static size_t sim_blob_plain_io_len;
static u8 sim_blob_bad_doff[0x10000];
static size_t sim_blob_bad_doff_len;
static u8 sim_blob_weighted[0x10000];
static size_t sim_blob_weighted_len;
static u8 sim_blob_bad[32];

static int sim_res_fw_hook(const char *name, const u8 **data, size_t *size)
{
	if (!strcmp(name, "missing.bin"))
		return -ENOENT;
	if (!strcmp(name, "bad.bin")) {
		*data = sim_blob_bad;
		*size = sizeof(sim_blob_bad);
		return 0;
	}
	if (!strcmp(name, "weighted.bin")) {
		*data = sim_blob_weighted;
		*size = sim_blob_weighted_len;
		return 0;
	}
	if (!strcmp(name, "plain_io.bin")) {
		*data = sim_blob_plain_io;
		*size = sim_blob_plain_io_len;
		return 0;
	}
	if (!strcmp(name, "bad_doff.bin")) {
		*data = sim_blob_bad_doff;
		*size = sim_blob_bad_doff_len;
		return 0;
	}
	*data = sim_blob_plain;
	*size = sim_blob_plain_len;
	return 0;
}

static void res_boot_live(void)
{
	static const u32 scr[] = { 8, 0xc440, 0x100, 0xf00000, 0, 0x100090, 0, ANE_H18_HS_READY };
	u64 bad_doff = 0x1000;

	kboot_setup();
	make_blob();
	memcpy(sim_blob_plain, sim_blob, sim_blob_len);
	sim_blob_plain_len = sim_blob_len;
	memcpy(sim_blob_plain_io, sim_blob, sim_blob_len);
	sim_blob_plain_io_len = sim_blob_len;
	/* io[0] (id=5): clear doff and dsize for a plain-input program without embedded data. */
	memset(sim_blob_plain_io + 0x20 + 6 * 0x20 + 16, 0, 16);
	memcpy(sim_blob_bad_doff, sim_blob_plain_io, sim_blob_plain_io_len);
	sim_blob_bad_doff_len = sim_blob_plain_io_len;
	/* io[0] (id=5): nonzero doff with dsize=0 must still be refused by CALL_IO. */
	memcpy(sim_blob_bad_doff + 0x20 + 6 * 0x20 + 16, &bad_doff, 8);
	make_blob_kern_weights();
	memcpy(sim_blob_weighted, sim_blob, sim_blob_len);
	sim_blob_weighted_len = sim_blob_len;
	memset(sim_blob_bad, 0xff, sizeof(sim_blob_bad));
	sim_fw_hook = sim_res_fw_hook;

	alias_mbox = rtkit_hello = rtkit_start = read_scratch = true;
	handshake = hs_chan = hs_cmds = hs_start = true;
	keep_alive = true;
	rtkit_stop = 1;
	sim_start_chan = 536;
	sim_start_hello = 3;
	sim_scr_model = sim_hs_table = 1;
	memcpy(sim_scr, scr, sizeof(scr));
}

static int res_open(struct file *f)
{
	struct inode ino = { .i_private = sim_res_entry.data };

	memset(f, 0, sizeof(*f));
	return sim_res_entry.ops->open(&ino, f);
}

static int res_close(struct file *f)
{
	struct inode ino = { .i_private = sim_res_entry.data };

	return sim_res_entry.ops->release(&ino, f);
}

static ssize_t res_cmd(struct file *f, const char *cmd, char *reply, size_t rsz)
{
	ssize_t w = sim_res_entry.ops->write(f, cmd, strlen(cmd), &f->f_pos);

	if (w < 0)
		return w;
	if (reply && rsz) {
		ssize_t r = sim_res_entry.ops->read(f, reply, rsz - 1, &f->f_pos);

		reply[r > 0 ? r : 0] = '\0';
		return r;
	}
	return w;
}

static int res_live_dma(void)
{
	int i, live = 0;

	for (i = 0; i < sim_n_dma; i++)
		live += sim_dma[i].live;
	return live;
}

static void pos_residency(void)
{
	/* RP1: exact non-LIFO wire order (two LOAD/CREATEs, four calls without reload, free A — first loaded — before B,
	 * then call B again and free B), exclusive per-open session, distinct FW IDs and digests, clean rmmod with zero DMA leak. */
	{
		static const u32 want_ops[] = {
			0x200, 0x202, 0x200, 0x202, 0x204, 0x204,
			0x204, 0x203, 0x201, 0x204, 0x203, 0x201,
		};
		struct file f1, f2;
		char hA[32], hB[32], r1[96], r2[96], r3[96], r4[96], rf[32], cmd[128];
		u64 ns1 = 0, ns2 = 0, ns3 = 0, ns4 = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0;
		u32 a, b;
		int ret, o1, o2;

		res_boot_live();
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-live-ready") && sim_res_ready && sim_res_entry.live && sim_res_entry.mode == 0600,
		    "RP1a keep_alive=1 boots live and exposes root-only 0600 debugfs resident");
		o1 = res_open(&f1);
		o2 = res_open(&f2);
		chk(o1 == 0 && o2 == -EBUSY, "RP1b exclusive per-open session: first open succeeds, second concurrent open returns -EBUSY");
		ret = (int)res_cmd(&f1, "retain sim.bin 0 0 0 0 0\n", hA, sizeof(hA));
		chk(ret == 17 && hA[16] == '\n', "RP1c retain A returns 16-hex handle + newline");
		hA[16] = '\0';
		ret = (int)res_cmd(&f1, "retain sim.bin 0 0 0 0 0\n", hB, sizeof(hB));
		chk(ret == 17 && hB[16] == '\n' && strcmp(hA, hB) != 0, "RP1d retain B returns distinct 16-hex handle");
		hB[16] = '\0';
		snprintf(cmd, sizeof(cmd), "call %s\n", hA);
		ret = (int)res_cmd(&f1, cmd, r1, sizeof(r1));
		sscanf(r1, "ns=%llu digest=%llx", (unsigned long long *)&ns1, (unsigned long long *)&d1);
		snprintf(cmd, sizeof(cmd), "call %s\n", hB);
		ret = ret > 0 ? (int)res_cmd(&f1, cmd, r2, sizeof(r2)) : ret;
		sscanf(r2, "ns=%llu digest=%llx", (unsigned long long *)&ns2, (unsigned long long *)&d2);
		snprintf(cmd, sizeof(cmd), "call %s\n", hA);
		ret = ret > 0 ? (int)res_cmd(&f1, cmd, r3, sizeof(r3)) : ret;
		sscanf(r3, "ns=%llu digest=%llx", (unsigned long long *)&ns3, (unsigned long long *)&d3);
		/* Non-LIFO: free A (first loaded) before B, then call B again without reload. */
		snprintf(cmd, sizeof(cmd), "free %s\n", hA);
		ret = ret > 0 ? (int)res_cmd(&f1, cmd, rf, sizeof(rf)) : ret;
		snprintf(cmd, sizeof(cmd), "call %s\n", hB);
		ret = ret > 0 ? (int)res_cmd(&f1, cmd, r4, sizeof(r4)) : ret;
		sscanf(r4, "ns=%llu digest=%llx", (unsigned long long *)&ns4, (unsigned long long *)&d4);
		snprintf(cmd, sizeof(cmd), "free %s\n", hB);
		ret = ret > 0 ? (int)res_cmd(&f1, cmd, rf, sizeof(rf)) : ret;
		res_close(&f1);
		ane_h18_remove(&pdev);
		a = sim_prog_ids[0];
		b = sim_prog_ids[2];
		chk(ret > 0 && sim_prog_n == (int)ARRAY_SIZE(want_ops) && !memcmp(sim_prog_ops, want_ops, sizeof(want_ops)),
		    "RP1e exact wire order: two LOAD/CREATEs, four calls without reload, two TERMINATE/DESTROYs");
		chk(a != b && sim_prog_ids[1] == a && sim_prog_ids[3] == b && sim_prog_ids[4] == a &&
		    sim_prog_ids[5] == b && sim_prog_ids[6] == a && sim_prog_ids[7] == a &&
		    sim_prog_ids[8] == a && sim_prog_ids[9] == b && sim_prog_ids[10] == b &&
		    sim_prog_ids[11] == b && sim_t2h_back == 8 && d1 != d2 && d1 == d3 && d2 == d4 &&
		    ns1 > 0 && ns4 > 0 && res_live_dma() == 0 && sim_owner_module.refs == 0 && !sim_res_ready,
		    "RP1f non-LIFO: distinct FW IDs + digests, free A before B then call B, clean rmmod with zero DMA leak");
	}

	/* RP2: 3 resident programs, middle-first free, double-free & stale-generation handle refused after slot reuse. */
	{
		struct file f;
		char hA[32], hB[32], hC[32], hD[32], rep[96], cmd[128];
		ssize_t r_callB, r_freeB2, r_staleB, r_callD;

		res_boot_live();
		probe_now();
		res_open(&f);
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hA, sizeof(hA)); hA[16] = '\0';
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hB, sizeof(hB)); hB[16] = '\0';
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hC, sizeof(hC)); hC[16] = '\0';
		snprintf(cmd, sizeof(cmd), "free %s\n", hB);
		res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "call %s\n", hB);
		r_callB = res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hB);
		r_freeB2 = res_cmd(&f, cmd, rep, sizeof(rep));
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hD, sizeof(hD)); hD[16] = '\0';
		snprintf(cmd, sizeof(cmd), "call %s\n", hB);
		r_staleB = res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "call %s\n", hD);
		r_callD = res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hC); res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hA); res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hD); res_cmd(&f, cmd, rep, sizeof(rep));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_callB == -ENOENT && r_freeB2 == -ENOENT && strcmp(hD, hB) != 0 && r_staleB == -ENOENT && r_callD > 0 && res_live_dma() == 0,
		    "RP2 middle-first free + slot reuse bumps generation: stale handle B returns -ENOENT, D runs clean");
	}

	/* RP3: disjoint fixed-IOVA + kern_slot=1 residency, overlap refusal (-EBUSY) BEFORE iommu_map/LOAD_PROGRAM,
	 * self-overlap refusal (-EINVAL), and range reuse after free. */
	{
		struct file f;
		char hA[32], hB[32], hC[32], rep[96], cmd[160];
		ssize_t r_ov_buf, r_ov_kern, r_self_ov, r_b, r_ca, r_cb, r_c;
		int io_before, prog_before, io_after, prog_after;

		res_boot_live();
		probe_now();
		res_open(&f);
		res_cmd(&f, "retain weighted.bin 0x30000000 0x3000c000 0x280 0x30020000 1\n", hA, sizeof(hA));
		hA[16] = '\0';
		chk(sim_load_sec2_iova == 0x30020000 && sim_load_sec2_size == 0x4000 && sim_load_sec2_b1 == 0x5a,
		    "RP3a resident retain with kern_slot=1 binds LOAD_PROGRAM slot 1 to pg_kern (size-0x4000, weights tail)");
		io_before = sim_n_iocalls;
		prog_before = sim_prog_n;
		r_ov_buf = res_cmd(&f, "retain weighted.bin 0x30004000 0x3010c000 0x280 0x30120000 1\n", rep, sizeof(rep));
		r_ov_kern = res_cmd(&f, "retain weighted.bin 0x30100000 0x3010c000 0x280 0x30020000 1\n", rep, sizeof(rep));
		r_self_ov = res_cmd(&f, "retain weighted.bin 0x30100000 0x30104000 0x280 0x30120000 1\n", rep, sizeof(rep));
		io_after = sim_n_iocalls;
		prog_after = sim_prog_n;
		chk(r_ov_buf == -EBUSY && r_ov_kern == -EBUSY && r_self_ov == -EINVAL && io_after == io_before && prog_after == prog_before,
		    "RP3b overlapping fixed IOVAs refused (-EBUSY) and self-overlap (-EINVAL) BEFORE any IOMMU map or LOAD_PROGRAM");
		r_b = res_cmd(&f, "retain weighted.bin 30100000 3010c000 280 30120000 1\n", hB, sizeof(hB));
		hB[16] = '\0';
		{
			struct sim_fw_prog *fpA = sim_fw_find(sim_prog_ids[0]);
			struct sim_fw_prog *fpB = sim_fw_find(sim_prog_ids[2]);

			chk(fpA && fpB && fpA->sec2_iova == 0x30020000 && fpA->sec2_size == 0x4000 &&
			    fpB->sec2_iova == 0x30120000 && fpB->sec2_size == 0x4000,
			    "RP3c LOAD_PROGRAM slot 1 (pg_kern) is bound per-program (A=0x30020000, B=0x30120000), never re-pointed");
		}
		snprintf(cmd, sizeof(cmd), "call %s\n", hA); r_ca = res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "call %s\n", hB); r_cb = res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hA); res_cmd(&f, cmd, rep, sizeof(rep));
		/* After freeing A (first loaded), B's slot-1 kern surface stays mapped and B still runs; A's range is reusable. */
		snprintf(cmd, sizeof(cmd), "call %s\n", hB);
		r_cb = r_cb > 0 ? res_cmd(&f, cmd, rep, sizeof(rep)) : r_cb;
		r_c = res_cmd(&f, "retain weighted.bin 30000000 3000c000 280 30020000 1\n", hC, sizeof(hC));
		hC[16] = '\0';
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_b == 17 && r_ca > 0 && r_cb > 0 && r_c == 17 && res_live_dma() == 0,
		    "RP3d disjoint bare-hex fixed-IOVA weighted pair switches without reload; freeing A keeps B live and frees A's range for reuse");
	}

	/* RP4: 16-slot host table cap (-ENOSPC on 17th), reuse after middle free, and automatic release() teardown. */
	{
		struct file f, f2;
		char handles[16][32], rep[96], cmd[128];
		int i, ok16 = 1;
		ssize_t r17, r18;

		res_boot_live();
		probe_now();
		res_open(&f);
		for (i = 0; i < 16; i++) {
			if (res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", handles[i], sizeof(handles[i])) != 17)
				ok16 = 0;
			handles[i][16] = '\0';
		}
		r17 = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", handles[7]);
		res_cmd(&f, cmd, rep, sizeof(rep));
		r18 = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", rep, sizeof(rep));
		res_close(&f);	/* tears down all 16 remaining LIVE slots */
		chk(ok16 && r17 == -ENOSPC && r18 == 17 && !sim_prog_loaded && res_open(&f2) == 0,
		    "RP4 16 host slots cap at -ENOSPC, middle slot reusable, close() tears down all LIVE slots and releases session");
		res_close(&f2);
		ane_h18_remove(&pdev);
		chk(res_live_dma() == 0 && sim_owner_module.refs == 0, "RP4b zero DMA leak after close() teardown and rmmod");
	}

	/* RP5: unload failure -> HELD slot retains surfaces + fixed ranges forever, pins module, NO out-of-band reset,
	 * other LIVE slots keep working! */
	{
		struct file f;
		char hA[32], hB[32], rep[96], cmd[128];
		ssize_t r_freeA, r_callA, r_ov, r_callB, r_freeB;
		u32 progA;
		int live_after_freeA;

		res_boot_live();
		probe_now();
		res_open(&f);
		res_cmd(&f, "retain weighted.bin 0x30000000 0x3000c000 0x280 0x30020000 1\n", hA, sizeof(hA));
		hA[16] = '\0';
		progA = sim_prog_ids[0];
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hB, sizeof(hB));
		hB[16] = '\0';
		sim_res_fail_op = 0x201;
		sim_res_fail_prog = progA;
		sim_res_fail_ack = 3;
		snprintf(cmd, sizeof(cmd), "free %s\n", hA);
		r_freeA = res_cmd(&f, cmd, rep, sizeof(rep));
		live_after_freeA = res_live_dma();
		sim_res_fail_op = 0;
		snprintf(cmd, sizeof(cmd), "call %s\n", hA);
		r_callA = res_cmd(&f, cmd, rep, sizeof(rep));
		r_ov = res_cmd(&f, "retain weighted.bin 0x30000000 0x3000c000 0x280 0x30020000 1\n", rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "call %s\n", hB);
		r_callB = res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hB);
		r_freeB = res_cmd(&f, cmd, rep, sizeof(rep));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_freeA == -EIO && r_callA == -ESHUTDOWN && r_ov == -EBUSY && r_callB > 0 && r_freeB > 0 &&
		    sim_owner_module.refs >= 1 && live_after_freeA > 0 && res_live_dma() > 0 && sim_rpm_gets == 1,
		    "RP5 failed DESTROY_PROGRAM marks slot HELD (surfaces + fixed range kept, module pinned, no engine reset, B still runs)");
	}

	/* RP6: short alias unmap on free -> slot HELD, leaked page not freed, range stays reserved, module pinned. */
	{
		struct file f;
		char hA[32], rep[96], cmd[128];
		ssize_t r_freeA, r_ov;

		res_boot_live();
		probe_now();
		res_open(&f);
		res_cmd(&f, "retain weighted.bin 0x30000000 0x3000c000 0x280 0x30020000 1\n", hA, sizeof(hA));
		hA[16] = '\0';
		sim_unmap_short_iova = 0x30000000;
		snprintf(cmd, sizeof(cmd), "free %s\n", hA);
		r_freeA = res_cmd(&f, cmd, rep, sizeof(rep));
		sim_unmap_short_iova = 0;
		r_ov = res_cmd(&f, "retain weighted.bin 0x30000000 0x3010c000 0x280 0x30120000 1\n", rep, sizeof(rep));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_freeA == -EIO && r_ov == -EBUSY && sim_owner_module.refs >= 1 && log_count("the pages are leaked, not freed") == 1 && res_live_dma() == 1,
		    "RP6 short alias unmap on free leaks that page, pins module, keeps range reserved in HELD slot (-EBUSY)");
	}

	/* RP7: bad syntax/basename/kern_slot, CREATE_PROCESS rollback (DESTROY_PROGRAM + zero leak), and keep_alive gating. */
	{
		struct file f;
		char rep[96];
		ssize_t r_path, r_miss, r_bad, r_ks, r_zero, r_cp;
		int live_before, live_after, ret_ka;

		res_boot_live();
		probe_now();
		res_open(&f);
		r_path = res_cmd(&f, "retain ../evil.bin 0 0 0 0 0\n", rep, sizeof(rep));
		r_miss = res_cmd(&f, "retain missing.bin 0 0 0 0 0\n", rep, sizeof(rep));
		r_bad = res_cmd(&f, "retain bad.bin 0 0 0 0 0\n", rep, sizeof(rep));
		r_ks = res_cmd(&f, "retain sim.bin 0 0 0 0 1\n", rep, sizeof(rep));
		r_zero = res_cmd(&f, "call 0\n", rep, sizeof(rep));
		chk(res_cmd(&f, "retain aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 0 0 0 0 0\n", rep, sizeof(rep)) == -EINVAL &&
		    res_cmd(&f, "retain bad$name.bin 0 0 0 0 0\n", rep, sizeof(rep)) == -EINVAL,
		    "RP7c 64-char basename and non-[A-Za-z0-9._-] characters are refused (-EINVAL)");
		live_before = res_live_dma();
		sim_res_fail_op = 0x202;
		sim_res_fail_ack = 3;
		r_cp = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", rep, sizeof(rep));
		sim_res_fail_op = 0;
		live_after = res_live_dma();
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_path == -EINVAL && r_miss == -ENOENT && r_bad == -EINVAL && r_ks == -EINVAL && r_zero == -ENOENT &&
		    r_cp == -EIO && live_after == live_before && !sim_prog_loaded && res_live_dma() == 0,
		    "RP7a input validation + CREATE_PROCESS refusal sends DESTROY_PROGRAM and frees candidate cleanly");

		kboot_setup();
		alias_mbox = rtkit_hello = rtkit_start = read_scratch = true;
		handshake = hs_chan = hs_cmds = true;
		hs_start = false;
		keep_alive = true;
		rtkit_stop = 1;
		ret_ka = probe_now();
		chk(ret_ka == -EINVAL && result("boot", "REFUSED", "keep_alive=1 needs hs_start=1, rtkit_stop=1 or 2, owned_fw=0 and hs_prog=0") && writes() == 0,
		    "RP7b keep_alive=1 without hs_start=1 is refused before any write");
	}

	/* RP8: wrong-resident completion event (state=1 + cookie 0xADD0, but +0x10/+0x14 carry B's prog/proc while calling A):
	 * BOTH timed peek and prog_t2h must reject it; call A fails -EIO, marks A HELD and pins module. */
	{
		struct file f;
		char hA[32], hB[32], rep[96], cmd[128];
		u32 progB;
		ssize_t r_wrong;

		res_boot_live();
		probe_now();
		res_open(&f);
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hA, sizeof(hA)); hA[16] = '\0';
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hB, sizeof(hB)); hB[16] = '\0';
		progB = sim_prog_ids[2];
		sim_res_spoof_prog = progB;
		sim_res_spoof_proc = sim_fw_find(progB)->proc_id;
		snprintf(cmd, sizeof(cmd), "call %s\n", hA);
		r_wrong = res_cmd(&f, cmd, rep, sizeof(rep));
		sim_res_spoof_prog = sim_res_spoof_proc = 0;
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_wrong == -EIO && sim_owner_module.refs >= 1 && res_live_dma() > 0,
		    "RP8 wrong-resident finish event (B's prog/proc on call A) is rejected by peek and t2h: -EIO, HELD, pinned");
	}

	/* RP9: partial-map rollback short unmap in ane_h18_hs_alloc_at() -> leaked pages NOT freed, module pinned,
	 * range stays HELD (-EBUSY on overlapping retain), non-overlapping retain still succeeds. */
	{
		struct file f;
		char hOk[32], rep[96];
		ssize_t r_fail, r_ov, r_ok;
		int live_after_fail;

		res_boot_live();
		probe_now();
		res_open(&f);
		sim_map_fail_iova = 0x30010000;	/* 2nd 16 KiB page of 32 KiB text surface at 0x3000c000 */
		sim_unmap_short_iova = 0x3000c000;
		r_fail = res_cmd(&f, "retain weighted.bin 30000000 3000c000 280 30020000 1\n", rep, sizeof(rep));
		sim_map_fail_iova = 0;
		sim_unmap_short_iova = 0;
		live_after_fail = res_live_dma();
		r_ov = res_cmd(&f, "retain weighted.bin 30000000 3000c000 280 30020000 1\n", rep, sizeof(rep));
		r_ok = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hOk, sizeof(hOk));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_fail == -ENOMEM && log_count("rollback unmapped") == 1 && sim_owner_module.refs >= 1 &&
		    live_after_fail >= 1 && res_live_dma() == 1 && r_ov == -EBUSY && r_ok == 17,
		    "RP9 short partial-map rollback keeps pages leaked, pins module, holds range (-EBUSY), other slots work");
	}

	/* RP10a: duplicate LIVE prog_id from LOAD_PROGRAM -> no DESTROY_PROGRAM (would kill A), A intact,
	 * slot HELD, module pinned, engine quarantined (-ESHUTDOWN). */
	{
		struct file f;
		char hA[32], rep[96], cmd[128];
		u32 progA;
		int prog_before, prog_after;
		ssize_t r_dup, r_callA, r_ret;
		struct sim_fw_prog *fpA;
		bool a_still_live;

		res_boot_live();
		probe_now();
		res_open(&f);
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hA, sizeof(hA));
		hA[16] = '\0';
		progA = sim_prog_ids[0];
		prog_before = sim_prog_n;
		sim_res_dup_prog = progA;
		r_dup = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", rep, sizeof(rep));
		sim_res_dup_prog = 0;
		prog_after = sim_prog_n;
		fpA = sim_fw_find(progA);
		a_still_live = fpA && fpA->live && fpA->has_proc;
		snprintf(cmd, sizeof(cmd), "call %s\n", hA);
		r_callA = res_cmd(&f, cmd, rep, sizeof(rep));
		r_ret = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", rep, sizeof(rep));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_dup == -EIO && prog_after == prog_before + 1 && sim_prog_ops[prog_before] == 0x200 &&
		    a_still_live && sim_owner_module.refs >= 1 && res_live_dma() > 0 &&
		    r_callA == -ESHUTDOWN && r_ret == -ESHUTDOWN,
		    "RP10a duplicate LIVE prog_id quarantines engine without DESTROY_PROGRAM, leaves A intact");
	}

	/* RP10b: malformed CREATE_PROCESS (proc == ~0u) -> no DESTROY_PROGRAM, slot HELD, module pinned,
	 * engine quarantined (-ESHUTDOWN). */
	{
		struct file f;
		char rep[96];
		ssize_t r_mal, r_ret;

		res_boot_live();
		probe_now();
		res_open(&f);
		sim_res_malformed_create = 1;
		r_mal = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", rep, sizeof(rep));
		sim_res_malformed_create = 0;
		r_ret = res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", rep, sizeof(rep));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_mal == -EIO && sim_prog_n == 2 && sim_prog_ops[0] == 0x200 && sim_prog_ops[1] == 0x202 &&
		    sim_owner_module.refs >= 1 && res_live_dma() > 0 && r_ret == -ESHUTDOWN,
		    "RP10b malformed CREATE_PROCESS (~0u) quarantines engine without guessing DESTROY_PROGRAM");
	}

	/* RP11: ANE_H18_IOC_CALL_IO on the same exclusive resident fd (512B input -> id5, 16 KiB output <- id4,
	 * tail zeroing, output zeroing, digest match, partial read, error/fault isolation, zero reload & zero leak). */
	{
		struct file f;
		char hA_str[32], hB_str[32], rep[96], cmd[128];
		u64 hA = 0, hB = 0, expect_digest;
		u8 in_big[1024], in_512[512], out_16k[0x4000], out_part[512 + 32];
		struct ane_h18_res_call_io req;
		size_t k;
		long rc;
		int tail_zero_ok, gold_ok, pad_zero_ok, part_ok;

		res_boot_live();
		probe_now();
		sim_res_io_echo = 1;
		res_open(&f);
		res_cmd(&f, "retain plain_io.bin 0 0 0 0 0\n", hA_str, sizeof(hA_str));
		hA_str[16] = '\0';
		sscanf(hA_str, "%llx", (unsigned long long *)&hA);
		res_cmd(&f, "retain plain_io.bin 0 0 0 0 0\n", hB_str, sizeof(hB_str));
		hB_str[16] = '\0';
		sscanf(hB_str, "%llx", (unsigned long long *)&hB);

		/* RP11a: First write 1024 non-zero bytes to id5 so bytes [512..1024) are dirty, then write 512 bytes
		 * and read full 16 KiB id4: verify input tail [512..16384) zeroed, output zeroed before call,
		 * 512 gold bytes + 15872 zero padding bytes returned, output_bytes==16384, latency_ns>0, exact FNV-1a digest. */
		for (k = 0; k < sizeof(in_big); k++)
			in_big[k] = (u8)(0xa5U ^ (k & 0xffU));
		memset(out_16k, 0xee, sizeof(out_16k));
		memset(&req, 0, sizeof(req));
		req.version = ANE_H18_RES_ABI_VERSION;
		req.handle = hA;
		req.in_surface_id = 5;
		req.out_surface_id = 4;
		req.in_len = sizeof(in_big);
		req.out_len = sizeof(out_16k);
		req.in_ptr = (u64)(uintptr_t)in_big;
		req.out_ptr = (u64)(uintptr_t)out_16k;
		rc = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&req);
		chk(rc == 0 && sim_res_io_out_was_zero && req.output_bytes == sizeof(out_16k) && req.latency_ns > 0 && sim_kv_live == 0,
		    "RP11a priming 1024B CALL_IO succeeds with zeroed output surface and zero bounce leak");

		for (k = 0; k < sizeof(in_512); k++)
			in_512[k] = (u8)((k * 7U + 3U) & 0xffU);
		memset(out_16k, 0xcc, sizeof(out_16k));
		req.in_len = sizeof(in_512);
		req.out_len = sizeof(out_16k);
		req.in_ptr = (u64)(uintptr_t)in_512;
		req.out_ptr = (u64)(uintptr_t)out_16k;
		req.latency_ns = 0;
		req.digest = 0;
		req.output_bytes = 0;
		rc = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&req);
		tail_zero_ok = (sim_res_io_last_in_sz == 0x4000 && !memcmp(sim_res_io_last_in, in_512, sizeof(in_512)));
		for (k = sizeof(in_512); k < sim_res_io_last_in_sz; k++)
			if (sim_res_io_last_in[k] != 0)
				tail_zero_ok = 0;
		gold_ok = 1;
		for (k = 0; k < sizeof(in_512); k++)
			if (out_16k[k] != (u8)(in_512[k] ^ 0x5aU))
				gold_ok = 0;
		pad_zero_ok = 1;
		for (k = sizeof(in_512); k < sizeof(out_16k); k++)
			if (out_16k[k] != 0)
				pad_zero_ok = 0;
		expect_digest = 0xcbf29ce484222325ULL;
		for (k = 0; k < sizeof(out_16k) / 4; k++) {
			u32 w;

			memcpy(&w, out_16k + k * 4, 4);
			expect_digest ^= w;
			expect_digest *= 0x100000001b3ULL;
		}
		chk(rc == 0 && sim_res_io_out_was_zero && tail_zero_ok && gold_ok && pad_zero_ok &&
		    req.output_bytes == sizeof(out_16k) && req.latency_ns > 0 && req.digest == expect_digest && sim_kv_live == 0,
		    "RP11b 512B id5 -> 16 KiB id4 CALL_IO zeroes input tail + output, returns 512B gold + zero padding + exact digest");

		/* RP11c: partial out_len (512B) copies exactly 512B without touching guard canary, digest still covers full id4. */
		memset(out_part, 0xdd, sizeof(out_part));
		req.handle = hB;
		req.in_len = sizeof(in_512);
		req.out_len = 512;
		req.in_ptr = (u64)(uintptr_t)in_512;
		req.out_ptr = (u64)(uintptr_t)out_part;
		rc = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&req);
		part_ok = (rc == 0 && req.output_bytes == 512 && req.digest == expect_digest && !memcmp(out_part, out_16k, 512));
		for (k = 512; k < sizeof(out_part); k++)
			if (out_part[k] != 0xdd)
				part_ok = 0;
		chk(part_ok && sim_prog_n == 7 && sim_prog_ops[4] == 0x204 && sim_prog_ops[5] == 0x204 && sim_prog_ops[6] == 0x204,
		    "RP11c partial out_len=512 copies exact 512B without overrun, switches handle B with zero reloads");

		/* RP11d: validation matrix (cmd, version, flags, reserved, zero lens, null ptrs, oversize, bad/swapped surface IDs, bad handle). */
		{
			struct ane_h18_res_call_io bad;
			int prog_before = sim_prog_n;
			long r_cmd, r_ver, r_flg, r_res, r_in0, r_out0, r_inp, r_outp;
			long r_inbig, r_outbig, r_noid, r_swapin, r_swapout, r_out_is_in, r_swapboth, r_badh;

			memset(&bad, 0, sizeof(bad));
			bad.version = ANE_H18_RES_ABI_VERSION;
			bad.handle = hA;
			bad.in_surface_id = 5;
			bad.out_surface_id = 4;
			bad.in_len = 512;
			bad.out_len = 512;
			bad.in_ptr = (u64)(uintptr_t)in_512;
			bad.out_ptr = (u64)(uintptr_t)out_part;

			r_cmd = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO + 1, (unsigned long)(uintptr_t)&bad);
			bad.version = 2; r_ver = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.version = 1;
			bad.flags = 1; r_flg = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.flags = 0;
			bad.reserved = 1; r_res = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.reserved = 0;
			bad.in_len = 0; r_in0 = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.in_len = 512;
			bad.out_len = 0; r_out0 = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.out_len = 512;
			bad.in_ptr = 0; r_inp = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.in_ptr = (u64)(uintptr_t)in_512;
			bad.out_ptr = 0; r_outp = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.out_ptr = (u64)(uintptr_t)out_part;
			bad.in_len = 0x4001; r_inbig = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.in_len = 512;
			bad.out_len = 0x4001; r_outbig = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.out_len = 512;
			bad.in_surface_id = 99; r_noid = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.in_surface_id = 5;
			bad.in_surface_id = 4; r_swapin = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.in_surface_id = 5;
			bad.out_surface_id = 5; r_swapout = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.out_surface_id = 4;
			bad.out_surface_id = 6; r_out_is_in = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.out_surface_id = 4;
			bad.in_surface_id = 4; bad.out_surface_id = 5; r_swapboth = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad); bad.in_surface_id = 5; bad.out_surface_id = 4;
			bad.handle = 0xdeadbeefULL; r_badh = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&bad);
			chk(r_cmd == -ENOTTY && r_ver == -EINVAL && r_flg == -EINVAL && r_res == -EINVAL &&
			    r_in0 == -EINVAL && r_out0 == -EINVAL && r_inp == -EINVAL && r_outp == -EINVAL &&
			    r_inbig == -EINVAL && r_outbig == -EINVAL && r_noid == -EINVAL && r_swapin == -EINVAL &&
			    r_swapout == -EINVAL && r_out_is_in == -EINVAL && r_swapboth == -EINVAL && r_badh == -ENOENT &&
			    sim_prog_n == prog_before && sim_kv_live == 0,
			    "RP11d CALL_IO rejects bad cmd (-ENOTTY), ABI/bounds/dir (-EINVAL), bad handle (-ENOENT) without issuing firmware call");
		}

		/* RP11e: user-copy faults (-EFAULT) and kvmalloc failure (-ENOMEM) leave DMA/firmware/caller struct clean. */
		{
			struct ane_h18_res_call_io ureq;
			int prog_before = sim_prog_n;
			long r_arg_fault, r_in_fault, r_out_fault, r_oom;

			memset(&ureq, 0, sizeof(ureq));
			ureq.version = ANE_H18_RES_ABI_VERSION;
			ureq.handle = hA;
			ureq.in_surface_id = 5;
			ureq.out_surface_id = 4;
			ureq.in_len = 512;
			ureq.out_len = 512;
			ureq.in_ptr = (u64)(uintptr_t)in_512;
			ureq.out_ptr = (u64)(uintptr_t)out_part;
			ureq.output_bytes = 0x1234;

			r_arg_fault = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, 0);
			sim_cfu_calls = 0; sim_cfu_fail_at = 2;	/* fail in_ptr copy after struct copy */
			r_in_fault = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&ureq);
			sim_cfu_fail_at = 0;
			sim_kvmalloc_calls = 0; sim_kvmalloc_fail_at = 2; /* fail out_buf alloc after in_buf alloc */
			r_oom = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&ureq);
			sim_kvmalloc_fail_at = 0;
			chk(r_arg_fault == -EFAULT && r_in_fault == -EFAULT && r_oom == -ENOMEM &&
			    sim_prog_n == prog_before && ureq.output_bytes == 0x1234 && sim_kv_live == 0,
			    "RP11e arg/in_ptr -EFAULT and -ENOMEM abort before PROCEDURE_CALL with zero bounce leak");

			sim_ctu_calls = 0; sim_ctu_fail_at = 1; /* fail out_ptr copy after matched finish */
			r_out_fault = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&ureq);
			sim_ctu_fail_at = 0;
			chk(r_out_fault == -EFAULT && ureq.output_bytes == 0x1234 && sim_kv_live == 0,
			    "RP11f out_ptr copy_to_user -EFAULT leaves caller struct untouched with zero bounce leak");
		}

		/* RP11g: firmware refusal (-EIO) does NOT copy output or mutate caller struct; handle remains usable, then free clean. */
		{
			struct ane_h18_res_call_io ereq;
			u8 sentinel[512];
			int sent_ok = 1;
			long r_fwerr;

			memset(sentinel, 0x77, sizeof(sentinel));
			memset(&ereq, 0, sizeof(ereq));
			ereq.version = ANE_H18_RES_ABI_VERSION;
			ereq.handle = hA;
			ereq.in_surface_id = 5;
			ereq.out_surface_id = 4;
			ereq.in_len = 512;
			ereq.out_len = sizeof(sentinel);
			ereq.in_ptr = (u64)(uintptr_t)in_512;
			ereq.out_ptr = (u64)(uintptr_t)sentinel;
			ereq.output_bytes = 0x5555;
			ereq.latency_ns = 0x9999;
			sim_res_fail_op = 0x204;
			sim_res_fail_ack = 3;
			r_fwerr = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&ereq);
			sim_res_fail_op = 0;
			for (k = 0; k < sizeof(sentinel); k++)
				if (sentinel[k] != 0x77)
					sent_ok = 0;
			chk(r_fwerr == -EIO && sent_ok && ereq.output_bytes == 0x5555 && ereq.latency_ns == 0x9999 && sim_kv_live == 0,
			    "RP11g firmware refusal (-EIO) never copies output or mutates caller struct");
		}

		snprintf(cmd, sizeof(cmd), "free %s\n", hA_str); res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hB_str); res_cmd(&f, cmd, rep, sizeof(rep));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(res_live_dma() == 0 && sim_owner_module.refs == 0 && sim_kv_live == 0,
		    "RP11h same-fd retain + CALL_IO + free closes and removes cleanly with zero DMA/bounce leak");
	}

	/* RP11i: spoofed/unmatched finish on CALL_IO -> -EIO, caller out_ptr & struct untouched, slot HELD, engine quarantined. */
	{
		struct file f;
		char hA_str[32], hB_str[32];
		u64 hA = 0;
		u32 progB;
		u8 in_512[512], sentinel[512];
		struct ane_h18_res_call_io sreq;
		size_t k;
		long r_spoof, r_after;
		int sent_ok = 1;

		res_boot_live();
		probe_now();
		sim_res_io_echo = 1;
		res_open(&f);
		res_cmd(&f, "retain plain_io.bin 0 0 0 0 0\n", hA_str, sizeof(hA_str));
		hA_str[16] = '\0';
		sscanf(hA_str, "%llx", (unsigned long long *)&hA);
		res_cmd(&f, "retain plain_io.bin 0 0 0 0 0\n", hB_str, sizeof(hB_str));
		progB = sim_prog_ids[2];
		memset(in_512, 0x33, sizeof(in_512));
		memset(sentinel, 0x66, sizeof(sentinel));
		memset(&sreq, 0, sizeof(sreq));
		sreq.version = ANE_H18_RES_ABI_VERSION;
		sreq.handle = hA;
		sreq.in_surface_id = 5;
		sreq.out_surface_id = 4;
		sreq.in_len = sizeof(in_512);
		sreq.out_len = sizeof(sentinel);
		sreq.in_ptr = (u64)(uintptr_t)in_512;
		sreq.out_ptr = (u64)(uintptr_t)sentinel;
		sreq.output_bytes = 0x4321;
		sim_res_spoof_prog = progB;
		sim_res_spoof_proc = sim_fw_find(progB)->proc_id;
		r_spoof = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&sreq);
		sim_res_spoof_prog = sim_res_spoof_proc = 0;
		for (k = 0; k < sizeof(sentinel); k++)
			if (sentinel[k] != 0x66)
				sent_ok = 0;
		r_after = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&sreq);
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_spoof == -EIO && sent_ok && sreq.output_bytes == 0x4321 && r_after == -ESHUTDOWN &&
		    sim_owner_module.refs >= 1 && res_live_dma() > 0 && sim_kv_live == 0,
		    "RP11i unmatched finish on CALL_IO returns -EIO without copying output, quarantines engine (-ESHUTDOWN)");
	}

	/* RP11j: CALL_IO refuses embedded-input control programs (doff != 0 or dsize != 0) with -EINVAL
	 * before touching DMA or issuing PROCEDURE_CALL, so legacy ASCII call still sees embedded input intact. */
	{
		struct file f;
		char hCtrl_str[32], hDoff_str[32], rep[96], cmd[128];
		u64 hCtrl = 0, hDoff = 0;
		u8 overwrite[512], out_buf[512];
		struct ane_h18_res_call_io creq;
		int prog_before;
		long r_embed, r_doff;
		ssize_t r_legacy;

		res_boot_live();
		probe_now();
		res_open(&f);
		res_cmd(&f, "retain sim.bin 0 0 0 0 0\n", hCtrl_str, sizeof(hCtrl_str));
		hCtrl_str[16] = '\0';
		sscanf(hCtrl_str, "%llx", (unsigned long long *)&hCtrl);
		res_cmd(&f, "retain bad_doff.bin 0 0 0 0 0\n", hDoff_str, sizeof(hDoff_str));
		hDoff_str[16] = '\0';
		sscanf(hDoff_str, "%llx", (unsigned long long *)&hDoff);

		memset(overwrite, 0x33, sizeof(overwrite));
		memset(out_buf, 0x88, sizeof(out_buf));
		memset(&creq, 0, sizeof(creq));
		creq.version = ANE_H18_RES_ABI_VERSION;
		creq.handle = hCtrl;
		creq.in_surface_id = 5;
		creq.out_surface_id = 4;
		creq.in_len = sizeof(overwrite);
		creq.out_len = sizeof(out_buf);
		creq.in_ptr = (u64)(uintptr_t)overwrite;
		creq.out_ptr = (u64)(uintptr_t)out_buf;

		prog_before = sim_prog_n;
		r_embed = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&creq);
		creq.handle = hDoff;
		r_doff = sim_res_entry.ops->unlocked_ioctl(&f, ANE_H18_IOC_CALL_IO, (unsigned long)(uintptr_t)&creq);
		/* Legacy ASCII call on hCtrl checks sim_buf_va(id5)[0] == 0xaa in fake FW; must still succeed. */
		snprintf(cmd, sizeof(cmd), "call %s\n", hCtrl_str);
		r_legacy = res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hCtrl_str); res_cmd(&f, cmd, rep, sizeof(rep));
		snprintf(cmd, sizeof(cmd), "free %s\n", hDoff_str); res_cmd(&f, cmd, rep, sizeof(rep));
		res_close(&f);
		ane_h18_remove(&pdev);
		chk(r_embed == -EINVAL && r_doff == -EINVAL && sim_prog_n == prog_before + 5 &&
		    r_legacy > 0 && res_live_dma() == 0 && sim_kv_live == 0,
		    "RP11j CALL_IO refuses embedded/nonzero-doff input (-EINVAL) with no call/overwrite; legacy call preserves 0xaa input");
	}
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "pos";

	if (!strcmp(mode, "pos")) {
		printf("sim-h18 positive controls:\n");
		pos_dt();
		pos_psd();
		pos_refuse();
		pos_power();
		pos_boot();
		pos_prog_parse();
		pos_residency();
		printf("%s\n", fails ? "SIM-TEST: FAILED" : "SIM-TEST: all positive controls PASS");
	} else if (!strcmp(mode, "neg-psd-never")) {
		fails = neg_never();
	} else if (!strcmp(mode, "neg-psd-wrong")) {
		fails = neg_wrong();
	} else if (!strcmp(mode, "neg-dt")) {
		fails = neg_dt();
	} else if (!strcmp(mode, "neg-boot")) {
		fails = neg_boot();
	} else if (!strcmp(mode, "neg-args")) {
		fails = neg_args();
	} else if (!strcmp(mode, "pos-reads-single")) {
		fails = pos_reads_single();
	} else if (!strcmp(mode, "pos-reads")) {
		fails = pos_reads();
	} else if (!strcmp(mode, "neg-reads")) {
		fails = neg_reads();
	} else {
		fprintf(stderr, "usage: sim_h18 [pos|neg-psd-never|neg-psd-wrong|neg-dt|neg-boot|neg-args|pos-reads-single|pos-reads|neg-reads]\n");
		return 2;
	}
	return fails ? 1 : 0;
}
