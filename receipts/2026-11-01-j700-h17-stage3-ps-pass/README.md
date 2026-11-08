# J700/H17 stage 3: ANE_SYS power-domain probe — PASS

First device-qualified result in this project. One bounded test: a single
genpd vote on `ps_ane_sys` and one PMGR read. No ANE engine MMIO, no DART,
no firmware, no mailbox, no reset.

## Identities

- Kernel `7.2.11-6-1-ARCH`; boot ID
  `1d31ba24-5f08-4962-ae99-37accde4ad5c`.
- Repo HEAD `78e08c295067618e6f49633816e6ae22bc275f4b`.
- Module `ane/h17/ane_h17_ps_probe.ko`, SHA-256
  `062131061c4a521bb06c1a145b0fcf72ee22fb3a4b0f56c0409e2c7eb16c9053`,
  vermagic `7.2.11-6-1-ARCH SMP preempt mod_unload aarch64`.
- `ane/h17/preflight.py`: verdict `PASS` (exit 0) before the load.

Preconditions confirmed from the live tree: `ane@400000000` is `disabled`,
all three ANE DARTs are `disabled`, `power-controller@290` is `okay`, and
the `ane-h17-ps-probe` overlay node is present and `okay`.

## Command and result

```
sudo insmod ane/h17/ane_h17_ps_probe.ko optin=1     # exit 0

[96324.067434] ane_h17_ps_probe soc:ane-h17-ps-probe:
    ane_h17_ps pmgr off=0x290 word=0x23ff target=0xf actual=0xf
[96324.067439] ane_h17_ps_probe soc:ane-h17-ps-probe:
    ane_h17_ps RESULT stage=3 verdict=PASS reason=ane-sys-on
```

`sudo rmmod ane_h17_ps_probe` exited 0 and `/proc/modules` no longer lists
it.

## Decode of `word = 0x23ff`

| Field | Bits | Value |
| --- | --- | --- |
| TARGET | 3:0 | `0xf` |
| ACTUAL | 7:4 | `0xf` |
| WAS_GATED | 9:8 | `0x3` |
| BUSY | 11 | `0` |
| AUTO_ENABLE | 28 | `0` |

TARGET and ACTUAL both `0xf` with BUSY clear means the domain is on and
settled. `WAS_GATED = 0x3` indicates it had been gated before the vote.

## What this proves

- The genpd/runtime-PM raise path works for `ps_ane_sys` on J700.
- The ANE_SYS domain was **gated** before the vote, i.e. the ANE is not
  left powered at boot. A blind engine read would have been taken against
  an off domain — which is the documented hang class.
- The power-domain prerequisite for any later register access is now
  demonstrated, not assumed.

## What it does not prove

- No ANE engine register was read; no `ACTUAL` check was made for the
  ANE-CPU island at PMGR `0xc000`.
- No DART, firmware, mailbox, or task path.
- This is not register-access qualification: the engine window still needs
  a confirmed-powered CPU island before any read.

## Next bounded step (proposal, not yet run)

Express `ps_ane_sys_cpu@c000` as a genpd node (the DTS patch in
`docs/t8140-ane-power.patch`) and read the PMGR words `0xc000/0xc008/`
`0xc010/0xc018` with a pmgr-only staged probe. Those are the same safe
class as this test (PMGR read, always-on block, no engine access). Only
after the CPU island reports `ACTUAL=0xf` does an engine read become a
candidate.
