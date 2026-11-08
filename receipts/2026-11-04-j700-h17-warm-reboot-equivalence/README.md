# J700/H17: warm reboot == cold boot; and the "unknown symbol" was my error

## 1. The cold-boot requirement was wrong

I had been demanding a cold power cycle before every probe. That was never
measured. What was measured is only this: engine register state persists
across *module loads within one boot* (the C-block handshake satisfied on
run 1, timed out on run 3 with `0x18a4008` holding a value from an earlier
write). I generalised that to "reboots don't clear it", which does not follow.

Test on this boot `90cd8a95-f770-4c43-96ce-fc09e71dc7fb`, reached by a
plain `sudo reboot` (warm):

```
AF_READ offset=0x0   value=0x16
AF_READ offset=0x338 value=0xffff
```

Identical to both cold-boot readings
(`a54fb0ee…` af_scan: `0x16`/`0xffff`; `376d6eb5…` hal probe: `0x0` live
`0x16`). So after a warm reboot the engine reads exactly what it reads
after a cold one, for this observation.

**Consequence: automation is viable.** The one-run-per-boot rule is a
*harness* rule (one measurement per boot), and a plain reboot satisfies it —
the new boot's journal contains no `ane_h17` activity, which is exactly what
`run-af-scan.py` already checks.

Limit, stated plainly: this shows read-only state matches after a warm
reboot. It does **not** show that a warm reboot clears residue left by a
*write* sequence. That specific case is still untested, and a reset during a
write probe is still the untrustworthy case.

## 2. "Unknown symbol in module" was a harness bug in my test

Three modules — `ane_h17_af_scan`, `ane_h17_clk_scan`, `ane_h17_clk_wide` —
appeared to stop loading, including one that had loaded successfully earlier
on the same boot. I chased it through symbol exports, ELF sections, BTF,
vermagic, `__ksymtab` presence and a clean rebuild. None of it was the cause.

The cause: those modules open the caller-supplied `log_path` with

```c
filp_open(log_path, O_WRONLY | O_APPEND | O_SYNC | O_NOFOLLOW, 0);
```

**without `O_CREAT`**. The launcher pre-creates the log file, so real runs
always worked. My ad-hoc checks passed a path that did not exist, `filp_open`
returned `-ENOENT`, the module init returned `-ENOENT`, and kmod renders
`ENOENT` from `finit_module` as **"Unknown symbol in module"**. The kernel
prints nothing for it, which is why dmesg and the journal were silent.

Every test that appeared to exonerate the symbols did exonerate them. The
error message was simply lying about what failed.

Fix: the three modules now pass `O_CREAT | 0600`, and take the mode argument
seriously instead of `0`. Verified: with a non-existent log path the module
now loads (`rc=0`) and creates the file.

## 3. What this changes

- Stop requiring cold boots. Use `sudo reboot` and the existing
  one-run-per-boot gate.
- The pending `ane_h17_clk_wide.ko` (192 bounded reads) can be run after a
  plain reboot.
- A bogus `-ENOENT` from `insmod` should be read as "the module's init
  returned ENOENT", not as a missing symbol. The message is kmod's, not the
  kernel's.

## 4. Not established

- That a warm reboot clears state left by engine writes. Untested.
- That a hung probe recovers unattended. A PMU reset has previously returned
  to a new boot on its own, but a hard hang would not, and nothing on this
  machine can power-cycle the battery/charger path unattended.
