# T6021: ANE node from a candidate m1n1 stage 2, hand-loaded module, first jobs (2026-11-05)

One 14-inch M2 Max MacBook Pro, one supervised boot, one `insmod` of `ane_t6021.ko`. Then the first ANE jobs on that laptop with this module. All numbers come from that one boot. One operator ran the boot and the load. A second operator ran the jobs.

## What the boot file adds

The ANE node comes from the m1n1 stage 2 on the ESP (`boot.bin`), not from an overlay applied on the disk.

| File | sha256 |
|---|---|
| stock `boot.bin` | `7e03d55ac451213e3d6528c634911c29cd76d3eebb66c207236648e0a062dacd` |
| candidate `boot.bin` | `40f57b9f15d9dc532a8fb5f2031bedb223ce1168ae4387ac755404b29e0fa14b` |
| device tree inside the candidate | `886cbfe5c1cda8ba05f007803e94d30497bcac29086fa6a25dd95d932850d1e6` |

After the boot from the candidate, `/proc/device-tree/soc/ane@284000000` had compatible `apple,t6021-ane` and status `okay`. The stock boot had no such node. The candidate is a lab build and is not published. This record does not say what else its device tree contains. A node alone proves nothing about the driver, so the load is a separate step.

The candidate changes the stage 2 for every Linux boot of that laptop until the stock file is put back. The default boot entry does not change.

## Load steps

The module was built for the running kernel, `7.1.12-2-7-ARCH`, omarchy-ane 0.5.10, 1,679,864 bytes, sha256 `0506923c2a4c7c83ab116184ce4ebd9914478348006d7b8373772ff3a0fe0078`. Nothing was installed under `/usr/lib/modules` and nothing autoloaded.

A guarded block ran once. Before the `insmod` it refuses on any of these:

- the module file is missing, is a symlink, or has another size or hash than the recorded ones
- the module `vermagic` kernel differs from `uname -r`
- the firmware at `/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho` is missing or does not have sha256 `a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`
- the ANE node is missing, or its compatible is not `apple,t6021-ane`, or its status is not `okay`
- `ane_t6021` is already loaded
- `boot.bin` on the ESP is not the candidate

Then one `timeout 30 insmod`, with no retry. Exit codes: 0 `LOADED`, 3 `REFUSED` (an error and no module), 4 `LOADED-DESPITE-ERROR`. The driver counts as bound only when `readlink -f` of the ANE device's `driver/module` is `/sys/module/ane_t6021`.

## Result of the load

| Item | Value |
|---|---|
| exit code | 0, `LOADED` |
| `bound` | `yes`, `driver_module=/sys/module/ane_t6021`, refcnt 2 |
| `lsmod` | `ane_t6021` 147456 bytes, 2 users |
| device | `/dev/accel/accel0`, `crw-rw-rw- root render` |

New kernel lines, in order: the out-of-tree taint line, then `LAB init resource[0x84] bit0=1: prevent nap`, `boot: run returned 0 (booted=1 fw_alive=1)` with the done handshake observed, `CPU_STATUS=0x2c`, `chman: table VALIDATED (mismatch mask 0x0)`, `[drm] Initialized ane 2.0.0 for 284000000.ane on minor 0`, and `loaded ane_t6021 0.5.10 (DRM major 2 minor 0; ABI 2; legacy_only=1 chman_ok=1 booted=1; state HELD; BO cap 12288 MiB)`. No error lines. The driver reports `module PINNED until reboot`: after the firmware starts, the only way to unload the module is a reboot.

The driver marks `scratch_result=00000000ff7a0000` as a raw device address with unsourced meaning. Do not read it as a success value.

## First jobs

A state check of 30 seconds first: no fault, SError, Oops or coprocessor crash lines; `ane_stats` busy_ns 0, jobs 0. Then the gate loop with `ane-run --check`, built from omarchy-ane `7763786f`:

| Op | Trials | Reopen and repeat 3 | `ane_stats` busy_ns, jobs (before to after) |
|---|---|---|---|
| add | 4 of 4 PASS, 512 of 512 lanes bit-exact | PASS | 0, 0 to 2171287, 7 |
| mul | 4 of 4 PASS, bit-exact | PASS | 2171287, 7 to 4311082, 14 |

No fault lines after both runs. Mean busy time is about 306 to 310 microseconds per job, derived from `busy_ns` over jobs; it includes the reopen pass.

## Native 1024x1024 matvec

A second operator ran this on the next boot of the same laptop, after the module was hand-loaded again (same module hash, same kernel). The program is a `mil-hwxc` H14 ANEC from `h14-ticket/mv/k1024n1024`: M=1, 2 MiB of weights baked in. It is not Qwen size.

| Item | Value |
|---|---|
| gate | 4 trials and reopen and repeat PASS |
| lanes in band | 1024 of 1024, max 0.121 conditioning units; 956 to 966 bit-exact per trial |
| ANE busy time | 2519585 ns over 7 jobs, 360 microseconds per job (derived from `ane_stats`) |
| CPU baseline | one thread, fp32, same weights, same laptop: 238.2 and 237.2 microseconds per matvec |

One ANE job is slower than one CPU core at this size. This is busy time on the device, not wall time per call, and nothing here shows a speedup.

The first run failed: 30 of 1024 lanes. The operator fed random weights, but the ANEC bakes in the compiler's own weights. The second run used the matching weights file and passed. Both runs are in the log. The checks ran with the same own script as above.

## Deviation from `gate.sh`

The laptop had no numpy and no pip. The sparse input files were generated on another machine with the same code and seeds as `ane/t6021/gate/gate.sh`, copied over, and run through the same `ane-run --check` loop by a separate script. This is not `gate.sh` itself. The operator reported that the programs come from the checkout at that commit (`fixtures/h14-anec`).

## Restore path, tested, and the state after

A guarded script on Linux put the stock `boot.bin` back. The next boot ran the stock stage 2: `boot.bin` matched the stock hash and the ANE node was absent. That proves the way back. The candidate was then installed again and the laptop booted on it. The ESP also holds `boot.bin.ane-stock`, a copy of the stock file for rollback.

On that second boot the node is present and the module is not loaded. The module has no autoload: it is loaded by hand, once per boot, with the same guarded block. The operator did that at 13:43Z. The candidate stays as the default stage 2 until a boot fails; any failed boot gets the stock file back at once.

If Linux does not return, the stock file goes back from macOS with a second script. A stock copy also sits off the ESP and on a network share.

## Not covered

- matvec at 2048x5120 or 2048x2048: no program with matching weights was available. The one 2048x2048 try failed on a weights mismatch with a single sample, so it is not a row.
- Wall time per call. `busy_ns` is device busy time.
- Per-job time beyond `busy_ns`, and any run under load.
- Any other T6021 laptop, kernel build or module build.
