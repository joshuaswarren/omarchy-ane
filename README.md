# Omarchy Neural Engine

Apple Neural Engine support for Omarchy Linux: a DRM accelerator driver and userspace library. Bind and execute are proven on M1 (`T8103`, `apple,t8103-ane`) and M1 Max (`T6001`, `apple,t6000-ane`). Every other SoC still needs its own PMGR, DART, SET, and TM offsets. eiln's original reverse engineering targeted one M1; this fork is where the other chips get wired.

**Current hardware status (2026-09-25, evening):** m1-host (T8103) is
recertified on the fresh Arch boot. The Qwen ANE staged-decode cell passes
against same-SoC macOS: decode 1.49x, TTFT 0.84x, e2e 0.69x, and prefill-512
1.223x (all PASS, 100/100 tokens exact); the Parakeet golden contract is
bit-exact on the installed module. Receipts live in
[joshuaswarren/ane-linux-experiments](https://github.com/joshuaswarren/ane-linux-experiments)
(`2026-09-25-jwm1-qwen-ane-layout-gate`, `2026-09-25-qwen-ane-export-513`,
`2026-09-25-jwm1-parakeet-golden-rerun`, `2026-09-25-jwm1-kernels2-clean`).
t6001-host (T6001) Linux ANE is live; TM recovery on T6001 now drains
retained tm/tq state (kill-race 10/10 reopen-clean, no reboot). T6021
(t6021-test-host) has no host TM path; the firmware program has proven the
release sequence on T6021 and T6001 (status 0x28), and with the VENC power
leg up the firmware reaches its service loop, but the mailbox FIFO never
drains and no RTKit HELLO has arrived; CoreSight is fused off on T6021.
T6021 ANE is not live-inference-qualified. The canonical record is
[docs/t6021-ane-bringup-findings.md](docs/t6021-ane-bringup-findings.md).
macOS CoreML / `aned` measurements do not establish Linux execution.

- `ane/`: DRM accelerator kernel module.
- `libane/`: userspace loader and submission library.
- `bindings/python/`: Python shared-library bindings.

This fork's `main` serializes submissions, holds GEM references across execution, waits for request-tagged last-task finish events, and retires the matching task-queue slot. While submits are in flight it also holds every CPU cluster at its top p-state (`ane_boost`, module parameter `boost_idle_ms`, default 100 ms after the last submit, 0 disables): on M1-class SoCs the memory-side performance state follows the CPU clusters, and a parked submitter halves the ANE's memory bandwidth. An uncertain completion blocks new work and normal reclamation, pins the module against ordinary unload, and requires a reboot. On T6001, recovery drains retained tm/tq state and names the module-reload door; the T8103 power-on reset path is unchanged. Runtime power must remain on. Forced platform/DT removal is unsupported: driver-core teardown can release managed resources despite the module pin.

The library requires driver ABI 1: successful submission guarantees terminal completion and CPU visibility. Older drivers are rejected; output values are never used as completion signals. Build the module against matching kernel headers with `make -C ane`, then build the library and Python binding from this checkout with `make -C libane && make -C bindings/python/dylib`.

Nothing here compiles a model. Programs come from the H13 backend in [joshuaswarren/mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler), whose runner validates each package and its reference outputs before it opens this library.

The ABI-1 stack passed all eight compiler qualification packages and a finite-input overflow case producing `+inf` (historical dated evidence, m1-test-host Linux boot prior to 2026-09-18; see the "Current hardware status" note above). Every output matched on three warmups and 30 measured iterations per package. With the optimized compiler, 512-element add-ReLU uses two programs and measures 0.679 ms per-op, or one program and 0.160 ms fused. The 768-to-1024-to-768 MLP uses 77 programs and measures 32.170 ms, versus 92 programs and 41.523 ms before whole-tensor binary selection on the same driver boot. Timing spans input transfer through output readback, excluding setup and reference evaluation. Cold power-on repeatability and general chain fusion remain unqualified. These are the standing numbers in the compiler evidence linked below; they are NOT a current recert.

Compiler evidence: [M1 native progress](https://github.com/joshuaswarren/mil-hwx-compiler/blob/main/receipts/2026-09-06-m1-native-progress.json). Raw Apple firmware and private host details are not distributed.

## Packaged install

The package installs the driver. You do not need `install.sh`.

- **DKMS.** `dkms.conf` builds `ane.ko` from `ane/` for each kernel that has headers. DKMS builds it again after each kernel update. Install `linux-aurora-headers`. The build does not include the T6021 lab modules in `ane/t6021/`. The chip gate in `ane.ko` decides which SoC binds. T8103 and T6001 bind. T6021 does not bind.
- **Device access.** `packaging/70-omarchy-ane.rules` sets the ANE `/dev/accel` node to `root:render`, mode `0660`, with the `uaccess` tag. The user at the local seat can open it. Other users and services must be in the `render` group. `install.sh --ane` from mlx-omarchy adds the user to `render`. It continues to work.
- **Check.** Run `omarchy-ane-check`. It checks the device-tree node, the module build for the running kernel, the loaded module, the bound device, and the device node. It does not load the module. It exits with 1 when a check fails. After a kernel update, run `omarchy-ane-check --installed`. It checks the module build for each installed kernel.
- **Firmware.** M1 chips need no firmware from Linux. iBoot loads it. On M2 Max (T6021), run `sudo omarchy-ane-firmware-fetch`. It reads the stub macOS version. It downloads only the ANE file from Apple and unwraps it. It installs the file only when the size and SHA-256 agree with the driver. It needs only Python 3. It stops and installs nothing when the version is unknown, the network is down, or the hash is different. We do not distribute Apple firmware.

## Chip coverage

Linux `compatible` is the driver match. Internal names follow Apple's SoC table (H13G, H14J, …); unknown means exactly that. Each SoC carries one of three driver states, mirroring the qualification tiers on the descriptors in `ane/src/ane_drv.c`:

- **qualified** — execution proven on this silicon; binds normally.
- **recognized-untested** — constants known, never run on the part; the driver binds only with `ane.allow_unqualified=1` and logs a loud warning naming what is unverified.
- **unsupported** — no proven constants; the driver refuses to bind (a guessed SET-block base external-aborts the SoC, so none is carried) and its refusal message names the data needed.

Tier is decided per `compatible`, so T6000 silicon reads recognized-untested below while its `apple,t6000-ane` descriptor is qualified on T6001 evidence.

| Marketing | SoC | Internal | Linux ANE `compatible` | Driver status | Test confirmation | Data needed |
| --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 | H13G | `apple,t8103-ane` | qualified (recertified 2026-09-25 on the fresh Arch boot) | bind + exact fp16 64-el smoke; o-proj + attention islands E2E certified 2026-09-17; Qwen ANE staged decode E2E PASS 2026-09-25 (1.49x macOS decode, 1.223x prefill-512) | none |
| M1 Pro | T6000 | H13J | `apple,t6000-ane` | recognized-untested | none on T6000 silicon | a tester plus the board DART/pmgr overlay (compatible and SET base shared with T6001 are proven); two community DT captures arrived 2026-09-17 |
| M1 Max | T6001 | H13J | `apple,t6000-ane` | qualified (t6001-test-host Linux ANE is live; TM recovery drains retained tm/tq state, kill-race 10/10 reopen-clean, 2026-09-25) | bind + exact fp16 64-el smoke; o-proj + attention islands E2E certified 2026-09-17, 104/104 | none |
| M1 Ultra | T6002 | H13J | `apple,t6000-ane` | recognized-untested | none | a tester plus a board overlay; dual-die SET base unverified — confirm before any bind |
| M2 | T8112 | H14G | unknown | unsupported | — | ANE node DT capture (quick collector works with no ANE node), SET-block base; H14 compiler backend is unqualified |
| M2 Pro | T6020 | H14J | `apple,t6020-ane` | unsupported | — | SET-block base, a qualified H14 compiler backend, and the board DART/pmgr overlay; three community DT captures and one native-macOS IORegistry capture arrived 2026-09-17 |
| M2 Max | T6021 | H14J | `apple,t6021-ane` | recognized-blocked; not live-inference-qualified. **13.5 (22G74) legacy ChMan `legacy_only` transport: EXPERIMENTAL, default off; full post-DONE handshake completes** (SCRATCH3 ack cleared by the fw after H2T slots are host-initialized; no command submission yet). Sha-pinned 13.5 selene `a9c4b771…`. Backing envelope is `fw_extra_ram=0x200000` (16 KiB-aligned, ≤ SZ_16M) + DMA32; the 24 MiB attempt was rejected before allocation — not a DMA-size finding. Receipts: [docs/t6021-ane-bringup-findings.md](docs/t6021-ane-bringup-findings.md), [receipts/2026-09-27-t6021-ring-owner-h2t-init.md](receipts/2026-09-27-t6021-ring-owner-h2t-init.md). | The [2026-09-18 qualification attempt](https://github.com/joshuaswarren/ane-linux-experiments/blob/main/receipts/2026-09-18-t6021-qualification.md) did not probe with its then-current device tree. Separate [firmware analysis](https://github.com/joshuaswarren/ane-linux-experiments/blob/main/receipts/2026-09-18-t6021-engine-layout-mined.md) identifies a firmware-owned task manager; H13 host TM/TQ offsets are not a safe bring-up path. Proven state and open blockers: [docs/t6021-ane-bringup-findings.md](docs/t6021-ane-bringup-findings.md). `legacy_only` excludes every RTKit surface (no devm_apple_rtkit_init, no RX poll worker, no apple_rtkit_boot). The legacy ChMan host server (SHAREDMALLOC/TERMINAL) was excluded from this publication pending independent review of acquire ordering, unchecked ring offsets/size/bit before deref/modulo, and TERMINAL cursor not returning the slot to the producer. | Qualified firmware boot (post-DONE on legacy ChMan), DART mappings, mailbox submission, and live output checks; macOS measurements alone do not qualify thi…
| M2 Ultra | T6022 | H14J | unknown | unsupported | — | DT capture, SET-block base (dual-die), qualified H14 backend |
| M3 | T8122 | H15G | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M3 Pro | T6030 | H15J | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M3 Max | T6031 / T6034 | H15J / H15S | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M4 | T8132 | H16G | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M4 Pro | T6040 | H16S | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M4 Max | T6041 | H16C | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M5 | T8142 | H17G | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M5 Pro / Max | T6050 | H17S / H17C | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M5 Ultra | TBD | TBD | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |
| M6 | TBD | TBD | unknown | unsupported | — | DT capture, SET-block base, qualified compiler backend |

### Contributing a row

Run the mlx-omarchy quick collector on the target machine: `python3 scripts/collect_quick.py --out capture.json` from an mlx-omarchy checkout. No install and no driver needed — it captures the ANE/DART/PMGR/AIC device-tree data even when no ANE node is present, redacts personal data, and finishes in seconds. Submit with `scripts/collect_submit.py` into the community diagnostics archive. The SET-block base is the one constant the collector cannot take from the device tree; on macOS, IORegistry-derived data helps too (a macOS ANE probe is being added to the collector).

Bring-up on a new SoC beyond the capture: PMGR labels and ranges, DART windows, SET/TM physical addresses, netconsole, and a bound `/dev/accel/accel0` before any program submit. Going from M1 to M1 Max was hours of reboot, netconsole, and PMGR/SET work; expect that on each new part. Do not write SET `0xf` from userspace. T6001 SET0 is `0x28e08c000`; genpd raises it on the driver's probe-time runtime resume and the driver holds that reference until remove, so the partition stays up while the module is bound. The T6001 overlay is `ane/t6001-j316c-set-domains.dts`. Product install is still a packaged board DTB, not a live overlay.

## T6021 legacy ChMan transport (`legacy_only` module parameter)

The M2 Max (T6021) ANE on Linux needs its ASC firmware boot, then either
the mainline RTKit handshake or, on the 13.5 (22G74) preloaded selene
image only, the firmware's legacy ChMan path. The `legacy_only=1` knob
takes the second route and excludes every RTKit surface — no
`devm_apple_rtkit_init`, no RX poll worker, no `apple_rtkit_boot` — so the
generic mailbox/RTKit path cannot interfere with the fw post-DONE
sequence. Built and verified as of `45dc9a7`; receipts in
[receipts/2026-09-27-t6021-13_5-legacy-only-publish.md](receipts/2026-09-27-t6021-13_5-legacy-only-publish.md).

**Scope — strict, default off, experimental-only:**

- The version contract is structural: `ane_t6021_fwload.c` sha-pins the
  13.5 (22G74) selene image `a9c4b771…` and refuses any other. The
  14.x/15.x/26/27 firmware paths are NOT covered. Do not enable
  `legacy_only` outside the Asahi stub's 13.5 preloaded image.
- Backing envelope is `fw_extra_ram=0x200000` (2 MiB, 16 KiB-aligned at
  `ANE_T6021_FW_ALIAS_PAGE = 0x4000`) + DMA32. The 24 MiB attempt
  (`0x1800000`) was rejected at probe top by
  `ane_t6021_fwload_options_ok()` BEFORE any `dma_alloc_coherent` ran —
  *not* a DMA-size finding. Whatever hard-hung the box on that single
  boot is undetermined and unrelated to the RAM grant.
- The probe-top predicate lives at `ane_t6021_fwload_options_ok()`, is
  shared between `ane_t6021_drv.c` and `ane_rtclient_probe`, and is unit
  tested by `make -C ane/t6021 check` (host-side, pulls the same
  inlines from `ane_t6021_diag_marker.h`). The executable test is the
  contract: 11 boundary cases including the 16 KiB cap, 16 KiB
  alignment, 24 MiB pre-alloc rejection, reserved-alias coupling.

**Open:**

- The post-ACK park inside `CDebugAgent`'s ctor2 that earlier runs
  reported is CLOSED as misdiagnosed: the firmware was consuming a
  host-authored null command from a zeroed H2T ring and faulting (ELR
  `0x128c0`), not stalling on a scheduler. With
  `ane_t6021_chman_host_init()` writing ownership `1` into every H2T
  slot before the ACK, the post-ACK exception globals stay zero and
  **SCRATCH3 clears to `0x00000000`** — the 13.5 post-DONE sequence
  completes end to end on the `legacy_only` path
  ([receipts/2026-09-27-t6021-ring-owner-h2t-init.md](receipts/2026-09-27-t6021-ring-owner-h2t-init.md)).
  Still handshake-only: no CSNE command, no inference, no program load.
- The legacy ChMan host server (SHAREDMALLOC/TERMINAL) is **not** in
  this publication. Root review flagged acquire-ordering, unchecked ring
  offsets/size/bit before deref/modulo, and TERMINAL cursor not returning
  the slot to the producer. It will land under a separate commit once
  those defects are addressed.

**Out of scope for the publication:** live inference, the inferred RTKit
mode path, and any lowering of the 16 MiB cap or alignment constraint.
macOS 14+ firmware decompiles are not in this repository.

## Branch note: fix/tm-recovery is held at b52064c for T8103

2026-09-16, m1-test-host (T8103): two hard resets landed on this lane while
proving recovery tips beyond `b52064c`, both on branch-family modules and
both with no journal tail (volatile journal, external-abort signature):
`95dbcf3` died inside a -110 recovery, and a guarded build with the
set0/base gate disabled still died under the deterministic island-submit
tm -5 workload. The same workload only wedges gracefully on `main`
(`6fa243a`), and `b52064c`'s recovery completed twice on T8103. The
T8103-unsafe delta is therefore in the recovery's post-cycle engine
re-init that `b52064c` lacked: the 8-queue `TQ_NID1`/`TQ_STATUS` clear
(`95d3062`) and/or the `TQ_EN |= 0x3000` rewrite (`f3ad6e5`), and
possibly the direct set0/base gate (`3442d00`/`95dbcf3`, T6001-motivated).

This branch tip stays at `b52064c` until that sequence is bisected and
re-proven per SoC. The T6001-motivated commits (`dcc3e5b`..`327fd12`,
including the set0/base gate, the ACTUAL poll, the pre-raise ordering and
the T8103 ps-map guard) live on `fix/tm-recovery-t6001` for the t6001-test-host lane.
Ledger and evidence: ane-linux-experiments
`receipts/2026-09-16-tm-recovery-t8103.md`.

For the replacement installation, see the 2026-09-20 clean-install receipt (private archive).
Current provisioning and recertification status is at the top of this README.
