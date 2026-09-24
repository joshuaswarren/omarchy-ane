# Proto-prep module attempt — 2026-09-24 ~11:30 UTC

## Build
On-box rebuild from omarchy-ane-ff0aeb8 tarball (/var/tmp/ane-proto),
mailbox bound, rtclient fw_load=1 fw_start=1 fw_start_venc_gates=1
plus the running tree used fw_alias_reserved=1 — but that option does
NOT exist in ff0aeb8, so this run used the staged-DMA alias.

## Outcome
- VENC all 0x3ff, rvbar skip, RUN, full poll, no READY, HELD, alive.
- Probe items 1-5 unreached (all gated on DONE). Module builds/binds clean.
- Next: hv trace with verified trigger.
