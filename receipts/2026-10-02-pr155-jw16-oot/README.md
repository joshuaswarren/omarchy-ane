# PR155 ane as an out-of-tree module on jw16 (T6001) — build kit

`ane-driver-aurora` PR #155 (`joshuaswarren/linux` @ `f088ca5c86ed`) in trees the
`drivers/accel/ane` driver (H13: T8103/T6000-class, ABI 1). jw16 (M1 Max, T6001,
stock kernel `7.1.13-3-2-ARCH`) tests that exact driver code as an out-of-tree
module — same sources, same uapi header — with zero boot risk. T6001 matches the
`apple,t6000-ane` compatible (`ane_of_match`: t8103, t6000 only); the existing DT
overlay already names the node.

This kit replaces nothing on disk: `build.sh` writes only inside the extracted
directory, and the packaged module (`omarchy-ane 0.2.0.r14`) is restored at the
end of the window.

## Kit contents

| file | role |
| --- | --- |
| `ane/` | PR155 driver sources (`ane_drv/ane_tm/ane_dart/ane_boost` + t6021 set, headers) at `f088ca5c86ed`; `ane/uapi/drm/ane_accel.h` is the branch uapi header |
| `ane/Kbuild` | out-of-tree makefile: `obj-m += ane.o ane_t6021.o`, object lists verbatim from the in-tree Makefile, `ccflags-y += -I$(src)/uapi -I$(srctree)/include/uapi/drm` so `<drm/ane_accel.h>` resolves to the branch header (stock headers ship none; it is required, not optional) and the branch header's sibling `#include "drm.h"` resolves from the kernel headers, not from a copied kernel header. The driver has no MODULE_VERSION and this kernel yields no srcversion — an empty `modinfo -F version`/`srcversion` is expected; the module identity is the sha256 build.sh prints |
| `build.sh` | run from the extracted kit root; sanity checks (running release vs header tree, `include/uapi/drm/drm.h`, gcc, `ane/` sources), then `make -C /usr/lib/modules/$(uname -r)/build M=$PWD/ane modules` (module root is `ane/`), then sha256 + vermagic + srcversion + alias checks per module (ane.ko must alias `apple,t8103-ane` + `apple,t6000-ane` and NOT t602x/t8112), and builds the GET_CAPS probe when `/usr/include/drm/drm.h` exists |
| `ane_get_caps.c` | GET_CAPS probe: opens `/dev/accel/accel0`, expects DRM driver `ane` version major 1, GET_CAPS `abi_version=1 chip_family=13 sizes=0/0/0`, nonzero flags refused with EINVAL (built with `-I ane/uapi -I /usr/include/drm`) |
| `gates.sh` | the in-window gate run: lock, pre-record, module swap, ABI probe, H13 fixture ops (manifest-driven `ane-run --anec` runs over `h13-explicit-chain-add-mul` with byte-exact fp16 oracle compares; relu/matvec are named skips — no H13 fixtures exist), whole-encoder n1 (`fca96f1355485ec3`), restore, boot_id + dmesg + final smoke |
| `mk-tarball.sh` | regenerates the tarball from a linux checkout (`git archive f088ca5c86ed`) — the tarball is never committed |

`ane_t6021.ko` also builds (the packaged dkms MAKE[0] builds both; proven on jw16
2026-10-01). jw16 never loads it: T6001 needs `ane.ko` only, and the standing
lesson is to never rmmod `ane_t6021` (it is not installed on jw16 anyway).

## Fetch and build (jw16, CPU only, /var/tmp, nice 19)

    scp omp-studio-local:/var/tmp/pr155-jw16-oot/pr155-jw16-oot.tar.gz .
    sha256sum pr155-jw16-oot.tar.gz        # compare with pr155-jw16-oot.tar.gz.sha256
    tar xzf pr155-jw16-oot.tar.gz && cd pr155-jw16-oot
    nice -n 19 ./build.sh

Record the printed hashes, vermagic, srcversion, and alias list in the entry.
Expect: vermagic `7.1.13-3-2-ARCH SMP preempt mod_unload aarch64` (= running
kernel; build.sh enforces the release match), aliases t8103 + t6000, no M2
aliases. Nothing here insmods, depmods, installs, or runs dkms.

## Window run (the jw16 lead's protocol, unchanged)

1. Announce the window: append the JW16_MAINTENANCE line on the studio side;
   verify on jw16 that `/var/tmp/JW16_MAINTENANCE` is ABSENT (it marks an
   unresolved prior window), no gpuwin process, llm-inference active, and record
   `boot_id`. START announcement to the lead pane.
2. Run the gate inside the gpuwin window (llm stopped/restored by the gpuwin
   trap):

       gpuwin.sh 'bash /var/tmp/pr155-jw16-oot/pr155-jw16-oot/gates.sh /var/tmp/pr155-jw16-oot/pr155-jw16-oot'

   gates.sh does, in order, stopping at the first failure (no in-window retries;
   a wedge is reported, not cured):
   - refuses if the JW16_MAINTENANCE gate file exists; read-opens
     `/var/tmp/ane-run.lock` on fd 8 and flocks it for the whole window;
   - pre-record: boot_id, loaded srcversion (packaged `9109B200A150B27F484F718`
     today), installed ane.ko sha256, dmesg bad-line count
     (`tm completion|quarantin|EXCH|DART fault|translation fault|Oops|BUG:|kernel panic`),
     binding, `/dev/accel/accel0`;
   - swap: `sudo -n rmmod ane` then `sudo -n insmod <kit>/ane/ane.ko`; verifies
     the loaded srcversion equals the built file's and the driver binds;
   - ABI gate: `ane_get_caps` (expect its `ANE_GET_CAPS: PASS ... abi=1
     family=13 sizes=0/0/0` line);
   - ops gate: drives the pad-fixed `ane-run` over the H13 abi-verify
     fixtures in `ANE_FIXTURE_DIR` (default `/var/tmp/abi-verify/bundle`,
     manifest + program-0/1.anec): add then the chained mul, each compared
     byte-exact against an fp16 oracle built from the manifest (the old
     bare `--check` contract is gone from main, and `--check OP` needs the
     fixture inputs at channels 0/1 — these H13 fixtures use 5/6/4, so the
     gate runs and compares surfaces directly); relu and matvec are named
     skips (no H13 fixtures exist on jw16);
   - encoder gate: `gap7-bench OUT 1` on the kit module; hidden16 must be
     `fca96f1355485ec3` (hard); n1 ~1075 ms (band 1055-1219 warn-only);
   - restore: `rmmod` kit module, `modprobe ane` (the packaged module from
     `updates/dkms/`), verify loaded srcversion and installed-file sha256 equal
     the pre-record;
   - post-record: boot_id unchanged (hard), dmesg bad-line count unchanged
     (hard), one final `gap7-bench OUT 1` on the packaged module;
   - prints `JW16-GATES: PASS` only when every hard check held.
3. Copy the console log off-host, record hashes in the entry, END announcement,
   remove the studio-side JW16_MAINTENANCE line.

## Restore path (failure semantics)

gates.sh attempts the packaged-module restore exactly once on any exit path and
prints `JW16-GATES: FAIL` with the failing step. It never retries, never
installs, never runs depmod/dkms, and never touches `ane_t6021`. If the restore
line says `VERIFY MANUALLY`, the window ends there and the lead reports — same
rule as every lane run: report, do not cure.

## What this kit does NOT prove

- The chroot proof build (macstudio, `m2-headers/7.1.13-3-1-ARCH`) proves the
  Kbuild shape and alias set only, against the 3-1 M2 headers. jw16's vermagic
  differs (`-3-2-`), so jw16 must build with its own headers — the tarball
  carries sources, not binaries, by design.
- No loadability or runtime claim exists until the jw16 window runs: binding,
  GET_CAPS values, op bit-exactness, and the encoder golden are all in-window
  gates.
- The `relu` and `matvec` gates have no H13 fixtures on jw16 (the lead searched
  every `*.anec` < 2 MB); gates.sh reports both as named skips, not failures. A
  `matvec` run additionally needs a weights file (`--check matvec --weights`).
