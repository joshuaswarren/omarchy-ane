# J615 (Apple M3, T8122) In-Tree ANE Proof Kit

One-page guide to prove the upstream `aurora-silicon/linux` in-tree ANE driver (`/dev/accel/accelN`, DRM ABI 3) on a J615 (Apple M3, `T8122`) using `omarchy-ane` userspace (`libane` and `ane-run`). No out-of-tree kernel module is required.

## 1. Prerequisites

- **Kernel**: `aurora-silicon/linux` `main` at or after `abf39d86cf87` and `0882c538809a`, built with `CONFIG_DRM_ACCEL_ANE=y` (or `m`) and `t8122-j615.dtb`.
- **Firmware**: macOS **27.2 (`26B123`)** firmware set (`ane_h15_build_t8122_26b123`).
- **Loader (`m1n1` / `aura`)**: must inject the two `no-map` `/reserved-memory` regions and attach `memory-region` and `memory-region-names` on `/soc/ane@310000000`.
- **Build tools on Linux**: `gcc`, `make`, `python3`, `libdrm` headers (`libdrm-dev` or `libdrm`).

## 2. Run (three commands)

```bash
git clone https://github.com/joshuaswarren/omarchy-ane.git
cd omarchy-ane
bash tools/j615-ane-kit/j615-ane-prove.sh
```

Options:
- `--repeat N`: execution count (default `100`).
- `--dev N`: accel device index (default `0`, `/dev/accel/accel0`).
- `--hwx FILE`: test a custom H15 (`subtype 6`) `add` `[1, 1, 128, 128]` HWX compiled from `fixtures/h15-hwx/add/model.mil` (`-t h15`). Defaults to `fixtures/h15-hwx/add/model.hwx` (`sha256 01d9439200cad6730a6c8755030ef325b2c4b84cabb0eb2aa0cadd2a44bd0fae`).
- `--raw-hwx`: send the unmodified HWX directly to `DRM_IOCTL_ANE_H16_LOAD`. By default the runner strips three non-executing Mach-O metadata items (`__TEXT` flag `SG_NORELOC=4` without scratch, `LC_NOTE "src model info"`, and 26 debug `SYMTAB` stabs) that Aurora `main`'s `drivers/accel/ane/ane_h16_hwx.c` currently rejects on H15 (`lines 178, 558-564, 719`), leaving `__TEXT,__text`, `__TEXT,__const`, the `0x890` program descriptor, and all tensor descriptors byte-identical.

## 3. Expected Output

```text
=== 1. Kernel, DT and /dev/accel preflight ===
kernel: <uname -r>
dt status: okay
dt compatible: apple,t8122-ane,apple,ane
dt memory-region: present (<N> B)
device: /dev/accel/accel0 (abi=3, chip_family=15, section_size=16, exec_io_size=16)
=== 2. Build omarchy-ane libane + ane-run ===
...
=== 4. Execute through omarchy-ane ane-run (--repeat 100) ===
exec ms over 100 calls (100 samples): min ... median ... p90 ... p99 ... max ... mean ...
16384/16384 lanes bit-exact vs the add-dense reference, padding lanes zero: PASS
output_sha256: d113d9ae6734b7a7e233458229f64f382da99a3f93868eef3abb06a29f7f4ce8
=== PASS: J615 T8122 in-tree ANE verified ===
SUMMARY: kernel=<uname -r> repeat=100 sha256=d113d9ae6734b7a7e233458229f64f382da99a3f93868eef3abb06a29f7f4ce8 exec ms over 100 calls ...
```

## 4. What to Send Back

- The `device:` line and the final `SUMMARY:` line (kernel version, `16384/16384` pass/fail, output SHA-256, and `min` / `median` / `p90` / `p99` / `mean` ms).
- Whether you ran with the default canonicalized HWX or `--raw-hwx`.
- If `preflight`, `H16_LOAD`, or `H16_EXEC` fails, include the script stderr and `dmesg | grep -iE 'ane|dart'`.
