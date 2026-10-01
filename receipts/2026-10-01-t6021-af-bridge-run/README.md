# T6021: the macOS ANE0 AXI2AF bridge tunables on the M2, read and applied (2026-10-01)

Status: rejected. The driver parameter was reverted in PR #34; the
measurements stay valid. The code is at `d3b8561`.

## Result: rejected

Under Linux, none of the 26 bridge registers that macOS programs
(`AppleT6020PMGR::applyBridgeTunables`) holds the macOS value. With the new
default-off parameter `af_bridge_macos=1`, the driver writes all 26 and they
read back as macOS writes them. The firmware still boots, and every output
stays bit-exact. The encoder time does not change: **254.274 ms** with the
macOS values, against **254.276 ms** and **254.215 ms** on the default boots
before and after (minimum of 20 block minimums, 16 calls per block). The
hypothesis "wrong bridge tunables cost time" is rejected. The 254 ms vs 89 ms
gap to macOS ([2026-10-01-t6021-native-vs-cross](../2026-10-01-t6021-native-vs-cross/README.md))
has another cause.

## Question and decision rule

The prepared probe ([2026-10-01-t6021-af-bridge](../2026-10-01-t6021-af-bridge/README.md))
reads the 26 words at engine base 0x284000000 + 0x000 .. 0xa00. The table
(offset, mask, value, `reg = (reg & ~mask) | value`) is in that receipt.

The rule was written before the first reboot (private notebook entry,
17:33Z). The test arm is a boot with `af_bridge_macos=1`; the control is the
release module on the same boot chain (same ESP image, kernel and tree).
The metric is the encoder minmin (NativeVsCross method: ane-run `--time`
exec ms per CALL, 20 blocks of 16 calls; minmin = minimum of the block
minimums, medmed = median of the block medians):

- supported: minmin drops by 15% or more (to 216 ms or less);
- partial: a 3-15% drop (then bisect the register groups 0x000-0x034,
  0x108-0x134, 0x400 and 0xa00 in later boots);
- rejected: a drop below 3%.

medmed must fall in the same band. Every correctness item must hold:
encoder hidden fp16 sha256 `fca96f13…` (golden max_abs 0), prog_020 t15 vs
the M1 golden rel L2 0.00117, prog_006 outputs byte-identical to the cross
outputs of NativeVsCross, gates add/mul/matvec 2048x5120 PASS, a 60 s
four-worker burst with 0 fail, and 0 new bad kernel lines.

## Stages

Device: M2 Max (T6021), stock `7.1.13-3-1-ARCH`, disk boot, `asahi,os-fw-version`
13.5. Release module 0.4.0 (sha256 `54c1da56…`). Userspace from the release
tree `9f37b47`. Every ANE call ran under `flock /var/tmp/ane-run.lock timeout
120`, and every device window and reboot ran inside a `gpu-turn` ticket, so no
GPU job ran at the same time.

### S1: the current boot (after our P-1)

The probe ([../../ane/t6021/probes/ane_afbridge_probe.c](../../ane/t6021/probes/ane_afbridge_probe.c),
built on the M2, sha256 `80c0d09e…`) read 26 of 26 words. No fault, and the
boot did not change. **Applied 0, NOT-applied 26** (`logs/s1-check.txt`).

| offsets | read (S1, after P-1) | after the macOS RMW (S3) |
|---|---|---|
| 0x000, 0x108 | 0x10 | 0x11 |
| 0x00c, 0x10c | 0x000fffff | 0xd |
| 0x010, 0x110 | 0x0007ffff | 0xc |
| 0x014, 0x018, 0x114, 0x118 | 0x5a0 | 0x1 |
| 0x01c-0x034, 0x11c-0x134 (14 words) | 0x5a0 | 0x3 |
| 0x400 | 0x40010001 (our P-1d) | 0xc0f10010 |
| 0xa00 | 0 | 0x01ffffff |

0x108 reads 0x10 although no Linux code writes it, and the S3 boot read 0x10
at 0x000 and 0x108 before any write. So 0x10 is the power-on value there, and
P-1a (0x000 <- 0x10) writes back that value. The other 24 registers read the
same values on the S1 and S3 boots before any write to them. So they are
most probably power-on values too; nothing before the driver sets them to
the macOS table.

### S2: a boot with `fw_start=0`

The question was the iBoot state before our P-1. With `fw_start=0` the
driver refuses at load (`legacy_only=1 requires fw_start=1`, -EINVAL) before
it raises its power domains (`logs/s2-driver.log`). At that point `ane_sys`
and `ane_cpu` are on (`ane_cpu` also has the two ANE DARTs as users), but the seven compute
islands are off (`logs/s2-genpd.txt`). The probe refused: `ane_sys
0x1f0003ff islands 0x300 0x300 0x300 0x300 0x300 0x300 0x300, no engine read`
(`logs/s2-probe.log`). So the pre-P-1 state cannot be read under the island
guard. The run did not retry.

### S3: the A/B write test

`af_bridge_macos` (bool, 0444, default 0) is a load-time parameter of
`ane_t6021`. When it is set, P-1 starts with the 26 macOS read-modify-writes
in the macOS table order, in place of P-1a (eng+0x000 <- 0x10) and P-1d
(eng+0x400 <- 0x40010001). The other ten P-1 writes follow in their old order
(0x038, 0x03c, 0x600, 0x738, 0x798, 0x7f8, 0x900, 0x410, 0x420, 0x430), then
the scratch setup and the CPU release. macOS writes the table after `ane_sys`
comes up and before `ane_cpu` comes up. Here genpd has already raised
`ane_cpu` and the islands at probe, and the writes come before the ASC CPU
release. For
each register the driver checks the PS guard (ane_sys ACTUAL and the seven
island words), reads the register, logs the value it will write at
KERN_EMERG with a 30 ms drain, checks the guard again, writes through the
non-posted engine map, reads back and logs both values. A guard miss
returns -EAGAIN before the CPU release.

The option was one-shot: a modprobe.d `install` line deleted its own file
and ran `sync` before it loaded the module with the parameter. So a reset
during the writes would have given a default load on the next boot, not a
reset loop. There was no reset.

On the S3 boot (`logs/s3-driver-afb.log`) all 26 readbacks equal the written
values, the firmware boot reached DONE (`booted=1 fw_alive=1`), and the probe
read **applied 26, NOT-applied 0** (`logs/s3-check.txt`). The 0x000, 0x108
and 0x400 results (0x11, 0x11, 0xc0f10010) are exactly what macOS 13.5 writes
in the hv trace. The power-on value of 0x400 is 0x00f00008.

### Timing and correctness

Three arms, one per boot, each started about 30 s after boot with
`scripts/ab-turn.sh` (`logs/*-enc.summary`, `logs/*-prog_0*.blocks`,
`logs/analysis.txt`):

| arm (boot) | encoder minmin | encoder medmed | prog_020 minmin / medmed | prog_006 minmin / medmed |
|---|---|---|---|---|
| default, before | 254.276 | 254.550 | 4.758 / 4.829 | 10.581 / 10.633 |
| `af_bridge_macos=1` | 254.274 | 254.459 | 4.764 / 4.832 | 10.570 / 10.631 |
| default, after | 254.215 | 254.408 | 4.761 / 4.830 | 10.578 / 10.630 |

All times in ms per CALL. Encoder drop with the macOS values: minmin +0.00%,
medmed +0.04%. Both are below 3%, so the verdict is **rejected**. prog_020
and prog_006 move by 0.1% or less. In each arm a few blocks in the first two
minutes after boot have medians 0.5-9 ms higher (default before: blocks 1-5;
test: 1-3; default after: 3-5). They do not set the minimum, and the medmed
is the median of all 20 blocks.

Correctness held in all three arms (`logs/*-checks.txt`): 21 of 21 encoder
processes bit-exact with fp16 sha256 `fca96f1355485ec3…`; prog_020 t15 max
abs 0.0007324219, rel L2 0.001174227; prog_006 10 of 10 outputs
byte-identical; gates add, mul and matvec 2048x5120 (5120/5120 bit-exact)
PASS; burst 13,160-13,287 runs with 0 fail; 0 new bad kernel lines.

## The driver change and its default

Code: `ane/t6021/ane_t6021_boot.h` (`ane_t6021_af_bridge_rmw`, the P-1 hook,
the `power_ok` and `bridge_log` io callbacks, `cfg.af_bridge_macos`),
`ane/t6021/ane_t6021_boot.c` (the parameter and the kernel callbacks; the
guard reads the DT "pmgr" window), `ane/t6021/ane_t6021_rtclient_main.c`
(hands that window to the boot core). The probe now checks the guard right
before each read, after the netconsole drain, and prints all PS words when
it refuses.

With the parameter at 0 the boot core issues the same MMIO sequence as
before. `scripts/default-trace.c` prints every read, write, phase and poll
call of `ane_t6021_boot_run` for 30 configurations (table mode 0-2, rtb 0-1,
stop_after 0-4). Built against the old and the new `ane_t6021_boot.h`, the
two traces are identical (1,252 lines, 468 writes). The old
`tools/h14_boot_regression.c` (105 checks) gives the same output with both
headers. The new regression passes 110 of 110; the five new checks cover the
RMW arithmetic, the order, the removal of P-1a/P-1d and the guard stop.

## Commands

    # build (on the M2, nice 19)
    make -C /usr/lib/modules/7.1.13-3-1-ARCH/build M=$PWD/ane/t6021/probes W=1 modules
    make -C /usr/lib/modules/7.1.13-3-1-ARCH/build M=$PWD/ane/t6021 ANE_VERSION=0.4.0-afb-d3b8561 modules
    # S1, S2, S3 probe
    gpu-turn -m 5 -- bash probe-run.sh OUT
    # S3 install (one-shot option) and the arms
    bash install-afb.sh
    gpu-turn -m 20 -- bash ab-turn.sh ARM
    python3 analyze.py RUN_DEFAULT RUN_AF RUN_DEFAULT_AFTER

## Limits

- One M2, one boot per arm, one fixture per program. The arms ran in
  sequence on separate boots, not interleaved; the two default arms bracket
  the test arm and differ from each other by 0.06 ms.
- The S2 question (the iBoot bridge state) has no answer: the islands are off
  until the driver raises them, and the rule forbids engine reads with the
  islands off.
- At boot, netconsole carried only 20 of the 52 AFB lines; the first one it
  carried was logged at 3.48 s. The journal has all of them. A reset at one
  of the first writes would have left no netconsole line.
- The test shows that the bridge values do not change the time. It does not
  show what the bridge settings do.
