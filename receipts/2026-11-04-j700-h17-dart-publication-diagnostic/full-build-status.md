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

`build-diagnostic.sh` retains the script launched for this attempt.
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
