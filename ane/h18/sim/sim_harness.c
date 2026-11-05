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
struct sim_iocall sim_iocalls[64];
int sim_n_iocalls, sim_io_fail, sim_n_dom_live;
int sim_start_chan = -1, sim_start_msgs, sim_start_hello;
u32 sim_po_out_off, sim_alias_credits = 1, sim_hello_min = 11, sim_hello_max = 12, sim_hello_ep, sim_hello_type = 1;

#include "../ane_h18_soc.c"
#include "../ane_h18_main.c"



/* ---- hs_prog: the program file the harness hands to request_firmware, and a firmware that answers the program commands ---- */
const u8 *sim_fw_data;
size_t sim_fw_size;
int sim_fw_err, sim_fw_released, sim_fw_requests;
static u8 sim_blob[0x8000];
static size_t sim_blob_len;
int sim_prog_mode, sim_prog_n, sim_prog_loaded, sim_prog_created, sim_t2h_back, sim_out_written, sim_t2h_posted;
u32 sim_prog_ops[16];
int sim_pub_wait, sim_premapped;

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

static int sim_buf_ok(u64 iova, u64 size)
{
	int d;

	for (d = 0; d < sim_n_dma; d++)
		if (sim_dma[d].live && iova >= sim_dma[d].iova && size <= sim_dma[d].size && iova - sim_dma[d].iova <= sim_dma[d].size - size)
			return 1;
	return 0;
}

static u8 *sim_buf_va(u64 iova)
{
	int d;

	for (d = 0; d < sim_n_dma; d++)
		if (sim_dma[d].live && iova >= sim_dma[d].iova && iova < sim_dma[d].iova + sim_dma[d].size)
			return (u8 *)sim_dma[d].va + (iova - sim_dma[d].iova);
	return NULL;
}

/* The firmware's side of LOAD_PROGRAM, CREATE_PROCESS, PROCEDURE_CALL, TERMINATE_PROCESS and DESTROY_PROGRAM. Returns the acknowledgement for +6: 0 ok, 3 refused by
 * the mode, 4 an id it does not know, 5 a program with a live process, 7 an address that is not in a buffer the host allocated. A call posts two IO_T2H events. */
static u16 sim_prog_cmd(u16 op, u8 *blk)
{
	u32 prog = 0, proc = 0, v;
	u64 cookie, a, s;
	int i, n;

	if (sim_prog_n < 16)
		sim_prog_ops[sim_prog_n] = op;
	sim_prog_n++;
	memcpy(&prog, blk + 8, 4);
	memcpy(&proc, blk + 0xc, 4);
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
	case 0x200:
		if (sim_prog_mode == 1)
			return 3;
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
			n++;
		}
		if (!n)
			return 7;
		if (sim_prog_mode != 4) {
			v = 0x5a;
			memcpy(blk + 0x1b8, &v, 4);
		}
		sim_prog_loaded = 1;
		return 0;
	case 0x202:
		if (sim_prog_mode == 2)
			return 3;
		if (sim_prog_mode == 6)
			return 0xffff;
		if (!sim_prog_loaded || prog != 0x5a)
			return 4;
		v = 0x77;
		memcpy(blk + 0xc, &v, 4);
		sim_prog_created = 1;
		return 0;
	case 0x204: {
		u32 cnt, prio;
		u64 *ring = (u64 *)((u8 *)sim_dma[1].va + 0xb000);
		u8 *pool = sim_dma[0].va;

		if (!sim_prog_created || prog != 0x5a || proc != 0x77)
			return 4;
		memcpy(&prio, blk + 0x18, 4);
		memcpy(&cookie, blk + 0x20, 8);
		memcpy(&cnt, blk + 0x28, 4);
		if (prio != 2 || cookie != 0xADD0 || !cnt || cnt > 8)
			return 7;
		for (i = 0; i < (int)cnt; i++) {
			const u8 *r = blk + 0x60 + i * 0x30;
			u32 flags, type;
			u8 *o;

			memcpy(&flags, r, 4);
			memcpy(&type, r + 8, 4);
			memcpy(&a, r + 0x18, 8);
			memcpy(&s, r + 0x20, 8);
			if (flags != 1 || !sim_buf_ok(a, s))
				return 7;
			{
				u32 bid;

				memcpy(&bid, r + 4, 4);
				if ((bid == 5 && sim_buf_va(a)[0] != 0xaa) || (bid == 6 && sim_buf_va(a)[0] != 0xbb) || (bid != 4 && bid != 5 && bid != 6))	/* the input data of the file is in the surface */
					return 7;
			}
			if (type == 1 && (o = sim_buf_va(a))) {
				v = 0x11223344;
				memcpy(o, &v, 4);
				memcpy(o + 4, &v, 4);
				sim_out_written = 1;
			}
		}
		if (sim_prog_mode == 3)
			return 0;
		for (i = 0; i < 2; i++) {
			u8 *ev = pool + 0x1000 + i * 0x40;
			u32 seq = i + 1, k = 0x300, st = i;

			memset(ev, 0, 0x28);
			memcpy(ev, &seq, 4);
			memcpy(ev + 4, &k, 4);
			memcpy(ev + 8, &cookie, 8);
			memcpy(ev + 0x10, &prog, 4);
			memcpy(ev + 0x14, &proc, 4);
			memcpy(ev + 0x1c, &st, 4);
			ring[i * 8 + 1] = 0x28;
			if (sim_prog_mode == 5) {	/* the length first, the address (the publication) three sleeps later */
				sim_pub_wait = 3;
				continue;
			}
			ring[i * 8] = sim_dma[0].iova + 0x1000 + i * 0x40;
			sim_t2h_posted++;
		}
		return 0;
	}
	case 0x203:
		if (!sim_prog_created || prog != 0x5a || proc != 0x77)
			return 4;
		sim_prog_created = 0;
		return 0;
	case 0x201:
		if (!sim_prog_loaded || prog != 0x5a)
			return 4;
		if (sim_prog_created)
			return 5;
		sim_prog_loaded = 0;
		return 0;
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
	f = 0;
	sim_hs_block_ok = F8(0x00) == ane_t8152_soc.fw_seg[0].iova && F8(0x08) == sim_dma[1].iova && F8(0x10) == hs_cfg_size && F8(0x18) == 0x10000000ULL - hs_cfg_size &&
			  F8(0x20) == (sim_n_dma > 2 ? sim_dma[2].iova : 0) && F8(0x28) == 0xf00000 && F8(0x50) == 0xc441 && F8(0x58) == pub && F8(0x60) == 0x40000 && F4(0x68) == 0x40 && F4(0x12c) == 4 &&
			  sim_dma[0].size == 0x40000 && sim_dma[1].size == 0x10000 && sim_dma[2].size == 0xf00000;
	if (sim_hs_mode == 2)
		sim_scr[7] = ANE_H18_HS_READY;	/* READY again, but no table address: the block address stays in SCRATCH0 and SCRATCH1 */
	if (sim_hs_mode == 0 && sim_hs_table) {
		/* a table like the board's: eight 0x100 byte entries at the start of the IPC surface, the M6 ring offsets */
		static const struct { const char *n; u32 type, bit; u64 slots, off; } ch[8] = {
			{ "TERMINAL", 2, 0, 0x200, 0x800 }, { "IO", 0, 1, 0x10, 0x8800 }, { "DEBUG", 0, 2, 8, 0x8c00 }, { "BUF_H2T", 0, 3, 0x40, 0x8e00 },
			{ "BUF_T2H", 1, 4, 0x40, 0x9e00 }, { "SHAREDMALLOC", 1, 5, 8, 0xae00 }, { "IO_T2H", 1, 6, 0x40, 0xb000 }, { "DATA_CHAIN_H2T", 0, 7, 0x10, 0xc000 },
		};
		u8 *t = sim_dma[1].va;
		int c;

		memset(t, 0, 8 * 0x100);
		for (c = 0; c < 8; c++) {
			u64 ring = sim_dma[1].iova + ch[c].off + (sim_table_bad == c + 1 ? 0x1000000 : 0);

			if (c == 2 && sim_table_bad == 100)
				ring = sim_dma[1].iova + 0x80;	/* inside the table entries */
			if (c == 2 && sim_table_bad == 102)
				ring = 0xffffffffffffff00ULL;	/* ring + slots * 0x40 wraps past 2^64 */
			if (c == 2 && sim_table_bad == 101)
				ring = sim_dma[1].iova + 0x8800;	/* the IO ring */

			strcpy((char *)t + c * 0x100, ch[c].n);
			memcpy(t + c * 0x100 + 0x40, &ch[c].type, 4);
			memcpy(t + c * 0x100 + 0x44, &ch[c].bit, 4);
			memcpy(t + c * 0x100 + 0x48, &ch[c].slots, 8);
			memcpy(t + c * 0x100 + 0x50, &ring, 8);
		}
		if (sim_chan_req) {
			u64 *sm = (u64 *)((u8 *)sim_dma[1].va + 0xae00);	/* the SHAREDMALLOC ring: a request nobody answered */

			memcpy((u8 *)sim_dma[1].va + 0x800, "ANE fw says hi", 14);	/* the firmware console */
			if (sim_chan_req == 1) {
				memcpy((u8 *)sim_dma[1].va + 0xf000, "fw text\n\x01", 9);	/* a console record that points to text: slot 2, flag 2, length 9, timestamp 0x77 */
				((u64 *)((u8 *)sim_dma[1].va + 0x800))[2 * 8] = (sim_dma[1].iova + 0xf000) | 2;
				((u64 *)((u8 *)sim_dma[1].va + 0x800))[2 * 8 + 1] = 9;
				((u64 *)((u8 *)sim_dma[1].va + 0x800))[2 * 8 + 3] = 0x77;
				memset((u8 *)sim_dma[1].va + 0xf100, 'a', 0x101);	/* the limit: slot 3 has length 0x100 (printed, ends in 'yz'), slot 4 has 0x101 (counted, not printed) */
				((u8 *)sim_dma[1].va)[0xf100 + 0xfe] = 'y';
				((u8 *)sim_dma[1].va)[0xf100 + 0xff] = 'z';
				((u8 *)sim_dma[1].va)[0xf100 + 0x100] = 'w';
				for (int q = 3; q <= 4; q++) {
					((u64 *)((u8 *)sim_dma[1].va + 0x800))[q * 8] = (sim_dma[1].iova + 0xf100) | 2;
					((u64 *)((u8 *)sim_dma[1].va + 0x800))[q * 8 + 1] = q == 3 ? 0x100 : 0x101;
					((u64 *)((u8 *)sim_dma[1].va + 0x800))[q * 8 + 3] = 0x75 + q;
				}
			}
			sm[0] = 0x1000;	/* a header that is an iova, with a size: no request has this shape */
			sm[1] = 0x40;
			sm[2] = 0;
			sim_pend_val = 0x20;
		}
		sim_scr[0] = (u32)sim_dma[1].iova;
		sim_scr[1] = (u32)(sim_dma[1].iova >> 32);
		sim_scr[7] = ANE_H18_HS_READY;
	} else if (sim_hs_mode == 0) {
		u32 mark[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };

		memcpy((u8 *)sim_dma[1].va + 0x100, mark, sizeof(mark));
		sim_scr[0] = (u32)(sim_dma[1].iova + 0x100);
		sim_scr[1] = (u32)((sim_dma[1].iova + 0x100) >> 32);
		sim_scr[7] = ANE_H18_HS_READY;
	}
}


/* A firmware console that prints on every poll (chan_req 4): the terminal ring changes at each sleep once the table exists. */
void sim_usleep_hook(void)
{
	if (sim_pub_wait && !--sim_pub_wait && sim_n_dma > 1) {	/* publish the two IO_T2H events of a call in mode 5 */
		u64 *ring = (u64 *)((u8 *)sim_dma[1].va + 0xb000);
		int q;

		for (q = 0; q < 2; q++) {
			ring[q * 8] = sim_dma[0].iova + 0x1000 + q * 0x40;
			sim_t2h_posted++;
		}
	}
	if (sim_chan_req == 4 && sim_n_dma > 1 && sim_scr_model && sim_scr[3] == 0 && sim_hs_table) {
		u32 *t = (u32 *)((u8 *)sim_dma[1].va + 0x800);

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
		u64 *sm = (u64 *)((u8 *)sim_dma[1].va + 0xae00);

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
		u64 *sm = (u64 *)((u8 *)sim_dma[1].va + 0xae00);

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
		u64 *r6 = (u64 *)((u8 *)sim_dma[1].va + 0xb000);
		int q;

		for (q = 0; q < 2; q++)
			if (r6[q * 8] & 1)
				sim_t2h_back = q + 1;
		return;
	}
	if (v != BIT(1) || sim_n_dma < 3)
		return;
	if (sim_chan_req == 2 && !(((u64 *)((u8 *)sim_dma[1].va + 0xae00))[0] & 1))
		return;	/* the firmware still waits for its memory */
	ring = (u64 *)((u8 *)sim_dma[1].va + 0x8800);
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
			u16 ack = sim_prog_cmd(op, blk);

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
			u64 *sm = (u64 *)((u8 *)sim_dma[1].va + 0xae00);

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
	char want[160];

	snprintf(want, sizeof(want), "ane_h18 RESULT stage=%s soc=t8152 verdict=%s reason=%s", stg, verdict, reason);
	return strstr(sim_log, want) != NULL;
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
	premap_second = 0;
	sim_premapped = sim_pub_wait = sim_prog_mode = sim_prog_n = sim_prog_loaded = sim_prog_created = sim_t2h_back = sim_out_written = sim_t2h_posted = 0;
	sim_fw_err = sim_fw_released = sim_fw_requests = 0;
	make_blob();
	sim_db_writes = sim_hs_table = sim_s3_mode = sim_cmd_mode = sim_cmds_seen = sim_table_bad = sim_chan_req = 0;
	sim_pend_val = 0;
	sim_ack_writes = 0;
	sim_cmd_blk_in_pool = sim_posted = sim_term_tick = 0;
	sim_ack_last = 0;
	memset(sim_dma, 0, sizeof(sim_dma));
	map_fw = false;
	sim_rpm_enabled = sim_rpm_gets = sim_rpm_puts = sim_rpm_get_fail = 0;
	sim_n_iocalls = sim_io_fail = sim_n_dom_live = 0;
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
	memset(sim_po_msg, 0, sizeof(sim_po_msg));
	memset(sim_po_n, 0, sizeof(sim_po_n));
	memset(sim_po_head, 0, sizeof(sim_po_head));
	sim_start_chan = -1;
	sim_start_msgs = 0;
	psd_timeout_ms = 1000;
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

		for (pass = 0; pass < 10; pass++) {
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
			ret = probe_now();
			nsc = events_in(SIM_SCRATCH_PA, SIM_SCRATCH_SIZE, 1);
			for (live = 0, k = 0; k < sim_n_dma; k++)
				live += sim_dma[k].live;
			nst = log_count("ane_h18 rtkit shutdown: ap_acked=1 iop_acked=1");
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

		for (pass = 0; pass < 39; pass++) {
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
					chk(ret == -EIO && sim_prog_n == 3 && sim_prog_ops[2] == 0x204 && log_count("gave no finish event") == 1 && live == 0 && sim_t2h_posted == 0,
					    "PG9 a call without a finish event: FAIL, nothing is unloaded (the firmware may be hung), the stop still runs");
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
					chk(ret == -EIO && sim_prog_n == 2 && sim_prog_ops[0] == 0x200 && sim_prog_ops[1] == 0x202 && sim_prog_loaded && live == 0 && log_count("rtkit shutdown:") == 1,
					    "PG17 CREATE_PROCESS not answered: FAIL, no DESTROY_PROGRAM and nothing else sent to the firmware, the stop still runs and the surfaces are freed afterwards");
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

		for (c = 0; c < 4; c++) {
			const char *what = "";

			boot_setup();
			switch (c) {
			case 0: sim_rvbar_val = 0x10000000000ULL; what = "IORVBAR without the valid bit"; break;
			case 1: sim_rvbar_val = 0x20000000001ULL; what = "IORVBAR at another address"; break;
			case 2: sim_cpu_status = 0x9; what = "the core already reports RUNNING"; break;
			case 3: sim_cpu_ctrl = 0x10; what = "CPU_CONTROL.RUN already set"; break;
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
	} else {
		fprintf(stderr, "usage: sim_h18 [pos|neg-psd-never|neg-psd-wrong|neg-dt|neg-boot|neg-args]\n");
		return 2;
	}
	return fails ? 1 : 0;
}
