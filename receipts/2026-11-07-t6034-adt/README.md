# T6034 boot ADT, 2026-11-07

The raw ADT (`adt.bin`, 573,440 B, sha256 in `SHA256SUMS`) is not in this
repository. It carries per-device identifiers (serial numbers, ECID, MAC
addresses, boot nonce), so it stays in a private lab store. `segments.txt`
is the decoded `ane0` segment table and `capture.txt` is the read-only
h15-capture.sh output; neither carries an identifier.

Reproduce: `python3 ane/h15/h15-segments.py adt.bin` against a dump from the
m1n1 `adt` phram (`/dev/mtdN` named `adt`).
