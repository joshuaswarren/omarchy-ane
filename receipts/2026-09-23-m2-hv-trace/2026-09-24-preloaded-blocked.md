# Preloaded-bytes compare blocked — 2026-09-24 ~10:30 UTC

Reserved-window reader (/dev/ane_phys, ioremap_np + copy_to_user):
- DATA 0x1000150c000 (inside reserved ane-firmware@10001400000): reads 64 zero bytes.
- TEXT 0x1000092c000 (System RAM, reclaimed): read traps EPERM.
- llseek reports full 64-bit position, then read behaves inconsistently.

TEXT head is unrecoverable from Linux (reclaimed System RAM). DATA zeros
are either cleared memory or a swallowed read. Compare cannot run via
this vehicle. Options: proxy memread on next proxy boot, or mapping test
(alias verified reserved windows + RUN retry).
