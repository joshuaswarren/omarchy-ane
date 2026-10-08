# Parakeet on the M2 — encoder islands on T6021 ANE (plan, 2026-09-30)

Audience: Joshua + the lead agent. Scope: read-only research, no code edits
outside this file. Facts are marked **[V]** verified in this session or
**[I]** inferred (logical from observed evidence, not directly read). M2-host
state I cannot probe from this CT is **[I]** and the executing agent must
verify via the documented ladders before each step (jwm1 USB/ACM proxy for
reboot; `ssh jw14m2-linux` for liveness; ffmpeg `/dev/video1` on jwm1 for
the M2 screen).

Notebook pre-registration:
`~/.local/share/apple-silicon-lab/entries/ParakeetM2Plan/20260930T021100Z-ct-parakeet-on-m2-plan.md`.

Sources consulted in this session, with path:line where useful:
- mlx-omarchy: docs/parakeet.md, docs/ane-encoder-placement.md,
  docs/ane-bundles.md, docs/ane-runtime.md, AGENTS.md,
  overlay/mlx/backend/omarchy/ane/{runtime_worker.cpp,
  worker_libane.cpp, ane_runtime data...}
- omarchy-ane-m2-installed-wt: libane/{ane.h, ane.c, ane_m2.c, ane_m2.h,
  ane_bind.h}, receipts/2026-09-29-t6021-installed-path/README.md, README.md
- mlx-omarchy/overlay/tools/mlx-omarchy-parakeet/share/mlx-omarchy/parakeet-1/parakeet-runtime-pin.json
- Notebook entries H14Mint, H14Encoders, H14Shapes, BuilderGeneral,
  M2Runtime (all under `~/.local/share/apple-silicon-lab/entries/...`).

## (a) Hosts, repos, builds for mlx-omarchy today; M2 checkout

**[V]** The pinned command surface is `mlx-omarchy-parakeet transcribe`,
backed by the wheel `mlx_omarchy-0.32.3.dev202609190758+50eeb29-cp314-cp314-linux_aarch64.whl`
sitting in `/home/joshuawarren/src/mlx-omarchy/dist/aarch64/`
(docs/parakeet.md:186; `ls dist/aarch64/`, this CT).

**[V]** The wheel is built only on `aarch64|arm64`; on those arches
`scripts/build-wheel.sh:196` adds `-DMLX_OMARCHY_ANE_DEVICE=ON`, which is
the only switch that compiles `mlx-omarchy-ane-worker` and the libane
seam. On any other architecture the worker does not build and the wheel
is GPU-only (scripts/build-wheel.sh:188-198, 207).

**[V]** `install.sh:80` refuses a non-`aarch64` host:
`[[ "$(uname -m)" == aarch64 ]] || die "mlx-omarchy runs on Apple Silicon (aarch64); ..."`.
Wheel filename is `cp314-cp314-linux_aarch64.whl` (install.sh:117-118).
Python host is 3.14.

**[V]** An M2-class wheel install is therefore legal on the M2 (aarch64).
A x86_64 Linux box cannot run the pinned transcription at all. The CT
(this host) is x86_64, Linux PVE, and has no aarch64 userland visible —
the **plan does not run on this host**. The plan is a ticket for the M2
through `jw14m2-linux` (M2 Tailscale 100.98.81.36 per `~/.omp/agent/rules/ane-fleet-facts.md`).

**[V]** Vulkan path is verified on the M2 third-silicon (Mesa Honeykrisp
1.4.354, `docs/compatibility.md` lines on t6021-test-host and the cited
ane-linux-experiments receipt). GPU frontend (vulkan_mel.py, joint) is
therefore qualified on the M2.

**[I]** No mlx-omarchy receipt in this repo records an M2 ANE run.
Searches over `receipts/2026-09-*.md` for `t6021|apple,t6021` returned
zero in-scope results; the only M2 receipts found were GPU/Vulkan
(card-promotion, speech-output-kokoro). The ANE path on the M2 is
documented in the omarchy-ane fork (README.md "T6021 (M2 Max, 2026-09-29)
... ops qualified, models not") and in the installed-path receipt, not in
mlx-omarchy.

**M2 evidence for the ANE wheel install is unknown** from this CT. The
executing agent must:

1. `ssh jw14m2-linux uname -m` → expect `aarch64`. **[unverified]**
2. `ssh jw14m2-linux 'python3 --version; which mlx-omarchy-parakeet 2>/dev/null;
   ls ~/.cache/mlx-omarchy/parakeet-reference/ 2>/dev/null'`.
   **[unverified]**
3. `ssh jw14m2-linux 'bash /home/joshuawarren/src/mlx-omarchy/install.sh --ane 2>&1
   | tail -40'` and capture the full transcript. **[unverified]**
4. `ssh jw14m2-linux 'MLX_OMARCHY_ANE_DEVICE=off mlx-omarchy-parakeet
   transcribe --help'` (the kill-switch, see parakeet.md:210).
   **[unverified]**

The `Vulkan device` files under `/var/tmp/VP_VULKANINFO_Apple_M2_Max*.json`
on the M2 are not readable from this CT and are not requested. The plan
calls for re-running `vulkaninfo` and dumping the JSON on the M2 in the
GPU-baseline phase (section e, step E2).

## (b) Bundle manifest schema-4, gates, pins, parity contract

### Schema 4 contract

**[V]** Manifest schema 4 is a strict superset of physical output storage
plus ordered logical views. The summary fields (docs/ane-bundles.md:36-67):

| Field | Contract |
|---|---|
| `manifest_version` | exact `4` |
| `driver_abi_major` | exact `1` |
| `compiler.target` | exact `h13` |
| `task_descriptors` | sum of all program task-descriptor counts |
| `inputs`, `outputs` | non-empty physical tensor lists |
| `logical_results` | non-empty ordered return views; every physical output referenced |
| `programs` | ordered, each ANEC payload referenced exactly once |
| `dispatch_plan` | permutation of program indices |
| `payloads` | one or more `anec` and at most one `weights`, unique filename + size + sha256 |
| `release_asset.model_sha256` | canonical collection SHA-256 over `json.dumps(records, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")` |

**[V]** The loader runs six steps without device access (ane-bundles.md:116-122):
parse → reject unknowns → list files → check sizes → check digests →
parse ANEC header. All digest checks precede ANEC parsing.

### What rejects an H14 bundle today

**[V]** `compiler.target` is the named rejector. An H14 ANEC (T6021 / M2)
is not an H13 target; schema 4 explicitly demands `h13`. Hence an
H14-only ANEC bundle will fail step 2 (unsupported schema field /
wrong target) and never reach device load.

**[V]** `driver_abi_major` is the second rejector. The M2 backend is
ABI-2 (`ANE_ABI_M2_MAJOR`, ane.c:296, 476). Any ABI-2 bundle will fail
the `exact unsigned driver_abi_major: 1` check (ane-bundles.md:60).

**[V]** A positional channel map is refused when `LIBANE_CONFIG_STRICT_BIND`
is set (ane.c:426-440): "task stream does not name every surface; channel
map is positional". The mlx-omarchy build enforces that (ane-runtime.md:5,
parakeet.md:186). H14 islands emitted from the new H14 compiler
(`tools/h14_sections.py`) currently encode their channel maps
**positionally** for the M2 ABI-2 backend (ane_m2.c uses dense TD address
records + a derived role→channel map rather than the explicit surface
table the strict loader requires — see (c)).

### Runtime pin and the gate

**[V]** `parakeet-runtime-pin.json` (read from
`mlx-omarchy/overlay/tools/mlx-omarchy-parakeet/share/mlx-omarchy/parakeet-1/parakeet-runtime-pin.json`,
schema `mlx-omarchy.parakeet-runtime-pin.v1`) pins:
- `assets.bundles` for `island-attn-a-kt`, `island-pv`, `island-select-8head`,
  and `parakeet-encoder-whole` (each with `program-N.anec` SHA-256).
- `assets.libane.libane-strict.so` = `d06222a8…7da8c`.
- `e2e.{transcript, encoder_hidden, mel, token_ids, durations, frame_indices}`
  + `cpu_tensor_events: 0`, `decode_control: "gpu-chain"`, `emissions: 104`.

**[V]** `transcribe` refuses and exits 1 on any pin mismatch (parakeet.md:201-219):
"any shipped asset hash does not match the pin (unverified ANE programs
never execute)". It also refuses on missing `/dev/accel/accel0`,
`MLX_OMARCHY_ANE_DEVICE=off`, the `ane` module not loaded, encoder
source MIL hash mismatch, or `cpu_tensor_events != 0`.

**[V]** The "lines 15 gate" of `docs/ane-runtime.md` is verbatim:
```
The worker accepts only Linux AArch64 with `apple,t8103-ane`, device-tree
status `okay`, driver version `f2a3e5e+lifecycle6`, a bound `ane` platform
driver, runtime PM `on/active`, and readable/writable `/dev/accel/accel0`.
```
The corresponding code is in `runtime_worker.cpp:108-156`:

- `verify_hardware_eligibility()` reads `/proc/device-tree/soc/ane@…/compatible`
  and **throws** unless it contains `apple,t8103-ane` (line 109-110).
- `module_version` must equal the constexpr `kQualifiedDriverVersion =
  "f2a3e5e+lifecycle6"` (line 32, 118-122).
- `libane_commit` is the constexpr `kQualifiedLibaneCommit =
  "6fa243ac7241119a9eb229abbf8cb4dd8949f915"` (line 30, 153).
- Driver ABI in the identity string is hard-coded `driver_abi=1`
  (line 154).

**[V]** The driver ABI is encoded as `driver_abi=1` in the worker
identity. The M2 backend is ABI 2. The T6021 module name is `ane_t6021`,
not `ane`; `/sys/module/ane/version` is therefore absent or wrong, and
`runtime_pm` for `apple,t6021-ane` may not be bound to the `ane` driver
at all. The M2 driver exposes `apple,t6021-ane` via the
`of:*apple,t6021-ane` alias (omarchy-ane README; installed-path receipt).

### Numerical contract

**[V]** From `docs/parakeet.md:91-105`:
```
encoder_hidden (vs golden ANE capture):
  max |Δ|            ≤ 0.30      (licensed clip: CPU 0.145203, GPU 0.139700)
  mean |Δ|           ≤ 0.02      (licensed clip worst: 0.004393)
  relative L2        ≤ 0.10      (licensed clip worst: 0.025172)
  NaN / Inf          = 0
decoder/joint: emitted token IDs must match exactly
  (plan section 40, layer 6; duration/frame metadata remains diagnostic)
transcript: must match exactly
```
Host preprocessing is **bit-identical** across CPU/GPU/ANE; only the
encoder body and downstream decode compare on the encoder-hidden
contract.

**[V]** The golden captures live at
`~/.cache/mlx-omarchy/parakeet-reference/captures/b650695c-75aec2a/`
with `manifest.sha256` per capture; primary golden is
`20260912T154759Z-librispeech/ane` (parakeet.md:147-156).

**[V]** Library capture harness is `swift build -c release` of
`overlay/tools/coreml/capture` (Apple Silicon Mac, not Linux); the
`fetch_parakeet_reference.py download` / `verify` / `path` / `info`
CLI wraps it on Linux (parakeet.md:138-145).

## (c) Worker → libane call surface vs ABI-2 backend; gaps

### What the worker dlsyms

**[V]** `overlay/mlx/backend/omarchy/ane/worker_libane.cpp:36-63` resolves
exactly nine symbols via `dlsym`:

| Symbol | Purpose |
|---|---|
| `__ane_init` | open program, default tile shift |
| `__ane_init_shift` | open program, explicit tile-shift |
| `__ane_free` | close program |
| `ane_exec` | submit one execution |
| `__ane_src_size` / `__ane_dst_size` | per-channel byte sizes |
| `__ane_send` / `__ane_read` | DMA in / DMA out |

The `LibaneApi::ok()` predicate is the same nine symbols; nothing else is
queried (worker_libane.cpp:42-44).

### What ABI-2 libane exports

**[V]** `libane/ane.c` and `libane/ane_m2.c` together export the
following symbols (definitions, not declarations):

| Symbol | Source | ABI-2 behavior |
|---|---|---|
| `__ane_init` | ane.c:505 | calls `__ane_init_shift(path, dev_id, TILE_SHIFT_DEFAULT=0xE)` |
| `__ane_init_shift` | ane.c:450 | dispatches to `ane_m2_open` if `abi_major == ANE_ABI_M2_MAJOR` |
| `__ane_free` | ane.c:510 | routes to `ane_m2_close` if `nn->m2` |
| `ane_exec` | ane.c:553 | routes to `ane_m2_exec` if `nn->m2` |
| `ane_exec_loop` | ane.c:561 | **unconditional M1 path; bails on size mismatch; never sees `nn->m2`** |
| `ane_kernel_capacity` | ane.c:583 | reads `nn->chans[0].size`; **`nn->m2` has no chans[0]** |
| `ane_bind_kernel` | ane.c:590 | `memcpy` into `nn->chans[0].map`; **no-op on ABI-2** |
| `__ane_src_size` / `__ane_dst_size` | ane.c:617, 626 | route to `ane_m2_src_size` / `ane_m2_dst_size` |
| `__ane_send` / `__ane_read` | ane.c:635, 646 | route to `ane_m2_send` / `ane_m2_read` |
| `__ane_tile_send` / `__ane_tile_read` | ane.c:743, 758 | **unconditional M1 path; bail with `-EINVAL` when `tile_fits` fails; no M2 branch** |
| `ane_tile` / `ane_untile` | ane.c:658, 690 | pure pack/unpack; ABI-agnostic |

ABI-2 backend definitions (`libane/ane_m2.c`, confirmed at
ane_m2.h:91-105 and ane_m2.c:319-859):

| Symbol | Definition | Notes |
|---|---|---|
| `ane_m2_program_build` | ane_m2.c:319 | header parser; H14 task-stream walk; BAR-ref derivation; tdprop walk |
| `ane_m2_sections_free` | ane_m2.c:538 | section free |
| `ane_m2_open` | ane_m2.c:656 | DRM IOCTL: PROG_LOAD + PROC_CREATE |
| `ane_m2_close` | ane_m2.c:756 | releases the process, IOVAs, sections |
| `ane_m2_exec` | ane_m2.c:774 | DRM IOCTL: EXEC |
| `ane_m2_send` / `ane_m2_read` | ane_m2.c:803-823 | direct `memcpy` to/from IOVA mapping |
| `ane_m2_src_size` / `ane_m2_dst_size` | ane_m2.c:830-851 | looks up `model.io[idx]` |

### Per-function gap list

**[V]** Worker ↔ ABI-2 mismatches, by entry point. The worker's calls land
on these libane functions (worker_libane.cpp:43-62); the ABI-2 backend
either covers them or not:

1. **`__ane_init` / `__ane_init_shift`** — covered. ABI-2 dispatch at
   ane.c:476 enters `ane_m2_open`, which parses the H14 ANEC, builds
   the six firmware sections (ids `{1,2,3,4,5,7}`,
   `ane_m2_section_ids`), and issues `PROG_LOAD` + `PROC_CREATE`. Pass.

2. **`__ane_free`** — covered. Routes to `ane_m2_close`; releases the
   process and sections. Pass.

3. **`ane_exec`** — covered. Routes to `ane_m2_exec`; single ioctl of
   type `DRM_IOCTL_ANE_EXEC`. Pass.

4. **`__ane_src_size` / `__ane_dst_size`** — covered. Routes to
   `ane_m2_src_size` / `ane_m2_dst_size`, indexed by ordinal position
   among the program's source or destination buffers
   (`nth_io(ctx, 0/1, idx)`). Pass.

5. **`__ane_send` / `__ane_read`** — covered semantically but with a
   shape hazard. `ane_m2_send` does `memcpy(ctx->io_bo[..].map, from,
   io->size)` — it copies the **exact** IOVA size, not the worker's
   logical binding size. The worker packs rows into a `binding.allocation_bytes`
   tile (worker_libane.cpp:108-124) and then `ane_send`s the whole
   tile. On ABI-1 the kernel trims to `tile_size(nn, bdx)` which equals
   `tiles[bdx] << tile_shift`. On ABI-2 the kernel maps the IOVA at the
   full `io->size` (the firmware-declared IO surface); copying more than
   `io->size` would overrun the BO. **Gap (medium):** the worker may
   over-copy if `binding.allocation_bytes > io->size`. This needs a
   one-line clamp at the libane layer or in the worker's send path,
   plus a regression test that asserts `allocation_bytes == io->size`
   on ABI-2.

6. **`ane_exec_loop`** — not called by the worker today, but the
   header exposes it and the island bundles do not need it (each
   island is single-shot). Pass for now; mark as future.

7. **`ane_kernel_capacity` / `ane_bind_kernel`** — not called by the
   worker; the worker reads the kernel section bytes itself from the
   ANEC at offset `kAnecPayloadOffset + aligned_task`
   (runtime_worker.cpp:160-170). Pass.

8. **`__ane_tile_send` / `__ane_tile_read`** — not called by the worker
   (the worker uses `__ane_send`/`__ane_read` with explicit packing via
   `ane_pack_rows` / `ane_unpack_rows`). Pass.

9. **Channel map strictness** — worker_libane.cpp:73-83 always passes
   `program.anec.c_str()` to `__ane_init_shift`; libane's `ane_bind_init`
   (ane.c:426) refuses positional maps when `LIBANE_CONFIG_STRICT_BIND`
   is set. The H14 ANECs from `tools/h14_sections.py` and
   `tools/h14_seq_program.py` (per BuilderGeneral notebook 09-29T21:30Z)
   encode their channel maps **positionally**. **[V]** The strict build
   refuses them today. **Gap (high):** either (a) loosen the strict
   build for ABI-2 (rebuild libane without `LIBANE_CONFIG_STRICT_BIND`,
   like the current pin's `provenance.libane` note does: "built without
   LIBANE_CONFIG_STRICT_BIND; island bundles bind some surfaces
   positionally, so strict refusal must stay off"), or (b) extend the
   H14 compiler to emit per-surface channel maps the strict loader
   accepts. The pin itself is shipped under the unstrict build.

10. **libane commit pin** — runtime_worker.cpp:30 hard-codes
    `kQualifiedLibaneCommit = "6fa243a…f915"`. The M2-installed-path
    libane is on a different commit (the installed-path receipt
    references commit `742a728` for Stage 1-4 and the README is at
    `agent/m2-installed-path` head `85fffaf`). **Gap (high):** the
    worker identity string records the wrong commit and the runtime
    gate enforces equality. Either the worker accepts an env-supplied
    `kQualifiedLibaneCommit` override for ABI-2, or the ABI-2 libane is
    cherry-picked or rebased onto the pinned commit. **[I]** A pure
    ABI-2 libane backport onto `6fa243a` is feasible because the ABI-2
    backend lives in `libane/ane_m2.{c,h}` and the M1 path in
    `libane/ane.c` is untouched.

11. **Driver ABI constant** — runtime_worker.cpp:154 records
    `driver_abi=1` in the identity. The M2 ABI is 2. The parity contract
    in ane-bundles.md:60 ("exact unsigned `driver_abi_major: 1`")
    likewise hard-codes ABI 1. **Gap (high):** schema 4 would need
    `driver_abi_major: 2` for ABI-2 bundles, plus a corresponding
    `compiler.target: h14`. Without these, the loader refuses the M2
    bundle before any device call.

12. **Compatible string** — runtime_worker.cpp:109 demands
    `apple,t8103-ane`. The M2 DT node is `apple,t6021-ane`. **Gap
    (high):** the gate string is a constexpr and is the first thing
    checked; the worker exits before opening the device. The fix is a
    compile-time switch (e.g. an env-overridable string set by
    `MLX_OMARCHY_ANE_COMPATIBLE` when ABI-2) and an associated driver-
    version constant. The T6001 lane already runs with
    `apple,t6000-ane` per omarchy-ane README ("T6001 Linux ANE is
    live"), so the loader already accepts multiple compatibles in the
    built artifact — **[I]** verify this in T6001 build artifacts
    before assuming a single switch is enough.

13. **Kernel bind / kernel section read** — ABI-2 reads the kernel from
    the ANEC at runtime_worker.cpp:160-170. **[I]** H14 ANEC layout has
    `ANEC_M2_HEADER_SIZE = 0x1000`, then the task stream, then the
    constant region at a 64-byte-aligned offset near the end (ane_m2.c:7-15).
    The runtime reads the kernel from offset `kAnecPayloadOffset +
    aligned_task`. **`kAnecPayloadOffset` is `0x1000`** (bundle.h:28).
    That matches the H14 ANEC header size. **[I]** But the H14 ANEC
    layout puts the kernel near the end of the file, not immediately
    after the task stream. The runtime's `kernel_size` is read from the
    ANEC header (ane_m2.c:319 build step), so the offset arithmetic is
    correct **iff** the runtime uses the same `task_size` / `kernel_size`
    semantics as the ANEC header builder. **Gap (medium):** requires
    a header-side unit test that round-trips an H14 ANEC and asserts
    `runtime_worker.read_kernel(...)` equals the bytes the builder put
    there. Open question raised by the goal note: the H14 ANECs have
    `firstTaskBytes % 16` in `{4, 8, 12}`, not the conventional 16-byte
    alignment. The H14 task walker (`split_h14_tasks`, ane_m2.c) and the
    strict loader both rely on 16-byte frame alignment for filler
    detection. **Gap (medium):** verify the H14 ANEC header records
    the same 16-byte alignment as the walker expects, or relax the
    walker to `FRAME_BYTES` as a soft min.

### Summary of gaps, in priority order

1. **Gate constants are M1-pinned** (compat string, driver version,
   libane commit, driver_abi): four constexprs in
   runtime_worker.cpp; bundle schema `driver_abi_major` and
   `compiler.target` in ane-bundles.md.
2. **Strict channel map** in libane + positional H14 ANECs: either
   relax to non-strict for ABI-2 (matching the current shipped pin) or
   teach the H14 builder to emit per-surface maps.
3. **Send clamp** at libane M2 path so the worker can't overrun the
   IOVA BO when `binding.allocation_bytes > io->size`.
4. **H14 ANEC 16-byte frame** assumption: verify or relax, plus the
   firstTaskBytes alignment question from the goal note.

The first three are blockers for `mlx-omarchy-parakeet transcribe` on
the M2 with islands on the ANE. The fourth is a follow-up that affects
correctness, not whether the program loads.

## (d) Per-island end-to-end chain on the M2

The compiler side is done for A/B/C in ANEC form
(H14Mint/H14Encoders notebook 09-30T00:35Z: 13 island select / batched-
matmul ANEC artifacts, 755-parity). The H14 builder is being extended
by another agent (BuilderGeneral) to generalize from the proven add
ANEC to mul/relu/clip/scalar/matvec/real-div (BuilderGeneral notebook
09-29T23:00Z: 9 ANEC packages staged).

Per island (rank-3 batched; rank-4 heads at runtime), the chain to a
device result on the M2 is:

```
MIL graph  →  mil-hwxc --target H14 --format anec  →  H14 ANEC
            (compiler worktree; closed at 755 parity)
H14 ANEC   →  tools/h14_sections.py build        →  H14 ANEC sections
            (BuilderGeneral worktree; staged for the 9 proven ANECs)
H14 ANEC   →  ane-run OR mlx-omarchy-ane-worker
              →  __ane_init_shift (tile_shift=0xE, H13-island units)
              →  ane_m2_open  (PROG_LOAD + PROC_CREATE)
              →  __ane_send / __ane_read per channel
              →  ane_exec → ane_m2_exec (DRM_IOCTL_ANE_EXEC)
              →  __ane_read returns
              →  bytes → CPU → mlx tensor
```

Per island (proven vs gap):

| Island | Compiler | Builder | Driver | Worker | Worker gate | Parity |
|---|---|---|---|---|---|---|
| A — `island-attn-a-kt` [1,8,375,128]x[1,8,128,749], 2 tasks | ANEC, parity 755 | H14 builder covers `matvec` family | ABI-2 path runs matvec (M2 receipt, 256/256 within 2 ulp on [1,256]x[256,256]) | same 9 dlsyms | blocked by gaps 1-4 | not on hardware |
| B — `island-select-8head` [1,8,375,375], bool cond, 5 tasks (+ const-fill) | ANEC, parity 755 | builder does not yet cover `select const-fill` (H14Encoders ongoing) | n/a | n/a | n/a | not on hardware |
| C — `island-pv` [1,8,375,375]x[1,8,375,128], 5 tasks | ANEC, parity 755 | builder covers `mul` (matmul-like NCHW) | ABI-2 runs mul + matvec | same | blocked | not on hardware |
| rms_norm chain C=128 / 2048, with/without gamma | ANEC form (H14Encoders ongoing) | H14Encoders still landing encoder/templates | n/a | n/a | n/a | not on hardware |

Dependencies by island:

- A: matvec extension is in scope of the H14 builder already (Stage 1-4
  receipt proves 2-task matvec up to 2048x5120, max 0.218 cond-units).
  The driver ABI-2 path already executes matvec. **Blocker:** worker
  gate (gap 1-4), not the island.
- B: select const-fill encoding needs H14Encoders' ongoing work. The
  ANEC stream decodes but the aligned blob header and 376-halfword
  stride are still under landing (H14Mint 09-29T23:59Z).
- C: same builder coverage as A; same blockers.
- rms_norm: H14Encoders thread holds this; landing requires parity +
  Apple mint with full constant-section bytes (already running).

## (e) Smallest path to a first Parakeet E2E transcript on the M2

Ordering: GPU baseline first (proves the Vulkan + decoder + pin path
without the ANE), then island B (smallest surface), then A and C in
either order. Each step has explicit preconditions, the repo that owns
the change, and the observable acceptance.

### E1. Pre-flight on the M2 (notebook-mandated)

Owner: M2 owner agent (M2Runtime). Repo: omarchy-ane.
Pre-flight per `~/.omp/agent/rules/ane-fleet-facts.md` and
`ane-fleet-verify-before-human.md` — no human asks.

1. `ssh jw14m2-linux uname -m; ssh jw14m2-linux uname -r` — record
   `aarch64` and the kernel (`7.1.13-ARCH-polltx` per installed-path
   receipt; any newer kernel is fine but record it).
2. `ssh jw14m2-linux 'lsmod | grep ane_t6021; cat
   /proc/device-tree/soc/ane@*/compatible | tr "\0" "\n"; cat
   /sys/module/ane_t6021/srcversion 2>/dev/null; ls -l
   /dev/accel/accel0'`.
3. `ssh jw14m2-linux 'sudo ane/t6021/gate/gate.sh OUTDIR mul
   2>&1 | tail -20'` — reproduces Stage 1-4 mul on a fresh boot. PASS
   required before anything else; on FAIL recover per the installed-path
   "Not proven" note (intermittent all-zero output on some boots).

Acceptance: 512/512 lanes exact (sparse, fp16, half-away); dmesg shows
the autoloaded module; `/dev/accel/accel0` is char device rw for the
caller. New notebook entry under `M2Runtime/<date>-preflight-parakeet.md`.

### E2. GPU baseline on the M2 (no ANE)

Owner: mlx-omarchy (read-only here; install + run by M2 owner). Goal:
prove the Parakeet transcript against the pinned fixture on the M2 GPU
path, with `MLX_OMARCHY_ANE_DEVICE=off` so the worker refuses and the
encoder stays on Vulkan. This isolates mlx-omarchy's GPU-side from the
ANE-side.

1. `ssh jw14m2-linux 'bash /home/joshuawarren/src/mlx-omarchy/install.sh
   --ane 2>&1 | tee /tmp/install.log'` — install ANE-enabled wheel
   (the install only requires `aarch64`; the wheel still ships even if
   we won't run the worker yet). **[I]** The install also writes the
   render-group lock and `/run/lock/mlx-omarchy-ane`. Confirm the M2
   user is in `render`; if not, `sudo usermod -aG render $USER`.
2. `ssh jw14m2-linux 'mlx-omarchy-parakeet download'` — fetch and verify
   the pinned reference (parakeet.md:138-145, lockfile at
   `overlay/tools/coreml/parakeet-reference.lock`).
3. `ssh jw14m2-linux 'MLX_OMARCHY_ANE_DEVICE=off mlx-omarchy-parakeet
   transcribe -o /tmp/parakeet-gpu 2>&1 | tee /tmp/transcribe-gpu.log'`.

Acceptance: transcript SHA-256 = `db501a8c080380ea027ffa50a4b4956c39df77cb692c4fb78e556311a11a0790`
(parakeet-runtime-pin.json `e2e.transcript_sha256`); 104/104 emissions;
`encoder_hidden` SHA-256 = `38c73261f29230276ed76f1fc017b76b024156d79218bd5f1347fdc7e7d43ec7`;
`cpu_tensor_events = 0`; `decode_control = gpu-chain`. Compare against
M1 Max (`receipts/2026-09-27-m1max-current-main-gold.md`) which uses
the same pinned fixture.

If FAIL: a M2 GPU-side transcription failure is an mlx-omarchy issue,
not ANE. Stop and route the failure back to mlx-omarchy without
touching the ANE stack. Notebook entry required either way.

### E3. Worker ABI-2 plumbing (gate constants + strict + send clamp)

Owner: mlx-omarchy (runtime_worker.cpp, runtime.cpp,
worker_libane.cpp, bundle.cpp / bundle.h) AND omarchy-ane (libane
ane.c, ane_m2.c, ane_bind.h). Both repos commit from a worktree on
`agent/m2-parakeet`, push to origin, and merge only after their
respective gates pass.

Changes (from section c gaps 1, 2, 3):

1. **runtime_worker.cpp** — change the four constexprs to be ABI-aware.
   Suggested form: read `MLX_OMARCHY_ANE_ABI` (or similar) at process
   start; the default stays 1 (the M1 / T8103 / T6001 lane is the only
   audited one). Add `apple,t6021-ane` and `apple,t6000-ane` to the
   `has_compatible` lookup table; do not loosen the per-ABI driver
   version or libane commit (each ABI gets its own pinned constant).
2. **ane-bundles.md / schema 4** — relax `driver_abi_major: 1` to a
   small enumerated set `{1, 2}`. Add `compiler.target: h14` as a
   second accepted value, and add the matching `compiler.toolchain`
   provenance expectations.
3. **libane** — keep `LIBANE_CONFIG_STRICT_BIND` for ABI-1; build the
   ABI-2 shared object without it (the existing pin's `provenance.libane`
   text documents this exactly: "built without LIBANE_CONFIG_STRICT_BIND;
   island bundles bind some surfaces positionally"). Ship a separate
   `libane-abi2.so` next to `libane-strict.so` and have the worker load
   it based on `MLX_OMARCHY_ANE_ABI`. Pin both in
   `parakeet-runtime-pin.json`.
4. **libane ane_m2.c** — add a size check at the top of `ane_m2_send`
   so that `from + size > ctx->io_bo[..].map + io->size` is rejected
   rather than overrun. Symmetric in `ane_m2_read`. This is the
   worker→libane gap (gap 5).
5. **libane ane_m2.c, split_h14_tasks / FRAME_BYTES** — re-derive or
   relax the 16-byte frame alignment (gap 13 / goal note) to match the
   compiled H14 ANEC header. **Unit test:** round-trip an H14 ANEC and
   assert the section-builder's `firstTaskBytes % 16` is honored
   (`{4, 8, 12}` accepted).

Acceptance: `make -C libane check` PASS; H14 ANEC round-trip bytes
identical; `ane_run` of the nine proven ANECs (mul/relu/clip/scalar/
matvec/real-div/add) PASS on the M2 against the Stage 1-4 receipts.

### E4. Worker on the M2 with H14 ANEC of one op (no Parakeet yet)

Owner: omarchy-ane + mlx-omarchy. Goal: prove the ABI-2 worker end
to end on a single island-shaped program.

1. Pick the smallest non-trivial H14 ANEC — `mul` at C=128 — that
   exercises (a) 16-byte-frame non-alignment, (b) channel map via
   the positional map (un-strict build), (c) the send-clamp.
2. Build an mlx-omarchy bundle for it: a single-program manifest,
   schema 4, `compiler.target: h14`, `driver_abi_major: 2`,
   `release_asset.model_sha256` computed per ane-bundles.md:65.
3. `ssh jw14m2-linux 'MLX_OMARCHY_ANE_ABI=2 mlx-omarchy-ane-worker
   /path/to/bundle'` against an M2-resident bundle; observe
   `verify_hardware_eligibility()` passes with the new gate, the
   worker loads the program, executes a single submit, and the
   `cpu_tensor_events` stays at zero.
4. If PASS: commit the bundle schema relaxation in mlx-omarchy,
   commit the ABI-2 plumbing in omarchy-ane, both via worktree +
   push + merge to main. Update the M2 chip-coverage table in
   omarchy-ane README.

### E5. Island B (select) on the M2

Owner: H14Encoders + M2 owner + mlx-omarchy. Goal: smallest island,
fewest sources/destinations.

1. Wait for H14Encoders to land the select const-fill encoder (the
   ongoing thread). Confirm `make test-h14-parity` PASS grows the
   island count without regression and that the ANEC bytes round-trip
   through `tools/h14_sections.py`.
2. Build `island-select-8head` bundle for the M2 (1 program, 5 tasks,
   8 heads). Pin in `parakeet-runtime-pin.json` under
   `assets.bundles.island-select-8head` (already present, sha to be
   updated when the ABI-2 bundle is built).
3. End-to-end: `MLX_OMARCHY_ANE_DEVICE=on MLX_OMARCHY_PLACED=B
   mlx-omarchy-parakeet transcribe -o /tmp/parakeet-island-b`.
4. Acceptance: transcript SHA matches; `cpu_tensor_events = 0`; per-
   island counters in `transcribe-report.json` show one bundle load
   per layer for island B (no fallback); `encoder_hidden` SHA matches
   `38c73261…`. Notebook entry required.

### E6. Island A (kt) then C (pv)

Same as E5 with `MLX_OMARCHY_PLACED=A` then `=C`, then `=ABC`. The
bundles already exist in the pin JSON and need an M2-companion SHA.
The decoder still runs on the GPU; only the encoder islands move.
This step's acceptance is the same transcript hash and
`encoder_hidden` hash, with the additional check that the per-layer
ANE island bundles load once and execute N times (one per layer)
without per-layer re-staging — that's the resident-batch payoff
(ane-encoder-placement.md:34-37).

### E7. Whole-encoder M2 fallback (optional)

The whole-encoder ANE bundle is `parakeet-encoder-whole`, 13,701 TDs,
which the H14 backend does not yet emit (M1-only on the current pin;
the whole-program path is H13-only per
`provenance.whole_encoder` in parakeet-runtime-pin.json). Until the H14
whole-encoder compiler lands, this step is moot — the encoder on the
M2 must run through islands (E5 + E6).

### Ordered work list with owners

| # | Item | Repo | Owner | Risk |
|---|---|---|---|---|
| E1 | M2 pre-flight, gate, autoload recheck | omarchy-ane | M2Runtime | low (proven op gate) |
| E2 | GPU baseline `transcribe` on M2 (ANE off) | mlx-omarchy | M2 owner | low (M1 Max gold exists; M2 Vulkan is third-silicon) |
| E3.1 | runtime_worker.cpp gate ABI-aware | mlx-omarchy | mlx-omarchy owner | medium (touches a security-relevant gate; audit) |
| E3.2 | schema 4 accept `driver_abi_major: 2` and `compiler.target: h14` | mlx-omarchy | mlx-omarchy owner | medium (load contract change; new fixtures required) |
| E3.3 | libane ABI-2 un-strict build artifact + ABI dispatch | omarchy-ane | libane owner | medium (separate `libane-abi2.so`; pin both) |
| E3.4 | send-clamp at libane ABI-2 send/read | omarchy-ane | libane owner | low (one-line bound check) |
| E3.5 | H14 ANEC 16-byte-frame relaxation + unit test | omarchy-ane | libane + BuilderGeneral | medium (correctness; needs M2 device check) |
| E4 | single-op ABI-2 worker run on M2 (mul C=128) | both | joint | medium (first ABI-2 worker smoke) |
| E5 | island B E2E on M2 | H14Encoders + both | joint | high (first island) |
| E6 | island A and C E2E on M2, then ABC | joint | joint | high (whole encoder) |

### Risks named, not triaged

The plan does not propose keep/defer splits or capacity judgment; the
load belongs to the lead. Risks named:

- **Compiler gap risk:** H14Encoders' select const-fill and rms_norm
  chain encoders may not land before the worker gate is open. Island B
  depends on the const-fill work, not just the H14Mint 755-parity
  state.
- **Driver risk:** the installed-path "intermittent all-zero output on
  some boots" defect is unexplained. E1 must verify on a fresh boot;
  any recurrence requires the M2 owner to halt and re-run the gate
  before continuing.
- **M2Runtime risk:** the M2 is parked inside ctor2 on the legacy-only
  path (09-27T15:43Z, per `M2Runtime/20260927T1440Z...md`); the post-
  H2T-init "SCRATCH3 clears to 0" claim in omarchy-ane README is from
  an earlier publication and is not yet exercised under the encoder
  load. Until the firmware is past ctor2 end-to-end, E4 cannot run.
  The M2 owner (M2Runtime) has the standing ticket.
- **Pin risk:** `parakeet-runtime-pin.json` pins `libane-strict.so`
  with one SHA. ABI-2 needs a second pin entry, and the lockfile
  schema needs a way to record the ABI alongside the bundle.
- **Schema risk:** relaxing schema 4 to accept ABI 2 and `h14` is a
  load-contract change; every existing H13 bundle must continue to
  load unchanged. The unit tests for schema rejection (ane-bundles.md
  "Tests") are the regression net.
- **Vendor risk:** Apple's macOS 13.5 (22G74) selene firmware is the
  pinned 13.5 image (`a9c4b771…`); 14.x/15.x/26/27 paths are NOT
  covered (omarchy-ane README "T6021 legacy ChMan transport"). The M2
  must boot from the stub ESP for the duration of this plan;
  if Joshua reboots into a different macOS image, every inference
  is invalid until the stub is restored.
- **H14 ANEC layout risk:** `firstTaskBytes % 16` in `{4, 8, 12}` was
  flagged by the goal note. E3.5 covers it; if the relaxation breaks
  parity on the Stage 1-4 ops, fall back to requiring 16-byte
  alignment and recheck the H14 builder output (the goal is to
  discover what the builder assumes, not to silently accept what the
  firmware gives).

## Non-goals (explicit)

- The whole-program H14 ANEC for the encoder (parakeet-encoder-whole).
  This is H13-only today; an H14 port is separate work and out of
  scope here.
- Inference on macOS 14+ firmware. The 13.5 selene image is the only
  pinned path.
- Any change to the T8103 / T6001 lane. E3 changes are additive.
- Mesa changes. The Mesa fork is `joshuaswarren/mesa-1` per project
  rule; not touched in this plan.
- The mlx-omarchy repo is not edited by this plan; the M2 owner only
  installs and runs.

## Out-of-band dependencies

- H14Encoders must land the select const-fill encoder.
- H14Encoders must land the rms_norm chain encoder.
- BuilderGeneral must land the H14 section builder's generalization
  (in progress).
- M2Runtime must clear the firmware park inside ctor2.
- The Mac must keep the recurrent-mint toolchain
  (`~/recurrent-mint/ane-compile-hwx`) available at the recorded
  SHA-256 (`2060776c…`).
- The apple-silicon-lab notebook is the source of record. Pre-
  registration lives at
  `~/.local/share/apple-silicon-lab/entries/ParakeetM2Plan/20260930T021100Z-ct-parakeet-on-m2-plan.md`.
