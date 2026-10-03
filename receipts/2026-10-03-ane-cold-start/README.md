# ANE cold start: where the first Parakeet encoder pass spends its time

Each new `mlx-omarchy-parakeet` process on the M1 Max (T6001) pays about
870 ms before its first encoder submit can start. The submit itself is not
slower the first time. The cost is the per-process program open: the worker
hashes and seals the 458 MB encoder program, then libane copies it into a
buffer object that the kernel allocates and maps page by page. A long-lived
worker would pay this once. This receipt gives the attribution, the macOS
reference, a ranked list of fixes, and the harness that measures the
remaining unknowns on the T6001.

Status: offline analysis and host-only code. No hardware ran in this work.
MEASURED means a number from a run log; INFERENCE means a number or claim
derived from source code or arithmetic.

## Evidence used

- 21 packaged-CLI reports from the t6001-test-host (2026-10-02, kernel
  7.1.13-3-2-ARCH, libane `d06222a8…`, encoder bundle program `13c74423…`,
  458,018,816 B, 13,701 task descriptors). Nine new processes and six
  reused sessions are comparable (the 32 s clip runs two windows).
- One `MLX_OMARCHY_OPEN_TIMING=1` worker log and three sealed-path CLI runs
  from the m1-test-host (T8103) with the same libane and bundle digests.
- One macOS 26.6.2 window on the same T6001 laptop (2026-09-28): one CoreML
  Parakeet process per run, 14 runs per compute unit.
- Source: omarchy-ane 160b209, omarchy-mlx 7bc4b5c73 (worker and CLI),
  Asahi kernel tree 57f8f6deaa3a (`apple-dart.c`, `iommu.c`).

## Result 1: the delta is the open, not the first submit (MEASURED)

| T6001, Linux | n | median ms | range ms |
|---|---:|---:|---:|
| session open (`ane.session.open_ms`), new process | 9 | 867.8 | 844.3 to 884.8 |
| first `exec_ms` in a new process | 9 | 440.22 | |
| `exec_ms` in a reused session | 6 | 439.96 | |

The first submit is 0.26 ms slower than a warm submit. That is 0.06 %. The
engine and its task manager have no measurable cold state.

The m1-test-host runs the same encoder submit in 141.6 ms (3.1 times faster),
but its sealed-path open is 860.3 ms (1001.9 ms encoder stage minus 141.6 ms
submit, n=3). The open is the same on both chips within 1 %. INFERENCE: the
open is CPU and memory work, not engine work.

## Result 2: attribution of the open

Stage times come from the m1-test-host worker log (one sample, same libane
and bundle). The T6001 split is not measured yet; the harness below measures
it. Source lines refer to the commits above.

| stage | ms | class | evidence |
|---|---:|---|---|
| parent: seal the 458 MB program (64 KiB reads, SHA-256, memfd copy, `F_ADD_SEALS`) | 434.8 | (e) userspace | MEASURED; `bundle.cpp:445-512` |
| parent: seal 3 island bundles (0.57 MB) | 17.2 | (e) | MEASURED |
| parent: seal libane | 0.1 | (e) | MEASURED |
| parent: fork to "loaded" handshake, the sum of the three child rows | 368.0 | | MEASURED |
| child: `dlopen` of libane | 0.2 | (e) | MEASURED |
| child: `__ane_init` of 4 island programs | 15.6 | (a)+(b)+(c) | MEASURED |
| child: `__ane_init` of the whole encoder | 352.0 | (a)+(b)+(c) | MEASURED; split below is INFERENCE |
| parent rows together | 820.1 | | MEASURED |
| rest: worker exec and dynamic link, Python `Popen`, batch round trip | about 48 | (e) | INFERENCE: T6001 open 867.8 minus 820.1 |
| first submit minus warm submit | 0.26 | (d) | MEASURED |

What the 352 ms `__ane_init` does for this program (INFERENCE from source).
The staging buffer holds 458,014,720 B, exactly 27,955 pages of 16 KiB. The
command buffer object holds at least that much, because libane copies the
whole program into it:

- (a) libane program load: read the 4 KiB header, read 458,014,720 B into a
  staging buffer (`ane_model_init`), parse the 10.2 MB task stream
  (`ane_bind_init`, `ane_bind_overrun`), copy 458,014,720 B into the command
  buffer object (`set_btsp_and_command`). `ane_bind_kernel` costs nothing
  here: the worker never calls it.
- (b) DART map: `BO_INIT` maps 27,955 pages with one `iommu_map()` each
  (`ane_drv.c:128-138`). Each `iommu_map()` calls `iotlb_sync_map`
  (`iommu.c:2731-2740`). The apple-dart driver answers with one TLB
  invalidate, with an MMIO busy poll, on each DART of the domain
  (`apple-dart.c:576-615`). The T6001 ANE has three DARTs, so one encoder
  open does 83,865 invalidates for pages that had no mapping before.
- (c) first touch: 27,955 anonymous page faults on the staging buffer;
  27,955 shmem pages that `drm_gem_get_pages` allocates and zeroes
  (`ane_drv.c:399`); 27,955 PTEs that `vm_map_pages` installs at `mmap`
  (`ane_drv.c:841`). Cache maintenance: zero. The default `map_mode=3` maps
  both sides cacheable, and `ane/src` has no cache flush call.
- (d) engine and power state: zero on origin/main. Probe takes a runtime-PM
  reference for the life of the device (`ane_drv.c:1164-1177`). The
  get/put in `ane_drm_open` (`ane_drv.c:704-717`) only moves a counter. With
  runtime-PM autosuspend (open work in another branch), an open after an
  idle gap pays one resume. The harness `gap` arm measures it.
- (e) userspace outside libane: the seal is the largest single stage. The
  CLI's own `startup_to_main` (286 ms, MEASURED by the requester) comes
  before the encoder stage and is not part of the 870 ms.

## Result 3: macOS on the same laptop (MEASURED)

| macOS 26.6.2, T6001, CoreML, compute ane | n | models ready s | first encoder call ms |
|---|---:|---:|---:|
| first process after boot (empty compiler cache) | 1 | 38.71 | 159 |
| later processes (cache hit) | 13 | median 0.31 (0.16 to 0.32) | median 146 (142 to 150) |

Each macOS run is a new process. macOS pays about 0.31 s per process to
load all Parakeet models (front end, encoder, decoder, joint), and its first
encoder call runs at the warm speed. Linux pays 0.87 s for the encoder open
alone. The macOS in-memory path (`_ANEInMemoryModel`) loads the compiled
whole encoder in 0.09 s on an M1 Ultra (macOS 26.6.2) and in 0.007 s on an
M2 Max (macOS 27.0). INFERENCE: macOS does not hash the 458 MB of weights in
each process; its compiler cache is keyed and kept by the system service.

## Ranking

| rank | lever | gain per CLI process | cost | risk |
|---:|---|---|---|---|
| 1 | persistent worker daemon (`docs/ane-worker.md`) | about 868 ms after the first request (MEASURED open) | medium: socket mode in the worker, client fallback, user units | medium: changes the seal contract to "authenticate once per daemon"; owner decision |
| 2 | one DART TLB sync per buffer object instead of one per page (`iommu_map_sg` or a batched sync in `ane_iommu_map_pages`) | the invalidate share of 352 ms; unmeasured, harness `kprof` measures it | small: one driver function | medium: DMA map path, stray-PTE repair logic, needs hardware |
| 3 | hash while loading: one pass that reads, hashes and copies into the buffer object, through a new libane load-from-fd entry point | up to the copy part of the 435 ms seal (INFERENCE: 100 to 200 ms) | medium: additive libane API and worker change | low to medium |
| 4 | no libane staging copy: read the file into the buffer object and validate there | one 458 MB copy, 27,955 faults and 458 MB of resident memory (INFERENCE: 50 to 120 ms) | small | medium: `nn->data` is a public field; its meaning changes |
| 5 | `calloc` for the staging buffer (done in this branch) | one memset pass over 458 MB (INFERENCE: below 20 ms); harness `main` vs `trace` arms measure it | none | none: same zeroed buffer, same `free()` |
| - | kernel program cache that keeps buffer objects after close | rejected | | unsafe: breaks the per-file teardown boundary (`ane_drm_postclose`) and the wedge accounting |

The daemon is not implemented. The T6001 split is not measured yet, the
daemon changes the seal contract, and the worker lives in omarchy-mlx.

## Change in this branch

- `libane/ane.c`: `ANE_TRACE_TIMING` (unset by default). When set to a value
  other than empty or `0`, libane prints one stderr line per load stage and
  per `ane_exec`: `LIBANE: TIMING stage=NAME ms=MS bytes=N`. Stages:
  `device_open`, `model_read`, `bind_check`, `bo_init`, `bo_mmap`, `copy`,
  `init_total`, `m2_open` (ABI 2), `exec`. The ABI and `ane.h` do not change.
  Unset, each stage costs one branch.
- `libane/ane.c`: the staging buffer comes from `calloc` instead of
  `posix_memalign` plus `memset`. glibc returns the fresh pages of a large
  request already zero, so the memset pass goes away.
- `tools/ane_cold_start.py`: the harness. `tools/test_ane_cold_start.py`
  checks its parsers and its idle gate.

## How to run the harness on the T6001

The harness starts the production worker once per run with the CLI's bundle
set and seal pins, waits for `resident loaded`, sends `quit`, and records the
open. It does no submit, sends no tensor and does no GPU work. Each run waits
for the idle rule (load1 below 0.5 and PSI cpu `some avg10` = 0.00, at most
120 s), then takes `/var/tmp/ane-run.lock` (at most 120 s). It records both
values. An unmet gate or a failed open stops the battery; nothing retries.

1. Set the paths. `S` is the CLI share directory that holds
   `parakeet-runtime-pin.json`, `bundles/` and `libane/`. `W` is the
   `mlx-omarchy-ane-worker` that the CLI runs.

   ```sh
   S=<venv>/lib/python3.14/site-packages/mlx/share/mlx-omarchy/parakeet-1
   W=<venv>/lib/python3.14/site-packages/mlx/bin/mlx-omarchy-ane-worker
   OUT=/var/tmp/ane-cold/out
   mkdir -p /var/tmp/ane-cold
   ```

2. Build two libane copies from omarchy-ane: `trace` from this branch and
   `main` from origin/main. Use git worktrees; do not switch branches in a
   shared checkout.

   ```sh
   git -C ~/src/omarchy-ane fetch origin \
     +refs/heads/main:refs/remotes/origin/main \
     +refs/heads/agent/ane-cold-start:refs/remotes/origin/agent/ane-cold-start
   git -C ~/src/omarchy-ane worktree add /var/tmp/ane-cold/src origin/agent/ane-cold-start
   git -C ~/src/omarchy-ane worktree add /var/tmp/ane-cold/main origin/main
   for v in src:trace main:main; do
     gcc -O3 -fPIC -shared -std=gnu99 -DLIBANE_CONFIG_STRICT_BIND \
       -I /var/tmp/ane-cold/${v%%:*}/libane -I /usr/include/libdrm \
       -I /var/tmp/ane-cold/${v%%:*}/ane/src/uapi/drm \
       /var/tmp/ane-cold/${v%%:*}/libane/ane.c /var/tmp/ane-cold/${v%%:*}/libane/ane_m2.c \
       -o /var/tmp/ane-cold/libane-strict-${v##*:}.so
   done
   sha256sum /var/tmp/ane-cold/*.so
   H=/var/tmp/ane-cold/src/tools/ane_cold_start.py
   ```

3. Run the arms, in this order, with nothing else on the ANE.

   ```sh
   python3 $H --out $OUT --label prod   --worker $W --share $S --libane $S/libane/libane-strict.so --runs 10
   python3 $H --out $OUT --label main   --worker $W --share $S --libane /var/tmp/ane-cold/libane-strict-main.so --runs 10
   python3 $H --out $OUT --label trace  --worker $W --share $S --libane /var/tmp/ane-cold/libane-strict-trace.so --runs 10
   python3 $H --out $OUT --label whole  --worker $W --share $S --libane /var/tmp/ane-cold/libane-strict-trace.so --runs 5 --no-islands
   strace -V && python3 $H --out $OUT --label strace --worker $W --share $S --libane $S/libane/libane-strict.so --runs 3 --strace
   ```

   - `prod`: the CLI open as shipped (sealed libane). Compare its `open_ms`
     with the CLI's 867.8 ms.
   - `main` and `trace`: libane from origin/main and from this branch. Both
     are unsealed. The `ane_init:N` difference is the `calloc` gain. `trace`
     also prints the libane stages.
   - `whole`: the encoder bundle alone, without the island bundles.
   - `strace`: `BO_INIT` ioctl time and `mmap` time per run. Compare its
     `open_ms` with `prod` to see the tracer cost.

4. Optional, only if `/sys/module/ane/parameters/autosuspend_ms` exists and
   is above 0: the resume arm. Each run sleeps longer than the autosuspend
   delay first.

   ```sh
   G=$(( $(cat /sys/module/ane/parameters/autosuspend_ms) / 1000 + 2 ))
   python3 $H --out $OUT --label gap --worker $W --share $S --libane /var/tmp/ane-cold/libane-strict-trace.so --runs 5 --gap-s $G
   ```

5. Optional, root, inside a GPU window with the inference service stopped
   (the GPU also maps through DARTs): the kernel function profile of one
   open. It needs `CONFIG_FUNCTION_PROFILER`.

   ```sh
   T=/sys/kernel/tracing
   for f in drm_gem_get_pages iommu_map apple_dart_iotlb_sync_map vm_map_pages; do
     echo $f | sudo tee -a $T/set_ftrace_filter || echo "no $f"
   done
   echo 1 | sudo tee $T/function_profile_enabled
   python3 $H --out $OUT --label kprof --worker $W --share $S --libane /var/tmp/ane-cold/libane-strict-trace.so --runs 1
   echo 0 | sudo tee $T/function_profile_enabled
   sudo cat $T/trace_stat/function* > $OUT/kprof/trace_stat.txt
   echo | sudo tee $T/set_ftrace_filter
   ```

## What to send back

- `tar czf ane-cold-out.tgz -C /var/tmp/ane-cold out`. Each arm directory
  has `env.txt` (kernel, module parameters, page size, THP, digests),
  `results.jsonl`, `summary.tsv`, one directory per run, and `SHA256SUMS`.
- The `sha256sum` output of the two libane builds and
  `git -C /var/tmp/ane-cold/src rev-parse HEAD`.
- Cleanup: `git -C ~/src/omarchy-ane worktree remove /var/tmp/ane-cold/src`,
  the same for `main`, then `git worktree prune`.

## Limits

- The stage split is from one m1-test-host sample, not from the T6001.
- The (a), (b) and (c) shares inside the 352 ms are INFERENCE until the
  `trace`, `strace` and `kprof` arms run.
- The `calloc` gain is not measured.
- The macOS model-load figure covers all Parakeet models, not the encoder
  alone.
