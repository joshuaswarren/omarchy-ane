/*
 * BMN-04 draft: the smallest ANE MMIO compute on T8103.
 *
 * Chainloaded in RAM by the lab aura/m1n1 loader over the KIS
 * debug-USB path (no OS, no ASC firmware — T8103 drives the ANE
 * through the host task manager, omarchy-ane docs section 1 of
 * t6021-ane-bringup-findings.md). Powers the PMGR chain, maps five
 * buffers through the three Gen1 ANE DARTs, enqueues the h13-anec
 * add [1,64,1,1] program on queue 4, polls the IRQ event lines for
 * completion, and prints the output word plus the 64 valid lanes
 * on the debug uart.
 *
 * NEVER RUN without Main's same-hour word: this is a lab-Mac boot
 * payload under the 2026-11-28 boot-change rule. Build and stage only.
 */

#include <stdint.h>
#include <stddef.h>
#include "regs.h"

#define LO32(x)		((uint32_t)(uintptr_t)(x))

/* ---------- MMIO ---------- */

static inline void mmio_w(uintptr_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(base + off) : "memory");
}
static inline uint32_t mmio_r(uintptr_t base, uint32_t off)
{
	uint32_t v;
	__asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(base + off) : "memory");
	return v;
}

static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

static unsigned long cache_line(void)
{
	unsigned long ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	return 16UL << ((ctr >> 16) & 0xf);	/* DminLine: log2(words) */
}

static void dc_op_range(void *p, size_t n, int clean)
{
	uintptr_t s = (uintptr_t)p & ~(cache_line() - 1);
	uintptr_t e = (uintptr_t)p + n;
	for (; s < e; s += cache_line()) {
		if (clean)
			__asm__ volatile("dc cvac, %0" :: "r"(s) : "memory");
		else
			__asm__ volatile("dc ivac, %0" :: "r"(s) : "memory");
	}
	dsb();
}

/* ---------- uart ---------- */

#ifndef UART_BASE
#define UART_BASE 0x235200000UL
#endif

static void uart_putc(char c)
{
	uint64_t n = 0;
	while (!(mmio_r(UART_BASE, UART_UTRSTAT) & UART_UTRSTAT_TXE)) {
		if (++n > (1UL << 22))
			return;			/* uart absent/hung: drop */
	}
	mmio_w(UART_BASE, UART_UTXH, (uint8_t)c);
}

static void puts_(const char *s)
{
	while (*s) {
		if (*s == '\n')
			uart_putc('\r');
		uart_putc(*s++);
	}
}
static void put_hex(uint64_t v, int digits)
{
	const char *h = "0123456789abcdef";
	for (int i = digits - 1; i >= 0; i--)
		uart_putc(h[(v >> (i * 4)) & 0xf]);
}
static void put_u32(uint32_t v) { put_hex(v, 8); }
static void put_u16(uint16_t v) { put_hex(v, 4); }
static void put_dec(uint32_t v)
{
	char b[10];
	int i = 0;
	do { b[i++] = '0' + v % 10; v /= 10; } while (v);
	while (i) uart_putc(b[--i]);
}

/* ---------- libc bits ---------- */

static void *memcpy_(void *d, const void *s, size_t n)
{
	uint8_t *dd = d;
	const uint8_t *ss = s;
	for (size_t i = 0; i < n; i++) dd[i] = ss[i];
	return d;
}
static void *memset_(void *d, int c, size_t n)
{
	uint8_t *dd = d;
	for (size_t i = 0; i < n; i++) dd[i] = (uint8_t)c;
	return d;
}

/* ---------- buffers (bss; VA == PA under the flat map) ---------- */

__attribute__((aligned(4096))) static uint64_t dart_mid[512];
__attribute__((aligned(4096))) static uint64_t dart_leaf[512];

__attribute__((aligned(4096))) static uint8_t buf_btsp[4096];
__attribute__((aligned(4096))) static uint8_t buf_cmd[20480];
__attribute__((aligned(4096))) static uint8_t buf_out[16384];
__attribute__((aligned(4096))) static uint8_t buf_in0[16384];
__attribute__((aligned(4096))) static uint8_t buf_in1[16384];

/* iova layout: all inside ttbr[0]/mid[0] */
#define IOVA_BTSP	0x00010000U
#define IOVA_CMD	0x00011000U
#define IOVA_OUT	0x00016000U
#define IOVA_IN0	0x0001a000U
#define IOVA_IN1	0x0001e000U

/* ---------- anec fixture ---------- */

extern const uint8_t anec_blob_start[], anec_blob_end[];

struct anec_hdr {
	uint64_t size;
	uint32_t td_size;
	uint32_t td_count;
	uint64_t tsk_size;
	uint64_t krn_size;
	uint32_t src_count;
	uint32_t dst_count;
	uint32_t tiles[0x20];
	uint64_t nchw[0x20][6];
};

#define ANEC_HEADER_BYTES	4096	/* on-disk header page */
#define ANEC_WANT_SIZE		16896
#define ANEC_WANT_TD_SIZE	504
#define ANEC_WANT_TD_COUNT	1
#define ANEC_WANT_TSK_SIZE	504
#define ANEC_WANT_KRN_SIZE	16384

/* ---------- fp16 fixture vectors ----------
 * Canonical h13 smoke inputs (packaging/omarchy-ane-smoke):
 *   a[j] = fp16((3 +  j    % 11)/4)   j=0..63
 *   b[j] = fp16((3 + (3*j) % 11)/4)
 *   e[j] = fp16(a+b) = fp16((6 + j%11 + (3j)%11)/4), exact quarter.
 * Lane j sits in the first two bytes of its 64-byte plane. */
static const uint16_t Q3[11] = {		/* fp16((3+k)/4) */
	0x3a00, 0x3c00, 0x3d00, 0x3e00, 0x3f00, 0x4000,
	0x4080, 0x4100, 0x4180, 0x4200, 0x4280,
};
static const uint16_t Q6[21] = {		/* fp16((6+m)/4) */
	0x3e00, 0x3f00, 0x4000, 0x4080, 0x4100, 0x4180, 0x4200,
	0x4280, 0x4300, 0x4380, 0x4400, 0x4440, 0x4480, 0x44c0,
	0x4500, 0x4540, 0x4580, 0x45c0, 0x4600, 0x4640, 0x4680,
};
#define LANE_BYTES 64
#define NLANES 64

static void fill_inputs(void)
{
	memset_(buf_in0, 0, sizeof buf_in0);
	memset_(buf_in1, 0, sizeof buf_in1);
	memset_(buf_out, 0, sizeof buf_out);
	for (int j = 0; j < NLANES; j++) {
		*(uint16_t *)(buf_in0 + j * LANE_BYTES) = Q3[j % 11];
		*(uint16_t *)(buf_in1 + j * LANE_BYTES) = Q3[(3 * j) % 11];
	}
}

static uint16_t expected_lane(int j)
{
	return Q6[(j % 11) + ((3 * j) % 11)];
}

/* ---------- pmgr ---------- */

static int pmgr_on(uint32_t off)
{
	uint32_t w = mmio_r(PMGR_BASE, off);
	w &= ~(PMGR_DEV_DISABLE | PMGR_PS_RESET | PMGR_AUTO_ENABLE |
	       PMGR_PS_FLAGS | PMGR_BUSY | PMGR_PS_TARGET);
	w |= PMGR_PS_ACTIVE;			/* target */
	mmio_w(PMGR_BASE, off, w);
	for (uint64_t t = 0; t < (1UL << 24); t++) {
		w = mmio_r(PMGR_BASE, off);
		if ((w & PMGR_PS_ACTUAL) == (PMGR_PS_ACTIVE << 4))
			return w & PMGR_BUSY ? -2 : 0;	/* busy latch note */
	}
	return -1;
}

static int power_chain(void)
{
	for (unsigned i = 0; i < sizeof(PMGR_CHAIN) / sizeof(PMGR_CHAIN[0]); i++) {
		int rc = pmgr_on(PMGR_CHAIN[i]);
		puts_("  pmgr 0x");
		put_hex(PMGR_CHAIN[i], 5);
		puts_(" word 0x");
		put_u32(mmio_r(PMGR_BASE, PMGR_CHAIN[i]));
		if (rc) {
			puts_(" rc=");
			put_dec((uint32_t)(-rc));
			puts_("\n");
			return -1;
		}
		puts_("\n");
	}
	return 0;
}

/* ---------- dart (Gen1) ----------
 * iova[31:30] -> ttbr; iova[29:21] -> mid; iova[20:12] -> leaf;
 * [11:0] -> offset.  All our iovas use ttbr[0], mid[0]. */

static void dart_map(uint32_t iova, uintptr_t pa)
{
	uint32_t leaf_idx = (iova >> 12) & 0x1ff;
	dart_leaf[leaf_idx] = (pa & DART_PTE_ADDR_MASK) |
			      DART_PTE_SUBPAGE_END | DART_PTE_VALID;
}

static int dart_setup(uintptr_t base)
{
	mmio_w(base, DART_TTBR + 0 * 4,
	       DART_TTBR_VALID | (LO32(dart_mid) >> DART_TTBR_SHIFT));
	mmio_w(base, DART_TCR + 0 * 4, DART_TCR_TRANSLATE);
	mmio_w(base, DART_STREAMS_ENABLE,
	       mmio_r(base, DART_STREAMS_ENABLE) | 1U);
	dsb();
	mmio_w(base, DART_STREAM_SELECT, 1U);
	mmio_w(base, DART_STREAM_COMMAND, DART_STREAM_CMD_INVAL);
	for (uint64_t t = 0; t < (1UL << 20); t++) {
		if (!(mmio_r(base, DART_STREAM_COMMAND) &
		      DART_STREAM_CMD_BUSY))
			return 0;
	}
	return -1;
}

static int dart_faulted(uintptr_t base)
{
	return mmio_r(base, DART_ERROR) & DART_ERROR_FLAG;
}

/* ---------- engine ---------- */

static void tm_enable(void)
{
	mmio_w(ANE_TM, TM_TQ_EN, mmio_r(ANE_TM, TM_TQ_EN) | 0x1000);
	for (int q = 0; q < ANE_TQ_COUNT; q++)
		mmio_w(ANE_TQ, TQ_PRTY(q), TQ_PRTY_TABLE[q]);
	mmio_w(ANE_TM, TM_IRQ_EN1, 0x4000000);
	mmio_w(ANE_TM, TM_IRQ_EN2, 0x6);
	dsb();
}

#define EXPECT_INFO	(0x05000000U | (ANE_FIFO_NID << 16))

/* Drain one IRQ event line; return nonzero if a line's count overflowed. */
static int drain_events(uint32_t *finished, uint32_t want_info,
			uint32_t td_count)
{
	for (int line = 0; line < 2; line++) {
		uint32_t count = mmio_r(ANE_TM, TM_IRQ_EVTC(line));
		if (count > 64)
			return -1;
		for (uint32_t n = 0; n < count; n++) {
			uint32_t info = mmio_r(ANE_TM, TM_IRQ_INFO(line));
			mmio_r(ANE_TM, TM_IRQ_UNK1(line));
			mmio_r(ANE_TM, TM_IRQ_TMST(line));
			mmio_r(ANE_TM, TM_IRQ_UNK2(line));
			if (info == (want_info | (td_count - 1)))
				*finished |= 1U << line;
		}
	}
	mmio_w(ANE_TM, TM_IRQ_ACK, mmio_r(ANE_TM, TM_IRQ_ACK) | 2);
	return 0;
}

int payload_main(void)
{
	puts_("\nbmn04 ane-add t8103 payload\n");

	/* fixture geometry gate */
	const struct anec_hdr *h = (const void *)anec_blob_start;
	puts_("anec size=");
	put_dec((uint32_t)h->size);
	puts_(" td_size=");
	put_dec(h->td_size);
	puts_(" td_count=");
	put_dec(h->td_count);
	puts_(" tsk_size=");
	put_dec((uint32_t)h->tsk_size);
	puts_(" krn_size=");
	put_dec((uint32_t)h->krn_size);
	puts_("\n");
	if (h->size != ANEC_WANT_SIZE || h->td_size != ANEC_WANT_TD_SIZE ||
	    h->td_count != ANEC_WANT_TD_COUNT ||
	    h->tsk_size != ANEC_WANT_TSK_SIZE ||
	    h->krn_size != ANEC_WANT_KRN_SIZE ||
	    (uint64_t)(anec_blob_end - anec_blob_start) <
	    ANEC_HEADER_BYTES + h->size) {
		puts_("fixture geometry mismatch; abort\n");
		return -1;
	}

	/* 1. power chain, parents first */
	puts_("pmgr chain:\n");
	if (power_chain()) {
		puts_("power-up failed; abort\n");
		return -1;
	}

	/* 2. dart tables: leaf pages for the five buffers, one shared
	 *    mid table, ttbr[0] on each of the three darts */
	for (uint32_t i = 0; i < 1; i++)
		dart_map(IOVA_BTSP + i * 4096, (uintptr_t)buf_btsp + i * 4096);
	for (uint32_t i = 0; i < 5; i++)
		dart_map(IOVA_CMD + i * 4096, (uintptr_t)buf_cmd + i * 4096);
	for (uint32_t i = 0; i < 4; i++)
		dart_map(IOVA_OUT + i * 4096, (uintptr_t)buf_out + i * 4096);
	for (uint32_t i = 0; i < 4; i++)
		dart_map(IOVA_IN0 + i * 4096, (uintptr_t)buf_in0 + i * 4096);
	for (uint32_t i = 0; i < 4; i++)
		dart_map(IOVA_IN1 + i * 4096, (uintptr_t)buf_in1 + i * 4096);
	dart_mid[0] = ((uintptr_t)dart_leaf & DART_PTE_ADDR_MASK) |
		      DART_PTE_SUBPAGE_END | DART_PTE_VALID;
	dc_op_range(dart_leaf, sizeof dart_leaf, 1);
	dc_op_range(dart_mid, sizeof dart_mid, 1);

	static const uintptr_t darts[3] = { DART0, DART1, DART2 };
	for (int i = 0; i < 3; i++) {
		if (dart_setup(darts[i])) {
			puts_("dart invalidate timed out on dart");
			put_dec(i);
			puts_("; abort\n");
			return -1;
		}
	}
	puts_("darts: 3x sid0 ttbr+tcr+enable+invalidate\n");

	/* 3. enable the task manager */
	tm_enable();
	puts_("tm: TQ_EN |= 0x1000; prty table; irq en\n");

	/* 4. stage buffers */
	memcpy_(buf_btsp, anec_blob_start + ANEC_HEADER_BYTES, h->td_size);
	uint32_t td0 = *(uint32_t *)buf_btsp;
	*(uint32_t *)buf_btsp = (td0 & ~(0xffU << 16)) | (ANE_FIFO_NID << 16);
	memcpy_(buf_cmd, anec_blob_start + ANEC_HEADER_BYTES, h->size);
	fill_inputs();
	dc_op_range(buf_btsp, sizeof buf_btsp, 1);
	dc_op_range(buf_cmd, sizeof buf_cmd, 1);
	dc_op_range(buf_in0, sizeof buf_in0, 1);
	dc_op_range(buf_in1, sizeof buf_in1, 1);
	dc_op_range(buf_out, sizeof buf_out, 1);
	isb();

	/* 5. enqueue q4, push q4 (ane_tm_enqueue + ane_tm_push_tq) */
	uint32_t size1 = ((h->td_size >> 2) - 1) << 16;
	uint32_t bar1 = IOVA_CMD + ((h->tsk_size + ANE_CMD_GRAN - 1) &
				    ~(ANE_CMD_GRAN - 1));

	uint32_t finished = 0;
	if (drain_events(&finished, EXPECT_INFO, h->td_count)) {  /* stale */
		puts_("event overflow pre-push; abort\n");
		return -1;
	}
	finished = 0;

	mmio_w(ANE_TQ, TQ_STATUS(QID), 0x1);
	for (int b = 0; b < ANE_TILE_COUNT; b++)
		mmio_w(ANE_TQ, TQ_BAR1(QID, b), 0);
	mmio_w(ANE_TQ, TQ_BAR1(QID, CMD_BUF_BDX), IOVA_CMD);
	mmio_w(ANE_TQ, TQ_BAR1(QID, KRN_BUF_BDX), bar1);
	mmio_w(ANE_TQ, TQ_BAR1(QID, OUT_BDX), IOVA_OUT);
	mmio_w(ANE_TQ, TQ_BAR1(QID, IN0_BDX), IOVA_IN0);
	mmio_w(ANE_TQ, TQ_BAR1(QID, IN1_BDX), IOVA_IN1);
	mmio_w(ANE_TQ, TQ_SIZE1(QID), size1);
	mmio_w(ANE_TQ, TQ_ADDR1(QID), IOVA_BTSP);
	mmio_w(ANE_TQ, TQ_NID1(QID), (ANE_FIFO_NID << 8) | 1);
	dsb();

	puts_("enqueue q4: bar0=0x");
	put_u32(IOVA_CMD);
	puts_(" bar1=0x");
	put_u32(bar1);
	puts_(" size1=0x");
	put_u32(size1);
	puts_(" btsp=0x");
	put_u32(IOVA_BTSP);
	puts_(" nid1=0x");
	put_u32((ANE_FIFO_NID << 8) | 1);
	puts_("\n");

	mmio_w(ANE_TM, TM_ADDR, mmio_r(ANE_TQ, TQ_ADDR1(QID)));
	mmio_w(ANE_TM, TM_INFO,
	       mmio_r(ANE_TQ, TQ_SIZE1(QID)) | h->td_count);
	mmio_w(ANE_TM, TM_PUSH,
	       TQ_PRTY_TABLE[QID] | ((QID & 7) << 8));
	puts_("pushed\n");

	/* 6. poll both event lines + idle */
	int done = 0;
	uint64_t spins = 0;
	for (; spins < (1UL << 27); spins++) {
		for (int i = 0; i < 3; i++) {
			if (dart_faulted(darts[i])) {
				puts_("dart fault on dart");
				put_dec(i);
				puts_(" addr 0x");
				put_u32(mmio_r(darts[i], DART_ERROR_ADDR_HI));
				put_u32(mmio_r(darts[i], DART_ERROR_ADDR_LO));
				puts_(" status 0x");
				put_u32(mmio_r(darts[i], DART_ERROR));
				puts_("; abort\n");
				return -1;
			}
		}
		if (drain_events(&finished, EXPECT_INFO, h->td_count)) {
			puts_("event overflow; abort\n");
			return -1;
		}
		if (finished == 3 && (mmio_r(ANE_TM, TM_STATUS) & TM_IS_IDLE)) {
			done = 1;
			break;
		}
	}
	puts_("tm_status=0x");
	put_u32(mmio_r(ANE_TM, TM_STATUS));
	puts_(" errors 0x");
	put_u32(mmio_r(ANE_TM, TM_ERROR1));
	puts_(" 0x");
	put_u32(mmio_r(ANE_TM, TM_ERROR2));
	puts_(" 0x");
	put_u32(mmio_r(ANE_TM, TM_ERROR3));
	puts_(" finished=");
	put_dec(finished);
	puts_("\n");
	if (!done) {
		puts_("poll timed out; abort\n");
		return -1;
	}

	/* 7. read back the output tile */
	dc_op_range(buf_out, sizeof buf_out, 0);
	uint32_t bad = 0;
	for (int j = 0; j < NLANES; j++) {
		uint16_t got = *(uint16_t *)(buf_out + j * LANE_BYTES);
		if (got != expected_lane(j))
			bad++;
	}
	puts_("lane0 out=0x");
	put_u16(*(uint16_t *)buf_out);
	puts_(" want 0x");
	put_u16(expected_lane(0));
	puts_("  mismatched lanes: ");
	put_dec(bad);
	puts_("/64\n");

	/* the 128 valid-lane bytes for the host-side sha256 */
	puts_("RESULT_VALID=");
	for (int j = 0; j < NLANES; j++) {
		uint16_t got = *(uint16_t *)(buf_out + j * LANE_BYTES);
		put_hex(got & 0xff, 2);
		put_hex(got >> 8, 2);
	}
	puts_("\n");

	puts_(bad ? "ANE_ADD_RESULT FAIL\n" : "ANE_ADD_RESULT PASS\n");
	return bad ? 1 : 0;
}
