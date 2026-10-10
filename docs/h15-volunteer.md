# H15 (M3) ANE stage 0/1 — volunteer runbook

Bring-up procedure for the first owner of an M3 Linux box. The module
`ane_h15.ko` is experimental and opt-in. Stage 0 touches no hardware.
Stage 1 powers the ANE power domains and reads five pmgr words. It
performs no other access. Send every log back; stop at the first
unexpected line.

## Before you start

- An M3 Linux box with root. T8122 boards: j433, j434, j504, j613,
  j615. T6030/T6031: same steps, matching overlay and `optin` key.
- A kernel from `joshuaswarren/aurora-linux-tbnet`, branch
  `ane-driver-aurora` (commit `efe6e359`) or a newer `aurora-wip`
  branch, with its t8122 board DTBs installed. Check `uname -r`.
- `dtc` and `fdtoverlay` 1.7.x on PATH.
- The omarchy-ane package, so `omarchy-ane-dt apply` exists.

The kernel DT has no ANE node yet: aurora `t8122.dtsi` has no `ane`
node, and `t8122-pmgr.dtsi` has only `ps_ane_sys` at offset 0x438.
The module binds only to an `ane` node in the device tree, so without
the overlay it cannot bind. The overlay adds the four missing ps
nodes (MPM/CPU/TD/BASE), four DART nodes, and the `ane` node.

First M3 record (a MacBook Air 13-inch 2024, board j613, published in
the community data on 2026-10-02): the running device tree has no `ane`
node (`ane_node_present` is false), the `/chosen` stub OS version is
14.7, and the boot loader is m1n1 v1.6.1-omarchy.aurora1. This matches
the paragraph above. That record has no ANE module and no ANE log
lines, so it says nothing yet about the register windows. Stage 0 and
stage 1 are still the first test of those.

The ANE firmware that iBoot preloads comes from the stub macOS image,
and it changes with the macOS version. We measured the T8122 member
`h15_ane_fw_themis_j51y.im4p` in the full restore images: 14.4, 14.5
and 14.6.1 each differ from each other and from 15.0 and 27.0, and the
27.0 file equals the 27.0.1 file only. macOS 14.7 never shipped as a
full restore image, so the firmware of a stub at 14.7 is not measured
and not pinned. Stage 0 and stage 1 do not need the firmware. A boot
stage (stage 3) is refused until a pin matches your stub version.
Please send the `/chosen` stub OS version string with your logs.

## Step 1 — build and install the overlay

```
git clone https://github.com/joshuaswarren/omarchy-ane
cd omarchy-ane/ane/h15
dtc -@ -I dts -O dtb -o ane-h15-t8122.dtbo t8122-ane-experimental.dts
sudo install -D -m 600 ane-h15-t8122.dtbo \
    /usr/lib/omarchy-mac-boot/dtb-overlays/t8122/ane-h15-t8122.dtbo
echo ane-h15-experimental | sudo tee -a /etc/omarchy-mac-boot/dtb-overlays.opt-in
sudo omarchy-ane-dt apply && sudo omarchy-ane-dt status
```

Note: `dtc` has no `-O dtbo` output format. Compile with `-O dtb` and
name the output `.dtbo` (INFERENCE: overlay directory path on your
install; `omarchy-ane-dt status` tells you if it disagrees).
Verify, then reboot:

```
ls /proc/device-tree/soc/ane@310000000/compatible   # apple,t8122-ane
sudo reboot
```

## Step 2 — build the module

```
make -C ane/h15 KERNELDIR=/lib/modules/$(uname -r)/build W=1 modules
```

(INFERENCE: your kernel headers path. Cross build instead:
`make -C ane/h15 KERNELDIR=<tree> ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu- W=1 modules`.)

## Step 3 — stage 0 (no hardware access)

```
cd ane/h15
sudo insmod ane_h15.ko optin=t8122 stage=0
dmesg | grep ane_h15
sudo rmmod ane_h15
```

PASS ends with this line (a dmesg device prefix sits in front):

```
ane_h15 RESULT stage=0 soc=t8122 verdict=PASS reason=dt-parse-only
```

You also see the five `pmgr.<NAME> offset=` lines and one `word[i]`
line per word with its tier: five lines tier 0 (the SAFE pmgr words),
two tier 1 (CPU_STATUS, RVBAR), three tier 2 (mbox ctrl, doorbell),
one tier 3 (CoreSight, FORBIDDEN).

Refusals: probe prints the required `optin` key and returns -EPERM;
a wrong `apple,ane-type` prints both values and returns -EINVAL; an
unknown stage name returns -EINVAL.

## Step 4 — stage 1 (power domains + five pmgr reads)

```
sudo insmod ane_h15.ko optin=t8122 stage=1
dmesg | grep ane_h15
sudo rmmod ane_h15
```

PASS looks like (five times, plus the RESULT line):

```
ane_h15 read pmgr.ANE_SYS pa=0x2d0700438 off=0x438
ane_h15 word=ANE_SYS pa=0x2d0700438 value=0xf actual=0xf pass=true
ane_h15 RESULT stage=1 soc=t8122 verdict=PASS reason=ps-guard+reads
```

FAIL looks like (word stuck below 0xf after 500 ms; the module then
refuses all further access):

```
pmgr ANE_SYS @+0x438 stuck at 0x0 (ACTUAL=0x0); refusing to touch the engine window
ane_h15 RESULT stage=1 soc=t8122 verdict=FAIL reason=pmgr-actual-stuck
```

A mapping failure prints `of_iomap failed (engine=..., pmgr=...)` and
returns -ENXIO with no RESULT line.

Stage 2 (`stage=2`) always refuses on H15:
`verdict=REFUSED reason="2 ADDR_MEASURED word(s) lack a macOS capture; role INFERENCE"`.
Stage 3 (`stage=3`) always refuses and names the missing measured
facts. You may run both to record the lines.

## What is read; what is never touched

Stage 1 maps the node's `reg` windows (0: engine, 1: pmgr) but reads
only these five pmgr words, each behind genpd power-on and the
ACTUAL=0xf gate:

| SoC   | pmgr base    | ANE_SYS | ANE_MPM | ANE_CPU | ANE_TD  | ANE_BASE |
|-------|--------------|---------|---------|---------|---------|----------|
| t8122 | 0x2d0700000  | +0x438  | +0xc000 | +0xc008 | +0xc010 | +0xc018  |
| t6030 | 0x350700000  | +0x498  | +0xc000 | +0xc008 | +0xc010 | +0xc018  |
| t6031 | 0x292280000  | +0x520  | +0x5a0  | +0x5a8  | +0x5b8  | +0x5c0   |

Never touched, at every stage: the engine window (CPU_STATUS
engine+0x1400048, RVBAR engine+0x1050000, mbox engine+0x1408110 and
+0x1408114, MBI engine+0x1844000), the wrapper window, and CoreSight
engine+0x1010000 (FORBIDDEN tier). The module has no write at all.
The only register write in stages 0-2 is the kernel's pmgr power
driver setting each ps word TARGET to 0xf during the runtime-PM
power-on, then polling ACTUAL.

## Stop rules

- External abort, SError, or DART fault in dmesg: stop. Keep the log.
  Do not retry the same stage.
- Stage 1 FAIL: do not continue. Send the log.
- A hang: hold the power button. Delete the
  `ane-h15-experimental` line from the opt-in file and reboot.

## What to send back

Run the helper instead of the manual insmod pair if you prefer; it
writes one JSON per stage and prints the path:

```
sudo tools/omarchy-ane-h15-stage t8122 0 --module ane/h15/ane_h15.ko
sudo tools/omarchy-ane-h15-stage t8122 1 --module ane/h15/ane_h15.ko
```

Send: both JSON files (field `h15_stage` names the stage), the full
`dmesg | grep ane_h15` output, `uname -r`, and your board model from
`/proc/device-tree/model`. Collector status: NOT WIRED. No code in
this repository reads the `h15_stage` JSON today (checked
2026-10-10), so send the files to the ANE thread directly.
