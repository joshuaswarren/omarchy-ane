# ane_stats: kernel-tree proof builds

Scope: prove that `ane.ko` and `ane_t6021.ko` build against two real
aarch64 kernel trees with no new W=1 warnings versus the pre-stats
baseline (main@daa7447). No module was loaded and no device was
touched; every .ko here is a build artifact only.

Branch: `agent/ane-stats-fix`. Carries the merged ane_stats feature
(9ec1a60) plus, in order:
- 8466893 / 38b9596 — cherry-picks of 97c792e and d6e3274 (the w71
  compile fixes: atomic64 acquire/release, no module_param in an inline
  helper, ane_stats_counters_init rename, show-TU includes).
- 69cbc9a — fops linkage and t6021 glue fixes (found by reading the
  target trees' macro definitions before the first build):
  - `DEFINE_SHOW_ATTRIBUTE` emits a static fops, so the cross-TU
    `extern` uses of `ane_timeline_fops` could not link. The fops is
    now defined non-static in ane_stats_show.c with the same members
    the 7.1 macro emits; the stray export of the static show function
    is gone.
  - the t6021 probe used `dev_attr_ane_stats` and `ane_timeline_fops`
    with no declaration (compile error); both are declared extern.
  - the t6021 probe copied `fw->stats_ring.slots` into
    `fw->stats_slots` after `ane_stats_counters_init` had zeroed the
    ring, so both stayed NULL and the first submission with stats=1
    would write through a NULL pointer. The slots array is attached
    after init, in the order ane_drv.c uses.
- 836b8ac — include-what-you-use: `string.h` for memset moved into
  ane_stats.h; ane_drv.c takes `device.h` and `slab.h` directly; the
  show TU keeps only includes it uses.
- 08ff1ec — ane_drv.c line 1 back to a terminated `// SPDX` comment
  (a7dfe4b had left an unterminated `/*`, which is a fresh `-Wcomment`
  warning under W=1).

## Tree A: M2 3-1 (ALARM chroot build oracle)

Kernel tree `~/src/m2-headers/7.1.13-3-1-ARCH`, kernel.release
`7.1.13-3-1-ARCH`, chroot image `dg-alarm-py314:sep23`, recipe shape
from receipts/2026-09-30-packaging-dkms and the AneDsid build log.
`CONFIG_DEBUG_INFO_BTF_MODULES=` (chroot has no pahole; the kernel
banner warning is environmental and appears in the baseline too).

```
make -j8 -C /work/kh M=/work/kit/ane W=1 CONFIG_DEBUG_INFO_BTF_MODULES= ANE_VERSION=0.4.0-g836b8ac modules
make -j8 -C /work/kh M=/work/kit/ane/t6021 W=1 CONFIG_DEBUG_INFO_BTF_MODULES= ANE_VERSION=0.4.0-g836b8ac modules
```

| module | sha256 | vermagic | version |
|---|---|---|---|
| ane.ko | `a1bf34e8c636edfcec5e97dad73f154d1bf83fd6454dc538aa10393f8ab6fdc1` | `7.1.13-3-1-ARCH SMP preempt mod_unload aarch64` | 0.4.0-g836b8ac |
| ane_t6021.ko | `ebead4dff767e639304886b5e496c068584fef345fa365c8b187fc87109e21d2` | `7.1.13-3-1-ARCH SMP preempt mod_unload aarch64` | 0.4.0-g836b8ac |

Baselines (daa7447, same flags): ane.ko
`225761b6eab3f701ddd926cfbd7a6d16f1e389f873a4a473a3901c9948ea1a04`,
ane_t6021.ko
`aea2412d62d70c801b37c00b00e3450c6f5115c8c7892e1f43bfff7487a18a65`.

W=1 result: the ane fix build has zero warnings. The t6021 fix build's
warnings are exactly the baseline set (`ane_t6021_load_sec_name`
unused-const, `t2h_ioq`/`t2h_buf` set-but-not-used; only line numbers
shift). `modinfo -F parm` lists `stats` on both modules.

## Tree B: 7.1.12-class (omarchy-linux ane-driver-aurora)

Source `~/src/omarchy-linux` @ origin/ane-driver-aurora
(9f99ebd9c6de) in a git worktree; config from the AneUapiClean receipt
(config.used); `ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-`,
O=/var/tmp/ane-stats-k12, gcc 12.2.0.

Two deviations from a bare modules_prepare, both recorded:
- `modules_prepare` alone leaves no symbol table, so modpost of
  external modules fails with undefined symbols. A `make vmlinux` run
  (rc=0) produces the symdump; this kbuild names it `vmlinux.symvers`
  and external modpost reads `Module.symvers`, so a symlink bridges
  the two names (same generator rule, same content).
- The M2 tree needed no such step; it is a complete build tree.

```
make -C <k12> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/ane-stats-k12 M=<abs>/ane W=1 ANE_VERSION=0.4.0-g836b8ac modules
make -C <k12> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/ane-stats-k12 M=<abs>/ane/t6021 W=1 ANE_VERSION=0.4.0-g836b8ac modules
```

kernel.release `7.1.12-ARCH+`.

| module | sha256 | vermagic | version |
|---|---|---|---|
| ane.ko | `323dc389cff8daf4bf63d2a6b9838406c27d621a04dc4e9152604fa6816f4051` | `7.1.12-ARCH+ SMP preempt mod_unload aarch64` | 0.4.0-g836b8ac |
| ane_t6021.ko | `355d16e99466bfc9f1b24bca60a03432cdfa2f4fea771d6cd2436113f7fb4a24` | `7.1.12-ARCH+ SMP preempt mod_unload aarch64` | 0.4.0-g836b8ac |

Baselines (daa7447, same flags): ane.ko
`88ec49cf63c90629667cd0ecf8e008bd6c7dccf4cc96c5de71ca6fdffe3f114b`,
ane_t6021.ko
`0a9e8d5aabdcad88fecd95ffdab0d43342b3827842ba73bba321f192a33462bf`.

W=1 result: the ane fix build has zero warnings (the baseline's
pre-existing `-Wformat-truncation` in ane_tm.c:522 disappears with the
stats code motion). The t6021 fix warnings are exactly the baseline
set. `stats` parm present in both modules.

## Host gates and consumer check

- `make -C tools check` PASS; `make all` rc=0; `pytest -q tests tools`
  27 passed, 1 skipped (same shape as the pre-stats baseline).
- coreglass `python3 -m unittest discover -s tests`: 14 tests OK.
- The sysfs show callback emits `busy_ns <u64>\njobs <u64>\n`; a file
  with those exact bytes fed to `coreglass.sampler.read_kv` parses to
  `{'busy_ns': 123456789, 'jobs': 42}` for sample bytes.

## For the hardware lane

```
# M1 family (ane.ko), after installing the .ko built above:
sudo insmod ane.ko stats=1        # default; files created at probe
sudo insmod ane.ko stats=0        # no files, hot path one branch
# M2 (ane_t6021.ko):
sudo insmod ane_t6021.ko stats=1
cat /sys/class/accel/accel0/device/ane_stats      # 0444, no root needed
sudo cat /sys/kernel/debug/ane/ane_timeline       # ane.ko
sudo cat /sys/kernel/debug/ane_t6021/ane_timeline # ane_t6021.ko
```

Reload is needed to flip `stats`; it is read once at probe. The .ko
files and full build logs live in the private lab notebook under
artifacts/AneStatsBuild2/ (m2-31-tree, k12-aurora-tree).

## Not verified here

- No insmod, no bind, no sysfs/debugfs read on real silicon.
- BTF (.ko BTF tags) is off in tree A for lack of pahole in the
  chroot; tree B builds carry BTF.
- Warning comparison covers these two kernel versions and configs
  only.
