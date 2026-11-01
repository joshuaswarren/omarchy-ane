# Ultra ANE test runbook (T6002 / T6022 die 0 and die 1)

For a volunteer with an M1 Ultra (T6002) or M2 Ultra (T6022) Mac running
Omarchy. M2 Ultra die 0 is enabled by default in the source release; the
other Ultra dies still need their own proof. Package versions can differ.
Opt-in changes the device tree used at boot and requires a reboot.
Removing a key takes effect only after applying the device tree and rebooting again.
Keep a working fallback boot before changing these keys.

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

Purpose: qualify T6002 die 0. The sibling chips' results do not qualify this die.

```
echo ane-t6002 | sudo tee -a /etc/omarchy-mac-boot/dtb-overlays.opt-in
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

The existing runner accepts `ANE_DEVICE`. The smoke tool now refuses a run
when the bound devices span both dies or their die identity cannot be read.
It does not guess a die from the first device. The collector also carries
shared check and fault fields; the promotion checker refuses ambiguous rows.

There is no complete die-1 qualification command here yet. Do not use the
old `ANE_DEVICE=1` smoke sequence as a passing die-1 promotion row.
The missing source boundary is a smoke result bound to the selected device,
plus separate check and fault evidence for that die.

Retain the stage-0 device-tree report and existing boot logs. Do not enable
the second die solely to fill this missing evidence.

For an existing die-1 attempt:
- A genpd timeout, an external abort, or a DART fault on the die-1 DARTs
  (0x2285800000-0x2285820000) or a fault naming the die-1 mailbox
  (0x2285408000) = STOP, power-cycle, send everything. Do not
  retry: the die-1 ps word layout is the open measurement, and the ACTUAL
  log from the first attempt is exactly what we need.

## T6022 (M2 Ultra): die 0 runs, die 1 unproven

Die 0 is on by default (one passing row, 316ea37e17f0); die 1 would use `ane-t6022-die1`, which ships
**data-only** today: nothing installs or applies it, by design. The die-1
firmware image (`t602x_ane1_fw_selene_rc4x`, firmware list `Ap,ANE1`) has no
recorded SHA-256 (docs/ultra-die1.md §9, fact 2), `omarchy-ane-firmware-fetch`
refuses with `no pin for T6022 die 1` when it sees a die-1 node, and the
driver validates whatever it stages against the die-0 pin — a non-identical
image refuses at load. So the honest state is: **T6022 die 1 is unproven end
to end** (firmware boot, RTKit handshake, mailbox, pmu page 0x228e084000 all
INFERENCE until a live capture; docs/ultra-die1.md §9, fact 2). A T6022 volunteer's useful stage 0 is the
same probe-only capture as above; do not fabricate a die-1 run.

## What each verdict means (promotion rule, per die)

- `omarchy-ane-check` not ready, or any ANE/DART/mailbox fault line (die-1
  addresses included) → the row FAILS; it still counts as evidence, send it.
- Smoke with exactly 20 bit-exact calls against the golden and a clean check
  → the row PASSES for the die its `die` field names (absent field = die 0).
- One passing row promotes that die's overlay key; a chip is fully on only
  when both dies have their own passing rows. `tools/promotion_check.py
  --remote` prints the live verdict per (chip, die).
