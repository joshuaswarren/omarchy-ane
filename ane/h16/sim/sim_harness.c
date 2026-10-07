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
 *                      pos-boot-hello
 * Negative scenarios (module MUST refuse/fail, harness MUST observe it):
 *                      neg-ps-stuck neg-mbox-silent neg-bad-pin
 *                      neg-foreign-preload neg-unknown-stage

 */
#include <inttypes.h>

#include "shim.h"

/* the module's real source */
#include "../ane_h16_main.c"
#include "../ane_h16_soc.c"

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
int sim_dma_outstanding;
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

static void fw_write_hook(u64 pa, u32 val)
{
	u32 off = (u32)(pa - SIM_ENGINE_PA);

	if (pa == reg_pa(sim_soc.scratch0 + 7 * 4) && val == 0)
		fw.scratch7_cleared = true;
	else if (pa == reg_pa(sim_soc.cpu_control) && val == 0x10)
		fw.armed = true;
	else if (off == sim_soc.mbox + 0x808)
		fw_on_a2i_send((u8)val,
			       (u64)reg_rd(sim_soc.mbox + 0x800) |
			       ((u64)reg_rd(sim_soc.mbox + 0x804) << 32));
}

static void fw_read_hook(u64 pa)
{
	if (pa == reg_pa(sim_soc.scratch0 + 7 * 4) && fw.armed &&
	    !fw.wake_delivered) {
		if (++fw.polls >= fw.wake_after) {
			reg_wr(sim_soc.scratch0 + 7 * 4,
			       ANE_H16_WAKE_ACK);
			reg_wr(sim_soc.cpu_status, 0x2a);
			fw.wake_delivered = true;
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
		{ TEXT_PRE_PA, 0, TEXT_IOVA, 0xc0000 },
		{ DATA_PRE_PA, 0, DATA_IOVA, 0x2ac000 },
	};
	u32 ane_type = sim_soc.ane_type;

	/* root: no props, one child (arm-io) */
	put32(b, &off, 0);
	put32(b, &off, 1);
	/* arm-io: one prop, one child */
	put32(b, &off, 1);
	put32(b, &off, 1);
	put_prop(b, &off, "name", "arm-io\0", 8);
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

/* The iBoot preload: the file laid out at its vm addresses, plus the
 * differences a real preload is expected to carry (patchbay/tunables
 * windows, one DATA-base IOVA word, one TEXT-entry IOVA word).
 */
static void build_preloads(void)
{
	const struct ane_h16_fw *img = sim_soc.fw;
	u8 *data_pre = calloc(1, img->data_vmsize);
	u64 w;

	sim_text_pre = calloc(1, img->text_vmsize);
	memcpy(sim_text_pre, fwbuf + 0x4000, img->text_vmsize);
	memcpy(data_pre, fwbuf + img->data_fileoff, img->data_filesize);
	memset(data_pre + img->patchbay_vm - img->data_vm, 0x11, 0x261);
	memset(data_pre + img->tunables_vm - img->data_vm, 0x22, 0x6a0);
	w = DATA_IOVA;
	memcpy(data_pre + 0x100000, &w, 8);
	w = TEXT_IOVA;
	memcpy(data_pre + 0x200000, &w, 8);
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
	n_ane.nreg = 2;
	n_ane.reg[0][0] = SIM_ENGINE_PA;
	n_ane.reg[0][1] = 0x2000000;
	n_ane.reg[1][0] = SIM_PMGR_PA;
	n_ane.reg[1][1] = 0x18000;
	n_ane.props[n_ane.nprops++] = (struct sim_prop){
		"apple,ane-type", &sim_soc.ane_type, 4 };
	n_ane.props[n_ane.nprops++] = (struct sim_prop){
		"interrupts", irq_cells, sizeof(irq_cells) };
	n_ane.props[n_ane.nprops++] = (struct sim_prop){
		"iommus", iommu_cells, sizeof(iommu_cells) };

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
	for (i = 0; i < 5; i++)
		sim_regions[1].backing[sim_soc.ps_off[i] / 4] = 0xff;

	sim_fw_read_hook = fw_read_hook;
	sim_fw_write_hook = fw_write_hook;
	fw.wake_after = 3;
	sim_dart = &sim_dart_storage;

	optin = "t8132";
	ps_wait_ms = 500;
	boot_wait_ms = 3000;
	hello_wait_ms = 0;
	stage = "status";

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
	chk(ev_engine_count(false) > 0, "engine readout happened behind the guard");
	chk(sim_rpm_gets == sim_rpm_puts, "power claim released");
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
	    "SCRATCH7 cleared once, before CPU release");
	chk(ev_count(true, reg_pa(sim_soc.cpu_control), 0) == 1 &&
	    ev_count(true, reg_pa(sim_soc.cpu_control), 0x10) == 1,
	    "CPU_CONTROL 0 then 0x10");
	chk(ev_engine_count(false) > ev_engine_count(true),
	    "more reads than writes");
	chk(sim_dart_storage.n == 48 + 171,
	    "staged image mapped at the ADT IOVAs (219 pages)");
	chk(sim_log_count("do NOT unload") == 1,
	    "post-start pin warning printed");
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
	chk(ret == -ETIMEDOUT, "probe returns -ETIMEDOUT");
	chk(sim_log_count("verdict=FAIL reason=boot-timeout") == 1,
	    "RESULT FAIL boot-timeout");
	chk(sim_log_count("reboot before retrying") == 1,
	    "reboot-required message");
	chk(sim_dart_storage.n > 0, "mapping LEFT in place (no unwind)");
	chk(sim_dma_outstanding == 1, "staged buffer not freed");
	chk(sim_log_count("do NOT unload") == 0,
	    "no firmware-started claim (it did not start)");
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
	chk(ev_engine_count(true) == 0, "zero engine writes on refusal");
	chk(sim_dart_storage.n == 0, "nothing mapped");
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
	chk(ev_engine_count(true) == 0, "zero engine writes on refusal");
	chk(sim_dart_storage.n == 0, "nothing mapped");
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

	if (argc != 2) {
		fprintf(stderr, "usage: sim_harness <scenario>\n");
		return 2;
	}
	sc = argv[1];
	printf("== ane_h16 sim: %s ==\n", sc);
	if (!strcmp(sc, "pos-dt"))
		sc_pos_dt();
	else if (!strcmp(sc, "pos-dt-noadt"))
		sc_pos_dt_noadt();
	else if (!strcmp(sc, "pos-status"))
		sc_pos_status();
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
	else if (!strcmp(sc, "neg-unknown-stage"))
		sc_neg_unknown_stage();
	else {
		fprintf(stderr, "unknown scenario %s\n", sc);
		return 2;
	}
	printf("== %s: %s (%d failure%s) ==\n", sc, fails ? "FAIL" : "PASS",
	       fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
