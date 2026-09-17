# Omarchy Neural Engine

Apple Neural Engine support for Omarchy Linux: a DRM accelerator driver and userspace library. Bind and execute are proven on M1 (`T8103`, `apple,t8103-ane`) and M1 Max (`T6001`, `apple,t6000-ane`). Every other SoC still needs its own PMGR, DART, SET, and TM offsets. eiln's original reverse engineering targeted one M1; this fork is where the other chips get wired.

- `ane/`: DRM accelerator kernel module.
- `libane/`: userspace loader and submission library.
- `bindings/python/`: Python shared-library bindings.

This fork's `main` serializes submissions, holds GEM references across execution, waits for request-tagged last-task finish events, and retires the matching task-queue slot. An uncertain completion blocks new work and normal reclamation, pins the module against ordinary unload, and requires a reboot. Runtime power must remain on. Forced platform/DT removal is unsupported: driver-core teardown can release managed resources despite the module pin.

The library requires driver ABI 1: successful submission guarantees terminal completion and CPU visibility. Older drivers are rejected; output values are never used as completion signals. Build the module against matching kernel headers with `make -C ane`, then build the library and Python binding from this checkout with `make -C libane && make -C bindings/python/dylib`.

Nothing here compiles a model. Programs come from the H13 backend in [joshuaswarren/mil-hwx-compiler](https://github.com/joshuaswarren/mil-hwx-compiler), whose runner validates each package and its reference outputs before it opens this library.

The ABI-1 stack passed all eight compiler qualification packages and a finite-input overflow case producing `+inf`. Every output matched on three warmups and 30 measured iterations per package. With the optimized compiler, 512-element add-ReLU uses two programs and measures 0.679 ms per-op, or one program and 0.160 ms fused. The 768-to-1024-to-768 MLP uses 77 programs and measures 32.170 ms, versus 92 programs and 41.523 ms before whole-tensor binary selection on the same driver boot. Timing spans input transfer through output readback, excluding setup and reference evaluation. Cold power-on repeatability and general chain fusion remain unqualified.

Compiler evidence: [M1 native progress](https://github.com/joshuaswarren/mil-hwx-compiler/blob/main/receipts/2026-09-06-m1-native-progress.json). Raw Apple firmware and private host details are not distributed.

## Chip coverage

Linux `compatible` is the driver match. Internal names follow Apple's SoC table (H13G, H14J, …); unknown means exactly that. Each SoC carries one of three driver states, mirroring the qualification tiers on the descriptors in `ane/src/ane_drv.c`:

- **qualified** — execution proven on this silicon; binds normally.
- **recognized-untested** — constants known, never run on the part; the driver binds only with `ane.allow_unqualified=1` and logs a loud warning naming what is unverified.
- **unsupported** — no proven constants; the driver refuses to bind (a guessed SET-block base external-aborts the SoC, so none is carried) and its refusal message names the data needed.

Tier is decided per `compatible`, so T6000 silicon reads recognized-untested below while its `apple,t6000-ane` descriptor is qualified on T6001 evidence.

| Marketing | SoC | Internal | Linux ANE `compatible` | Driver status | Test confirmation | Data needed |
| --- | --- | --- | --- | --- | --- | --- |
| M1 | T8103 | H13G | `apple,t8103-ane` | qualified | bind + exact fp16 64-el smoke; o-proj + attention islands E2E certified 2026-09-17 | none |
| M1 Pro | T6000 | H13J | `apple,t6000-ane` | recognized-untested | none on T6000 silicon | a tester plus the board DART/pmgr overlay (compatible and SET base shared with T6001 are proven); two community DT captures arrived 2026-09-17 |
| M1 Max | T6001 | H13J | `apple,t6000-ane` | qualified | bind + exact fp16 64-el smoke; o-proj + attention islands E2E certified 2026-09-17, 104/104 | none |
| M1 Ultra | T6002 | H13J | `apple,t6000-ane` | recognized-untested | none | a tester plus a board overlay; dual-die SET base unverified — confirm before any bind |
| M2 | T8112 | H14G | unknown | unsupported | — | ANE node DT capture (quick collector works with no ANE node), SET-block base; H14 compiler backend is unqualified |
| M2 Pro | T6020 | H14J | `apple,t6020-ane` | unsupported | — | SET-block base, a qualified H14 compiler backend, and the board DART/pmgr overlay; three community DT captures and one native-macOS IORegistry capture arrived 2026-09-17 |
| M2 Max | T6021 | H14J | unknown | unsupported | — | same as T6020, plus confirmation of the compatible |
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

## Branch note: fix/tm-recovery is held at b52064c for T8103

2026-09-16, jwm1 (T8103): two hard resets landed on this lane while
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
the T8103 ps-map guard) live on `fix/tm-recovery-t6001` for the jw16 lane.
Ledger and evidence: ane-linux-experiments
`receipts/2026-09-16-tm-recovery-t8103.md`.
