# tools

Host-side helpers: `ane-run`, `ane-session`, `ane-selfcheck`, `ane-prog-pack` and their tests, run by `make check`.

The check builds an AddressSanitizer binary; on hosts with `vm.mmap_rnd_bits` above 28 (every Proxmox node in this fleet sets 32) ASan crashes at startup with exit 139 before any test runs - lower the sysctl to 28 or run the check on another machine.
