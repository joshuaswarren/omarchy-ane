// SPDX-License-Identifier: GPL-2.0
/* ane/h16/sim/sim_harness.c -- userspace sim for the REAL ane_h16 module.
 *
 * Compiles ane_h16_main.c + ane_h16_soc.c unmodified against shim.h, with
 * a firmware model (SCRATCH7 wake word 0x08042006 after N polls, optional
 * RTKit HELLO), a fake boot ADT, and a fake DART. One scenario per run
 * (argv[1]); exit 0 = the scenario's assertions held.
 *
 * A green run proves the harness logic only: stage dispatch, the ps
 * guard, the pin/diff gates, the boot state machine and the RESULT
 * grammar. It is NOT silicon: no claim about a real M4 comes out of
 * this directory.
 *
 * Positive scenarios:  pos-dt pos-dt-noadt pos-status pos-boot
 *                      pos-boot-hello pos-h17-boot pos-h17-irq
 *                      pos-h17-scratch
 * Negative scenarios (module MUST refuse/fail, harness MUST observe it):
 *                      neg-ps-stuck neg-mbox-silent neg-bad-pin
 *                      neg-foreign-preload neg-unknown-stage

 */
#include <inttypes.h>

#include "shim.h"

/* the module's real source */
/* The run-time overlay has its own sim (sim_overlay.c); here the module only needs the two entry points to exist. */
int ane_h16_register(struct platform_driver *drv, const char *optin, bool apply_overlay)
{
	(void)drv;
	(void)optin;
	(void)apply_overlay;
	return 0;
}

void ane_h16_unregister(struct platform_driver *drv)
{
	(void)drv;
}

#include "../ane_h16_main.c"
#include "../ane_h16_soc.c"

/* The real H17 row: the boot scenarios exercise it end to end. */
#include "../../h17/ane_h17_soc.c"

/* ---- shim globals ---- */
struct sim_region sim_regions[8];
int sim_n_regions;
struct sim_event sim_events[SIM_MAX_EVENTS];
int sim_n_events;
bool sim_events_overflow;
int sim_ioremaps;
void (*sim_fw_read_hook)(u64 pa);
void (*sim_fw_write_hook)(u64 pa, u32 val);
unsigned long sim_now_ms;
char *sim_log[SIM_LOG_MAX];
int sim_n_log;
struct device_node *sim_dt_nodes[8];
int sim_n_dt_nodes;
const void *sim_match_data;
struct sim_phys sim_phys_maps[8];
int sim_n_phys_maps;
int sim_rpm_gets, sim_rpm_puts;
int sim_module_refs;
bool sim_pwgate_mapped = true;
int sim_snapshots, sim_snapshot_attempts, sim_snapshot_fail;
struct sim_ram_range sim_sys_ram[4] = { { 0x710000000ull, 0x8000000000ull } };
int sim_n_sys_ram = 1;
int sim_iounmaps;
int sim_pd_count, sim_pd_attached, sim_pd_detached, sim_pd_links, sim_pd_unlinked;
int sim_pd_fail_at = -1, sim_pd_link_fail_at = -1;
int sim_rpm_get_rc;
struct device sim_pd_devs[8];
struct device_link sim_pd_link_objs[8];
int sim_dma_outstanding;
int sim_dma_allocs, sim_dma_frees;
int sim_dma_fail_at, sim_dma_bad_at;
int sim_dma_wmbs, sim_dma_rmbs;
u64 sim_resv_length;
struct iommu_domain sim_dart_storage;
struct iommu_domain *sim_dart;
const struct firmware *sim_fw_file;
const char *sim_fw_name;

void sim_log_line(const char *tag, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("%s %s\n", tag, buf);
	if (sim_n_log < SIM_LOG_MAX)
		sim_log[sim_n_log++] = strdup(buf);
}

static int sim_log_count(const char *sub)
{
	int i, n = 0;

	for (i = 0; i < sim_n_log; i++)
		if (strstr(sim_log[i], sub))
			n++;
	return n;
}

static int sim_active_maps(void)
{
	int i, n = 0;
	for (i = 0; i < sim_dart_storage.n; i++)
		if (sim_dart_storage.map[i].len)
			n++;
	return n;
}

/* ---- sha256 (self-tested against FIPS 180-2 "abc" before any pin) ---- */
static const u32 sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
	0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
	0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
	0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
	0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
	0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define SHR(x, n) ((x) >> (n))
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))

static void sha256_block(u32 h[8], const u8 blk[64])
{
	u32 w[64], a, b, c, d, e, f, g, hh;
	size_t j;

	for (j = 0; j < 16; j++)
		w[j] = ((u32)blk[j * 4] << 24) |
		       ((u32)blk[j * 4 + 1] << 16) |
		       ((u32)blk[j * 4 + 2] << 8) | (u32)blk[j * 4 + 3];
	for (j = 16; j < 64; j++)
		w[j] = SSIG1(w[j - 2]) + w[j - 7] + SSIG0(w[j - 15]) +
		       w[j - 16];
	a = h[0]; b = h[1]; c = h[2]; d = h[3];
	e = h[4]; f = h[5]; g = h[6]; hh = h[7];
	for (j = 0; j < 64; j++) {
		u32 t1 = hh + BSIG1(e) + CH(e, f, g) + sha_k[j] + w[j];
		u32 t2 = BSIG0(a) + MAJ(a, b, c);

		hh = g; g = f; f = e; e = d + t1;
		d = c; c = b; b = a; a = t1 + t2;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256(const u8 *data, size_t len, u8 *out)
{
	u32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	u64 bitlen = (u64)len * 8;
	u8 tail[128] = {};
	size_t tail_len = len % 64, i, blocks, t;
	size_t full = len - tail_len;

	for (i = 0; i < full; i += 64)
		sha256_block(h, data + i);
	memcpy(tail, data + full, tail_len);
	tail[tail_len] = 0x80;
	blocks = (tail_len + 9 > 64) ? 2 : 1;
	tail[blocks * 64 - 1] = (u8)bitlen;
	tail[blocks * 64 - 2] = (u8)(bitlen >> 8);
	tail[blocks * 64 - 3] = (u8)(bitlen >> 16);
	tail[blocks * 64 - 4] = (u8)(bitlen >> 24);
	tail[blocks * 64 - 5] = (u8)(bitlen >> 32);
	tail[blocks * 64 - 6] = (u8)(bitlen >> 40);
	tail[blocks * 64 - 7] = (u8)(bitlen >> 48);
	tail[blocks * 64 - 8] = (u8)(bitlen >> 56);
	for (t = 0; t < blocks; t++)
		sha256_block(h, tail + t * 64);
	for (i = 0; i < 8; i++) {
		out[i * 4] = (u8)(h[i] >> 24);
		out[i * 4 + 1] = (u8)(h[i] >> 16);
		out[i * 4 + 2] = (u8)(h[i] >> 8);
		out[i * 4 + 3] = (u8)h[i];
	}
}

/* ---- fixture constants (the leto row's layout, from ane_h16_soc.c;
 * IOVA values reuse the only hardware-proven ANE map, the T6021
 * TEXT/DATA remap, purely as plausible fixture addresses) ----
 */
#define TEXT_IOVA 0x10000848000ull
#define DATA_IOVA 0x10001400000ull
#define ADT_PA 0x600000000ull
#define TEXT_PRE_PA 0x700000000ull
#define DATA_PRE_PA 0x701000000ull

static struct ane_h16_fw sim_fw;	/* mutable copy of the leto row */
static struct ane_h16_soc sim_soc;	/* mutable copy of the t8132 row */
static u8 *fwbuf;
static u8 *sim_text_pre;
static size_t fwbuf_len;
static struct platform_device sim_pdev;
static struct device_node n_ane, n_rm, n_adt;

/* ---- firmware model ---- */
static struct {
	bool armed;		/* CPU_CONTROL saw 0x10 */
	bool scratch7_cleared;
	bool wake_delivered;
	bool hello_posted, hello_consumed;
	bool hello_reply_seen;
	u32 reply_version;
	u64 hello_msg;
	int polls, wake_after;	/* deliver the wake word on poll N */
	bool want_hello;
	bool table_enabled, table_pending, descriptor_valid;
	int table_mode; /* 1=silent, 2=one-byte overrun, 3=foreign, 4=padding */
	u64 table_address;
} fw;

static u64 reg_pa(u32 off)
{
	return SIM_ENGINE_PA + off;
}

static u32 reg_rd(u32 off)
{
	int i;

	for (i = 0; i < sim_n_regions; i++)
		if (sim_regions[i].pa == SIM_ENGINE_PA)
			return sim_regions[i].backing[off / 4];
	return 0;
}

static void reg_wr(u32 off, u32 v)
{
	int i;

	for (i = 0; i < sim_n_regions; i++)
		if (sim_regions[i].pa == SIM_ENGINE_PA)
			sim_regions[i].backing[off / 4] = v;
}

static void fw_post_hello(void)
{
	u32 ctrl = reg_rd(sim_soc.mbox + 0x114);

	fw.hello_msg = FIELD_PREP(ANE_H16_MGMT_TYPE, ANE_H16_MGMT_HELLO) |
		       FIELD_PREP(ANE_H16_MGMT_HELLO_MINVER, 1) |
		       FIELD_PREP(ANE_H16_MGMT_HELLO_MAXVER, 3);
	reg_wr(sim_soc.mbox + 0x830, (u32)fw.hello_msg);
	reg_wr(sim_soc.mbox + 0x834, (u32)(fw.hello_msg >> 32));
	reg_wr(sim_soc.mbox + 0x838, ANE_H16_EP_MGMT);
	reg_wr(sim_soc.mbox + 0x114, ctrl & ~((u32)ANE_H16_MBOX_CTRL_EMPTY));
	fw.hello_posted = true;
}

static void fw_on_a2i_send(u8 ep, u64 msg)
{
	u8 type = FIELD_GET(ANE_H16_MGMT_TYPE, msg);

	if (ep == ANE_H16_EP_MGMT && type == ANE_H16_MGMT_HELLO_REPLY) {
		fw.hello_reply_seen = true;
		fw.reply_version = FIELD_GET(ANE_H16_MGMT_HELLO_MINVER, msg);
	}
}

static void fw_check_descriptor(void)
{
	struct ane_h16 *ane = platform_get_drvdata(&sim_pdev);
	u8 expected[0x180] = {};
	u64 first[6] = {
		ANE_H17_DMA_BASE, ane->chanbuf[1].dma, 0x380000, 0xfc80000,
		ane->chanbuf[2].dma, 0xf00000,
	};
	u64 last[3] = { 0xc441, ane->chanbuf[0].dma, 0x40000 };
	u32 marker = 0x40, flags = 4;
	u64 published = ((u64)reg_rd(sim_soc.scratch0 + 4) << 32) |
		reg_rd(sim_soc.scratch0);

	/* Independent store-list oracle, not the module's encoder helper. */
	memcpy(expected, first, sizeof(first));
	memcpy(expected + 0x50, last, sizeof(last));
	memcpy(expected + 0x68, &marker, sizeof(marker));
	memcpy(expected + 0x12c, &flags, sizeof(flags));
	fw.descriptor_valid = ane->chanbuf[0].cpu &&
		published == ane->chanbuf[0].dma && sim_dma_wmbs == 1 &&
		!memcmp(ane->chanbuf[0].cpu, expected, sizeof(expected));
	fw.table_pending = fw.descriptor_valid;
}

static void fw_write_hook(u64 pa, u32 val)
{
	u32 off = (u32)(pa - SIM_ENGINE_PA);

	/* CE handshake: a request word at 0x18a4000 is acknowledged in
	 * the status/ack register (bringup_order.h polls it). */
	if (off == 0x18a4000)
		reg_wr(0x18a4008, val);
	else if (pa >= SIM_PMGR_PA && pa + 4 <= SIM_PMGR_PA + 0x18000) {
		/* pmgr ps words: ACTUAL (7:4) follows TARGET (3:0), the
		 * condition every kext PS poll waits for. */
		unsigned int idx = (unsigned int)(pa - SIM_PMGR_PA) / 4;

		sim_regions[1].backing[idx] =
			(val & ~0xf0u) | ((val & 0xf) << 4);
	}
	else if (pa == reg_pa(sim_soc.scratch0 + 7 * 4) && val == 0)
		fw.scratch7_cleared = true;
	else if (pa == reg_pa(sim_soc.cpu_control) && val == 0x10)
		fw.armed = true;
	else if (pa == reg_pa(sim_soc.scratch0 + 28) &&
		 val == ANE_H17_TABLE_REQUEST && fw.table_enabled)
		fw_check_descriptor();
	else if (off == sim_soc.mbox + 0x808)
		fw_on_a2i_send((u8)val,
			       (u64)reg_rd(sim_soc.mbox + 0x800) |
			       ((u64)reg_rd(sim_soc.mbox + 0x804) << 32));
}

static void fw_read_hook(u64 pa)
{
	if (pa == reg_pa(sim_soc.scratch0 + 28) && fw.table_pending &&
	    fw.table_mode != 1) {
		struct ane_h16 *ane = platform_get_drvdata(&sim_pdev);
		u8 *table = (u8 *)ane->chanbuf[1].cpu + 0x100;
		unsigned int i;

		for (i = 0; i < 8; i++) {
			/* Non-pointer test data: even foreign buffer fields must
			 * only be logged, never translated or dereferenced.
			 */
			snprintf((char *)table + i * 0x100, 32, "fixture-%u", i);
			memset(table + i * 0x100 + 0x50, 0xff, 8);
		}
		fw.table_address = ane->chanbuf[1].dma + 0x100;
		if (fw.table_mode == 2)
			fw.table_address = ane->chanbuf[1].dma + 0xc441 - 0x800 + 1;
		else if (fw.table_mode == 3)
			fw.table_address = TEXT_IOVA;
		else if (fw.table_mode == 4)
			fw.table_address = ane->chanbuf[1].dma + 0xc000;
		reg_wr(sim_soc.scratch0, (u32)fw.table_address);
		reg_wr(sim_soc.scratch0 + 4, fw.table_address >> 32);
		reg_wr(sim_soc.scratch0 + 28, ANE_H16_WAKE_ACK);
		fw.table_pending = false;
		return;
	}
	if (pa == reg_pa(sim_soc.scratch0 + 7 * 4) && fw.armed &&
	    !fw.wake_delivered) {
		if (++fw.polls >= fw.wake_after) {
			reg_wr(sim_soc.scratch0 + 7 * 4,
			       ANE_H16_WAKE_ACK);
			reg_wr(sim_soc.cpu_status, 0x2a);
			fw.wake_delivered = true;
			if (fw.table_enabled) {
				reg_wr(sim_soc.scratch0, fw.table_mode == 5 ? 9 : 8);
				reg_wr(sim_soc.scratch0 + 4, 0xc440);
				reg_wr(sim_soc.scratch0 + 12, 0xf00000);
			}
			if (fw.want_hello && !fw.hello_posted)
				fw_post_hello();
		}
	} else if (pa == reg_pa(sim_soc.mbox + 0x830) && fw.hello_posted &&
		   !fw.hello_consumed) {
		u32 ctrl = reg_rd(sim_soc.mbox + 0x114);

		reg_wr(sim_soc.mbox + 0x114,
		       ctrl | (u32)ANE_H16_MBOX_CTRL_EMPTY);
		fw.hello_consumed = true;
	}
}

/* ---- fake DT ---- */

static void put32(u8 *b, size_t *off, u32 v)
{
	b[(*off)++] = (u8)v;
	b[(*off)++] = (u8)(v >> 8);
	b[(*off)++] = (u8)(v >> 16);
	b[(*off)++] = (u8)(v >> 24);
}

static void put_prop(u8 *b, size_t *off, const char *name, const void *data,
		     u32 len)
{
	memset(b + *off, 0, 32);
	memcpy(b + *off, name, strlen(name));
	*off += 32;
	put32(b, off, len);
	memcpy(b + *off, data, len);
	*off += ALIGN(len, 4);
}

/* One /arm-io/<ane> node with segment-ranges naming the preload
 * windows; the boot parser (adt_node_at/adt_child/adt_prop) walks
 * exactly this layout.
 */
static u8 *build_adt(size_t *out_len)
{
	u8 *b = calloc(1, 4096);
	size_t off = 0, len;
	struct {
		u64 phys, pad, remap, size;
	} segs[2] = {
		/* J700's TEXT record has auxiliary dword 1 after its u32 size. */
		{ TEXT_PRE_PA, 0, TEXT_IOVA, 0x1000c0000ULL },
		{ DATA_PRE_PA, 0, DATA_IOVA, 0x2ac000 },
	};
	u32 ane_type = sim_soc.ane_type;

	if (fw.table_enabled) {
		segs[0].remap = ANE_H17_DMA_BASE;
		segs[1].remap = ANE_H17_DMA_BASE + 0xc0000;
	}

	/* root: no props, one child (arm-io) */
	put32(b, &off, 0);
	put32(b, &off, 1);
	/* arm-io: one prop, one child */
	put32(b, &off, 1);
	put32(b, &off, 1);
	/* Real ADTs encode six letters plus one NUL, not two NULs. */
	put_prop(b, &off, "name", "arm-io", 7);
	/* ane0: three props, no children */
	put32(b, &off, 3);
	put32(b, &off, 0);
	put_prop(b, &off, "name", "ane0", 5);
	put_prop(b, &off, "ane-type", &ane_type, 4);
	put_prop(b, &off, "segment-ranges", segs, sizeof(segs));
	len = off;
	sim_phys_maps[sim_n_phys_maps++] =
		(struct sim_phys){ ADT_PA, len, b };
	*out_len = len;
	return b;
}

static u8 *build_fw(void)
{
	const struct ane_h16_fw *img = sim_soc.fw;
	u8 *b = calloc(1, img->size);

	memset(b + 0x4000, 0x5a, img->text_vmsize);
	memset(b + img->data_fileoff, 0xa5, img->data_filesize);
	fwbuf_len = img->size;
	return b;
}

/* Synthetic preload: file layout plus differences only within the
 * position-whitelisted patchbay/tunables windows. This is not device data.
 */
static void build_preloads(void)
{
	const struct ane_h16_fw *img = sim_soc.fw;
	u8 *data_pre = calloc(1, img->data_vmsize);

	sim_text_pre = calloc(1, img->text_vmsize);
	memcpy(sim_text_pre, fwbuf + 0x4000, img->text_vmsize);
	memcpy(data_pre, fwbuf + img->data_fileoff, img->data_filesize);
	memset(data_pre + img->patchbay_vm - img->data_vm, 0x11, 0x261);
	memset(data_pre + img->tunables_vm - img->data_vm, 0x22, 0x6a0);
	sim_phys_maps[sim_n_phys_maps++] = (struct sim_phys){
		TEXT_PRE_PA, img->text_vmsize, sim_text_pre };
	sim_phys_maps[sim_n_phys_maps++] = (struct sim_phys){
		DATA_PRE_PA, img->data_vmsize, data_pre };
}

/* ---- event helpers ---- */
static int ev_count(bool write, u64 pa, u32 val)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write == write && sim_events[i].pa == pa &&
		    (!write || sim_events[i].val == val))
			n++;
	return n;
}

static int ev_access_count(bool write, u64 pa)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write == write && sim_events[i].pa == pa)
			n++;
	return n;
}

static int ev_engine_count(bool write)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write == write &&
		    sim_events[i].pa >= SIM_ENGINE_PA &&
		    sim_events[i].pa < SIM_ENGINE_PA + 0x2000000)
			n++;
	return n;
}

/* ---- assertions ---- */
static int fails;
static void chk(bool ok, const char *what)
{
	printf("     %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		fails++;
}

/* ---- common setup ---- */
static void setup_common(void)
{
	u8 abc_digest[32], abc_expect[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	static u32 irq_cells[2] = { 0x101, 3 };	/* phandle + number */
	static u32 iommu_cells[1] = { 0x102 };
	int i;

	sha256((const u8 *)"abc", 3, abc_digest);
	if (memcmp(abc_digest, abc_expect, 32)) {
		fprintf(stderr, "SIM-BUG: sha256 self-test failed\n");
		exit(2);
	}

	sim_fw = ane_fw_leto;
	sim_soc = ane_t8132_soc;
	sim_soc.fw = &sim_fw;
	sim_match_data = &sim_soc;

	fwbuf = build_fw();
	sim_fw_name = sim_soc.fw->name;
	build_preloads();

	memset(&n_ane, 0, sizeof(n_ane));
	n_ane.name = "ane0";
	n_ane.nreg = 3;
	n_ane.reg[0][0] = SIM_ENGINE_PA;
	n_ane.reg[0][1] = 0x2000000;
	n_ane.reg[1][0] = SIM_PMGR_PA;
	n_ane.reg[1][1] = 0x18000;
	n_ane.reg[2][0] = SIM_PWGATE_PA;
	n_ane.reg[2][1] = 0x4000;
	n_ane.props[n_ane.nprops++] = (struct sim_prop){
		"apple,ane-type", &sim_soc.ane_type, 4 };
	n_ane.props[n_ane.nprops++] = (struct sim_prop){
		"interrupts", irq_cells, sizeof(irq_cells) };
	n_ane.props[n_ane.nprops++] = (struct sim_prop){
		"iommus", iommu_cells, sizeof(iommu_cells) };
	n_ane.props[n_ane.nprops++] = (struct sim_prop){
		"reg-names", "engine\0pmgr", 12 };

	memset(&n_rm, 0, sizeof(n_rm));
	n_rm.name = "reserved-memory";
	sim_dt_nodes[sim_n_dt_nodes++] = &n_rm;
	sim_dt_nodes[sim_n_dt_nodes++] = &n_ane;

	sim_regions[sim_n_regions++] = (struct sim_region){
		SIM_ENGINE_PA, 0x2000000,
		calloc(0x2000000 / 4, 4), "engine" };
	sim_regions[sim_n_regions++] = (struct sim_region){
		SIM_PMGR_PA, 0x18000,
		calloc(0x18000 / 4, 4), "pmgr" };
	sim_regions[sim_n_regions++] = (struct sim_region){
		SIM_PWGATE_PA, 0x4000,
		calloc(0x4000 / 4, 4), "pwgate" };
	for (i = 0; i < 5; i++)
		sim_regions[1].backing[sim_soc.ps_off[i] / 4] = 0xff;

	sim_fw_read_hook = fw_read_hook;
	sim_fw_write_hook = fw_write_hook;
	fw.wake_after = 3;
	sim_dart = &sim_dart_storage;
	sim_dart->dma_api = true;
	sim_dart->alias_reserved = true;

	optin = "t8132";
	ps_wait_ms = 500;
	boot_wait_ms = 3000;
	hello_wait_ms = 0;
	irq_read = false;
	scratch_read = false;
	chanreg = false;
	stage = "status";
	confirm_read = true;
	read_scratch = false;

	sim_pdev.name = "ane_h16";
	sim_pdev.dev.of_node = &n_ane;
}

static void setup_adt_node(void)
{
	size_t adt_len;
	u8 *adt = build_adt(&adt_len);

	memset(&n_adt, 0, sizeof(n_adt));
	n_adt.name = "adt";
	n_adt.props[n_adt.nprops++] = (struct sim_prop){ "label", "adt", 4 };
	n_adt.nreg = 1;
	n_adt.reg[0][0] = ADT_PA;
	n_adt.reg[0][1] = adt_len;
	n_rm.children[n_rm.nchildren++] = &n_adt;
	(void)adt;
}

/* ps words already 0xff; the boot-positive pin must match the fixture
 * file, so the scenario pins the leto row to the fixture digest.
 */
static struct firmware sim_fw_struct;

static void pin_to_fixture(void)
{
	sha256(fwbuf, fwbuf_len, (u8 *)sim_fw.sha256);
	sim_fw_struct.size = fwbuf_len;
	sim_fw_struct.data = fwbuf;
	sim_fw_file = &sim_fw_struct;
}

static int run_probe(void)
{
	return ane_h16_probe(&sim_pdev);
}

/* ---- scenarios ---- */

/* Every stage=dt run must be hardware-silent: zero bus events, zero
 * ioremaps, and the RESULT line the ladder expects.
 */
static void sc_pos_dt(void)
{
	int ret;

	setup_common();
	setup_adt_node();
	stage = "dt";
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(sim_n_events == 0, "ZERO bus accesses (hardware-silent)");
	chk(sim_ioremaps == 0, "zero ioremaps");
		chk(sim_log_count("ane_h16 RESULT stage=0 soc=t8132 "
	    "verdict=PASS reason=dt-parse-only") == 1,
	    "RESULT PASS dt-parse-only");
	chk(sim_log_count("group=pmgr tier=0 word=ANE_SYS") == 1,
	    "ps guard group printed");
	chk(sim_log_count("group=forbidden tier=3 word=CORESIGHT") == 1,
	    "forbidden CoreSight group printed");
	chk(sim_log_count("reg[0] pa=0x500000000 size=0x2000000 (engine)") == 1,
	    "engine window printed");
	chk(sim_log_count("dt irq cells=2 iommu cells=1") == 1,
	    "irq/iommu cell counts printed");
	chk(sim_log_count("adt seg0 phys=") == 1,
	    "boot-ADT segment-ranges reported");
}

static void sc_pos_dt_noadt(void)
{
	int ret;

	setup_common();
	stage = "dt";
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(sim_n_events == 0, "ZERO bus accesses");
	chk(sim_log_count("iBoot preload presence on this boot is unknown") == 1,
	    "missing ADT is a finding, not a failure");
	chk(sim_log_count("verdict=PASS reason=dt-parse-only") == 1,
	    "still RESULT PASS dt-parse-only");
}

/* Engine reads at one offset (every read is one bus event). */
static int ev_engine_read_at(u32 off)
{
	return ev_count(false, SIM_ENGINE_PA + off, 0);
}

static int ev_scratch_reads(void)
{
	int i, n = 0;

	for (i = 0; i < 8; i++)
		n += ev_engine_read_at(sim_soc.scratch0 + 4 * i);
	return n;
}

/* What the module reads when it is allowed to read: RVBAR (a 64-bit read, two bus words), CPU_STATUS and the two mailbox control
 * words. Nothing else. */
static void chk_status_reads_only(void)
{
	chk(ev_engine_read_at(sim_soc.rvbar) == 1 && ev_engine_read_at(sim_soc.rvbar + 4) == 1,
	    "RVBAR read once");
	chk(ev_engine_read_at(sim_soc.cpu_status) == 1, "CPU_STATUS read once");
	chk(ev_engine_read_at(sim_soc.mbox + ANE_H16_MBOX_A2I_CTRL) == 1,
	    "mailbox A2I control read once");
	chk(ev_engine_read_at(sim_soc.mbox + ANE_H16_MBOX_I2A_CTRL) == 1,
	    "mailbox I2A control read once");
}

static void sc_pos_status(void)
{
	int ret;

	setup_common();
	stage = "status";
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(sim_log_count("ane_h16 RESULT stage=1 soc=t8132 "
	    "verdict=PASS reason=ps-guard+reads") == 1,
	    "RESULT PASS ps-guard+reads");
	chk(sim_log_count("actual=0xf pass=true") == 5,
	    "five ps words logged with ACTUAL 0xf");
	chk(ev_engine_count(true) == 0, "zero engine WRITES");
	chk(ev_engine_count(false) == 5, "exactly five engine bus reads (RVBAR is two words)");
	chk_status_reads_only();
	chk(ev_scratch_reads() == 0, "SCRATCH words are not read by default");
	chk(sim_rpm_gets == sim_rpm_puts, "power claim released");
	chk(sim_ioremaps == 2 && sim_iounmaps == 0,
	    "engine and pmgr mapped without a region request, still mapped while bound");
	ane_h16_remove(&sim_pdev);
	chk(sim_iounmaps == 2, "both mappings released on remove");
}

static void sc_pos_status_scratch(void)
{
	int ret;

	setup_common();
	stage = "status";
	read_scratch = true;
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(ev_scratch_reads() == 8, "read_scratch=1 reads exactly the eight SCRATCH words");
	chk_status_reads_only();
	chk(ev_engine_count(true) == 0, "zero engine WRITES");
}

/* Defaults captured before any scenario sets a parameter. */
static const char *init_stage;
static bool init_confirm_read, init_read_scratch;

static void sc_pos_defaults(void)
{
	chk(init_stage && !strcmp(init_stage, "dt"), "default stage is dt (no MMIO)");
	chk(!init_confirm_read, "confirm_read defaults off");
	chk(!init_read_scratch, "read_scratch defaults off");
}

/* No consent, no access: nothing is mapped, powered or read. */
static void sc_neg_no_confirm(void)
{
	int ret;

	setup_common();
	stage = "status";
	confirm_read = false;
	sim_pd_count = 5;
	ret = run_probe();
	chk(ret == -EPERM, "probe refuses with -EPERM");
	chk(sim_log_count("ane_h16 RESULT stage=1 soc=t8132 "
	    "verdict=REFUSED reason=confirm_read-required") == 1,
	    "RESULT REFUSED confirm_read-required");
	chk(sim_n_events == 0, "ZERO bus accesses");
	chk(sim_ioremaps == 0, "zero ioremaps");
	chk(sim_rpm_gets == 0 && sim_pd_attached == 0, "nothing powered or attached");
}

/* Five listed power domains are all attached, linked, and released again on remove. */
static void sc_pos_pds(void)
{
	int ret;

	setup_common();
	stage = "status";
	sim_pd_count = 5;
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(sim_pd_attached == 5 && sim_pd_links == 5, "five domains attached and linked");
	chk(sim_pd_detached == 0, "none detached while bound");
	chk(sim_log_count("attached 5 ANE power-domains") == 1, "attach logged");
	ane_h16_remove(&sim_pdev);
	chk(sim_pd_detached == 5 && sim_pd_unlinked == 5, "all five unlinked and detached on remove");
}

/* One listed domain is the platform's own job: the module attaches nothing. */
static void sc_pos_one_pd(void)
{
	int ret;

	setup_common();
	stage = "status";
	sim_pd_count = 1;
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(sim_pd_attached == 0 && sim_pd_links == 0, "a single domain is left to the platform");
}

/* A domain that fails to attach unwinds the ones before it, before any mapping or read. */
static void sc_neg_pd_fail(void)
{
	int ret;

	setup_common();
	stage = "status";
	sim_pd_count = 5;
	sim_pd_fail_at = 2;
	ret = run_probe();
	chk(ret == -EPROBE_DEFER, "probe returns the attach error");
	chk(sim_pd_attached == 2 && sim_pd_detached == 2 && sim_pd_unlinked == 2,
	    "the two attached domains are unlinked and detached");
	chk(sim_n_events == 0 && sim_ioremaps == 0, "no mapping and no read");
}

/* A failed link still detaches the domain that was just attached. */
static void sc_neg_pd_link_fail(void)
{
	int ret;

	setup_common();
	stage = "status";
	sim_pd_count = 5;
	sim_pd_link_fail_at = 1;
	ret = run_probe();
	chk(ret < 0, "probe fails");
	chk(sim_pd_attached == 2 && sim_pd_detached == 2, "both attached domains detached");
	chk(sim_pd_unlinked == sim_pd_links, "every link that was made is removed");
	chk(sim_n_events == 0 && sim_ioremaps == 0, "no mapping and no read");
}

/* A failed get_sync still took a usage count: it is given back, and the probe unwinds everything it set up. */
static void sc_neg_getsync_fail(void)
{
	int ret;

	setup_common();
	stage = "status";
	sim_pd_count = 5;
	sim_rpm_get_rc = -EIO;
	ret = run_probe();
	chk(ret == -EIO, "probe returns the get_sync error");
	chk(sim_rpm_gets == sim_rpm_puts, "the usage count of the failed get_sync is given back");
	chk(sim_ioremaps == 2 && sim_iounmaps == 2, "both windows are unmapped");
	chk(sim_pd_attached == 5 && sim_pd_detached == 5, "all five domains are detached");
	chk(sim_n_events == 0, "no register was read");
}

/* ane_h15 saw -EACCES from get_sync with several domains attached; the domains are already up through their links, so the ACTUAL gate decides. */
static void sc_pos_getsync_eacces(void)
{
	int ret;

	setup_common();
	stage = "status";
	sim_pd_count = 5;
	sim_rpm_get_rc = -EACCES;
	ret = run_probe();
	chk(ret == 0, "probe goes on");
	chk(sim_log_count("get_sync -13 ignored") == 1, "the ignored error is logged");
	chk(sim_rpm_gets == sim_rpm_puts, "power claim released");
	chk(sim_log_count("verdict=PASS") == 1, "the ACTUAL gate and the reads still ran");
}

/* With no domain attached by the module, -EACCES is an ordinary failure. */
static void sc_neg_getsync_eacces_one_pd(void)
{
	int ret;

	setup_common();
	stage = "status";
	sim_pd_count = 1;
	sim_rpm_get_rc = -EACCES;
	ret = run_probe();
	chk(ret == -EACCES, "probe fails with -EACCES");
	chk(sim_rpm_gets == sim_rpm_puts, "the usage count is given back");
	chk(sim_n_events == 0 && sim_iounmaps == sim_ioremaps, "no read, windows unmapped");
}

static void sc_pos_boot(void)
{
	u64 expect_rvbar;
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	stage = "boot";
	boot_wait_ms = 300;
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(sim_log_count("SCRATCH7 wake word 0x8042006: firmware is running") == 1,
	    "wake word line");
	chk(sim_log_count("verdict=PASS reason=boot-wake") == 1,
	    "RESULT PASS boot-wake");
	expect_rvbar = ANE_H16_RVBAR_ENTRY_BASE |
		(TEXT_IOVA & ANE_H16_RVBAR_ADDR_MASK);
	chk(ev_count(true, reg_pa(sim_soc.rvbar), (u32)expect_rvbar) == 1,
	    "RVBAR written with the composed entry");
	chk(ev_count(true, reg_pa(sim_soc.scratch0 + 7 * 4), 0) == 1,
	    "H16 SCRATCH7 cleared once before CPU release");
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0) == 1 &&
	    ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 1,
	    "CPU_CONTROL 0 then 0x10");
	chk(ev_engine_count(false) > ev_engine_count(true),
	    "more reads than writes");
	chk(sim_dart_storage.n == 2 * (48 + 171),
	    "DMA source and staged aliases each map 219 pages");
	chk(iommu_iova_to_phys(sim_dart, TEXT_IOVA) == SIM_DMA_SOURCE_PA,
	    "alias uses DMA-translated PA, not CPU pointer or DMA handle");
	chk(sim_log_count("do NOT unload") == 1,
	    "post-start pin warning printed");
	chk(sim_module_refs == 1, "module pinned after CPU release");
	chk(sim_rpm_gets == 1 && sim_rpm_puts == 0,
	    "runtime power retained after CPU release");
}

static void sc_pos_boot_hello(void)
{
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	stage = "boot";
	boot_wait_ms = 300;
	hello_wait_ms = 1000;
	fw.want_hello = true;
	sim_dart = &sim_dart_storage;
	ret = run_probe();
	chk(ret == 0, "probe returns 0");
	chk(fw.hello_reply_seen, "firmware model received HELLO_REPLY");
	chk(fw.reply_version == 2, "reply version min(max=3, 2)");
	chk(sim_log_count("RTKit HELLO: min 1 max 3 -> replying version 2") == 1,
	    "HELLO exchange logged");
	chk(sim_log_count("verdict=PASS reason=boot-hello") == 1,
	    "RESULT PASS boot-hello");
}

static void sc_neg_ps_stuck(void)
{
	int ret;

	setup_common();
	sim_regions[1].backing[sim_soc.ps_off[0] / 4] = 0x30;
	ps_wait_ms = 40;
	stage = "status";
	ret = run_probe();
	chk(ret == -ETIMEDOUT, "probe returns -ETIMEDOUT");
	chk(sim_log_count("verdict=FAIL reason=pmgr-actual-stuck") == 1,
	    "RESULT FAIL pmgr-actual-stuck");
	chk(ev_engine_count(false) == 0 && ev_engine_count(true) == 0,
	    "ZERO engine accesses behind a failed guard");
	chk(sim_rpm_gets == sim_rpm_puts, "power claim released");
}

static void sc_neg_mbox_silent(void)
{
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	stage = "boot";
	boot_wait_ms = 200;
	fw.wake_after = 1 << 30;	/* never */
	sim_dart = &sim_dart_storage;
	ret = run_probe();
	chk(ret == 0, "binding retained after uncertain CPU release");
	chk(((struct ane_h16 *)platform_get_drvdata(&sim_pdev))->boot_error == -ETIMEDOUT,
	    "timeout retained as boot_error, not reported as success");
	chk(sim_log_count("verdict=FAIL reason=boot-timeout") == 1,
	    "RESULT FAIL boot-timeout");
	chk(sim_log_count("reboot before retrying") == 1,
	    "reboot-required message");
	chk(sim_dart_storage.n > 0, "mapping LEFT in place (no unwind)");
	chk(sim_dma_outstanding == 1, "staged buffer not freed");
	chk(sim_log_count("firmware is running") == 0,
	    "no firmware-started claim (it did not start)");
	chk(sim_module_refs == 1, "module pinned after timeout");
	chk(sim_rpm_gets == 1 && sim_rpm_puts == 0,
	    "runtime power retained after timeout");
}

static void sc_neg_bad_pin(void)
{
	int ret;

	setup_common();
	setup_adt_node();
	/* fixture file, REAL leto pin: mismatch must refuse pre-write
	 */
	sim_fw_struct.size = fwbuf_len;
	sim_fw_struct.data = fwbuf;
	sim_fw_file = &sim_fw_struct;
	stage = "boot";
	ret = run_probe();
	chk(ret == -EINVAL, "probe returns -EINVAL");
	chk(sim_log_count("verdict=REFUSED reason=fw-pin") == 1,
	    "RESULT REFUSED fw-pin");
	/* The H17 ordering (clock/power enable) legitimately precedes the
	 * pin check; the pin must still refuse before any release-relevant
	 * access: no scratch, CPU_CONTROL or RVBAR writes. */
	chk(ev_count(true, reg_pa(sim_soc.scratch0), 0) == 0,
	    "no scratch writes before the pin passes");
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0) == 0 &&
	    ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 0,
	    "no CPU_CONTROL writes before the pin passes");
	chk(ev_count(true, reg_pa(sim_soc.rvbar), 0) == 0,
	    "no RVBAR writes before the pin passes");
	chk(sim_active_maps() == 0, "nothing mapped");
	chk(sim_dma_allocs == 0 && sim_dma_frees == 0,
	    "refused before any allocation");
}

static void sc_neg_foreign_preload(void)
{
	u64 poison = 0xdeadbeefdeadbeefull;
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	/* one foreign word in TEXT, outside patchbay/tunables
	 */
	memcpy(sim_text_pre + 0x2000, &poison, 8);
	stage = "boot";
	ret = run_probe();
	chk(ret == -EINVAL, "probe returns -EINVAL");
	chk(sim_log_count("verdict=REFUSED reason=preload-diff") == 1,
	    "RESULT REFUSED preload-diff");
	/* ordering writes are allowed; no release-relevant access */
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 0,
	    "no CPU release on preload refusal");
	chk(sim_active_maps() == 0, "nothing mapped");
	/* a refusal must NOT leak the staged buffer: allocs == frees.
	 * (The deliberate exception is a post-release timeout, where the
	 * buffer stays because the wedged firmware may still DMA.)
	 */
	chk(sim_dma_allocs == sim_dma_frees,
	    "refusal frees the staged buffer (no DMA leak)");
}

/* A DART map that fails midway must unwind the pages it already
 * mapped before the staged buffer is freed: partial IOVA mappings
 * over freed memory hand the next map a stale translation. The
 * alias guard (iova already mapped -> -EBUSY) supplies the mid-loop
 * failure: pre-seed one colliding page in the DATA segment, so 48
 * TEXT pages and 100 DATA pages map, then page 101 refuses.
 */
static void sc_neg_dart_map_partial(void)
{
	struct iommu_domain *d = &sim_dart_storage;
	u64 alias = DATA_IOVA + 100 * SZ_16K;
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	d->map[d->n].iova = alias;
	d->map[d->n].pa = 0xdead0000;
	d->map[d->n].len = SZ_16K;
	d->n++;
	stage = "boot";
	ret = run_probe();
	chk(ret == -EBUSY, "probe returns -EBUSY");
	chk(sim_log_count("verdict=REFUSED reason=dart-map") == 1,
	    "RESULT REFUSED dart-map");
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 0,
	    "no CPU release on map refusal");
	chk(sim_dma_allocs == sim_dma_frees,
	    "refusal frees the staged buffer (no DMA leak)");
	chk(iommu_iova_to_phys(d, TEXT_IOVA) == 0 &&
	    iommu_iova_to_phys(d, TEXT_IOVA + 47 * SZ_16K) == 0 &&
	    iommu_iova_to_phys(d, DATA_IOVA) == 0 &&
	    iommu_iova_to_phys(d, DATA_IOVA + 99 * SZ_16K) == 0,
	    "partial map unwound before free_stage");
	chk(iommu_iova_to_phys(d, alias) == 0xdead0000,
	    "pre-existing foreign mapping preserved after collision");
}

/* preload ownership negative: a segment inside allocator-owned System RAM
 * is not an owned preload window. */
static void sc_neg_preload_sysram(void)
{
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	sim_sys_ram[0].start = DATA_PRE_PA + SZ_16K;	/* overlap seg1 */
	stage = "boot";
	ret = run_probe();
	chk(ret == -EINVAL, "System-RAM-overlapping preload refused");
	chk(sim_snapshots == 0, "no snapshot taken from allocator RAM");
	chk(sim_dma_allocs == sim_dma_frees, "no DMA backing leaked");
	chk(sim_active_maps() == 0, "no alias mappings after refusal");
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 0,
	    "no CPU release on ownership refusal");
}

static void sc_neg_preload_bytes(unsigned int mode)
{
	int ret;
	u64 entry = TEXT_IOVA;
	u8 *data;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	data = sim_phys_maps[1].host;
	if (mode == 0)
		memcpy(data + 0x100000, &entry, sizeof(entry));
	else if (mode == 1)
		data[sim_fw.patchbay_vm - sim_fw.data_vm + 0x261] ^= 1;
	else
		sim_snapshot_fail = 2;
	stage = "boot";
	ret = run_probe();
	chk(ret == (mode == 2 ? -ENOMEM : -EINVAL),
	    "unqualified BSS, exact whitelist boundary or snapshot failure refused");
	chk(sim_snapshots == 0, "all snapshots released on refusal");
	chk(sim_dma_allocs == sim_dma_frees, "staged backing released before CPU use");
	chk(sim_active_maps() == 0, "no DMA mappings left after refusal");
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 0,
	    "no CPU release on preload refusal");
}

static void sc_neg_stage_domain(bool dma_api)
{
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	sim_dart->dma_api = dma_api;
	sim_dart->alias_reserved = !dma_api;
	stage = "boot";
	ret = run_probe();
	chk(ret == (dma_api ? -EINVAL : -ENODEV),
	    "unreserved alias or incompatible domain refused");
	chk(sim_dma_allocs == 0, "refusal precedes DMA allocation");
	chk(ev_engine_count(true) == 0, "refusal precedes engine writes");
}

static void sc_neg_short_unmap(void)
{
	struct iommu_domain *d = &sim_dart_storage;
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	d->map[d->n].iova = DATA_IOVA + SZ_16K;
	d->map[d->n].pa = 0xdead0000;
	d->map[d->n].len = SZ_16K;
	d->n++;
	d->short_unmap = true;
	stage = "boot";
	ret = run_probe();
	chk(ret == -EIO, "short unmap reported as cleanup failure");
	chk(sim_dma_allocs == 1 && sim_dma_frees == 0,
	    "buffer retained while translations may reference it");
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 0,
	    "no CPU release before map refusal");
	chk(iommu_iova_to_phys(d, DATA_IOVA + SZ_16K) == 0xdead0000,
	    "foreign mapping survives short-unmap failure");
}

static void sc_pos_four_ps(void)
{
	struct ane_h16 ane = {
		.dev = &sim_pdev.dev,
		.soc = &sim_soc,
		.pmgr = (void *)(uintptr_t)SIM_PMGR_PA,
	};
	int ret;

	setup_common();
	sim_soc.ps_count = 4;
	/* The excluded fifth word must not be read or polled. */
	sim_regions[1].backing[sim_soc.ps_off[4] / 4] = 0;
	ret = ane_h16_ps_wait(&ane);
	chk(ret == 0, "four valid PS words accepted with absent fifth word");
	chk(sim_n_events == 8, "only four PS words read twice");
	sim_soc.ps_count = 6;
	ret = run_probe();
	chk(ret == -EINVAL, "out-of-bounds PS count refused");
	chk(sim_n_events == 8, "invalid count adds no bus accesses");
	chk(sim_ioremaps == 0 && sim_rpm_gets == 0,
	    "invalid count refused before mapping or power acquisition");
}

static void sc_neg_adt_bounds(void)
{
	size_t len;
	u8 *buf;
	size_t off = 0;
	unsigned int i;

	setup_common();
	buf = build_adt(&len);
	adt = buf;
	adt_len = len;
	chk(adt_validate() == 0, "valid seven-byte arm-io fixture accepted");
	adt_len = len - 1;
	chk(adt_validate() == -EINVAL, "truncated final property refused");
	adt_len = 7;
	chk(adt_validate() == -EINVAL, "truncated node header refused");
	memset(buf, 0, 4096);
	for (i = 0; i < 34; i++) {
		put32(buf, &off, 0);
		put32(buf, &off, i < 33 ? 1 : 0);
	}
	adt_len = off;
	chk(adt_validate() == -EINVAL, "excessive tree depth refused");
	chk(sim_n_events == 0, "zero bus accesses");
}

static void sc_adt_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	long len;
	u8 *buf;
	struct adt_node node;
	struct ane_h16 ane = { };
	struct ane_h16_seg segs[2];
	const void *data;
	u32 size;
	int ret;
	unsigned int i;

	if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) < 8 ||
	    len > 16 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) {
		fprintf(stderr, "cannot read ADT fixture\n");
		exit(2);
	}
	buf = malloc(len);
	if (!buf || fread(buf, 1, len, f) != (size_t)len)
		exit(2);
	fclose(f);
	adt = buf;
	adt_len = len;
	ret = adt_validate();
	chk(ret == 0, "saved ADT validates");
	if (ret)
		goto out;
	ret = adt_find_ane(0x200, &node);
	chk(ret == 0, "saved ADT contains H17 ANE");
	if (ret)
		goto out;
	ret = adt_prop(node, "segment-ranges", &data, &size);
	chk(ret == 0, "saved H17 node contains segment-ranges");
	if (ret)
		goto out;
	ret = ane_h16_seg_decode(&ane, data, size, segs);
	chk(ret == 0, "saved segment-ranges decoded");
	if (!ret)
		for (i = 0; i < 2; i++)
			printf("seg%u phys=%#llx iova=%#llx size=%#llx\n",
			       i, segs[i].phys, segs[i].iova, segs[i].size);
	if (!ret) {
		chk(segs[0].size == 0xc0000, "TEXT u32 size excludes auxiliary dword");
		chk(segs[1].size == 0x2ac000, "DATA u32 size decoded");
		chk(segs[0].iova == 0x10000000000ULL, "TEXT DVA matches latched RVBAR address");
	}
out:
	free(buf);
	adt = NULL;
	adt_len = 0;
	chk(sim_n_events == 0, "zero bus accesses");
}

/* The H17 path on the real row: ordering sequence, wake, post-wake
 * steps 19-21, durable per-access logging. */
static void sc_pos_h17_boot(void)
{
	int ret;

	setup_common();
	sim_soc = ane_t8140_soc;
	sim_soc.fw = &sim_fw;
	sim_match_data = &sim_soc;
	setup_adt_node();
	pin_to_fixture();
	sim_regions[1].backing[0x290 / 4] = 0xff;	/* ANE_SYS word */
	optin = "t8140";
	stage = "boot";
	boot_wait_ms = 300;
	hello_wait_ms = 1000;
	ret = run_probe();
	chk(ret == 0, "t8140 boot succeeds through the H17 path");
	chk(sim_log_count("verdict=PASS reason=boot-wake") == 1,
	    "RESULT PASS boot-wake");
	chk(sim_log_count("H17 post-wake RTKit management poll skipped") == 1,
	    "H17 skips unqualified H16 mailbox even when requested");
	chk(sim_log_count("RTKit poll done") == 0,
	    "H17 does not access candidate H16 mailbox");
	chk(ev_count(true, SIM_PMGR_PA + 0xc000, 0xf) == 1, "PS step 1: 0xf");
	chk(ev_count(true, SIM_PMGR_PA + 0xc000, 0x1000000f) == 1,
	    "PS step 5: 0x1000000f");
	chk(ev_count(true, SIM_PMGR_PA + 0xc008, 0xf) == 1,
	    "PS step 7: subblocks 0xf");
	chk(ev_count(true, SIM_PWGATE_PA + 0x268c, 0) == 1,
	    "PWGATE step 3: 0");
	chk(ev_count(true, reg_pa(sim_soc.scratch0 + 6 * 4), 1) == 1,
	    "step 10: SCRATCH6 set");
	chk(ev_count(true, reg_pa(sim_soc.scratch0 + 7 * 4), 0) == 2,
	    "cold branch: SCRATCH7 zeroed by init and step 12");
	chk(ev_count(true, reg_pa(sim_soc.wake_cfg1), 1) == 1,
	    "step 19 after the wake word");
	chk(ev_count(true, reg_pa(sim_soc.wake_cfg2), 0) == 1, "step 20");
	chk(ev_count(true, reg_pa(sim_soc.doorbell_en), 1) == 1,
	    "step 21 doorbell setBit");
	chk(sim_log_count("h17-order seq=") >= 80,
	    "every ordered access durably logged before MMIO");
	chk(sim_log_count("H17 IRQ_STATUS off=") == 0,
	    "irq_read off (default): no IRQ status read");
	chk(sim_log_count("H17 SCRATCH off=") == 0,
	    "scratch_read off (default): no scratch-word snapshot");
	chk(sim_log_count("H17 TABLE") == 0 && sim_dma_allocs == 1 &&
	    ev_count(true, reg_pa(sim_soc.scratch0 + 28), ANE_H17_TABLE_REQUEST) == 0,
	    "chanreg off: no discovery allocations, request or table log");
	chk(ev_count(false, reg_pa(sim_soc.irq_status), 0) == 0 &&
	    ev_count(false, reg_pa(sim_soc.irq_ack), 0) == 0 &&
	    ev_count(true, reg_pa(sim_soc.irq_ack), 0) == 0,
	    "irq status and ack words untouched when irq_read=0");
	chk(sim_module_refs == 1 && sim_rpm_gets == 1 && sim_rpm_puts == 0,
	    "module and power retained after CPU release");
}

/* The post-wake IRQ-status rung: with irq_read=1 the wake path logs
 * exactly one read-only snapshot of the IRQ status word and never
 * touches the ack word. */
static void sc_pos_h17_irq(void)
{
	int ret;

	setup_common();
	sim_soc = ane_t8140_soc;
	sim_soc.fw = &sim_fw;
	sim_match_data = &sim_soc;
	setup_adt_node();
	pin_to_fixture();
	sim_regions[1].backing[0x290 / 4] = 0xff;	/* ANE_SYS word */
	sim_regions[0].backing[sim_soc.irq_status / 4] = 0x00000200;
	optin = "t8140";
	stage = "boot";
	boot_wait_ms = 300;
	hello_wait_ms = 1000;
	irq_read = true;
	ret = run_probe();
	chk(ret == 0, "t8140 boot with irq_read succeeds through the H17 path");
	chk(sim_log_count("verdict=PASS reason=boot-wake") == 1,
	    "RESULT PASS boot-wake");
	chk(sim_log_count("H17 post-wake RTKit management poll skipped") == 1,
	    "H17 skips unqualified H16 mailbox even with irq_read");
	chk(sim_log_count("H17 IRQ_STATUS off=0x188c000 value=0x00000200") == 1,
	    "one logged IRQ status snapshot with the modelled value");
	chk(ev_count(false, reg_pa(sim_soc.irq_status), 0) == 1,
	    "exactly one read of the IRQ status word");
	chk(ev_count(false, reg_pa(sim_soc.irq_ack), 0) == 0 &&
	    ev_count(true, reg_pa(sim_soc.irq_ack), 0) == 0,
	    "IRQ ack word untouched");
}

/* The post-wake scratch-word rung: with scratch_read=1 the wake path
 * logs exactly one in-order snapshot of the eight scratch words and
 * adds no scratch writes beyond the boot recipe. */
static void sc_pos_h17_scratch(void)
{
	int ret;
	unsigned int n;

	setup_common();
	sim_soc = ane_t8140_soc;
	sim_soc.fw = &sim_fw;
	sim_match_data = &sim_soc;
	setup_adt_node();
	pin_to_fixture();
	sim_regions[1].backing[0x290 / 4] = 0xff;	/* ANE_SYS word */
	optin = "t8140";
	stage = "boot";
	boot_wait_ms = 300;
	hello_wait_ms = 1000;
	scratch_read = true;
	ret = run_probe();
	chk(ret == 0, "t8140 boot with scratch_read succeeds through the H17 path");
	chk(sim_log_count("verdict=PASS reason=boot-wake") == 1,
	    "RESULT PASS boot-wake");
	chk(sim_log_count("H17 SCRATCH off=0x1880020 values=0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000001,0x08042006") == 1,
	    "one logged scratch snapshot: S0..5 carry the boot clears, S6 the step-10 set, S7 the wake word");
	chk(sim_log_count("H17 IRQ_STATUS off=") == 0,
	    "irq_read off (default): no IRQ status read");
	for (n = 0; n < 8; n++)
		/* S0..6: snapshot only (read_scratch defaults off); S7 adds the quiescence
		 * check and the 3 modelled wake polls.
		 */
		chk(ev_count(false, reg_pa(sim_soc.scratch0 + 4 * n), 0) ==
		    (n == 7 ? 5 : 1),
		    "scratch words read only by the snapshot and boot checks");
	chk(ev_count(true, reg_pa(sim_soc.scratch0 + 7 * 4), 0) == 2 &&
	    ev_count(true, reg_pa(sim_soc.scratch0 + 6 * 4), 1) == 1 &&
	    ev_count(true, reg_pa(sim_soc.scratch0 + 6 * 4), 0) == 1 &&
	    ev_count(true, reg_pa(sim_soc.scratch0), 0) == 1,
	    "scratch writes are the boot recipe only: S0..5 cleared, S6 cleared then set, S7 cleared twice");
	chk(ev_count(false, reg_pa(sim_soc.irq_status), 0) == 0 &&
	    ev_count(false, reg_pa(sim_soc.irq_ack), 0) == 0 &&
	    ev_count(true, reg_pa(sim_soc.irq_ack), 0) == 0,
	    "IRQ status and ack words untouched");
}

static void sc_h17_table(int mode)
{
	struct ane_h16 *ane;
	int ret, expected_error = 0;
	int expected_allocs = 4;
	unsigned int n;
	const char *reason = "verdict=PASS reason=boot-table-discovery";

	setup_common();
	sim_soc = ane_t8140_soc;
	sim_soc.fw = &sim_fw;
	sim_match_data = &sim_soc;
	fw.table_enabled = true;
	setup_adt_node();
	pin_to_fixture();
	sim_regions[1].backing[0x290 / 4] = 0xff;
	optin = "t8140";
	stage = "boot";
	chanreg = true;
	fw.table_enabled = true;
	fw.table_mode = mode;
	if (mode == 1) {
		expected_error = -ETIMEDOUT;
		reason = "verdict=FAIL reason=table-reply-timeout";
	} else if (mode >= 2 && mode <= 4) {
		expected_error = -EINVAL;
		reason = "verdict=REFUSED reason=table-reply-outside-ipc";
	} else if (mode == 5) {
		expected_error = -EINVAL;
		expected_allocs = 1;
		reason = "verdict=REFUSED reason=table-input-envelope";
	} else if (mode >= 6) {
		expected_error = mode == 7 ? -EINVAL : -ENOMEM;
		expected_allocs = mode == 8 ? 3 : 2;
		reason = "verdict=REFUSED reason=table-dma-allocation-or-range";
		if (mode == 7)
			sim_dma_bad_at = 2;
		else
			sim_dma_fail_at = mode == 8 ? 4 : 3;
	}
	ret = run_probe();
	ane = platform_get_drvdata(&sim_pdev);
	chk(ret == 0 && ane->boot_error == expected_error,
	    "binding retained; boot_error records table rung result");
	chk(sim_log_count(reason) == 1 &&
	    sim_log_count("verdict=PASS reason=boot-wake") == 0,
	    "only the table-specific verdict, never a fallback wake PASS");
	chk(sim_dma_outstanding == expected_allocs && sim_dma_frees == 0 &&
	    sim_module_refs == 1 && sim_rpm_puts == 0,
	    "all firmware-visible memory, binding and power retained until reboot");
	chk(ane->stage_size == 0x380000 &&
	    ane->stage_mapped[0] + ane->stage_mapped[1] == 0x380000 &&
	    iommu_iova_to_phys(sim_dart, ANE_H17_DMA_BASE + 0x37c000) != 0,
	    "entire advertised FWIM range, including padding, is coherently backed");
	chk(ev_access_count(true, reg_pa(sim_soc.scratch0 + 12)) == 1 &&
	    ev_count(true, reg_pa(sim_soc.scratch0 + 12), 0) == 1,
	    "SCRATCH3 only cleared by boot recipe; registration is NEVER sent");
	for (n = 0; n < 8; n++) {
		int writes = n >= 6 ? 2 : 1;

		if (mode < 5 && (n == 0 || n == 1 || n == 7))
			writes++;
		chk(ev_access_count(true, reg_pa(sim_soc.scratch0 + n * 4)) == writes,
		    "scratch writes are exactly boot recipe plus published S0/S1/S7");
	}
	chk(ev_access_count(true, reg_pa(0x1884000)) == 0 &&
	    ev_access_count(false, reg_pa(sim_soc.irq_status)) == 0 &&
	    ev_access_count(false, reg_pa(sim_soc.irq_ack)) == 0 &&
	    ev_access_count(true, reg_pa(sim_soc.irq_ack)) == 0,
	    "no doorbell, IRQ status or IRQ ack access of any value");
	chk(!sim_events_overflow, "bus event recorder did not overflow");
	if (mode < 5) {
		chk(fw.descriptor_valid && sim_dma_wmbs == 1 &&
		    ev_count(true, reg_pa(sim_soc.scratch0 + 28), ANE_H17_TABLE_REQUEST) == 1,
		    "full descriptor matches independent byte oracle before one published request");
	} else {
		chk(sim_dma_wmbs == 0 &&
		    ev_count(true, reg_pa(sim_soc.scratch0 + 28), ANE_H17_TABLE_REQUEST) == 0,
		    "preparation refusal never publishes a request");
	}
	if (mode == 0) {
		chk(sim_log_count("H17 TABLE_COPY ") == 1 &&
		    sim_log_count("H17 CHANNEL index=") == 8 &&
		    sim_log_count("H17 TABLE_STOP before SCRATCH3") == 1,
		    "one table copy, eight opaque entries, explicit pre-registration stop");
		chk(sim_dma_rmbs == 1 &&
		    !memcmp(ane->chan_table, (u8 *)ane->chanbuf[1].cpu + 0x100, 0x800),
		    "exactly the validated owned IPC bytes are copied after read barrier");
	} else {
		chk(sim_dma_rmbs == 0 && sim_log_count("H17 TABLE_COPY ") == 0 &&
		    sim_log_count("H17 CHANNEL index=") == 0,
		    "timeout/refusal never copies or follows an untrusted reply");
	}
}

static void sc_neg_table_scope(void)
{
	setup_common();
	chanreg = true;
	stage = "boot"; /* t8132, wrong target */
	chk(run_probe() == -EINVAL && sim_n_events == 0 && sim_ioremaps == 0,
	    "chanreg on non-H17 target refuses before hardware access");
}

static void sc_neg_table_reservation(void)
{
	setup_common();
	sim_soc = ane_t8140_soc;
	sim_soc.fw = &sim_fw;
	sim_match_data = &sim_soc;
	fw.table_enabled = true;
	setup_adt_node();
	pin_to_fixture();
	sim_regions[1].backing[0x290 / 4] = 0xff;
	optin = "t8140";
	stage = "boot";
	chanreg = true;
	sim_resv_length = 0x36c000; /* covers segments, NOT advertised arena */
	chk(run_probe() == -EINVAL && sim_module_refs == 0 &&
	    sim_dma_outstanding == 0 && sim_dma_allocs == 0 &&
	    ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 0,
	    "short FWIM alias reservation refuses before allocation or CPU release");
}

/* apply_overlay removes iommus while the DART nodes are disabled, and the device then has no IOMMU domain. stage=boot must refuse before it
 * writes anything to the engine: firmware must never run without translation. */
static void sc_neg_no_domain(void)
{
	int ret;

	setup_common();
	setup_adt_node();
	pin_to_fixture();
	sim_dart = NULL;
	stage = "boot";
	ret = run_probe();
	chk(ret == -ENODEV, "probe returns -ENODEV");
	chk(sim_log_count("verdict=REFUSED reason=dma-domain-or-alias-reservation") == 1,
	    "RESULT REFUSED dma-domain-or-alias-reservation");
	chk(ev_engine_count(true) == 0, "zero engine writes on refusal");
	chk(sim_dma_allocs == sim_dma_frees, "refusal frees the staged buffer (no DMA leak)");
	chk(sim_rpm_gets == sim_rpm_puts, "power claim released");
}

static void sc_neg_unknown_stage(void)
{
	int ret;

	setup_common();
	stage = "2";		/* not an H16 module stage */
	ret = run_probe();
	chk(ret == -EINVAL, "probe returns -EINVAL");
	chk(sim_log_count("RESULT") == 0, "NO RESULT line on unknown stage");
	chk(sim_n_events == 0, "zero bus accesses");
	chk(sim_ioremaps == 0, "zero ioremaps");
}

int main(int argc, char **argv)
{
	const char *sc;

	if (argc != 2 && !(argc == 3 && !strcmp(argv[1], "adt-file"))) {
		fprintf(stderr, "usage: sim_harness <scenario>\n");
		return 2;
	}
	sc = argv[1];
	init_stage = stage;
	init_confirm_read = confirm_read;
	init_read_scratch = read_scratch;
	printf("== ane_h16 sim: %s ==\n", sc);
	if (!strcmp(sc, "pos-dt"))
		sc_pos_dt();
	else if (!strcmp(sc, "pos-dt-noadt"))
		sc_pos_dt_noadt();
	else if (!strcmp(sc, "pos-status"))
		sc_pos_status();
	else if (!strcmp(sc, "pos-status-scratch"))
		sc_pos_status_scratch();
	else if (!strcmp(sc, "pos-defaults"))
		sc_pos_defaults();
	else if (!strcmp(sc, "pos-pds"))
		sc_pos_pds();
	else if (!strcmp(sc, "pos-one-pd"))
		sc_pos_one_pd();
	else if (!strcmp(sc, "neg-no-confirm"))
		sc_neg_no_confirm();
	else if (!strcmp(sc, "neg-pd-fail"))
		sc_neg_pd_fail();
	else if (!strcmp(sc, "neg-pd-link-fail"))
		sc_neg_pd_link_fail();
	else if (!strcmp(sc, "neg-getsync-fail"))
		sc_neg_getsync_fail();
	else if (!strcmp(sc, "pos-getsync-eacces"))
		sc_pos_getsync_eacces();
	else if (!strcmp(sc, "neg-getsync-eacces-one-pd"))
		sc_neg_getsync_eacces_one_pd();
	else if (!strcmp(sc, "pos-boot"))
		sc_pos_boot();
	else if (!strcmp(sc, "pos-boot-hello"))
		sc_pos_boot_hello();
	else if (!strcmp(sc, "neg-ps-stuck"))
		sc_neg_ps_stuck();
	else if (!strcmp(sc, "neg-mbox-silent"))
		sc_neg_mbox_silent();
	else if (!strcmp(sc, "neg-bad-pin"))
		sc_neg_bad_pin();
	else if (!strcmp(sc, "neg-foreign-preload"))
		sc_neg_foreign_preload();
	else if (!strcmp(sc, "neg-dart-map"))
		sc_neg_dart_map_partial();
	else if (!strcmp(sc, "neg-short-unmap"))
		sc_neg_short_unmap();
	else if (!strcmp(sc, "neg-unreserved-alias"))
		sc_neg_stage_domain(true);
	else if (!strcmp(sc, "neg-non-dma-domain"))
		sc_neg_stage_domain(false);
	else if (!strcmp(sc, "neg-preload-bss"))
		sc_neg_preload_bytes(0);
	else if (!strcmp(sc, "neg-preload-boundary"))
		sc_neg_preload_bytes(1);
	else if (!strcmp(sc, "neg-preload-snapshot"))
		sc_neg_preload_bytes(2);
	else if (!strcmp(sc, "neg-preload-sysram"))
		sc_neg_preload_sysram();
	else if (!strcmp(sc, "neg-no-domain"))
		sc_neg_no_domain();
	else if (!strcmp(sc, "neg-unknown-stage"))
		sc_neg_unknown_stage();
	else if (!strcmp(sc, "pos-h17-boot"))
		sc_pos_h17_boot();
	else if (!strcmp(sc, "pos-h17-irq"))
		sc_pos_h17_irq();
	else if (!strcmp(sc, "pos-h17-scratch"))
		sc_pos_h17_scratch();
	else if (!strcmp(sc, "pos-h17-table"))
		sc_h17_table(0);
	else if (!strcmp(sc, "neg-table-timeout"))
		sc_h17_table(1);
	else if (!strcmp(sc, "neg-table-overrun"))
		sc_h17_table(2);
	else if (!strcmp(sc, "neg-table-foreign"))
		sc_h17_table(3);
	else if (!strcmp(sc, "neg-table-padding"))
		sc_h17_table(4);
	else if (!strcmp(sc, "neg-table-envelope"))
		sc_h17_table(5);
	else if (!strcmp(sc, "neg-table-alloc"))
		sc_h17_table(6);
	else if (!strcmp(sc, "neg-table-dma-range"))
		sc_h17_table(7);
	else if (!strcmp(sc, "neg-table-heap-alloc"))
		sc_h17_table(8);
	else if (!strcmp(sc, "neg-table-scope"))
		sc_neg_table_scope();
	else if (!strcmp(sc, "neg-table-reservation"))
		sc_neg_table_reservation();
	else if (!strcmp(sc, "neg-adt-bounds"))
		sc_neg_adt_bounds();
	else if (!strcmp(sc, "pos-four-ps"))
		sc_pos_four_ps();
	else if (!strcmp(sc, "adt-file") && argc == 3)
		sc_adt_file(argv[2]);
	else {
		fprintf(stderr, "unknown scenario %s\n", sc);
		return 2;
	}
	printf("== %s: %s (%d failure%s) ==\n", sc, fails ? "FAIL" : "PASS",
	       fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
