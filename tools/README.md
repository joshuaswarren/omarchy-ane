# tools

Host-side helpers: `ane-run`, `ane-session` and `ane-selfcheck`, with their tests, run by `make check`.

The check builds an AddressSanitizer binary; on hosts with `vm.mmap_rnd_bits` above 28 (every Proxmox node in this fleet sets 32) ASan crashes at startup with exit 139 before any test runs - intermittent, so one green run proves little. The verified workaround that needs no root in a container: run the check under `setarch -R make check`; otherwise lower the sysctl to 28 or use another machine.
