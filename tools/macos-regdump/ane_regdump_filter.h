/* Shared read policy for the T6021 ANE capture. Host-testable: no kernel
 * headers, plain C. The kext and the CLI both include it.
 *
 * Two facts bound what a dump may touch:
 *  - engine+0x1400818 / +0x140081c / +0x1400820 are the ASC wrapper IRQ
 *    event queue. A read pops an event the firmware never sees.
 *  - the ASC mailbox FIFOs pop on read too. The host reads I2A_RECV0/1
 *    (engine+0x1408830/0x1408838) to drain the coprocessor's outbox, and
 *    A2I_RECV0/1 (engine+0x1408810/0x1408818) is the coprocessor's own
 *    inbox. Either read steals a message from the live macOS driver.
 *  - engine+0x1010000 is CoreSight. Fused off on T6021, and never touched.
 *
 * The engine window itself is only readable while every ANE pmgr island
 * reads ACTUAL = 0xf in bits [7:4]. A read with an island down wedges the
 * fabric. pmgr and DRAM carveout reads are always safe.
 */
#ifndef ANE_REGDUMP_FILTER_H
#define ANE_REGDUMP_FILTER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define ANE_ENGINE_PHYS     0x284000000ull
#define ANE_ENGINE_LEN      0x02000000ull
#define ANE_CORESIGHT_OFF   0x01010000ull
#define ANE_CORESIGHT_LEN   0x00010000ull

#define ANE_PMGR_PHYS       0x28e080000ull
#define ANE_PMGR_LEN        0x00010000ull

#define ANE_DART0_PHYS      0x285800000ull
#define ANE_DART_STRIDE     0x00010000ull
#define ANE_DART_COUNT      3u

/* Words a read must never touch, as engine-window offsets. */
#define ANE_BAN_IRQ_POP0    0x01400818ull
#define ANE_BAN_IRQ_POP1    0x0140081cull
#define ANE_BAN_IRQ_POP2    0x01400820ull
#define ANE_BAN_A2I_RECV0   0x01408810ull
#define ANE_BAN_A2I_RECV1   0x01408818ull
#define ANE_BAN_I2A_RECV0   0x01408830ull
#define ANE_BAN_I2A_RECV1   0x01408838ull

/* Eight ANE pmgr power-state words. ACTUAL is bits [7:4]. */
#define ANE_ISLAND_COUNT    8u
static const uint32_t ane_island_off[ANE_ISLAND_COUNT] = {
	0x2e0, 0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x4030
};

/* A word is banned when any byte of it overlaps a banned word or the
 * CoreSight window. `off` is an engine-window offset, `nbytes` 1..8. */
static inline int ane_word_banned(uint64_t off, uint32_t nbytes)
{
	static const uint64_t ban[] = {
		ANE_BAN_IRQ_POP0, ANE_BAN_IRQ_POP1, ANE_BAN_IRQ_POP2,
		ANE_BAN_A2I_RECV0, ANE_BAN_A2I_RECV1,
		ANE_BAN_I2A_RECV0, ANE_BAN_I2A_RECV1
	};
	uint64_t end = off + nbytes;
	size_t i;

	if (off < ANE_CORESIGHT_OFF + ANE_CORESIGHT_LEN &&
	    end > ANE_CORESIGHT_OFF)
		return 1;
	for (i = 0; i < sizeof(ban) / sizeof(ban[0]); i++)
		if (off < ban[i] + 4 && end > ban[i])
			return 1;
	return 0;
}

/* True when every island word reads ACTUAL = 0xf. */
static inline int ane_islands_up(const uint32_t ps[ANE_ISLAND_COUNT])
{
	size_t i;

	for (i = 0; i < ANE_ISLAND_COUNT; i++)
		if ((ps[i] & 0xf0u) != 0xf0u)
			return 0;
	return 1;
}

/* What the CLI may change without a new kext. The kernel still refuses
 * a gate the idle word would pass, still skips pop-on-read words, and
 * still treats every engine-window range as gated. */
#define ANE_REQ_MAGIC       0x414e4552u  /* 'ANER' */
#define ANE_REQ_VERSION     1u
#define ANE_REQ_RANGE_MAX   40u
#define ANE_POLL_DEFAULT_US 2000000u
#define ANE_POLL_CAP_US     10000000u
#define ANE_GATE_MIN_MASK   0xf0u
#define ANE_GATE_MIN_WANT   0xf0u
#define ANE_REQ_F_HANDOFF   1u
#define ANE_REQ_F_ADT       2u
#define ANE_IDLE_WORD       0x00000300u
#define ANE_IDLE_CPU_WORD   0x0f000300u

struct ane_req_range {
	char     name[24];
	uint64_t pa;
	uint32_t len;
	uint32_t flags; /* bit 0: caller asks for the gate; kernel may force it */
};

struct ane_req {
	uint32_t magic;
	uint32_t version;
	uint32_t poll_us;
	uint32_t nranges;
	uint64_t pmgr_pa;
	uint32_t pmgr_len;
	uint32_t n_islands;
	uint32_t gate_mask;
	uint32_t gate_want;
	uint32_t flags;
	uint32_t island_off[ANE_ISLAND_COUNT];
	struct ane_req_range range[ANE_REQ_RANGE_MAX];
};

/* True when [pa, pa+len) overlaps the engine window. */
static inline int ane_overlaps_engine(uint64_t pa, uint32_t len)
{
	uint64_t end = pa + len;

	return pa < ANE_ENGINE_PHYS + ANE_ENGINE_LEN && end > ANE_ENGINE_PHYS;
}

/* True when [pa, pa+len) overlaps CoreSight. Never map that page. */
static inline int ane_overlaps_coresight(uint64_t pa, uint32_t len)
{
	uint64_t cs = ANE_ENGINE_PHYS + ANE_CORESIGHT_OFF;
	uint64_t end = pa + len;

	return pa < cs + ANE_CORESIGHT_LEN && end > cs;
}

/* Engine-window ranges are gated no matter what the caller asks. */
static inline int ane_range_must_gate(const struct ane_req_range *r)
{
	return (r->flags & 1) || ane_overlaps_engine(r->pa, r->len);
}

/* The caller's predicate must be at least as strict as ACTUAL=0xf, and
 * it must reject the two idle words this chip has already returned. */
static inline int ane_gate_predicate_ok(uint32_t mask, uint32_t want)
{
	if ((mask & ANE_GATE_MIN_MASK) != ANE_GATE_MIN_MASK)
		return 0;
	if ((want & ANE_GATE_MIN_WANT) != ANE_GATE_MIN_WANT)
		return 0;
	if ((ANE_IDLE_WORD & mask) == want)
		return 0;
	if ((ANE_IDLE_CPU_WORD & mask) == want)
		return 0;
	return 1;
}

static inline int ane_words_pass(const uint32_t *ps, uint32_t n,
    uint32_t mask, uint32_t want)
{
	uint32_t i;

	if (!n || n > ANE_ISLAND_COUNT)
		return 0;
	for (i = 0; i < n; i++)
		if ((ps[i] & mask) != want)
			return 0;
	return 1;
}

/* 0 when the request is safe to act on. */
static inline int ane_req_acceptable(const struct ane_req *r)
{
	uint32_t i;
	uint64_t pmgr_end;

	if (!r || r->magic != ANE_REQ_MAGIC || r->version != ANE_REQ_VERSION)
		return 0;
	if (r->poll_us > ANE_POLL_CAP_US)
		return 0;
	if (!r->n_islands || r->n_islands > ANE_ISLAND_COUNT)
		return 0;
	if (!ane_gate_predicate_ok(r->gate_mask, r->gate_want))
		return 0;
	if (!r->pmgr_len || r->pmgr_len > 0x100000u || (r->pmgr_len & 3))
		return 0;
	if (ane_overlaps_engine(r->pmgr_pa, r->pmgr_len))
		return 0;
	pmgr_end = r->pmgr_pa + r->pmgr_len;
	if (pmgr_end < r->pmgr_pa)
		return 0;
	for (i = 0; i < r->n_islands; i++) {
		if ((r->island_off[i] & 3) ||
		    r->island_off[i] > r->pmgr_len - 4)
			return 0;
	}
	if (r->nranges > ANE_REQ_RANGE_MAX)
		return 0;
	for (i = 0; i < r->nranges; i++) {
		const struct ane_req_range *g = &r->range[i];
		uint64_t end = g->pa + g->len;

		if (!g->len || (g->len & 3) || (g->pa & 3) || g->len > 0x1000000u)
			return 0;
		if (end < g->pa)
			return 0;
		if (ane_overlaps_coresight(g->pa, g->len))
			return 0;
	}
	return 1;
}

static inline void ane_req_default(struct ane_req *r)
{
	static const struct ane_req_range def[] = {
		{ "pmgr-ps",  ANE_PMGR_PHYS,              0x4040,  0 },
		{ "wrapper",  ANE_ENGINE_PHYS + 0x1400000, 0x14000, 1 },
		{ "mailbox",  ANE_ENGINE_PHYS + 0x1408000, 0x1000,  1 },
		{ "rvbar",    ANE_ENGINE_PHYS + 0x1050000, 0x8,     1 },
		{ "scratch",  ANE_ENGINE_PHYS + 0x1840048, 0x28,    1 },
		{ "dart0",    ANE_DART0_PHYS,              0x2000,  1 },
		{ "dart1",    ANE_DART0_PHYS + ANE_DART_STRIDE, 0x2000, 1 },
		{ "dart2",    ANE_DART0_PHYS + 2 * ANE_DART_STRIDE, 0x2000, 1 },
		{ "patchbay", 0x10001406870ull,            0x40,    0 },
	};
	uint32_t i;

	memset(r, 0, sizeof(*r));
	r->magic = ANE_REQ_MAGIC;
	r->version = ANE_REQ_VERSION;
	r->poll_us = ANE_POLL_DEFAULT_US;
	r->pmgr_pa = ANE_PMGR_PHYS;
	r->pmgr_len = ANE_PMGR_LEN;
	r->n_islands = ANE_ISLAND_COUNT;
	r->gate_mask = ANE_GATE_MIN_MASK;
	r->gate_want = ANE_GATE_MIN_WANT;
	r->flags = ANE_REQ_F_HANDOFF | ANE_REQ_F_ADT;
	for (i = 0; i < ANE_ISLAND_COUNT; i++)
		r->island_off[i] = ane_island_off[i];
	r->nranges = sizeof(def) / sizeof(def[0]);
	for (i = 0; i < r->nranges; i++)
		r->range[i] = def[i];
}

#endif
