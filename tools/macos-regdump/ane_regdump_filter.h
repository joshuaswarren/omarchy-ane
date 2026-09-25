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

#endif
