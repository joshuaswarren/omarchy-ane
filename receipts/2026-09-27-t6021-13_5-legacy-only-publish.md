# T6021 / 13.5 (22G74) legacy ChMan — verified evidence behind this publication

Date: 2026-09-27
Source candidate: `/tmp/m2-startup-recovery/ane/t6021/` (maintained off-tree,
not in this repo). Snapshot SHA-256: `5174471a…` for
`ane_t6021_rtclient_main.c`; `1578ff67…` for the matching header candidate.
Published here with the legacy-server block excised; nothing here is rebuilt
on a private path.

## Version scope (firmware contract)

- The T6021 ANE firmware preloaded by the Asahi stub on the testbed
  is the macOS 13.5 (22G74) selene image, sha-256
  `a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`.
- `ane_t6021_fwload.c` (this commit) refuses every other image and tags
  the boot path with the SHA-pinned contract. The published `legacy_only`
  parameter is structurally bound to this exact image — do not run it on
  14.x / 15.x / 26 / 27 firmware.
- All evidence below is from the 13.5 (22G74) firmware only. The README
  M2 RTKit-only narrative is stale *with respect to this exact image*;
  the modern macOS narrative is unchanged.

## Proven on this image (verified, repeatable)

| Run | Boot | Finding | Cited output |
| --- | --- | --- | --- |
| `legacy-discriminator-20260927T152309` | 64e745ca | After host ACK (legacy P8 SCRATCH3 ← `0x08042006`), ctor1 globals went nonzero at +227s post-ACK: `CSharedMemory::instance 0x4f86d8 = 0x2000681660`, encode pair `0x4f86e8/f0 = {0x21bf0, 0x21c14}` (predicted EncodeLegacy/Encode5bPacking exactly), `CTaskPool::instance 0x4f8710 = 0x2000696148`. Pre-ACK dump at 217–218s showed all three at 0. | `artifacts/M2Runtime/legacy-discriminator-20260927T152309/dmesg-ane-complete.log` (2185 lines). |
| `legacy-envfields-20260927T154327` | 57e17349 | Same 2 MiB backing, ENV window rows: post-ACK `env+278 = 0x2000681660` (CSharedMemory-returned, ctor1 finished) but `env+290 = 0` (CDebugAgent ctor never returned — its store at 6d64 is before any CIPSynchro path); `env+298 = 0x1b3d4440` (validator, positive control). | `artifacts/M2Runtime/legacy-envfields-20260927T154327/envfields-dmesg-ane.log` (340 lines). |

- These runs are reproducible from the invocations on a fresh boot with
  `fw_load=1 fw_alias_reserved=1 fw_extra_ram=0x200000 legacy_only=1`
  (see `legacy-envfields-invocation.sh` for the exact module-param set).
  The global address values are firmware-build offsets, not host memory;
  they name *which* ctor returned without leaking a usable pointer.

## Parked inside CDebugAgent ctor2 (open decode)

- ENV-fields explicit dump window captured once during the
  `legacy-envfields-20260927T154327` boot, post-ACK (the dump fires
  on a single tagged ioread): `env+290 = 0` (CDebugAgent ctor never
  returned — its store at 6d64 is before any CIPSynchro code path).
  `env+278 = 0x2000681660` (CSharedMemory returned, paired with the
  +227 s discriminator observation). `env+298 = 0x1b3d4440`
  (validator positive control, unchanged). SCRATCH3 read at the same
  tagged window: `0x08042006`.
- The GPIO3 clear at 6d84…6d98 — the only path that confirms post-ACK
  fw progress in 13.5 — was also not observed at the dump window.
- A separate SCRATCH3 re-poll observed SCRATCH3 = `0x08042006` at
  +10 min after probe (raw second read; not an ENV-window read, so
  it does not extend the env+290 negative observation). Both readings
  are consistent with the fw parked inside `CDebugAgent::C2`'s body
  (0x13604 in the 13.5 payload) at every reading taken.
- Leading suspect (M2Protocol slice6): the ctor2 spawn chain
  (TCB / thread-stack via `ffwAlignedAlloc`, semaphores via try-only
  pool, `RTK_thread_create` fail-fast) draws from the FFW heap
  (host-granted backing).
- Next-boot discriminator (proposed, NOT run): the thread table
  `0x4fa490` / count `0x4fa498` + a TCB walk from `fwbuf_audit`. That
  is a separate, smaller change behind the same experimental gate.

## Backing-envelope contract (the only verified one)

- `fw_extra_ram = 0x200000` (2 MiB, 16 KiB-aligned at
  `ANE_T6021_FW_ALIAS_PAGE = 0x4000`) + DMA_BIT_MASK(32) — verified
  under the runs above.
- This commit MOVES the bound/alignment/rejected-extra-when-not-reserved
  check to the SHARED probe-top predicate `ane_t6021_fwload_options_ok()`,
  called from both `ane_t6021_probe` (drv.c) and `ane_rtclient_probe`
  (rtclient) BEFORE `devm_kzalloc`/power. The same rule still runs at
  the alloc site in `ane_t6021_fwload.c` (line ~404) as defense in depth.
- Historical 0x1800000 (24 MiB) attempt: the prior code rejected
  `fw_extra_ram > SZ_16M` (or unaligned) at the late fwload alloc site
  — after `devm_kzalloc` ran and AFTER the power raise had completed,
  not at probe top. The actual `insmod` log for that boot was lost
  (M2Runtime recovery cleanups superseded the buffer). What the code
  PREDICTED (then and now) is the same -EINVAL; what was OBSERVED was
  a single box hard-hang whose cause is undetermined and unrelated to
  the RAM grant — DMA size is not implicated in this publication's
  evidence, and the historical log cannot prove which side of the
  alloc actually fired. Do NOT read it as "the 24 MiB grant hangs the
  box" and do NOT lower the 16 MiB guard or loosen the 16 KiB
  alignment without independent verification on a new boot.

## Bounded source change set (this commit)

- Carried over from the candidate source set, with the legacy-server
  block removed (acquire-ordering bug, unchecked ring offsets/size/bit
  before deref/modulo, TERMINAL cursor never returns slot to producer —
  root review flagged all three). The `legacy_only` parameter is the
  verified, bounded surface; the host-server remains absent.
- `legacy_only` default is OFF (0444). Opt-in with the param set above;
  no qualify / no enable-by-default flag flips.

## What is NOT in this commit

- The candidate `ane_t6021_boot.c` and `ane_t6021_fwload.c` are the
  full experimental surface that powers `fw_start=1`. They are
  reproduced verbatim from the candidate (no private paths, no
  unredacted blobs). `ane_t6021_diag_marker.h`,
  `ane_t6021_dart_observer.h`, and `ane_t6021_fw_validate.h` are the
  matched diagnostic / observer / validator headers. The README's
  `docs/t6021-ane-bringup-findings.md` remains the canonical narrative.
- macOS raw firmware, private host details, and boot IDs are
  intentionally NOT in this repo. The 13.5 selene image is referenced
  only by its SHA-256; download it from a public source (Apple IPSW
  for macOS 13.5 22G74) before any local build.
- The legacy ChMan host server (SHAREDMALLOC + TERMINAL), per root
  review, is excluded from this publication. It will land under a
  separate commit once the three flagged defects are addressed.

## Provenance

- Boot IDs and box identities are intentionally omitted; the runs were
  performed on the single T6021 testbed that the program uses.
- Receipt paths above are within the canonical M2 bring-up artifact
  store; the runs themselves are reproducible from the documented
  invocation scripts.
