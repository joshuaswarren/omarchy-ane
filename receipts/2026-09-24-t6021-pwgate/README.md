# PWGATE set-window words (2026-09-24, 17:41 CDT)

Main asked for a read of phys 0x28e092cc and 0x28e093cc and said: if they
already read 3 and 0, the theory is dead without a write.

That literal address is not on the derivation. The static receipt
(ane-linux-experiments agent/ane-static-start-r,
receipts/2026-09-24-t6021-macos-start-sequence §19) derives:
PWGATE+0x12cc <- 3, PWGATE+0x13cc <- 0, PWGATE = the set window
0x28e08c000, so literal 0x28e092cc / 0x28e093cc. But 0x28e08c000+0x12cc is
0x28e08e2cc, not 0x28e092cc. The literal is 0x052cc past the set window,
past its stated 0x4000 length. The same receipt elsewhere uses a PWGATE
window of 0x28e092000, which makes PWGATE+0x12cc = 0x28e0932cc — yet
another address. I did not guess a pmgr write.

## Read-only result (vehicle unloaded)

Offsets are from pmgr-window base 0x28e080000. `pwgate` vehicle
(staged, this worktree): `ane_ascdbg_pwgate.c`.

- 0xd2cc = 0x00000000
- 0xd3cc = 0x00000000
- 0x132cc = 0x00000000
- 0x133cc = 0x00000000
- 0x1359c = 0x00000000 (pre-RUN write target)
- 0x122dc = 0x00000000 (the RMW row)

Islands were on, STATUS 0x28, no watchdog. Both pairs read 0/0. No release
was run on this already-released boot.

CORRECTION (17:52): the first report said this theory was dead. It is not:
the kext wants 3/0, and 0/0 means the 3 is missing.

Full access log: ascdbg.log.

## Base bound (17:52 CDT, read-only)

macOS registry capture (ane-linux-experiments
.work/m2-macos-ane-ioreg/ane0-full.txt): `H11ANE <class H11ANEIn>` is a
direct child of `ane0@84000000 <class AppleARMIODevice>`
(`IOProviderClass = AppleARMIODevice`), so start()'s provider is the ane
nub. Its IODeviceMemory:

    0  0x284000000  0x2000000
    1  0x28e080000  0x4034
    2  0x28e08c000  0x4000   <- index 2

Linux DT: /soc/ane@284000000 (parent /soc, simple-bus), reg-names
engine/pmgr/set, same three ranges.

    base 0x28e08c000 + 0x12cc = 0x28e08d2cc  (kext writes 3)
    base 0x28e08c000 + 0x13cc = 0x28e08d3cc  (kext writes 0)

Live (islands on, ps_ane_cpu 0x1f0003ff):

    STATUS 0x28, RVBAR 0x10000000001
    pr32 0xd2cc = 0x00000000   (want 3)
    pr32 0xd3cc = 0x00000000   (want 0)

The 3 is missing. A release has run this boot, so the write needs a
fresh-boot, pre-release test.

## First-release run with the PWGATE writes (17:58 boot, Main go)

The boot was fresh: uptime 0 at 17:57, and no ANE module had been loaded.
`sid15_prep.sh` (../2026-09-24-t6021-coresight-dart) read:

    STATUS 0x2a  RVBAR 0x10000000001  I2A 0x00020001  SCRATCH7 0
    TCR0 0x9, TTBR0 0x1000e0c9, ENABLE 0xffff on all three DARTs
    TCR15 <- 0x2 on all three, read back 0x2
    pr32 0xd2cc = 0, pr32 0xd3cc = 0, ps_ane_cpu 0x1f0003ff

Then `insmod ane_t6021_rtclient.ko fw_cache_test=1 fw_pwgate=1`
(sha256 53faf241..., source `ane_t6021_rtclient_main.c` here):

    both segments mapped IOMMU_CACHE, iova_to_phys ok
    pte[0]=000fff1000084801 valid=1 nocache=0
    PWGATE set+0x12cc=00000000 set+0x13cc=00000000   <- after writing 3 / 0
    pre-release status=0000002a
    t=0..50  scratch7=0 i2a=00020001 recv0=0 status=0x28
             err=00a00000/00f00000/10700000
    PWGATE restored 00000000/00000000

No apple-dart fault or SError appeared. The write of 3 did not latch, so
the kext's state was never set up, and the firmware still stalled. Full
dmesg: `dmesg-first-release.txt`. Access log: `ascdbg.log`. The module
source here also contains the earlier IOMMU_CACHE runs.

## Ordered run: PS raise, then PWGATE (18:08 boot, Main go, unreleased)

The boot was fresh: uptime 0 at 18:08, no ANE module, genpd ane_cpu on.
The fallback ESP boot.bin was 43ec6090. Script: `pwgate_prep.sh`.

    pre   STATUS 0x2a, ps_ane_cpu 0x1f0003ff
          0x28e088000 = 0x0f   0x28e088008/10/18 = 0x2f   0x28e088020 = 0
          0x28e08d2cc = 0      0x28e08d3cc = 0
    TCR15 <- 0x2 on all three DARTs, read back 0x2
    PS    0x8008/0x8010/0x8018 <- 0xf, each read 50 times: stay 0x2f
          (ACTUAL never 0xf)
    PWGATE set+0x12cc <- 3, polled 50 times for (v&3)==3: 0
           set+0x13cc <- 0: 0
    NOT-LATCHED -> set+0x12cc <- 0, no release
    post  STATUS 0x2a (core still unrun), box alive

The Linux pmgr node covers only 0x28e080000 + 0x8000, so no genpd domain
owns the 0x28e088xxx words. The macOS hv traces (trace-135,
trace-atcrt) show the OS itself writing 0x2f/0x20/0xf/0 to
0x28e088004..0x28e088018 during boot. Access log: `ascdbg.log`.
