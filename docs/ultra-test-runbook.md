# Ultra ANE test runbook (T6002 / T6022 die 0 and die 1)

For a volunteer with an M1 Ultra (T6002) or M2 Ultra (T6022) Mac running
Omarchy (or willing to install it). Everything here is opt-in: nothing turns
on by itself, and every stage is reversible by removing one line from
`/etc/omarchy-platform/dtb-overlays.opt-in` and running `sudo omarchy-ane-dt
apply && sudo update-m1n1 && reboot`. Stages build on each other; do not skip.

Rules that keep this safe:

- **Stop rule:** if a stage hangs longer than 60 s (no console output, no SSH
  answer over two routes), power-cycle. Do not retry the same stage twice in a
  row; report the stage, the exact commands, and the console tail instead.
- **One change per stage.** Each stage below names the only thing that
  changes. If you changed anything else (kernel, m1n1, firmware files), say so
  in the report — an unexplained state makes the row unusable.
- **Send back** (every stage): the stage's command output as files, the
  `dmesg` lines named per stage, and the JSON lines the tools print. The
  collector wants: probe JSON (stage 0), `omarchy-ane-check` output, smoke
  JSON with its `die` field, and the kernel lines quoted in each stage.
- The driver refuses unproven hardware loudly. A refusal message is a RESULT:
  send it verbatim, it names the next measurement.

## Stage 0 — probe only (no module, no overlay)

Purpose: prove the machine's device tree and record the ANE facts without
loading anything.

```
sudo omarchy-ane-probe        # reads the live DT; no driver, no writes
ls /sys/firmware/devicetree/base/soc@2200000000/ 2>/dev/null
```

What to send back: the probe JSON, and the `ls` output (it shows whether your
kernel tree carries the die-1 bus at all).

Verdict: stage 0 passes when the probe prints the die-0 ANE facts (engine,
pmgr and SET windows, DARTs, IRQ) and exits 0. A probe that cannot find the
ANE node means the kernel tree needs the overlay first — go to stage 1.

## Stage 1 — die 0 only (T6002: key `ane-t6002`)

Purpose: the known-good die. This is the M0/M1-proven path.

```
echo ane-t6002 | sudo tee /etc/omarchy-platform/dtb-overlays.opt-in
sudo omarchy-ane-dt apply && sudo update-m1n1 && sudo reboot
# after boot:
omarchy-ane-dt status
sudo omarchy-ane-check
sudo omarchy-ane-check --smoke
```

What to send back: `omarchy-ane-dt status` (it prints `node=... dtbs_source=...`
and the ANE node path), the full `omarchy-ane-check` output, the smoke JSON
line (it carries `chip`, `die`, 20 `sha256` values), and
`journalctl -k -g ane | tail -40` (expected: one `loaded ane` line,
`ANERD ps probe act=0xffffff`, no fault lines).

Verdict: `omarchy-ane-check` exit 0 and smoke exit 0 (20/20 bit-exact) = a
die-0 passing row. With the collector (`python3 scripts/collect_deep.py
--ane-smoke --submit` from an omarchy-mlx checkout) this is exactly the row
the promotion rule judges for `t6002` die 0.

## Stage 2 — die 1 as the second device (T6002: key `ane-t6002-die1`)

Purpose: the second accel device. The die-1 SET base is RECOGNIZED, not
QUALIFIED: the first boot needs an explicit override, and the driver logs the
power-island ACTUAL nibbles through the SET window before every engine write
so the first resume names the die-1 word layout.

```
echo ane-t6002-die1 | sudo tee -a /etc/omarchy-platform/dtb-overlays.opt-in
echo 'options ane allow_unqualified=1' | sudo tee /etc/modprobe.d/ane-unqualified.conf
sudo omarchy-ane-dt apply && sudo update-m1n1 && sudo reboot
# after boot:
omarchy-ane-dt status                 # node= now lists BOTH ane nodes
ls -l /dev/accel/                     # expect accel0 AND accel1
sudo dmesg | grep -E 'loaded ane|UNQUALIFIED|ps probe'
omarchy-ane-smoke --timeout 120       # die field says which device it ran
sudo omarchy-ane-run --dev 1 --anec /usr/share/omarchy-ane/fixtures/h13-anec/add/program-0.anec \
  --in 0=/tmp/a.fp16 --in 1=/tmp/b.fp16 --out /tmp/y.fp16
```

(`a.fp16`/`b.fp16`: any 16 KiB fp16 files; the smoke tool is the check that
matters — the `--dev 1` run above is the manual "die 1 opens" probe.)

What to send back: `omarchy-ane-dt status`, `ls -l /dev/accel/`, the dmesg
lines above (especially the ACTUAL-nibble probe lines and the
`UNQUALIFIED bind forced by allow_unqualified` warning), the smoke JSON, and
`journalctl -k -g ane | tail -80`.

Verdict:
- **Die-1 bind** = two `loaded ane` lines, two accel nodes, no DART fault.
- **Die-1 passing row** = the smoke run against die 1 (the smoke `die` field
  says 1) with 20/20 bit-exact against the same H13 golden, and no
  ANE/DART/mailbox fault line. Promotion rules judge dies separately: die 1
  never rides the die-0 row.
- A genpd timeout, an external abort, or a DART fault on die-1 addresses
  (0x2285800000-0x2285820000) = STOP, power-cycle, send everything. Do not
  retry: the die-1 ps word layout is the open measurement, and the ACTUAL
  log from the first attempt is exactly what we need.

## T6022 (M2 Ultra): same shape, firmware boot unproven

Die 0 uses key `ane-t6022`; die 1 would use `ane-t6022-die1`, which ships
**data-only** today: nothing installs or applies it, by design. The die-1
firmware image (`t602x_ane1_fw_selene_rc4x`, BuildManifest `Ap,ANE1`) has no
recorded SHA-256 (docs/ultra-die1.md §9.2), `omarchy-ane-firmware-fetch`
refuses with `no pin for T6022 die 1` when it sees a die-1 node, and the
driver validates whatever it stages against the die-0 pin — a non-identical
image refuses at load. So the honest state is: **T6022 die 1 is unproven end
to end** (firmware boot, RTKit handshake, mailbox, pmu page 0x228e084000 all
INFERENCE until a live capture). A T6022 volunteer's useful stage 0 is the
same probe-only capture as above; do not fabricate a die-1 run.

## What each verdict means (promotion rule, per die)

- `omarchy-ane-check` not ready, or any ANE/DART/mailbox fault line (die-1
  addresses included) → the row FAILS; it still counts as evidence, send it.
- Smoke with exactly 20 bit-exact calls against the golden and a clean check
  → the row PASSES for the die its `die` field names (absent field = die 0).
- One passing row promotes that die's overlay key; a chip is fully on only
  when both dies have their own passing rows. `tools/promotion_check.py
  --remote` prints the live verdict per (chip, die).
