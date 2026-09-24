# SCRATCH6 answer — 2026-09-24 ~11:35 UTC

## Question: did the proto-prep run write SCRATCH6=0 before RUN?
No. Contract source ane_t6021_boot.h:572-578 (inherited by ff0aeb8):
P1 S1 writes SCRATCH6=1 (legacy ChMan/MBI select) before RUN. The run
above also bound the older /var/tmp/ane-rtclient3 tree, not the ff0aeb8
tree at /var/tmp/ane-proto — so neither the SCRATCH6=0 select nor the
SCRATCH3 ack was exercised. New-contract test still open.

## Committed: fw_alias_reserved option (ane_t6021_fwload.c)
Reserved SEG0/SEGi phys at entry IOVAs vs staged-DMA alias.
