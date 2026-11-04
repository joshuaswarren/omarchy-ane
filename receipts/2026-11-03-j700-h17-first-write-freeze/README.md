# J700/H17: the first engine write froze the machine — at the scratch clear

Boot `8ee27fc5-c329-4e24-9841-0c72d0f71a5a`, run at uptime 782 s and
786 s, cold boot at 18:16:14 PST. The machine reset at 18:28:44.
Next boot `82683392-4836-42dd-a4b2-a1a074ed8db8`.

## What makes this different from every earlier result

Every previous reset was minutes after an access, which left the cause
ambiguous. This one landed **between two consecutive write calls**, and
the durable log pins it to a single loop:

```
=== run at uptime 782s dry=1 ===
step1 domains raised
  sub0(0xc000)=0x1f0003ff target=0xf actual=0xf
  sub1(0xc008)=0x3ff      target=0xf actual=0xf
  sub2(0xc010)=0x3ff      target=0xf actual=0xf
step3 RVBAR=0x10000000001 CPU_STATUS=0x30006a CPU_CONTROL=0x0
dry=1, no engine writes; stopping here
released
=== run at uptime 786s dry=0 ===
step1 domains raised
  sub0(0xc000)=0x1f0003ff target=0xf actual=0xf
  sub1(0xc008)=0x3ff      target=0xf actual=0xf
  sub2(0xc010)=0x3ff      target=0xf actual=0xf
step3 RVBAR=0x10000000001 CPU_STATUS=0x30006a CPU_CONTROL=0x0
```

The `dry=1` run completed every read and released cleanly. The `dry=0`
run produced **no further line** — and the next line in the source is
written only after this loop returns:

```c
	for (i = 0; i < 8; i++)
		writel(0, eng + H17_SCRATCH0 + 4 * i);
	h17_log("step2 scratch cleared, SCRATCH6=%#x\n",
		readl(eng + H17_SCRATCH6));
```

So the machine stopped inside the first eight engine writes to ever run
on this board.

## What is ruled out by this run

- **Domains.** Raised identically in both runs; the dry run survived.
- **Engine reads.** CPU_CONTROL, CPU_STATUS, RVBAR read fine in both.
- **The genpd raise and release path.** The dry run did both and released.
- **The logging path.** The log was written and fsync'd at step 3 and is
  intact after the reset, which is the whole point of the durable log.

## What it does not establish

- **Which of the eight cells is fatal.** The loop writes
  `0x1880020 .. 0x188003c` with no logging between iterations, so the log
  cannot distinguish `SCRATCH0` from `SCRATCH7`. The next run must select
  cells and log each write individually.
- **That a write is fatal at all**, as opposed to the machine having
  reached the end of a 13-minute window that had nothing to do with the
  write. The dry run 4 seconds earlier argues against that, but it is one
  observation. Repeating with a cell selection will settle it: if the box
  survives writes to some cells and dies on a specific one, the write is
  the trigger.
- Whether the block is read-only, gated by a clock we never enabled, or
  in a different power domain. Nothing here distinguishes those.

## Note on the earlier "engine access causes resets" model

This result sharpens it. The survey run read all eight scratch cells at
uptime 709 s and the host survived past 780 s. Reading the block is
demonstrably not fatal. Writing it froze the box within the same second.

That moves the fault from "ANE access" to "engine writes, specifically
the scratch block", which is a much narrower target.
