# J700/H17: opening the PWGATE changes engine state, and writes still freeze

Two runs, two freezes, both logged to disk before the machine died.

## Run A — the power-on sequence (commit `2077391`)

`/var/log/h17-poweron.log`:

```
=== run at uptime 178s write_test=1 ===
domains raised, ps_cpu=0x1f0003ff target=0xf actual=0xf
before: 0x268c=0x3 0x26b0=0x0
step1 PS 0xc000 := 0xf
step2 validate 0xff
step3 PWGATE 0x268c := 0
step4 validate 0x268c mask3 == 0, now=0x0
step5 PS 0xc000 := 0x1000000f
step6 validate 0x100000ff, now=0x0
power-on done ps_cpu=0x1f0003ff 0x268c=0x0 0x26b0=0x0
read RVBAR=0x10000000001 CPU_STATUS=0x6a CPU_CONTROL=0x0
WRITE BEGIN CPU_CONTROL(0x1600044) := 0
```

Findings:

- The gate really was closed before. `0x268c` read `0x3` (Apple's
  power-off value) and became `0x0` (Apple's power-on value). Every
  earlier probe left it closed.
- **Opening it changed the engine:** `CPU_STATUS` went from `0x30006a`
  to `0x6a`. Bits 21:20 cleared. Whatever they report, the gate is not
  cosmetic.
- `RVBAR` unchanged (`0x10000000001`).
- The `CPU_CONTROL := 0` write — the exact write that froze the box in
  the previous experiment — **still froze it**.

So the closed gate was a real defect in our bring-up, but not the cause
of the write freeze.

## Run B — the power-on sequence plus the HAL tunables

`/var/log/h17-tunable.log`:

```
=== run at uptime 210s write_test=1 ===
domains raised, ps_cpu=0x1f0003ff target=0xf actual=0xf
before: 0x268c=0x3 0x26b0=0x0
step1 PS 0xc000 := 0xf
step2 validate 0xff
step3 PWGATE 0x268c := 0
step4 validate 0x268c mask3 == 0, now=0x0
step5 PS 0xc000 := 0x1000000f
step6 validate 0x100000ff, now=0x0
power-on done ps_cpu=0x1f0003ff 0x268c=0x0 0x26b0=0x0
```

Nothing after that line. The next statement is

```c
	t = ioremap(0x1984000, 0x1000);
	if (t) {
		v = readl(t);
		h17_log("tunable mcw(0x1984000) before=%#x\n", v);
```

The log line carries the value read, so it can only be missing if the
**read** of `0x1984000` never returned. That is `sne_mcw`, the master
clock wrapper the HAL read-modify-writes (`mask 0x1f00 -> 0`,
`AppleANETunableApplyFunction+0x440`).

This is the important new fact: the freeze is not confined to writes.
Reading the master-clock-wrapper block hangs the machine too, while reads
of the CPU block (`0x1600044/0x1600048`), `RVBAR`, the scratch block
(`0x1880020..3c`) and both mailbox controls all return normally.

## Interpretation, labelled inference

A block that reads fine, a neighbouring block that hangs on read, and a
CPU block that hangs on write is what an ANE whose clocks are not running
looks like from the outside. The ANE clock IDs (`325/326/327`) and gates
(`291/338`) are modelled by nothing on Linux
(`receipts/2026-11-03-j700-h17-clock-gap/`), so every access we make is
into a block that Apple would have clocked first.

The alternative — that the exclave or a firewall blocks our accesses —
is not excluded. The earlier exclave analysis covered reads only, and
reads of some blocks do work.

## What is now ruled in

- The full Apple power-on sequence (six steps) executes cleanly from
  Linux and moves real hardware state.
- That is not sufficient to make the engine writable.

## Next

Offline: extract `com.apple.driver.AppleH16ANEInterface`'s complete
ordered write sequence for ane-type `0x200`, to find what the driver does
between power-on and the first `CPU_CONTROL` write. If the answer is a
provider-method call (clock vote) rather than MMIO, the remaining problem
is the PMGR clock map, not the engine.
