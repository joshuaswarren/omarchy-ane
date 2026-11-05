# Diagnostic-kernel deployment gates — draft, not authorization

No deployment or boot configuration changes are made by this plan.

## Build gate

Use kernel pin `4fdd0fc5c931ef9f14474942b37c3d451e97fc04` plus the
reviewed publication patch. Recipe helpers are from package-repository
commit `158e9d36f67595e06e455e2aec559915bbac6e8e`; verify each helper
against the retained PKGBUILD's SHA-256 list. The isolated build script
is `/var/tmp/opencode/h17-kernel-kit/build-diagnostic.sh`; its stdout and
stderr go to `full-build.log` in the same directory. Toolchain downloads
and artifacts remain outside system installation directories.

Build `Image modules dtbs` with LLVM18, Rust 1.85.0 for the kernel,
Rust 1.99.0 for bindgen construction and rustfmt, and the recipe's
patched private bindgen. Preserve Rust platform drivers. Use release
suffix `-14-1-h17pub-ARCH` to avoid overwriting the installed module tree.
Record config differences, final release, source/patch hashes, compiler
versions and artifact hashes. A successful object compile is insufficient.
Do not disable missing capabilities to get a successful build.

## Deployment gate (separate future operation)

Before installing anything, review the full build result and inspect this
machine's boot assembly mechanism. Establish how to retain and select the
known-good kernel **and its matching DTBs, modules, initramfs and boot
bundle**. The installed boot path must not be inferred from filenames
alone. A bare Image copy is not an acceptable deployment plan. Do not
replace the bootloader, base DT or device overlay as a side effect.

Keep diagnostic opt-in off during any non-rung boot. Confirm the compiled
parameter prefix from build metadata before proposing a kernel command
line; no assumption about hyphen/underscore spelling is sufficient.

## Runner gate

The existing `run-table-discovery.sh` records kernel release but does not
validate the diagnostic kernel, parameter state, patch or build identity.
Before using it for this rung, add and independently review a separate
fail-closed diagnostic preflight. It must require:

- The expected fresh boot, exact diagnostic release and qualified build
  identity; a new release string alone does not prove patch identity.
- Presence and enabled state of the intended diagnostic parameter.
- A newly built ANE module matching the diagnostic kernel, not the
  current installed kernel's `.ko`; record its hash and vermagic.
- Pushed clean ANE checkout and one-shot marker, no preloaded ANE module,
  no existing attempt/evidence directory for this boot.
- Expected unchanged firmware hash, device binding/DT and staged-rung
  geometry. The publication patch must not silently substitute mappings.

Do not create the pending marker until build, deployment and runner gates
are reviewed. The current failed boot remains parked with its ANE module
pinned; never unload/reload it. The next measurement requires the
authorized reboot, not a module swap or retry in this boot.

## Evidence gate

Start dmesg capture before ANE module load; preserve early diagnostic
snapshots as well as the rung's request/reply log. Record sequence limits
and whether the candidate addresses still cover the new allocations.
Keep the existing table judge authoritative for rung PASS/FAIL; interpret
publication samples separately. Root-entry equality is not proof of a
complete hardware walk, and a printed low address is not independent
qualification of exception register semantics.

Stop before SCRATCH3 registration, doorbell, task submission and ANE IRQ
acknowledgement. A timeout/fault keeps all module state pinned until a
reboot. No result read after that failure is evidence for a later rung.
