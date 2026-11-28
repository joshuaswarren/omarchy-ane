/* SPDX-License-Identifier: MIT */
/*
 * kex0-probe.c — kexec channel probe for the BMN-03 bare-metal payload path.
 *
 * Runs under the same contract as bmn03-payload (x0 = fdt, MMU off) but is
 * delivered by `kexec -e` from jwm1's running Linux instead of an aura-try
 * boot. Proves, in one run and with hardware-guaranteed return:
 *   1. the kernel's kexec_file_load accepts our arm64 Image and vectors in,
 *   2. the t8103 primary WDT (0x23d2b0000) arms from bare-metal code,
 *   3. the isp-heap marker hole survives the reset for Linux-side readback.
 *
 * Order matters: the WDT is armed BEFORE the marker is written so a hang
 * anywhere after entry is still watchdog-bounded. Arm sequence is the
 * chip-agnostic part of aura src/wdt.c (wdt_reboot / wdt_arm): COUNT=0,
 * ALARM=cycles, CTL=4. Offsets are valid on t8103's single-reg-layout WDT;
 * the version-2 secondary registers that wdt_arm checks do not exist here.
 *
 * Marker (32-bit words at 0xbd2bc0000):
 *   +0x00 magic "KEX0"   +0x04 stage (1=entered, 2=armed, 3=sealed)
 *   +0x08 cntpct (u64)   +0x10 ctl readback  +0x14 alarm readback
 *   +0x18 arm_ok (1 if ctl&4 and alarm matched)
 */

typedef unsigned int u32;
typedef unsigned long long u64;

#define WDT_BASE  0x23d2b0000ULL
#define WDT_COUNT 0x10
#define WDT_ALARM 0x14
#define WDT_CTL   0x1c

#define MARKER_BASE 0xbd2bc0000ULL
#define PROBE_SECONDS 90u

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

void payload_main(void)
{
    wr(WDT_BASE + WDT_CTL, 0);
    wr(WDT_BASE + WDT_COUNT, 0);
    wr(WDT_BASE + WDT_ALARM, PROBE_SECONDS * 24000000u);
    wr(WDT_BASE + WDT_CTL, 4);
    __asm__ volatile("dsb sy" ::: "memory");

    wr(MARKER_BASE + 0x00, 0x4b455830); /* "KEX0" */
    wr(MARKER_BASE + 0x04, 1);
    u64 t0 = cntpct();
    wr(MARKER_BASE + 0x08, (u32)t0);
    wr(MARKER_BASE + 0x0c, (u32)(t0 >> 32));

    u32 ctl = rd(WDT_BASE + WDT_CTL);
    u32 alm = rd(WDT_BASE + WDT_ALARM);
    wr(MARKER_BASE + 0x10, ctl);
    wr(MARKER_BASE + 0x14, alm);
    wr(MARKER_BASE + 0x18, ((ctl & 4) && alm == PROBE_SECONDS * 24000000u) ? 1 : 0);
    wr(MARKER_BASE + 0x04, 2);

    wr(MARKER_BASE + 0x54, 0xdeadbee5);
    wr(MARKER_BASE + 0x04, 3);

    for (;;)
        __asm__ volatile("wfe");
}
