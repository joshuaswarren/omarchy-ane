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
