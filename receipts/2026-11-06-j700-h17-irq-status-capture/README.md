# J700/H17 post-wake IRQ-status capture

Status: **read-only snapshot rung PASS. No PING or task was sent.** This
measurement is not a route, reply-schema, or IRQ-ownership qualification.

## Run identity

- Board: Apple MacBook Neo, J700/T8140; compatible `apple,t8140-ane`
- Boot: `3fcfb31b-577c-43f3-8a1e-094098397d39`
- Kernel: `7.2.11-14-1-ARCH`
- Repository: `neo/h17-bringup`, commit
  `f41b2293e126375c2aa08654a9090ee663bd897f`, clean at run
- Module SHA-256:
  `5353f8c356d31bc5431b49e010591379343a02781f13d1f927e5d2833b10c91d`
- Firmware SHA-256:
  `32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c`
- Gate: one-shot `omarchy-ane-h17-bootwake.service`; runner
  `ane/h17/run-bootwake.sh`; parameters `optin=t8140 stage=boot
  confirm_read=1 hello_wait_ms=1000 irq_read=1 scratch_read=1`
- Verdict: `PASS`, reason `wake-irq-scratch-status-and-poll-skipped`

## Observation

After the firmware wake word `SCRATCH7=0x08042006`, the runner took exactly
one read of IRQ status `0x188c000`; it was `0x00000000`. It then read
SCRATCH0..7 exactly once:

| Word | Value |
|---|---:|
| SCRATCH0 | `0x00000008` |
| SCRATCH1 | `0x0000c440` |
| SCRATCH2 | `0x00000100` |
| SCRATCH3 | `0x00f00000` |
| SCRATCH4 | `0x00000000` |
| SCRATCH5 | `0x00100090` |
| SCRATCH6 | `0x00000001` |
| SCRATCH7 | `0x08042006` |

The H16 RTKit management poll was explicitly skipped. This rung did not
register channels, ring a doorbell, acknowledge an IRQ, send a command, or
submit a task. The source has one guarded read of the T8140 `irq_status`
offset (`ane/h16/ane_h16_main.c`, `irq_read` block); the captured log contains
one status line and no access to the IRQ ack offset `0x1890000`.

Interpretation is deliberately narrow: at this post-wake snapshot instant,
no status bit was set. It says nothing about a line that may assert later,
and does not identify the owner of an IRQ line or prove a queue route. The
earlier observations still leave the `SHAREDMALLOC` selector's runtime value,
its correspondence to host channel 7, the response route, and PING's accepted
wire payload/status unresolved. **Keep `CSNE_CMD_PING` unsent.**

## Runner correction

The first boot attempt after the requested reboot was refused at the module's
`confirm_read` guard, before power-domain attachment, mapping, or MMIO, because
the runner omitted the mandatory `confirm_read=1`. The failure log is retained
in `/var/tmp/opencode/h17-bootwake-90117065-0bbd-440a-90a7-ef75971af85b/`.
Commit `f41b229` added that required opt-in and extended
`ane/h17/test-bootwake-verdict.sh` to assert the read-only rung flags. All six
fixtures passed before this successful fresh-boot run.

## Raw evidence

- `run.log` — run identity, hashes, and complete rung log
- `dmesg-rung.log` — captured kernel log window
- `verdict.txt` — one-shot runner verdict

SHA-256:

```
28980c6180780075ee6cada130e056c27d85fecc24a66e3c18e9a361e4c7c526  run.log
a018d8f161f0916cf9f9d3b4d0d39394950a1e80d9502fd0a251618d2e693acf  dmesg-rung.log
21d9f428ebbdad782f63ff16a93d41dbcd01c818b9c237496dc11d9f30e66481  verdict.txt
```
