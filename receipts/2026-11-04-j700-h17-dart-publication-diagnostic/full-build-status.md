# Full-kernel build attempts

This is build-only evidence. Nothing has been installed or armed.

## Attempt 1 — prerequisite provisioning failed

`full-build-attempt1.log` retains the error. In private homes
`/var/tmp/opencode/h17-rustup` and `/var/tmp/opencode/h17-cargo`, Rust
1.85.0 downloaded but rustup then refused because its executable was not
installed in that Cargo home. No kernel configuration or compilation ran.
The empty output directory was preserved as
`/home/admin/aurora-build/h17-publication-kernel-attempt1`.

## Attempt 2 — private bindgen built; fixture tests failed

Commit `f969625` retains the script launched for this attempt as
`build-diagnostic.sh` (the file has subsequently been hardened).
It provisions private rustup/proxy executables, verifies the actual source
diff against the reviewed diagnostic patch and checks recipe-helper hashes
before executing them. It requires the exact distinct release
`7.2.11-14-1-h17pub-ARCH`, rejects an existing module tree of that name,
uses a fresh output directory and builds with two jobs at reduced priority.

`full-build-attempt2.log` retains the failure. The private Rust 1.85.0 and
1.99.0 toolchains installed; patched bindgen compiled, but its fixture
test runner could not find `clang` on PATH. Kbuild's absolute LLVM prefix
does not set PATH for that test runner. No kernel configuration ran.
Artifacts are preserved in output directory suffix `-attempt2` and
`/var/tmp/opencode/h17-kernel-kit/private-bindgen-attempt2`.

## Attempt 3 — launched, result pending

The retained script now puts `/usr/lib/llvm18/bin` on its private build
PATH and records resolved compiler, Rust, formatter and bindgen versions
plus source/patch hashes before configuration. This is a provisioning
fix, not a change to the driver patch or qualified kernel capabilities.

The exact launched script is `build-diagnostic-attempt3.sh`. Re-review
identified that its `git diff` checks omitted staged/untracked changes.
A separate audit of this actual worktree compared `git diff HEAD` with
explicit `i/`/`w/` prefixes against the retained patch: identical; staged
file list and untracked (non-ignored) file list were empty. No source
changes were made. `build-diagnostic.sh` now enforces those stronger
checks for future launches. The running script was not edited in place.
This is not a claim of hermeticity against ignored files or concurrent
source modification; a final source check remains a completion gate.

Output: `/home/admin/aurora-build/h17-publication-kernel`.
Log: `/var/tmp/opencode/h17-kernel-kit/full-build.log`.
The process runs asynchronously; this receipt does not claim completion.
Do not start a concurrent build against those directories or treat output
file existence as success. Completion must include the script's successful
exit and all requested Image/modules/DTB targets.

Independent review of the initial build script/deployment draft (session
`ses_e56319f3effenctpyqgG3ktsys`) identified missing enforcement of input
hashes and full release matching. Attempt 2 adds those checks and documents
Rust 1.99.0's bindgen/formatter role. Final compiler/tool versions, source and
patch digests, config changes, artifact hashes and deployment review remain
required before any hardware operation. That review was not build-result
approval, and no deployment is authorized by it.

### Attempt-3 setup/configuration milestone (not build completion)

The following artifacts were inspected for prerequisite/configuration
validation while compilation runs; they are not completion markers:

- `attempt3-build-identity.txt`: LLVM/LLD 18.1.8, kernel Rust 1.85.0,
  bindgen-build Rust 1.99.0, rustfmt 1.10.0, bindgen 0.73.2, source and
  recipe-helper hashes. The patched source SHA-256 is
  `cc6fc81ceebdd51293c6903c03dbc0a8227a740c915d03e3caeebdbb0f8ec5c6`.
- `attempt3-bindgen-result.json`: private tool built and fixture-tested;
  binary SHA-256
  `63e47fbaf206a27899a9e564650902ca33d81d3a4839438b2d4ba50612cfeaf9`.
- Diagnostic release resolves to `7.2.11-14-1-h17pub-ARCH`.
- Resolved `.config` is **byte-identical** to
  `/lib/modules/7.2.11-14-1-ARCH/build/.config`: both SHA-256
  `127a9570ea7c294621566e63b628bb4759ad71729b0148b2b8ca90d527dba903`.
  `config-vs-installed.diff` is empty. No Rust or platform capability
  was dropped to obtain this configuration. The distinct release comes
  from the output directory's localversion file, not a config change.

Source diff versus `HEAD` was rechecked against the reviewed patch at this
milestone; staged and non-ignored untracked file lists were empty. This
does not substitute for the final source/artifact identity check after a
successful full build. Nothing has been installed or armed.
