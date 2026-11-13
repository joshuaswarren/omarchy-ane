# tools

Host-side helpers: `ane-run`, `ane-session` and `ane-selfcheck`, with their tests, run by `make check`.

The check builds an AddressSanitizer binary; on hosts with `vm.mmap_rnd_bits` above 28 (every Proxmox node in this fleet sets 32) ASan crashes at startup with exit 139 before any test runs - intermittent, so one green run proves little. The verified workaround that needs no root in a container: run the check under `setarch -R make check`; otherwise lower the sysctl to 28 or use another machine.

## `ANE_CPU_FLOOR` -- the opt-in CPU clock floor

With `ANE_CPU_FLOOR=<kHz>` set, `ane-run` holds the t6021 P cluster's minimum
clock (`cpu4/cpufreq/scaling_min_freq`, cpus 4-7) at `<kHz>` while it serves
and restores the value it found on every exit path: normal exit, SIGINT,
SIGTERM (an inherited SIG_IGN, for example from nohup, is put back untouched
and the floor survives the delivery), SIGKILL and crashes through a forked
watchdog child, and a stale state file under `/run/ane-run` that the next run
reconciles before it raises again. The watchdog restores only once the dying
holder is really gone: the dying process releases its file descriptors (the
watchdog's pipe among them) before it becomes a zombie, so one reconcile at
pipe close can still see it alive and would leave the floor raised. Concurrent
instances share the raise under a lock; the first raises and the last restores.
`ANE_CPU_FLOOR=1968000` is the value the measurement rows use. With the
variable unset, zero, or not a number -- trailing junk included,
`1968000kHz` is not a number -- nothing is read or written under sysfs.
The floor needs write access to the sysfs file (a run under sudo or a root
service); the package grants and installs no such privilege. On any SoC
whose device tree does not name t6021, or without policy4, it is a silent
no-op even when set. policy4 is the first P cluster: the M2 Max's second P
cluster (policy8, cpus 8-11) is not floored, so work the scheduler places
there runs ungated.
