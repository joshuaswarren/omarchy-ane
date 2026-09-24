# Aliasing closed, pre-RUN list exhausted — 2026-09-24 ~14:45 UTC

## Resolution
Kext validated writes target the provider window (reads on), not the
engine-window mirror (reads 0x00, leave alone). No missing power write
behind any TSV row.

## Pre-RUN ledger (all rows closed, zero writes)
- genpd/VENC: made. RVBAR: lawful skip. PWGATE: already on.
- ps rows: provider-owned, ACTUAL on. Mailbox/SCRATCH-publish/RVBAR/
  boot-args: confirmed absent both sides.

## Next
Firmware-side fetch: hv trace window.
