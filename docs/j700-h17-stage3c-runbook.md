# J700/H17 stage 3c runbook: raise the ANE sub-domains via genpd

Status: **executed 2026-11-02 — PASS.** Boot `3134730e-…` on kernel
`7.2.11-14-1-ARCH`, preflight PASS, overlay applied, all three sub-domains
reached `ACTUAL=0xf` and returned `off-0` on unload. Receipt:
`receipts/2026-11-02-j700-h17-stage3c-vote-pass/`.

Stage 4 (`ane_h17_engine_probe.ko`) is built but **not run**: it performs
the first engine-window read, and a stalled read would force a reboot.

## What is prepared

- `ane/h17/t8140-ane-ps-probe.dts` now adds three pwrstate nodes
  (`power-controller@c000`, `@c008`, `@c010`, labels `ane_sub0/1/2`) as
  genpds chained off `ps_ane_sys`, plus a consumer node
  `ane-h17-cpu-vote` carrying all three as power domains.
- `ane/h17/ane_h17_cpu_vote_probe.c` binds that consumer, adds a
  runtime-active device link per domain so the pmgr driver does its own
  TARGET-write + ACTUAL-poll, then reads back `0x290/0xc000/0xc008/0xc010`.
  It never writes a ps-word and never touches the engine window.
- `packaging/arch/omarchy-ane-h17-ps-probe-dt/PKGBUILD` bumped to
  `pkgrel=2`.

Offline validation done: the overlay compiles and
`fdtoverlay -i dtbs/t8140-j700.dtb -o merged.dtb t8140-ane-ps-probe.dtbo`
exits 0, and the merged tree contains all three sub-domain nodes and both
consumer nodes.

## Reboot-gated steps

1. Rebuild and install the overlay package:
   ```sh
   cd packaging/arch/omarchy-ane-h17-ps-probe-dt && makepkg -f
   sudo pacman -U omarchy-ane-h17-ps-probe-dt-0.1.0-2-aarch64.pkg.tar.zst
   sudo update-m1n1
   ```
   The opt-in key `ane-h17-ps-probe` is already in
   `/etc/omarchy-mac-boot/dtb-overlays.opt-in`.
2. **Reboot** (planned; the current kernel keeps running until then).
3. After reboot, confirm the merge:
   ```sh
   ls /proc/device-tree/soc/power-management@300700000/ | grep c0
   ```
   Expect `power-controller@c000`, `@c008`, `@c010`.
4. Load the vote probe:
   ```sh
   sudo insmod ane/h17/ane_h17_cpu_vote_probe.ko optin=1
   sudo dmesg --color=never | grep 'ane_h17_cpu_vote' | tail -8
   sudo rmmod ane_h17_cpu_vote_probe
   ```

## Pass criterion

`RESULT stage=3c verdict=PASS reason=all-subdomains-on`, with `actual=0xf`
for `sub0/sub1/sub2`. `reason=read-ok-some-off` means at least one word
did not reach `0xf`; stop and investigate, do not read the engine.

## Risk assessment

- **Reboot**: planned, not caused by the probe.
- **Boot merge**: verified offline; the pmgr driver registers three
  additional genpds that stay off until voted.
- **Vote**: the pmgr driver's own TARGET-write + ACTUAL-poll with a 100 ms
  timeout; a failure is a clean probe error, not a stalled bus read.
- **Engine**: not touched. The engine read stays a separate stage that is
  attempted only if every sub-domain reports `0xf`.

## What remains unknown

- Which of `c000/c008/c010` is MPM vs CPU vs TD. The names are neutral on
  purpose; stage 3c assigns nothing.
- Whether `c018` (read all-zero in stage 3b) is a real island on T8140.
- Whether the engine needs any domain not present here. The H16 note that
  "ane_td and ane_base are required; with only the other six raised, block
  access resets the machine" is the warning that keeps the engine read
  behind this gate.

## Result (2026-11-02)

```
ane_h17_cpu_vote ane_sys off=0x290  word=0x23ff     target=0xf actual=0xf
ane_h17_cpu_vote sub0    off=0xc000 word=0x1f0003ff target=0xf actual=0xf
ane_h17_cpu_vote sub1    off=0xc008 word=0x3ff      target=0xf actual=0xf
ane_h17_cpu_vote sub2    off=0xc010 word=0x3ff      target=0xf actual=0xf
ane_h17_cpu_vote RESULT stage=3c verdict=PASS reason=all-subdomains-on
```

`0xc000` is the only word with `APPLE_PMGR_PS_AUTO` set (bits 27:24 =
`0xf`), consistent with it being the head of the sub-chain. After `rmmod`
every domain is `off-0`.

## Stage 4 (built, not run)

`ane_h17_engine_probe.ko` repeats the stage-3c vote, refuses unless every
PMGR word reads `ACTUAL=0xf`, then maps `ane@400000000` and reads
`CPU_STATUS` (`0x1600048`) and `RVBAR` (`0x1050000`). Both are on the
H16/H17 read whitelist. This is the first engine-window access; if the
domain set is insufficient for block access the read can stall the bus,
and recovery is a reboot. It is therefore held until a reboot is
acceptable.
