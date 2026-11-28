/* SPDX-License-Identifier: MIT */
/*
 * bmn03-payload.c — BMN-03 bare-metal ANE power-up proof for t8103 (jwm1).
 *
 * Runs as the appended kernel payload of a one-shot aura trial stage2.
 * MMU is off, no caches needed for ordering, the 178 s trial watchdog is
 * armed behind us. We must be freestanding: no globals, no rodata, no libc —
 * every reference is a pc-relative branch or a hardcoded physical address.
 *
 * What it does, in order:
 *   1. Writes a live-progress marker into the reserved isp-heap DRAM hole so
 *      a later Linux boot can read back exactly where the payload got to.
 *   2. Raises all eight ANE power domains (PMGR PS registers, t8103-pinned
 *      offsets taken from jwm1's live Linux device tree).
 *   3. Records each PS register's post-write value plus a per-domain
 *      ACTUAL==PS_ACTIVE bitmask.
 *   4. Probes the ANE engine MMIO window: 4 scratch-register reads, then a
 *      write/readback on SCRATCH0. If any read faults, the CPU takes a sync
 *      exception with no handler and hangs; the trial watchdog resets, and
 *      the marker still holds the PMGR results.
 *   5. Seals the marker and WFE-loops. The trial watchdog resets the box;
 *      the spent aura-try marker sends the next boot down the stock path.
 *
 * Result channel (32-bit words at MARKER_BASE = 0xbd2bc0000, inside jwm1's
 * Linux-reserved isp-heap region 0xbd2bb0000+0x1ac000):
 *   +0x00 magic "BMN3"          +0x04 layout version (1)
 *   +0x08 cntpct at start (u64) +0x10 reserved
 *   +0x14 stage (1=entered, 2=powered, 3=probed, 4=sealed)
 *   +0x18 ps_ok bitmask (bit i: domain i polled ACTUAL==0xf)
 *   +0x20..0x3f eight PS register readbacks
 *   +0x40..0x4f four scratch-register reads (0x26b840048..0x54)
 *   +0x50 scratch-write probe readback
 *   +0x54 seal magic 0xdeadbee5
 */

typedef unsigned int u32;
typedef unsigned long long u64;

#define PMGR_PS_BASE 0x23b700000ULL

/* PMGR PS register fields (aura src/pmgr.c) */
#define PMGR_PS_TARGET_ACTIVE 0xf
#define PMGR_PS_ACTUAL_MASK   0xf0
#define PMGR_PS_ACTUAL_ACTIVE 0xf0
#define PMGR_WRITE_MASK                                                                       \
    ((1u << 28) /* AUTO_ENABLE */ | (1u << 9) /* WAS_CLKGATED */ | (1u << 8) /* WAS_PWRGATED */ \
     | 0xf /* TARGET */)

#define MARKER_BASE 0xbd2bc0000ULL
#define ANE_SCRATCH0 0x26b840048ULL

#define POLL_SPINS 0x200000 /* ~seconds at uncached speeds; far over PMGR's 10 ms */

static inline u32 rd(u64 addr)
{
    u32 v = *(volatile u32 *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static inline void wr(u64 addr, u32 v)
{
    *(volatile u32 *)addr = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline u64 cntpct(void)
{
    u64 v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* PMGR power-on of one domain at byte offset `off` from PMGR_PS_BASE:
 * clear AUTO_ENABLE|WAS_*|TARGET, set TARGET=PS_ACTIVE, poll ACTUAL.
 * Returns the final register value. */
static u32 ps_raise(u32 off, u32 *ok_shift)
{
    u64 a = PMGR_PS_BASE + off;
    u32 v = rd(a);

    if ((v & PMGR_PS_ACTUAL_MASK) != PMGR_PS_ACTUAL_ACTIVE) {
        v &= ~PMGR_WRITE_MASK;
        v |= PMGR_PS_TARGET_ACTIVE;
        wr(a, v);

        u32 spins = POLL_SPINS;
        while (spins--) {
            v = rd(a);
            if ((v & PMGR_PS_ACTUAL_MASK) == PMGR_PS_ACTUAL_ACTIVE)
                break;
        }
    }
    if ((v & PMGR_PS_ACTUAL_MASK) == PMGR_PS_ACTUAL_ACTIVE)
        *ok_shift = 1;
    return v;
}

static u32 raise_one(u32 off, u32 i)
{
    u32 ok = 0;
    u32 v = ps_raise(off, &ok);
    wr(MARKER_BASE + 0x20 + i * 4, v);
    return ok << i;
}

void payload_main(void)
{
    wr(MARKER_BASE + 0x00, 0x424d4e33); /* "BMN3" */
    wr(MARKER_BASE + 0x04, 1);
    u64 t0 = cntpct();
    wr(MARKER_BASE + 0x08, (u32)t0);
    wr(MARKER_BASE + 0x0c, (u32)(t0 >> 32));
    wr(MARKER_BASE + 0x14, 1);

    /* ps register offsets, t8103-pinned (jwm1 live DT):
     *   ane_sys +0x470 (root), ane_sys_cpu +0xc000, ane_base +0xc008,
     *   ane_set1..5 +0xc010..0xc030 */
    u32 ok_mask = raise_one(0x470, 0) | raise_one(0xc000, 1) | raise_one(0xc008, 2) |
                  raise_one(0xc010, 3) | raise_one(0xc018, 4) | raise_one(0xc020, 5) |
                  raise_one(0xc028, 6) | raise_one(0xc030, 7);
    wr(MARKER_BASE + 0x18, ok_mask);
    wr(MARKER_BASE + 0x14, 2);

    /* Engine probe, stage A: four read-only scratch reads. */
    for (u32 i = 0; i < 4; i++)
        wr(MARKER_BASE + 0x40 + i * 4, rd(ANE_SCRATCH0 + i * 4));
    wr(MARKER_BASE + 0x14, 3);

    /* Stage B: scratch write/readback on SCRATCH0. */
    wr(ANE_SCRATCH0, 0xa5b3c7d1);
    wr(MARKER_BASE + 0x50, rd(ANE_SCRATCH0));

    wr(MARKER_BASE + 0x54, 0xdeadbee5);
    wr(MARKER_BASE + 0x14, 4);
}
