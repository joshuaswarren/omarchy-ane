# ane.ko: one DART TLB sync per buffer object

Each Parakeet encoder open makes `ane.ko` map 27,955 pages of 16 KiB in
BO_INIT. Before this change, every page cost one `iommu_map` call, and
every call made apple-dart invalidate the TLB of all three ANE DARTs, with
an MMIO busy poll each time: 83,865 invalidates for one open. Branch
`agent/ane-iommu-batch` maps the whole buffer object with one
`iommu_map_sgtable` call, which writes the same PTEs and invalidates each
DART once. That removes 83,862 of the 83,865 invalidates. This receipt
gives the source proof that the batch is valid on both target kernels, the
change, the build and host proofs, and the A/B protocol for the T6001 and
T8103 lanes. Lever 2 of `receipts/2026-10-03-ane-cold-start/README.md`.

Status: host-only work. No hardware ran. MEASURED means a number from a
run log, a build log, or a file read; INFERENCE means a number or claim
derived from source code or arithmetic.

## Is the batch valid? (source read, both kernels)

Trees: omarchy-linux `57f8f6deaa3a` (7.1.13, the kernel family of the
lanes and of the M2 3-1 headers) and `josh/ane-driver-aurora`
`f227145f50e4` (7.1.12). `drivers/iommu/iommu.c` lines 2582-2930 and all
of `drivers/iommu/io-pgtable-dart.c` are identical in the two trees
(MEASURED: byte compare).

| Question | Answer | Evidence |
|---|---|---|
| Does `map_pages` do TLB work? | No. `dart_map_pages` writes PTEs, then `wmb()`. It returns `-EEXIST` (and warns) when any target PTE is valid, and maps nothing in that call. | `io-pgtable-dart.c` 112-137, 226-291 |
| Where is the TLB sync? | `iommu_map` = `iommu_map_nosync` + `iommu_sync_map` (one `iotlb_sync_map` call per `iommu_map`). | `iommu.c` 2690-2746 |
| What does apple-dart do in `iotlb_sync_map`? | For each DART of the domain: runtime-PM get, one stream INVALIDATE command, busy poll (`readl_poll_timeout_atomic`, 1 µs step), runtime-PM put. | 7.1.13 `apple-dart.c` 469-497, 576-615; 7.1.12 `apple-dart.c` 889-951 |
| How many DARTs? | Three on T8103 and T600x. | `packaging/dt/t8103-ane.dts` 157, `packaging/dt/t600x-ane.dtsi` 167 |
| Can a module call `iommu_map_nosync` + one `iommu_sync_map`? | No. Neither is exported, in both trees and in the M2 3-1 `Module.symvers`. | `iommu.c` export lines 2746, 2852, 2879, 2930 |
| Exported batch API? | `iommu_map_sg` (`EXPORT_SYMBOL_GPL`; `iommu_map_sgtable` is its inline wrapper). `ane.ko` is `Dual MIT/GPL`, so it can use it. | `iommu.c` 2881-2930; `include/linux/iommu.h` 1576-1581 |
| What does `iommu_map_sg` do? | Calls `iommu_map_nosync` once for each physically contiguous run, then `iommu_sync_map` once over the whole mapped range. | `iommu.c` 2890-2922 |
| Scattered pages? | `drm_gem_get_pages` gives shmem pages in any order. Each run can be one page, so `map_pages` still runs up to 27,955 times, but the sync runs once. | same |
| Unwind on failure? | `iommu_map_nosync` unmaps the partial run; `iommu_map_sg` then unmaps every earlier run. Nothing the call mapped stays mapped. | `iommu.c` 2724-2726, 2924-2928 |
| Can a page be unmapped alone later? | Yes. The DART has one page size, so each PTE is its own 16 KiB leaf; `dart_unmap_pages` clears any count of leaves. The per-page unmap paths stay valid. | `io-pgtable-dart.c` 293-335 |

Verdict: the batch is valid on both kernels. The DART sees the same PTEs.
Only the number of TLB syncs changes.

## Change

`ane/src/ane_drv.c` at `fd3bee8`:

- `ane_iommu_map_batch` (193-226): `sg_alloc_table_from_pages` over
  `bo->pages`, one `iommu_map_sgtable`, `sg_free_table`. The sg table is
  transient: at most one 32-byte entry for each page (about 0.9 MB for the
  encoder), freed before the ioctl returns.
- Stray PTEs: after an `-EEXIST`, `iommu_map_sg` has already unmapped what
  it mapped, so every valid PTE left in the just-reserved range is a stray
  (a PTE that no `drm_mm` node owns). `ane_iommu_clear_range` (121-133)
  clears them, the driver logs one warning with the count, and it retries
  the map once. The per-page path clears one stray for each page; the
  batch path clears all of them in one scan. The same helper now does the
  probe-time purge of stale mappings (`ane_iommu_purge_stale`, 1040-1052).
- `ane_iommu_map_each` (144-185): the old per-page loop, moved without a
  change in behavior.
- `ane_iommu_map_pages` (236-281) reserves the IOVA node as before, reads
  `map_mode` and `map_batch` once, calls one of the two paths, and on
  failure removes the node and frees it. Before, `map_mode` was read again
  for each page.
- Module parameter `map_batch` (bool, 0644, default 1). The driver reads it
  once for each BO_INIT. `echo 0 > /sys/module/ane/parameters/map_batch`
  selects the per-page path for the next BO_INIT, with no reload.

Not changed: the unmap paths (`ane_iommu_unmap_pages` 283-309,
`ane_reclaim_preserved` 348-385, `ane_gem_free_object` 417-447), the
`drm_mm` IOVA allocation and its alignment, the wedge preserve logic, the
DART containment in `ane_dart.c`, runtime PM, `ane_stats`, the UAPI and
`ane_t6021`. `git diff 5a457a3 fd3bee8` touches only `ane/src/ane_drv.c`
and `CHANGELOG.md`.

## Expected saving

| Ingredient | Value | Class |
|---|---|---|
| Pages in the encoder command BO | at least 27,955 (staging buffer 458,014,720 B / 16 KiB) | MEASURED file size; "at least" is INFERENCE (cold-start receipt) |
| DARTs per ANE | 3 | MEASURED (DT source) |
| TLB syncs per encoder BO_INIT | 27,955 before, 1 after | MEASURED in the host model below, which runs the driver code against a copy of `iommu_map_sg` |
| DART invalidates per encoder BO_INIT | 83,865 before, 3 after | same |
| Time per invalidate (PM get/put, lock, 2 MMIO writes, at least 1 MMIO read, 1 µs poll step if busy) | unknown; no DART timing exists in the lab record | INFERENCE |
| Whole child `__ane_init` of the encoder (T8103) | 352.0 ms (one sample) | MEASURED (cold-start receipt) |

The saving per encoder open is 83,862 × t, where t is the time of one
invalidate with its PM get/put (INFERENCE):

| t per invalidate | saving per open |
|---:|---:|
| 0.3 µs | 25 ms |
| 1 µs | 84 ms |
| 2 µs | 168 ms |

The saving cannot exceed the BO_INIT share of the 352 ms. The other
per-page work (the page-table walk in `map_pages`, the trace hooks) does
not change. The per-page unmap at process exit (another 83,865 invalidates,
inside the harness `release_ms`) does not change either. The `trace` arms
below measure the real number as `libane:bo_init`.

## Safety

- IOVA ownership. The stray rule does not change. A `drm_mm` node that
  BO_INIT has just reserved owns every PTE in its range. Scratch drains
  reserve their node first, and wedge-preserved nodes stay inserted. The
  scan runs under `iommu_lock` and only inside the new node.
- Unwind. On any failure, `iommu_map_sg` leaves no PTE of its own; the
  driver then removes and frees the node. A second failure after a stray
  clear also leaves nothing mapped (host model, below).
- Runtime PM. BO_INIT runs inside `ane_drm_ioctl` (`ane_drv.c` 862-898 at
  `fd3bee8`), which holds a usage reference from line 888 until the ioctl
  returns. apple-dart also takes its own DART reference for each sync. The
  batch path makes fewer PM get/put pairs, not more.
- DART containment. BO_INIT holds `engine_lock`, so no job runs and no
  fault IRQ is masked during the map. `ane_dart.c` does not change.
- Page size. `ane_iommu_domain_init` still requires `PAGE_SIZE` in the
  domain's `pgsize_bitmap` and sets `shift = PAGE_SHIFT` (16 KiB on both
  target configs, MEASURED in `.config`). `iommu_map_sg` splits each run
  into pages of the one DART page size.
- Lock hold time. The map holds `iommu_lock` and `engine_lock` for a
  shorter time, because 27,954 fewer syncs run inside them.

## Build proofs

W=1, `ane.ko` only (no `ane_t6021` input changes). Base is origin/main
`5a457a3`, fix is `fd3bee8`, same tree and flags.

| Tree | Build | sha256 | vermagic | srcversion |
|---|---|---|---|---|
| Arch Linux ARM 7.1.13-3-1-ARCH headers, ALARM chroot, gcc 16.1.1 | base | `f91c72b4adda2185463ef98390559527801295de52a7f4f3243abc5e34c2da46` | 7.1.13-3-1-ARCH SMP preempt mod_unload aarch64 | `B94A022ECB6F0FF27D17BB8` |
| same | fix | `1dec6c9741576d7bdd1cb543f608abc3fa77210df8defa4701f56357037c7f3a` | same | `02D7B063016CE56355CE136` |
| linux-aurora 7.1.12 `f227145f50e4`, aarch64-linux-gnu-gcc 12.2.0 | base | `f8150b6df5adee62828f7b8e7c81dbe3eb7a048d672a4a92b8fe9b5ed7aafb94` | 7.1.12-ARCH+ SMP preempt mod_unload aarch64 | `B94A022ECB6F0FF27D17BB8` |
| same | fix | `a536c2b31f31958fc99ee97902afca1d4073f377707e3bc1a3e9091e77aa7f2e` | same | `02D7B063016CE56355CE136` |

- 7.1.13: no compiler warning in base or fix (only the pahole version
  notice). `modpost` ran against the full `Module.symvers` with no
  warning, so every new import resolves: `iommu_map_sg`
  (`EXPORT_SYMBOL_GPL`), `sg_alloc_table_from_pages_segment` and
  `sg_free_table` (`EXPORT_SYMBOL`). `modinfo -F parm` lists `map_batch`
  on the fix only.
- 7.1.12: the same single compiler warning in base and fix (`ane_tm.c`
  532, `-Wformat-truncation`, older than this change). That config has
  `CONFIG_MODVERSIONS` off and no `Module.symvers`, so `modpost` reports
  114 unresolved symbols for base and 117 for fix. `nm -u` shows that the
  difference is exactly the three new imports, and the tree exports all
  three (`iommu.c` 2930, `lib/scatterlist.c` 264 and 599).

## Host tests and host model

- `make -C tools check`: rc 0 on `fd3bee8`.
- `pytest -q tests tools` (after `make -C tools ane-run`): 58 passed,
  1 skipped, on `fd3bee8`.
- Host model (not committed; private lab record). It compiles the
  driver's map functions, extracted verbatim from `ane_drv.c`, with ASan
  and UBSan. The functions run against a PTE model with io-pgtable-dart
  semantics and a copy of `iommu_map_sg`. Results for 27,955 pages, with
  each value of `map_batch`:
  - Contiguous and scattered pages map to the right physical pages.
  - Syncs: 27,955 with the per-page path, 1 with the batch path.
  - Two stray PTEs are cleared, and the map succeeds.
  - An `-ENOMEM` at page 12,345 leaves 0 PTEs and no node.
  - A stray followed by a failing retry leaves 0 PTEs.
  - A valid PTE outside the BO range stays.
  - No sanitizer report.

## Hardware A/B protocol (T6001 jw16 lane, T8103 jwm1 lane)

Each lane runs this inside its own pre-registered window, with its usual
owner, guards and off-box console. `AGENTS.md` forbids loading a modified
module or submitting work outside a staged procedure. After any hang or
wedge, reboot; do not `rmmod` and reload. Plan one module load per window:
`map_batch` switches at run time, and on 2026-10-02 a second bind on the
T6001 failed with `-EACCES` until reboot (autosuspend receipt).

Every timed step takes the ANE lock (wait at most 120 s) and then waits
for the idle gate with the lock held. Record load1 and the PSI value each
time.

```sh
gate() {   # load1 < 0.5 and PSI cpu some avg10 = 0.00, wait at most 120 s
  for _ in $(seq 120); do
    l=$(cut -d' ' -f1 /proc/loadavg)
    p=$(awk '/^some/{sub("avg10=","",$2); print $2}' /proc/pressure/cpu)
    echo "gate load1=$l psi_avg10=$p"
    awk -v l="$l" -v p="$p" 'BEGIN{exit !(l < 0.5 && p == "0.00")}' && return 0
    sleep 1
  done
  return 1
}
locked() {   # run "$@" under /var/tmp/ane-run.lock after the idle gate
  exec 9>/var/tmp/ane-run.lock
  flock -w 120 9 || { echo "lock timeout"; return 1; }
  gate || { echo "idle gate not met"; flock -u 9; return 1; }
  "$@"; rc=$?
  flock -u 9
  return $rc
}
P=/sys/module/ane/parameters/map_batch
```

`tools/ane_cold_start.py` takes the same lock and checks the same gate
itself for each run. On jw16, also hold `flock /tmp/m1-gpu.lock` for the
`kprof` arm, because the GPU also maps through DARTs.

0. Build on the lane against the running kernel's headers, and load:

   ```sh
   git -C ~/src/omarchy-ane fetch origin \
     +refs/heads/agent/ane-iommu-batch:refs/remotes/origin/agent/ane-iommu-batch
   git -C ~/src/omarchy-ane worktree add /var/tmp/ane-iommu-batch origin/agent/ane-iommu-batch
   make -C /lib/modules/$(uname -r)/build M=/var/tmp/ane-iommu-batch/ane \
        ANE_VERSION=0.4.1.r59.gfd3bee8 modules
   modinfo -F parm /var/tmp/ane-iommu-batch/ane/ane.ko | grep map_batch
   sha256sum /var/tmp/ane-iommu-batch/ane/ane.ko
   sudo rmmod ane && sudo insmod /var/tmp/ane-iommu-batch/ane/ane.ko
   cat $P   # Y
   ```

   Record `uname -r`, the boot ID, `modinfo -F version ane`, the sha256 of
   the loaded `ane.ko` and the `dmesg` line count. The first submission
   after the load is the H13 add smoke (`omarchy-ane-smoke`); stop at the
   first failure. Build the harness libane from the same worktree (the
   cold-start receipt's step 2 recipe; this branch does not change
   libane):

   ```sh
   B=/var/tmp/ane-iommu-batch; mkdir -p /var/tmp/ane-cold
   gcc -O3 -fPIC -shared -std=gnu99 -DLIBANE_CONFIG_STRICT_BIND \
     -I $B/libane -I /usr/include/libdrm -I $B/ane/src/uapi/drm \
     $B/libane/ane.c $B/libane/ane_m2.c -o /var/tmp/ane-cold/libane-strict-batch.so
   sha256sum /var/tmp/ane-cold/libane-strict-batch.so
   ```

1. Cold open of the whole encoder, three arms in the order 1, 0, 1. Use
   the cold-start receipt's `S`, `W`, `H` and `OUT` values:

   ```sh
   LB=/var/tmp/ane-cold/libane-strict-batch.so
   echo 1 | sudo tee $P; python3 $H --out $OUT --label batch1  --worker $W --share $S --libane $LB --runs 10 --trace --no-islands
   echo 0 | sudo tee $P; python3 $H --out $OUT --label batch0  --worker $W --share $S --libane $LB --runs 10 --trace --no-islands
   echo 1 | sudo tee $P; python3 $H --out $OUT --label batch1b --worker $W --share $S --libane $LB --runs 10 --trace --no-islands
   ```

   Each arm's `env.txt` records `map_batch`. Report the medians and
   minimums of `open_ms`, `libane:bo_init` and `release_ms` from each
   `summary.tsv`.
   - Pass: the `libane:bo_init` median of `batch1` and of `batch1b` are
     both below the `batch0` median; `batch1` and `batch1b` agree within
     5 %; every run has rc 0. The `open_ms` difference is the result.
     `release_ms` (the per-page unmap) does not change by more than the
     drift between `batch1` and `batch1b`.

2. Optional, only if `/sys/kernel/tracing/function_profile_enabled` exists
   (the 7.1.13-3-1-ARCH config has `CONFIG_FUNCTION_TRACER` off): run the
   cold-start receipt's step 5 once with `map_batch=1` (label `kprof1`) and
   once with `map_batch=0` (label `kprof0`), filter
   `apple_dart_iotlb_sync_map iommu_map iommu_map_sg`. Expected hit counts
   for `apple_dart_iotlb_sync_map` during one encoder open: about 27,955 for
   `kprof0` and a small number for `kprof1`. Its average time in `kprof0`
   is the measured t of the saving table above. If the file is absent,
   record "kprof: not available" and skip.

3. Whole-encoder regression, `map_batch=1` against `map_batch=0`: 20 blocks
   x 16 calls per arm, each block through `locked`, with the lane's
   own encoder anchor. Pass: every output digest bit-exact against the
   anchor in both arms, and min-of-min within 1 % between the arms (the
   map path must not change the submit).

4. `ane_stats`: read `$(readlink -f /sys/class/accel/accel0/device)/ane_stats`
   before and after steps 3 and 5. The `jobs` delta equals the number of
   submits exactly, in both arms.

5. `omarchy-ane-smoke --timeout 900` 20/20 with `map_batch=1`, each run
   through `locked` (`locked omarchy-ane-smoke --timeout 900`). All 20
   hashes equal
   `5ad7eccd2977a88a375f123fe42c6780855953a1e9df4ac442cae383bc240dd6` (the
   T6001 and T8103 value in `receipts/2026-10-02-h13-smoke/README.md`),
   `errors=0`, exit 0.

6. Stress, 10 minutes, starting with `map_batch=1`. Each loop does one
   smoke run through `locked` and one harness open. Do not wrap the
   harness in `locked`, because it takes the lock itself. Every 60 s the
   loop writes the other value to `$P`, so BOs from both paths are freed
   in all orders:

   ```sh
   end=$((SECONDS + 600)); flip=$((SECONDS + 60)); n=0
   while [ $SECONDS -lt $end ]; do
     locked omarchy-ane-smoke --timeout 900 > stress-smoke-$n.json || break
     python3 $H --out $OUT --label stress-$n --worker $W --share $S \
       --libane $LB --runs 1 --no-islands || break
     if [ $SECONDS -ge $flip ]; then
       case $(cat $P) in Y|1) v=0 ;; *) v=1 ;; esac
       echo $v | sudo tee $P; flip=$((SECONDS + 60))
     fi
     n=$((n + 1))
   done; echo "stress iterations=$n elapsed_s=$((SECONDS - end + 600))"
   ```

   Pass: the loop runs the full 10 minutes, every smoke JSON has 20 golden
   hashes and `errors=0`, every harness run has rc 0, and `wedged` reads 0
   at the end.

7. `dmesg`: no new line from the step 0 count onwards that matches
   `iommu_map|stray DART PTE|unmap short|rollback unmap|dart_init_pte|WARNING:|SError|external abort|Internal error|tm completion failed|recovering|DART.*fault|busy bit did not clear|Unbalanced pm_runtime`.
   A "cleared ... stray DART PTE" line is a finding to report, not a pass.

8. Close-out: `echo 1 | sudo tee $P`, then restore the packaged module
   (`sudo rmmod ane && sudo modprobe ane`) and confirm the bind with
   `omarchy-ane-check`. Clean up with `git worktree remove` and
   `git worktree prune`.

Save the harness output tree (`tar czf ane-iommu-batch-out.tgz -C
/var/tmp/ane-cold out`), the smoke JSON, the `ane_stats` reads, `dmesg`,
and the command transcript, with SHA256SUMS. Merge only after both lanes
pass steps 1 and 3 to 7. After the merge, a follow-up removes
`ane_iommu_map_each` and the `map_batch` parameter, because the A/B is
their only use.

## Not verified here

- Nothing ran on an ANE. The saving, the drift, and the absence of new
  `dmesg` lines are predictions until a lane measures them.
- The time per DART invalidate is unknown, so the saving in milliseconds
  is a range, not a number.
- The host model checks the driver's control flow against models of
  io-pgtable-dart and `iommu_map_sg`. It does not test apple-dart, the
  DART hardware, or memory ordering.
- `iommu_map_sg` returns the sum of the segment lengths that it mapped.
  It skips only segments marked as PCI P2P bus addresses, which a shmem
  page array never has, so the driver does not compare the return value
  with the BO size.
- The per-page unmap at close keeps its 83,865 invalidates. One
  `iommu_unmap` per BO would batch it, but that is a separate change.
  `iommu_unmap` stops at the first hole. A range unmap therefore needs a
  per-page fallback after a short return. Without it, a lost PTE in the
  middle would leave later pages mapped while their memory goes back to
  the kernel.
- The in-tree `accel/ane` driver needs the same change in a later step.
