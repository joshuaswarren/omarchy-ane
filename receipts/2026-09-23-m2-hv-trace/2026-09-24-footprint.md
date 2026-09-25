# Firmware footprint: none — 2026-09-24 ~15:20 UTC

## Snapshots (reserved SEG1 DATA, 0x430000 readable bytes)
- Before RUN and after RUN + 60 s poll: byte-identical (cmp clean).
- Snapshots: /tmp/m2kstart/data_before.bin + data_after.bin.

## Verdict
Firmware stores nothing after RUN: faults before first store
(fetch or TEXT level). EPERM earlier was a read-size artifact of
the snap script, not a bind-held refusal; chunked reads work.
