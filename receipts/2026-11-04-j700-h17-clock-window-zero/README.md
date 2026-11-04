# J700/H17: ANE clock window reads all zero

Boot `522531d1-45ac-4bd3-8da4-748e2dd4536b`, single run at uptime ~60 s,
commit `ec2e980`, manifested and fsynced before load.

## Result

Domain votes succeeded (`0xc000 = 0x1f0003ff`, `0xc008/0xc010 = 0x3ff`),
then 24 reads of the ANE clock window at `0x3082c8000 + offset`:

```
CLK_READ offset=0x1000 value=0x0
CLK_READ offset=0x1004 value=0x0
...
CLK_READ offset=0x101c value=0x0
CLK_READ offset=0x1100 value=0x0
...
CLK_READ offset=0x113c value=0x0
COMPLETE
```

**No fault, no freeze, no SError.** Every read returned `0x0`.

## What this does establish

- The window `0x3082c8000..0x3082c8fff` is **decoded and safe to read** from
  Linux. A non-existent physical address faults on this platform; this one
  does not. So the address is real, which independently supports the ADT
  `reg` decoding in `receipts/2026-11-04-j700-h17-clock-window-derivation/`.
- The read is repeatable and does not destabilise the machine — useful,
  because the earlier "unknown window" reads were feared.

## What it does not establish

Reading zeros has at least three explanations, and this run cannot separate
them:

1. **The block is not clocked/powered**, so it returns zeros rather than
   faulting. That would be circular in the worst way: the clock we want to
   enable is gated until it is enabled.
2. **The base is wrong.** `map 0x71 -> resource 5` was an inference from a
   size match and PMGR declaring the window, never proven by locating
   `initRegMap(0x71, ...)`. If map `0x71` binds elsewhere, these offsets are
   simply the wrong registers.
3. **The registers are genuinely zero** in this state, e.g. a gate block
   where "disabled" is encoded as an all-zero word.

Compare with the AF block in `0x400000000`, which returned `0x16`,
`0xb000b`, `0xffff` and accepted writes. A live, initialised block does not
look like this one.

## Why I am not going to guess a write here

The whole point of the exercise is to flip a gate bit. Writing an inferred
value into a 16 KiB window whose contents are uniformly zero, on a block
that may be powered down, is exactly the class of access that has frozen
this machine. Nothing here identifies a bit to set.

## Next options

1. **Widen the read** across the whole `0x4000` window (or `0x0..0x1000`) to
   see whether *anything* in it is non-zero. Cheap, still read-only, still
   one boot. If the entire window is zero, base-or-power is wrong and the
   mapping should be abandoned rather than probed further.
2. **Attack the base question offline**: find `initRegMap` for map `0x71`
   and the `_regMaps[0x71][die].memMap` assignment, which would either confirm
   resource 5 or point elsewhere.
3. **macOS route**: a kernel-debugging session on the Neo is the only way to
   read the live `_regMaps`/`_regGroups` tables. Stock macOS will not expose
   them to a script.
