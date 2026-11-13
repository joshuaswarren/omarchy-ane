# tools

Host-side helpers: `ane-run`, `ane-session` and `ane-selfcheck`, with their tests, run by `make check`.

The check builds an AddressSanitizer binary; on hosts with `vm.mmap_rnd_bits` above 28 (every Proxmox node in this fleet sets 32) ASan crashes at startup with exit 139 before any test runs - intermittent, so one green run proves little. The verified workaround that needs no root in a container: run the check under `setarch -R make check`; otherwise lower the sysctl to 28 or use another machine.

## `ANE_CPU_FLOOR` -- the opt-in CPU clock floor

With `ANE_CPU_FLOOR=<kHz>` set, `ane-run` holds the t6021 P cluster's minimum
clock (`cpu4/cpufreq/scaling_min_freq`, cpus 4-7) at `<kHz>` while it serves
and restores the value it found on every exit path: normal exit; SIGINT,
SIGTERM, SIGHUP, SIGQUIT, SIGPIPE and SIGALRM through a chained handler that
gives the signal back, so the exit status still says how the run died (an
inherited SIG_IGN, for example from a background job, is put back untouched
and the floor survives the delivery; the restore takes the state lock
non-blocking with a bounded retry, because the frame the signal interrupted
may hold the lock itself, and if it never frees, only a SIG_DFL disposition
re-raises and dies by the signal, while the watchdog restores at death);
SIGKILL and crashes through a forked watchdog child; and a stale state file
under `/run/ane-run` that the next run reconciles before it raises again.
The watchdog runs detached in its own session with SIGHUP, SIGINT, SIGQUIT
and SIGTERM ignored, so a group kill that takes the run (systemd's
KillMode, the OOM killer, a pkill sweep) leaves the watchdog alive to
restore - that closes issue #413. What still strands the floor until the
next `ANE_CPU_FLOOR` run reconciles it: a SIGKILL aimed at the watchdog
itself, the OOM killer taking the watchdog, or a begin whose fork failed. The watchdog restores only once the dying
holder is really gone: the dying process releases its file descriptors (the
watchdog's pipe among them) before it becomes a zombie, so one reconcile at
pipe close can still see it alive and would leave the floor raised. A holder
counts as dead from `/proc/<pid>/stat` alone - gone, zombie, or a start time
that no longer matches - because `kill()` alone answers for a pid reused by
another user. Concurrent
instances share the raise under a lock; the first raises and the last
restores. No begin ever lowers the file: a fresh raise sits out when the file
already sits at or above the floor, a joiner raises only above the file's
current content (another holder's raise, or an administrator change, wins),
and a stale undo writes back exactly the recorded saved value.
`ANE_CPU_FLOOR=1968000` is the value the measurement rows use. With the
variable unset, zero, or not a number -- trailing junk included,
`1968000kHz` is not a number -- nothing is read or written under sysfs.
The floor needs write access to the sysfs file (a run under sudo or a root
service); the package grants and installs no such privilege. On any SoC
whose device tree does not name t6021, or without policy4, it is a silent
no-op even when set. policy4 is the first P cluster: the M2 Max's second P
cluster (policy8, cpus 8-11) is not floored, so work the scheduler places
there runs ungated. The host-test overrides (`ANE_RUN_CPUFREQ_ROOT`,
`ANE_RUN_STATE_DIR`) are compiled out of the production `ane-run`: it is
built without `-DANE_CPU_FLOOR_TESTING` and ignores both variables, so even
a run with a preserved environment (sudo -E, env_keep) cannot have its
sysfs writes or state directory pointed anywhere but the real paths; only
the test binaries honor them.
