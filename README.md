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
retained tm/tq state (kill-race 10/10 reopen-clean, no reboot).
**T6021 (M2 Max, 2026-09-30):** the ANE firmware runs from an autoloaded
`ane_t6021` module (`ane/t6021/`, DRM ABI 2 from commit 23b8eef, merged to
main in 1df4412) and executes compiled H14
programs through `libane`: fp16 add, mul, relu, scalar add/mul/div, clip,
and matvec up to 2048x5120 (20 MiB weights) pass on hardware, on the
valid lanes of each surface (`ane/t6021/gate/gate.sh`, receipt
[receipts/2026-09-29-t6021-installed-path](receipts/2026-09-29-t6021-installed-path/README.md)).
The four Parakeet attention island families (A kt, A p1, C pv, select)
run per layer on real data-dependent operands with the loop closed:
120 device submissions, zero failures, and the decoded transcript is
byte-identical to the golden transcript from the pinned macOS ANE
capture (receipt
[receipts/2026-09-30-t6021-parakeet-encoder-islands](receipts/2026-09-30-t6021-parakeet-encoder-islands/README.md)).
Not proven: the full Parakeet encoder on the ANE (every op outside the
four island sites runs on the CPU) and Qwen on the M2 as a model. All 38
Qwen programs match the M1 outputs on the device, program by program, at six
decode steps with the M1 inputs. This also holds with the resident state
chained on the M2 for three steps: 456 of 456 runs pass, the largest relative
L2 is 0.007, and the 18 DeltaNet programs are bit-exact. The host steps
between programs and a token-level decode over all 38 have not run (receipt
[receipts/2026-09-30-t6021-qwen-chain/conformance.md](receipts/2026-09-30-t6021-qwen-chain/conformance.md)).
One intermittent all-zero-output failure seen on three boots is
unexplained (a 1 ms post-call settle is the mitigation). The module also
runs on the stock linux-asahi `7.1.13-3-1-ARCH` kernel: 20 of 20 gate
loads on each of three boots, the third with the packaged overlay
`packaging/dt/t6021-ane.dts` (receipt
[receipts/2026-09-30-t6021-stock-mailbox](receipts/2026-09-30-t6021-stock-mailbox/README.md)).
Every boot so far is a USB chain load from the M1 host. The host must
not touch TM registers while the firmware runs; T6021 needs the pinned
13.5 firmware and a DT overlay, and the module cannot be unloaded.
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

- **DKMS.** `dkms.conf` builds `ane.ko` from `ane/` and `ane_t6021.ko` from `ane/t6021/` for each kernel that has headers. DKMS builds them again after each kernel update. Install the headers for the kernel you run, for example `linux-aurora-headers` or `linux-asahi-headers`. The chip gate in `ane.ko` decides which SoC binds. T8103 and T6001 bind. T6021 does not bind to `ane.ko`. `ane_t6021.ko` does not load until you opt in (see "M2 Max opt-in").
- **Device access.** The package adds no udev rule. The systemd default rule (`50-udev-default.rules`) sets each `/dev/accel` node to mode `0666`. Every user can open the ANE node.
- **Check.** Run `omarchy-ane-check`. It checks the device-tree node, the module build for the running kernel, the loaded module, the bound device, and that every user can open the device node. On an M2 Max it checks `ane_t6021` and the opt-in state. It does not load the module. It exits with 1 when a check fails. After a kernel update, run `omarchy-ane-check --installed`. It checks that `ane.ko` and `ane_t6021.ko` are built for each installed kernel.
- **Firmware.** M1 chips need no firmware from Linux. iBoot loads it. On M2 Max (T6021), `omarchy-ane-m2-enable` runs `omarchy-ane-firmware-fetch`. It reads the stub macOS version. It downloads only the ANE file from Apple and unwraps it. It installs the file only when the size and SHA-256 agree with the driver. It needs only Python 3. It stops and installs nothing when the version is unknown, the network is down, or the hash is different. We do not distribute Apple firmware.

### Device-tree node

`ane.ko` binds only to a device-tree node with an `apple,t*-ane` compatible. The device trees in `linux-aurora` 7.1.12 and `linux-asahi` 7.1.13 do not have this node. The package adds it with an overlay until the kernel's device trees have it.

- **Overlays.** `packaging/dt/` holds one overlay for each SoC. `packaging/build-dtbo` compiles each `PREFIX-NAME.dts` to `/usr/lib/omarchy-platform/dtb-overlays/PREFIX/omarchy-NAME.dtbo`. The prefix selects the board device trees by file name: `t8103` selects every `t8103-*.dtb`. The T8103 overlay makes the same ANE, DART and power-domain nodes that the bound M1 host has. The T6001 overlay makes the nodes of the lab overlay `ane/t6001-j316c-set-domains.dts`. The T6021 overlay is opt-in: it has the root string `omarchy,opt-in = "ane-t6021"`, and it applies only when `ane-t6021` is a line of `/etc/omarchy-platform/dtb-overlays.opt-in`. T6021 also has an opt-in overlay that is not an ANE part: see "U-Boot input" below.
- **The kernel wins.** Each ANE overlay names its compatible in `omarchy,skip-if-compatible`. When the kernel's board device tree has that node, that overlay is not used. The other overlays still apply.
- **Arch Linux ARM (asahi-alarm).** `omarchy-ane-dt apply` finds this Mac's board device tree from `/sys/firmware/devicetree/base/compatible`. For each installed kernel, it applies the overlays to a copy of that device tree. It checks that dtc can read the result and that the ANE node is present, and that every reference in the new nodes resolves. It writes the copy to `/var/lib/omarchy-ane/dtbs/KERNEL/`. It does not change a file that a package owns. It adds one line to `/etc/default/update-m1n1`. That line sources `/usr/lib/omarchy-ane/update-m1n1-dtbs`, which puts the copy in `DTBS` only while the copy was made from the same kernel file. A second run changes nothing. If a step fails, the original device trees stay in use and the tool prints the reason.
- **Kernel updates.** The pacman hook `90-omarchy-ane-dt.hook` runs `omarchy-ane-dt apply` after a kernel update. It runs before `95-m1n1-install.hook`, so `update-m1n1` reads the new copy. omarchy-ane does not run `update-m1n1` itself. After the first install, run `sudo update-m1n1`, then reboot.
- **Omarchy Macs (omarchy-mac-boot).** omarchy-mac-boot builds `boot.bin` and checks it on each `omarchy update`. A version of omarchy-mac-boot with device tree overlay support ([omacom/omarchy-mac#677](https://github.com/omacom/omarchy-mac/pull/677), draft) applies `/usr/lib/omarchy-platform/dtb-overlays` itself, and `omarchy-ane-dt` does nothing. With an older omarchy-mac-boot, `omarchy-ane-dt` refuses, because its boot check stops `omarchy update` when a device tree changes.
- **Removal.** Removing the package runs `omarchy-ane-dt remove`. It removes the line and the copies. `boot.bin` keeps the node until `update-m1n1` runs again: run `sudo update-m1n1`.
- **Status.** `omarchy-ane-dt status` and `omarchy-ane-check` tell you where the running node comes from: the kernel's DTB, the omarchy-ane overlay, or no node.

### M2 Max opt-in

The M2 Max (T6021) ANE works only with `ane_t6021.ko`, and that module cannot be unloaded. When it starts the ANE firmware, only a reboot releases it. So the package does not let it load. `/etc/modprobe.d/ane_t6021.conf` has the line `install ane_t6021 /bin/false`, and the T6021 overlay is off.

- **Opt in.** Run `sudo omarchy-ane-m2-enable`. It refuses, and changes nothing, when this Mac is not a T6021, when `ane_t6021.ko` is not built for the kernel, or when the ANE mailbox in the resulting device tree lacks an interrupt. The stock `apple-mailbox` binds the mailbox only when it has both `recv-not-empty` and `send-empty`. The T6021 overlay gives it both (AIC2 lines 884 and 1833; 1833 never fires), so no kernel patch is needed. Next, it runs `omarchy-ane-firmware-fetch` and refuses if the firmware is not the pinned image. Then it turns the T6021 overlay on, applies it with `omarchy-ane-dt`, and comments out the block line. pacman keeps that edit. Run `sudo update-m1n1`, then reboot. `ane_t6021` loads on that boot.
- **Reboot-only rule.** Do not `rmmod ane_t6021`. Only a reboot unloads it.
- **Opt out.** Run `sudo omarchy-ane-m2-enable --disable`. It restores the block line, turns the overlay off, and removes the fetched firmware. Run `sudo update-m1n1`, then reboot.
- **U-Boot input (separate opt-in, one laptop).** On one M2 Max laptop, every boot from the internal disk stopped at the U-Boot prompt. The U-Boot internal keyboard input (`mtpkbd`, the MTP DockChannel HID) gives a key during the 1 s autoboot countdown, and with the countdown off it stops the GRUB menu timeout. Nobody pressed a key. This input may be specific to that laptop. The workaround is `packaging/dt/t6021-uboot-serial-stdin.dts`: it sets U-Boot's `/config` to skip the key check and to take console input from serial only. It is off by default and is not part of the ANE opt-in. To turn it on, add the line `uboot-serial-stdin-t6021` to `/etc/omarchy-platform/dtb-overlays.opt-in`, run `sudo omarchy-ane-dt apply`, then `sudo update-m1n1`, then reboot. Cost: the internal keyboard does not work at the U-Boot prompt or in the GRUB menu; only the serial console gives input there. To turn it off, remove the line and run the same commands. This overlay goes away when uboot-asahi gets a fix that passes only keyboard reports from `mtpkbd`. Receipt: [t6021-disk-boot](receipts/2026-10-01-t6021-disk-boot/README.md).
- **State.** `omarchy-ane-m2-enable --status` prints one line: module blocked or enabled, firmware pinned or not, overlay on or off, the mailbox check, and whether `ane_t6021` is loaded. `omarchy-ane-check` shows the same state on an M2 Max.
- **Fixed.** The mailbox receive-IRQ storm and the latency stalls it caused are fixed by PR #8: `ane_t6021.hello_wait_ms` now defaults to 0, so the mailbox never starts. Before the fix, line 884 fired about 700,000 times per second (about one CPU in interrupt time), and the `add` p90 was 95 to 152 ms. After it, the line does not fire, the p90 is 1.29 to 1.42 ms and the median is 1.28 to 1.40 ms, and a 30 s loop runs about 4,800 processes (it was 203). The per-boot BO cap is fixed by PR #9, a recycle pool for io BOs. Before the pool, a boot stopped after about 14,500 `ane-run` processes (the 2 GiB cap). With the pool, one boot ran 105,232 processes in 420 s with 0 failures and flat memory. Receipts: [t6021-stock-mailbox](receipts/2026-09-30-t6021-stock-mailbox/README.md) ("Option A applied") and [t6021-bo-pool](receipts/2026-09-30-t6021-bo-pool/README.md).
- **Fixed.** A CALL waited only until the last task was dispatched, so a long program returned before it had finished: Qwen program 20 ran for about 3.3 ms after the wait ended and read an all-zero output. A CALL now waits for the firmware's finish event on the IO_T2H ring (3.5 ms after the ack for program 20). Receipt: [t6021-call-wait](receipts/2026-09-30-t6021-call-wait/README.md).
- **Fixed.** One boot could not hold the sections of all 38 Qwen programs (2.6 GiB) under the old 2 GiB BO cap. The cap is now the parameter `ane_t6021.bo_total_max_mb` (12 GiB by default), and one boot loaded and ran all 38 programs once each. The 32-bit DMA mask still limits all BOs to 4 GiB of IOVA. Receipt: [t6021-bo-cap](receipts/2026-10-01-t6021-bo-cap/README.md).
- **Fixed.** `ane-run --ports` read only the first output of a program. libane checked the output index against the ANEC header, which records one output in every Apple-compiled Qwen ANEC, and ane-run wrote uninitialized memory for each later output. Send and read now use the M2 port model. Receipt: [conformance](receipts/2026-09-30-t6021-qwen-chain/conformance.md).
- **Known limits.**
  - One M2 Max booted from the internal disk with `update-m1n1` and the packaged T6021 overlays on 2026-10-01: `ane_t6021` loaded at boot and six gate ops were bit-exact ([receipt](receipts/2026-10-01-t6021-disk-boot/README.md)). That boot used a lab m1n1 stage 2, which adds the two `ane-firmware` reserved-memory nodes that `ane_t6021` maps. The packaged m1n1 1.6.1 does not add them, so a disk boot with the packaged m1n1 is not proven.
  - An intermittent all-zero output, seen on three boots, is not explained. A 1 ms settle after each call is the mitigation.

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
| M2 Max | T6021 | H14J | `apple,t6021-ane` | research driver, opt-in, not enabled by default (the packaged `ane.ko` chip gate does not bind T6021 and the packaged overlay is off until opt-in); no unload after firmware start, reboot-only reclamation | Autoloaded `ane_t6021` (DRM ABI 2) + libane: add, mul, relu, add/mul/div-scalar, clip, matvec up to 2048x5120 exact or within the recorded tolerance, 2026-09-29 ([receipt](receipts/2026-09-29-t6021-installed-path/README.md)); Parakeet attention islands (A kt, A p1, C pv, select) per layer on real operands, transcript byte-identical to golden, 2026-09-30 ([receipt](receipts/2026-09-30-t6021-parakeet-encoder-islands/README.md)); 160/160 gate trials on four parallel workers, 150 matvec loads with no BO exhaustion; stock linux-asahi `7.1.13-3-1-ARCH`, 20/20 gate loads on each of three boots, the third with the packaged overlay, 2026-09-30 ([receipt](receipts/2026-09-30-t6021-stock-mailbox/README.md)). Pinned 13.5 selene `a9c4b771…`. | Full Parakeet encoder and Qwen are NOT yet on the M2 ANE (H14 compiler coverage: rms_norm, softmax, silu/sigmoid shapes, batched matmul; Qwen program 20 matches the M1 golden with one call, [receipt](receipts/2026-09-30-t6021-call-wait/README.md), and all 38 Qwen programs load and run once on one boot with unchecked outputs, [receipt](receipts/2026-10-01-t6021-bo-cap/README.md)); an explanation for the intermittent all-zero output on some boots; the DT overlay as a packaged board DTB; a proven disk boot (every boot so far, stock kernel included, is a USB-proxy chain load) |
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
