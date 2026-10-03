# libane no-staging load: payload straight into the command buffer object

Status: MERGED as #111 (shipped in 0.4.3) after both A/B lanes passed. On the
T8103 lane the whole-encoder cold open went from 178.3 to 107.9 ms
`libane:init_total` (182.4 to 101.1 ms on the repeat arm), `open_ms` 67-72 ms
less; on the T6001 lane from 361.4 to 286.5 ms, `open_ms` 828.5 to 760.1 ms.
Encoder output bit-exact across arms, smoke 20/20 on both chips. MEASURED
means a number from a run log or command output here; INFERENCE means derived
from source, arithmetic, or the directional CT microbenchmark.

## Design

Branch `agent/libane-no-staging` (base `bfd962f`, origin/main). The M1 (ABI 1)
program load no longer keeps a staging buffer:

- Before (staged, now the fallback): `pread` the payload at `0x1000` into a
  16 KiB-aligned `malloc` buffer (`nn->data`), parse the task stream there,
  then `memcpy` it twice -- once into the `chans[0]` buffer object
  (`anec->size` bytes) and once into the bootstrap channel (`td_size` bytes).
  For the 458,018,816 B whole-encoder program that is two 458,014,720 B passes
  and 458 MB of resident staging memory held for the life of `ane_nn`.
- After (direct, default): the payload is loaded straight into the `chans[0]`
  mapping, the task stream is parsed in place, and only the `td_size` copy to
  the bootstrap channel remains (`set_nid` patch unchanged). `nn->data` stays
  `NULL`; no allocation is freed at close.
- Payload loader (`ane_load_payload`): `mmap` the file `PROT_READ|MAP_PRIVATE`
  and `memcpy` the payload into the buffer object -- one userspace pass.
  Host measurement picked this over a `pread` into the mapping (below). When
  the file cannot be mapped, it falls back to the previous `fread`-based
  `ane_pread`. A short file copies only the bytes that exist and the tail of
  `anec->size` is zeroed -- byte-identical to the staged path, whose fresh
  buffer-object pages are zero anyway.
- Parse placement: `ane_bind_init`/`ane_bind_overrun` read exactly the bytes
  the staged path parsed (payload + zero tail), now in the `chans[0]` mapping.
  No in-place patching of the task stream exists; the only program mutation is
  the bootstrap-channel `set_nid`, unchanged. `BO_INIT`/`BO_MMAP` now happen
  before the bind checks, so a program the binder refuses allocates and frees
  its buffer objects first (host-side cost on the error path only).
- ABI and headers unchanged: `libane/ane.h`, `libane/ane_m2.h`,
  `ane/src/uapi/drm/ane_accel.h` untouched; ioctl order unchanged;
  `__ane_init` signature unchanged. The ABI 2 (T6021) `ane_m2_open` path is
  untouched.
- Fallback and trace: `ANE_LOAD_STAGED=1` (unset/empty/`0` = direct) restores
  the staged path verbatim for A/B. Direct `ANE_TRACE_TIMING` stages are
  `model_header`, `model_map`, `model_map_copy` (or `model_pread_fallback`),
  `model_zero_tail`, `bind_check`, `copy`, and `init_total`; BO stages remain
  unchanged. Staged path keeps `model_read`, `bind_check`, and `copy`.
  `tools/ane_cold_start.py` parses stages generically.

## Mapping-mode finding (ane/src/ane_drv.c)

`map_mode` defaults to 3: cacheable CPU vma (`ane_drm_mmap`, `vm_map_pages`
over shmem GEM pages) plus `IOMMU_CACHE` DART descriptors. So a direct load
into the default mapping is an ordinary cached-memory write. Only the
documented rollback (`map_mode=0`, or the retained prior module) maps the CPU
vma write-combine, where a 458 MB streaming write can be slower than a memcpy
from cached memory -- the hardware A/B must record `map_mode` (the harness
already writes it to `env.txt`).

## Host microbenchmark (MEASURED, directional; CT is not an ANE host)

omp-studio-local (x86_64, kernel 6.17.2-1-pve, 4K pages, THP always), 458 MB
file (sha256 in artifacts), `anec->size` = 458,014,720 B payload, shmem-backed
`MAP_SHARED` mapping as the buffer-object stand-in, 5 reps, warm page cache
(the production open follows the seal, which has just read the whole file):

| shape | median ms | MB/s |
|---|---:|---:|
| staged: read into staging + memcpy to mapping | 102.1 | 4484 |
| direct: read into mapping | 72.0 | 6361 |
| direct (chosen): mmap file + memcpy into mapping | 30.1 | 15230 |

`staged - direct(mmap) = 72.0 ms` on this host. LIMITATIONS (INFERENCE for
hardware): host pages are 4K (the anonymous `MAP_SHARED` BO stand-in was
explicitly 16 KiB virtual-address aligned), THP is enabled, mapping is cached,
and copy paths are x86-specific. This CT does not provide 16 KiB physical pages
or the Apple BO cache attributes. `preadv` into a mapping returned `EOPNOTSUPP`
on this PVE kernel, so the read arm used `readinto` (the same single-copy
shape as libane's `fread` for large blocks). A first, not-explicitly-aligned
run measured 207.9/131.0/52.7 ms (staged/readinto/mmap+memcpy); both runs are
retained separately in `apple-silicon-lab/artifacts/LibaneNoCopy/no-staging/transcript.txt`.
The exact hardware result is in the Status line above: both lanes ran the
A/B below and passed.

Expected saving (INFERENCE until the hardware A/B): one less 458 MB copy and
the corresponding page touches. A bandwidth-only estimate for a cached M1 BO
mapping (map_mode=3) is order 10-25 ms; the host measured 72.0 ms for the
staged-minus-mmap+memcpy shape on a slower x86/ZFS CT, which cannot be mapped
directly to the Apple hardware. The cold-start receipt's prior 50-120 ms
estimate predates the cached default. The exact number comes from
`libane:init_total` staged vs direct per lane.

## Host gates (MEASURED)

- `make -C libane` -- clean (`-Wall -Werror -Wextra -Wdeclaration-after-statement`).
- `make -C tools check` -- PASS, including the new cases:
  - staged/direct byte-identical `chans[]` + bootstrap channel on the full H14
    fixture, the 64 B short file, and the 1 MiB truncated payload.
  - short-file zero-tail checks in direct and staged modes; staged `nn->data`
    remains 16 KiB aligned.
  - ABI 1 load/submit and mmap-fail cases in both modes (staged fallback).
- `ANE_TRACE_TIMING=1 ./test_libane_ioctl` -- PASS; direct traces show
  `model_header`, `model_map`, `model_map_copy`, and `model_zero_tail`; the
  staged path keeps `model_read`.
- `gcc -DLIBANE_CONFIG_STRICT_BIND` build of the harness -- PASS.
- `pytest -q tests tools` -- 100 passed, 1 skipped (final run after trace-stage change).

## A/B protocol (T8103 lane and T6001 lane)

Lane owners ran one lane each. One libane build serves both arms; the env var
selects the path, and `tools/ane_cold_start.py` inherits the environment, so
no harness change is needed. Build exactly as the cold-start receipt
(receipts/2026-10-03-ane-cold-start) prescribes, from this branch:

```sh
git -C ~/src/omarchy-ane fetch origin \
  +refs/heads/main:refs/remotes/origin/main \
  +refs/heads/agent/libane-no-staging:refs/remotes/origin/agent/libane-no-staging
git -C ~/src/omarchy-ane worktree add /var/tmp/ane-nostag/src origin/agent/libane-no-staging
gcc -O3 -fPIC -shared -std=gnu99 -DLIBANE_CONFIG_STRICT_BIND \
  -I /var/tmp/ane-nostag/src/libane -I /usr/include/libdrm \
  -I /var/tmp/ane-nostag/src/ane/src/uapi/drm \
  /var/tmp/ane-nostag/src/libane/ane.c /var/tmp/ane-nostag/src/libane/ane_m2.c \
  -o /var/tmp/ane-nostag/libane-strict-branch.so
sha256sum /var/tmp/ane-nostag/libane-strict-branch.so
H=/var/tmp/ane-nostag/src/tools/ane_cold_start.py
```

Arms (S, W, OUT as in the cold-start receipt; >= 9 valid cold runs each;
`--runs 10` gives headroom). Alternate the arm order on the two hosts so any
thermal/boot-age drift (H186 side finding) does not line up with one arm:

```sh
# T8103 lane: staged first; T6001 lane: direct first
ANE_LOAD_STAGED=1 python3 $H --out $OUT --label staged \
  --worker $W --share $S --libane /var/tmp/ane-nostag/libane-strict-branch.so \
  --runs 10 --trace
python3 $H --out $OUT --label direct \
  --worker $W --share $S --libane /var/tmp/ane-nostag/libane-strict-branch.so \
  --runs 10 --trace
```

Acceptance per host, all required:

1. `libane:init_total` (largest program = whole encoder): median and min of
   `staged` vs `direct`; the difference is the measured saving. Also compare
   the `model_read` and `copy` stage lines.
2. `open_ms` median improves by at least the `init_total` delta (the rest of
   the open is the seal, which this change does not touch).
3. Bit-exact outputs: the encoder hidden-state sha256 from the direct arm
   equals the staged arm's and the host's known-good digest
   (`554a3d66f6885a3552d531d509bbd30d632d5bd424296028e8c1523f9f6f4ec4` on the
   t8103 acceptance corpus; use the host's own pinned digest otherwise).
4. Jobs exact: the `ane_timeline`/stats job counts match the run count on both
   arms (no lost or duplicated submits).
5. Smoke 20/20: `omarchy-ane-check --smoke` prints
   `add-fixture on <soc>: 20/20 calls bit-exact` with the direct path active
   (env unset), once per host, after the arms.
6. Record `cat /sys/module/ane/parameters/map_mode` per arm (it lands in
   `env.txt`). If a host must run `map_mode=0` (write-combine rollback), mark
   that arm separately: the direct load's destination write is WC there and
   the comparison is not the default-configuration result.

Send back the `out` tree (`results.jsonl`, `summary.tsv`, `env.txt`,
`SHA256SUMS`), the libane sha256, and `git -C /var/tmp/ane-nostag/src
rev-parse HEAD`. Cleanup: `git -C ~/src/omarchy-ane worktree remove
/var/tmp/ane-nostag/src && git -C ~/src/omarchy-ane worktree prune`.

Merge rule (standing): do not merge before both lanes report; merge only with
a direct-arm saving >= 0 ms, bit-exact outputs, exact jobs, and smoke 20/20 on
both hosts.

## Risks

- `nn->data` is a public field of `struct ane_nn`; it is now `NULL` on the M1
  path by default. In-repo consumers are covered by the harness. Out-of-repo
  consumers (mlx-omarchy worker) build against these headers; the sealed-path
  worker does not dereference it in-repo (INFERENCE: not audited here), and
  the direct arm of the hardware A/B runs the production worker end to end --
  a stale dereference fails that arm loudly.
- `map_mode=0` rollback (write-combine CPU vma): the direct load writes 458 MB
  through a WC mapping and could be slower than staged there. That mode is a
  documented rollback, not the default; the protocol records it either way.
- `mmap` of the program file: a file truncated concurrently with the load can
  SIGBUS where the staged path took a graceful short read. Production program
  files are sealed (F_ADD_SEALS) and immutable once placed; no other consumer
  is known to truncate a live ANEC. `ane_pread` fallback covers unmappable
  files, not racing truncation.
- Parse now runs after `BO_INIT`/`BO_MMAP`: a program the binder refuses (or a
  failing BO mmap) allocates and frees buffer objects before the error. Error
  paths are covered by the harness mmap-fail cases; cost is host-side only.
- ABI 2 (T6021) loads are untouched; the M2 program path keeps its own loader.
