# J700/H17 stage 3c: sub-domain genpd vote — PASS

Third device-qualified test. The ANE power domains are raised through the
pmgr driver (TARGET write + ACTUAL poll), not a raw ps-word write.

## Identities

- Kernel `7.2.11-14-1-ARCH`; boot ID
  `3134730e-2fb8-49ae-aa43-42f37ec08457`; board `Apple MacBook Neo`
  (`apple,j700`).
- `ane/h17/preflight.py`: verdict `PASS` (exit 0).
- Overlay: `omarchy-ane-h17-ps-probe-dt 0.1.0-2`, merged into
  `boot.bin`; live nodes `power-controller@c000/@c008/@c010` with
  labels `ane_sub0/1/2`, plus consumer `ane-h17-cpu-vote`.
- Module `ane/h17/ane_h17_cpu_vote_probe.ko`, vermagic
  `7.2.11-14-1-ARCH`, built after the reboot target was identified.

## Result

```
sudo insmod ane/h17/ane_h17_cpu_vote_probe.ko optin=1   # exit 0

ane_h17_cpu_vote ane_sys off=0x290  word=0x23ff     target=0xf actual=0xf was_gated=0x3 busy=0
ane_h17_cpu_vote sub0    off=0xc000 word=0x1f0003ff target=0xf actual=0xf was_gated=0x3 busy=0
ane_h17_cpu_vote sub1    off=0xc008 word=0x3ff      target=0xf actual=0xf was_gated=0x3 busy=0
ane_h17_cpu_vote sub2    off=0xc010 word=0x3ff      target=0xf actual=0xf was_gated=0x3 busy=0
ane_h17_cpu_vote RESULT stage=3c verdict=PASS reason=all-subdomains-on
```

`rmmod` exited 0, and the genpd summary returned to `off-0` for
`ane_sys`, `ane_sub0`, `ane_sub1`, `ane_sub2`.

## What the words show

| Word | TARGET | ACTUAL | AUTO | BUSY |
| --- | --- | --- | --- | --- |
| `0x290` | `0xf` | `0xf` | 0 | 0 |
| `0xc000` | `0xf` | `0xf` | `0xf` | 0 |
| `0xc008` | `0xf` | `0xf` | 0 | 0 |
| `0xc010` | `0xf` | `0xf` | 0 | 0 |

`0xc000` is the only word with the `APPLE_PMGR_PS_AUTO` field set
(`GENMASK(27,24) = 0xf`), consistent with it being the head of the
sub-chain (MPM-like). The CPU/TD role of `c008`/`c010` is still
unassigned.

## What this proves

- The pmgr driver's own TARGET-write + ACTUAL-poll raises every ANE
  sub-domain on J700. This is the safe path the H16/H17 family needs;
  no raw ps-word write was performed.
- The domains settle (`BUSY=0`) and release (`off-0`) cleanly.

## What it does not prove

- No ANE engine register has been read. The engine block still has to be
  shown readable while these domains are held on.
- Whether `c000/c008/c010` are sufficient for block access. The H16 note
  that `ane_td`/`ane_base` are required and that partial raising plus
  block access resets the machine is the reason the engine read remains
  a separate gate.

## Next step: stage 4 (engine read)

A module that keeps the three links active and reads one whitelisted
engine register inside that window — `CPU_STATUS` at `0x1600048`
(H14G reads `0x2a` before start) or `RVBAR` at `0x1050000` (64-bit).
This is the first engine-window access and carries the residual hang
risk. It is not attempted here.
