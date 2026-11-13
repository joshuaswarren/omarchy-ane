# tools

Host-side helpers: `ane-run`, `ane-session` and `ane-selfcheck`, with their tests, run by `make check`.

The check builds an AddressSanitizer binary; on hosts with `vm.mmap_rnd_bits` above 28 (every Proxmox node in this fleet sets 32) ASan crashes at startup with exit 139 before any test runs - intermittent, so one green run proves little. The verified workaround that needs no root in a container: run the check under `setarch -R make check`; otherwise lower the sysctl to 28 or use another machine.

## `ANE_CPU_FLOOR` -- the opt-in CPU clock floor

With `ANE_CPU_FLOOR=<kHz>` set, `ane-run` holds the t6021 P cluster's minimum
clock (`cpu4/cpufreq/scaling_min_freq`, cpus 4-7) at `<kHz>` while it serves
and restores the value it found on every exit path: normal exit, SIGINT,
SIGTERM, SIGKILL and crashes through a forked watchdog child, and a stale
state file under `/run/ane-run` that the next run reconciles before it raises
again. Concurrent instances share the raise under a lock; the first raises
and the last restores. `ANE_CPU_FLOOR=1968000` is the value the measurement
rows use. With the variable unset nothing is read or written under sysfs.
The floor needs write access to the sysfs file (a run under sudo or a root
service); the package grants and installs no such privilege. On any SoC
whose device tree does not name t6021, or without policy4, it is a silent
no-op even when set.
