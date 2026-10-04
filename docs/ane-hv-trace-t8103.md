# ANE m1n1 hypervisor trace on T8103: feasibility, risk, and protocol

Scope doc. Question: what does the macOS ANE driver do at job start that
Linux never does? The measured gap: on this one M1 laptop (T8103), the Linux
encoder runs 13,701 tasks in 138.7 ms per call; macOS runs the same encoder
in 111.4 ms from call 2 on, and 137-153 ms on call 1. Linux sits at the
macOS cold level forever. macOS changes a state during the first job. The
AP side gives no lever: no ANE clock readout, empty perfStats, SIP on.
Candidate (b): boot the macOS 13.5 kernelcache as an m1n1 hypervisor guest
and trace every MMIO write and read that AppleH11ANEInterface does at
stream open and job start.

Precedent: the T6021 (M2) HV work in `receipts/2026-09-23-m2-hv-trace/`
and `tools/m2hv_*`. This doc ports that machinery to T8103. Labels:
MEASURED = observed on the fleet or in committed receipts. INFERENCE =
reasoned, not observed.

## 1. What runs where

Roles for the trace window:

| Role | Host | Runs |
|---|---|---|
| Trace target | one M1 laptop (T8103) | m1n1 proxy image from its EFI partition; the macOS 13.5 kernelcache runs as the HV guest |
| Proxy host | one M2 Max laptop, Linux side | m1n1 proxyclient, `tools/m2hv_catch_and_run.sh`, the trace module |
| Shared link | one USB-C cable | the M1 left-back port to the M2 left-back port (i2c address 0x38 on the M2) |

The guest is the macOS 13.5 (22G74) kernelcache, not any installed macOS.
MEASURED precedents fix the guest mechanics:

- A 13.5 kernelcache under the m1n1 HV uses the stock entry contract
  (x0 = boot-args pointer). The 26/27 entry needs an undefined iBoot
  handoff struct plus an MMU change in m1n1; that route is disproven.
  Source: `receipts/2026-09-23-m2-hv-trace/2026-09-25-entry-abi.md`.
- The first T6021 13.5 runs died at 34-36 s because the guest had no root
  filesystem; the fix (guest console capture, AMFI-safe boot-args, CSR
  word, panic parking) is in `tools/m2hv_catch_and_run.sh`. Source:
  `receipts/2026-09-23-m2-hv-trace/2026-09-25-usb-death-root-cause.md`.
- RELEASE 13.5 panics on `cs_enforcement_disable` but honors
  `amfi_get_out_of_my_way`, `amfi_allow_any_signature` and
  `amfi_unrestricted_local_signing` with no gate. Source:
  `receipts/2026-09-23-m2-hv-trace/2026-09-25-amfi-boot-args-analysis.md`.
  The launcher's default boot-args already match; never add
  `cs_enforcement_disable`.

How the M1 enters proxy mode. MEASURED on the M1 laptop this session:

- The EFI partition carries the Linux boot chain's m1n1 at
  `/boot/efi/m1n1/boot.bin` (9,322,901 B, sha256 `ab649a8b...`), refreshed
  by `update-m1n1` from package `omarchy-mac-boot 20260921-10`
  (`m1n1-aurora 1.6.1.aurora3-1`), with a `boot.bin.old` backup pattern
  (sha256 `45dcb36f...`).
- The same partition already carries the T8103 13.5 kernelcache at
  `/boot/efi/asahi/kernelcache.release.mac13g` (25,579,919 B, sha256
  `49847f11...`). This is the board's `kernelcache.release.mac13g` member
  of the 13.5 IPSW; verify the hash against the IPSW member at stage 0.
- So the m1n1 that boots for a trace is the Linux chain's m1n1, re-staged
  to a proxy/wait image. No macOS volume is involved. The macOS boot
  policy is not touched.

Security policy. The trace needs NO macOS security change. The guest runs
from RAM through the proxy; no boot object on any macOS volume changes;
SIP stays enabled (recorded `csrutil status: enabled` in the 2026-10-03
macOS window entry; macOS on the second volume is 27.0). The reduced
security route (`csrutil disable`, `bputil`, `kmutil configure-boot` on a
macOS volume) is only the fallback in section 6(c), and it is an owner
decision.

Proxy host readiness. MEASURED on the M2 laptop (Linux) this session:
`tuxvdmtool 0.2.0` at `/usr/local/bin` with `dfu`, `reboot`, `debugusb`,
`disconnect`, `serial` subcommands on i2c 0x38; python 3.14.7; no pyserial,
no construct, no m1n1 checkout, no ACM devices at rest. Stage 0 must copy
the proxyclient tree plus `pylib` from the M1 laptop and install
python-pyserial. `tuxvdmtool reboot` and `tuxvdmtool dfu` are the M2-side
reset and last-resort recovery paths for the M1; `p.reboot()` through the
proxy is the first path while the proxy lives.

The m1n1 source of record on the M1 laptop: `~/m2proxy/hvproxy/proxyclient`
(the tree the T6021 HV runs used; the receipt names commit 4184923) and
`~/m2proxy/m1n1-1.6.1/`. m1n1 is public GPL code; read it, never edit it.

The guest ADT. iBoot hands m1n1 the board's real ADT; the HV guest sees it
unchanged (MEASURED: the T6021 runs uploaded the stub ADT). The trace
module derives the windows below from the live ADT at run start instead of
hard-coding them.

## 2. What to trace

MEASURED windows, from the 13.5 ADT parse for this board
(`/arm-io/ane`, compatible `ane,t8020`):

| Node | Window | Trace mode |
|---|---|---|
| `ane` reg[1] (register and perf block) | `0x23b700000 + 0x8c000` | reads + writes |
| `ane` reg[0] (ANE region) | `0x26a000000 + 0x2000000` | writes only |
| `dart-ane` (1 SID, vm-size 3.75 GB) | `0x26b800000`, `0x26b804000`, `0x26b810000`, `0x26b820000`, each `+ 0x4000` | reads + writes |

Inside reg[1] sit the power/perf words the job-start question is about
(pmgr ANE device words from the same parse): `ANE_SYS` at `0x23b700470`
and `ANE_SYS_CPU` at `0x23b70c000`.

Rules, all carried from the T6021 work:

- Trace ONLY these ANE-related ranges (`hv.trace_range`). No CoreSight
  window: reading an unclocked debug domain hangs the guest, and the
  standing lab rule bans the CoreSight offset. This ADT lists no ANE
  CoreSight node.
- Filter the log: reads and writes on reg[1] and the DART, writes only on
  the 32 MiB reg[0]. Bounded log size: T6021 runs produced 138 KB to
  1.04 MB per run; the module writes one `-l` file per run and the run is
  time-bounded (section 3).
- Expected first events: the driver's power-down writes when probing ends.
  On T6021 these were the last events of every guest boot. Do not read the
  engine window while the island is unpowered (fleet falsified list).

## 3. Protocol, stop rules, rollback

Stage 0 — prep, no target access (half a day):

1. Verify `kernelcache.release.mac13g` on the EFI partition against the
   13.5 IPSW member (sha256). Build the stage-0 input manifest.
2. Copy proxyclient, `pylib`, `tools/m2hv_catch_and_run.sh`,
   `m2hv_guest_debug.py`, `m2hv_ramdisk.py` from the M1 laptop to the M2
   laptop; install python-pyserial; smoke one proxyclient import.
3. Build the T8103 proxy/wait image from the hvproxy m1n1 tree. The image
   MUST chainload the normal payload (U-Boot) on a timeout if no client
   catches it. The T6021 lesson: a proxy image with no timeout strands the
   target. Verify the fallback boots Linux before any trace day.
4. Write `trace_ane_t8103.py` from `trace_ane_v5.py`: the windows in
   section 2, derived from the live ADT at start. Dry-run the module logic
   off-box.
5. Write the boot-stage script: `set -euo pipefail`, backup `boot.bin`
   with sha256, stage the proxy image, byte-verify, and a restore mode.

Stage 1 — smoke (half a day): catch the M1 proxy from the M2 laptop, run
the launcher with the 13.5 kernelcache, console module, `M2HV_TIMEOUT=600`.
Pass: the vuart log carries kernel console past 40 s (the old death time)
and the link stays up. The ramdisk-root path
(`M2HV_PREMOD=tools/m2hv_ramdisk.py`, `rd=md0 -rootdmg-ramdisk`) is the
root fix; it is staged for T6021 but unrun on any host, so stage 1 also
probes it.

Stage 2 — traced minimal client (one day): the guest root carries a small
arm64e IOKit client that opens the ANE service and submits host TM/TQ
tasks (extend the staged `ane_open` client; the M1 kext path is
host-driven TM/TQ). Trace active on the section-2 windows. Deliverable:
the ordered write/read list of the kext's stream-open and first-submit
sequence.

Stage 3 — encoder job (one to two days): run the CoreML encoder workload
in the guest and correlate the 25 ms step with traced writes. Gated on
stage 2 landing and on the seal-check risk below.

Stop rules:

- Proxy silence over 60 s with the guest parked: Ctrl-C to the hv shell,
  `p.reboot()`.
- Guest panic: the launcher's default profile lets the SoC reset itself
  and the proxy image falls back to Linux. `M2HV_DEBUG=1` (parked panic
  spin) only for a hang that the default profile cannot show.
- Two consecutive failed catches: stop, restore `boot.bin`, report.
- While a trace is live, nobody reboots the M1 laptop or touches its USB.
  One owner per laptop; the M2 laptop owns the cable during the window.

Rollback: restore `/boot/efi/m1n1/boot.bin` from the hash-verified backup
(or run `update-m1n1` to re-stage the package build), `tuxvdmtool
disconnect` then `reboot` to clear the link, verify a normal Linux boot
(new boot ID, `uname`, ANE device present). The macOS volumes are never
booted and need no restore.

Timing overhead. A traced run is slower. Trace runs give the write/read
SEQUENCE only. Every timing claim (138.7 vs 111.4 ms) stays on untraced
runs. Never quote a traced-run duration.

Expected artifacts per run (private lab notebook): `catch.log`,
`run.log`, `vuart.log`, `trace.log`, the extracted write-list TSV, one
SHA256SUMS. Boot-stage scripts and image hashes go with them.

## 4. Minimum alternative

Smoke only: stage 1 with no ANE trace and no client. It proves the HV
mechanics on T8103 (proxy image, catch, 13.5 guest console past 40 s) at
about one session of work and zero ANE data. If the owner wants the
cheapest first step, this is it; the job-start question stays open.

## 5. Risks

| Risk | Effect | Mitigation |
|---|---|---|
| `boot.bin` staging error | M1 Linux boot broken | hash-verified backup, self-falling-back image, `tuxvdmtool dfu` + restore as last resort |
| Shared cable is the mutual reset path | trace window blocks the other lane's resets | M2 laptop owns the cable and the window; announce the window; nobody touches the M1 USB while live |
| HV overhead | wrong timings | trace runs for sequence only (section 3) |
| Guest panic or watchdog | lost run, SoC reset | launcher default profile resets cleanly; bounded `M2HV_TIMEOUT` |
| Guest never reaches userspace | no job-start data | the seal-check risk on the modified restore root is open (staged, unrun); stage 1 probes it first; the fallback is section 6(c) |
| Reads of an unpowered island | wedge (falsified list) | ANE windows only, reads gated to the register/DART ranges, no CoreSight |
| Wrong AMFI boot-arg | RELEASE guest panics at boot | use the launcher default set verbatim; never `cs_enforcement_disable` |
| 26/27 kernelcache attempted | known dead end | 13.5 kernelcache only; 26/27 entry is disproven (receipt) |

## 6. Owner decisions

(a) Not needed for stages 0-2: no SIP or boot-policy change on any macOS
volume. State this plainly: the usual "reduced security" worry does not
apply to the proxy route.

(b) Needed: staging the proxy image on the M1 laptop's Linux boot chain
(EFI `m1n1/boot.bin`). This is a reversible boot-chain change on the Linux
side; boot writes follow the set -euo pipefail + byte-verification rule.

(c) Needed only if the ramdisk-root route fails: a macOS 13.5 volume with
a custom boot object under m1n1. That path needs reduced security
(`csrutil disable`), `bputil`/`kmutil configure-boot`, and hands in 1TR.
The 1TR pairing rule applies: boot the macOS volume first, shut down from
macOS, then hold the power button for recovery. This is a policy change on
the owner's macOS install; the owner decides.

(d) The in-guest submit client (an arm64e IOKit binary) runs in guest RAM
only; no policy question, listed for visibility.

## 7. Estimate

With the M2 laptop as host and all T6021 tooling reused: stage 0 half a
day to a day, stage 1 half a day, stage 2 one day, stage 3 one to two
days. Total about 2.5 to 4.5 agent-days, plus one owner decision (b)
before any target reboot.

Missing before stage 0: python-pyserial on the M2 laptop; the mac13g
hash verification; the fallback-capable proxy image build; the T8103 trace
module; the extended in-guest submit client. No network fetch is needed;
every file copies from the M1 laptop's home directory or this repository.

## 8. Facts missing (explicit)

- sha256 of the ESP `kernelcache.release.mac13g` vs the IPSW member:
  hash of the ESP copy measured (`49847f11...`); IPSW member comparison
  not done.
- Live commit identity of the hvproxy proxyclient tree: receipt-sourced
  (4184923); the directory is not a git checkout, so not re-verified.
- csrutil/bputil state of the M1 laptop's macOS: recorded `csrutil status:
  enabled` on 2026-10-03; not re-verified live in this session (no ssh
  route answered on three probes).
- Whether the modified restore root passes the guest's seal check:
  untested on any host.
- Whether a 13.5 guest on T8103 reaches userspace at all: the point of
  stage 1.
- T8103 `dart-ane` driver behavior in the guest (DART owned by iBoot's
  map at handoff on T6021; T8103 map state at guest start not captured).

## 9. Sources

- `receipts/2026-09-23-m2-hv-trace/` (README; amfi-boot-args; entry-abi;
  ramdisk-root-route; usb-death-root-cause; catch scripts).
- `tools/m2hv_catch_and_run.sh`, `tools/m2hv_guest_debug.py`,
  `tools/m2hv_ramdisk.py`, `tools/m2hv_entry_abi.py` (26/27 only).
- `docs/t6021-ane-bringup-findings.md` (T6021 context).
- Private lab notebook: 13.5 ADT parses for this board
  (`adt-13.5-ane.txt`, `adt-qos-13.5-j293.txt`); the 2026-10-03 macOS
  window entry (csrutil, encoder ramps); the h243 Linux first-call entry.
- Live probes, 2026-10-04: EFI partition layout and hashes, package
  versions, `tuxvdmtool 0.2.0` help, M2 laptop python and device state.
