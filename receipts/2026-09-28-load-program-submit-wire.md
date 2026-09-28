# LOAD_PROGRAM mailbox submit wire (2026-09-28)

No device write. The M2 lane owns the insmod.

## What changed

`ane_rtclient_csne_cmd` copied an 8-byte header. `LOAD_PROGRAM` is 440
bytes (`0x1b8`). The helper now copies `len` bytes and doorbells
`cursor | (len << 24)`.

`csne_load_program=1` (default off) loads `apple/ane/load_program.bin`,
refuses a size other than `0x1b8` or an id other than `0x0200`, copies
the bytes to ring cursor `0x100`, and sends word `0x1b8000100`.

The blob comes from `ane-linux-experiments` `tools/h14_load_program.py`
`pack()`, branch `agent/h14-load-program`. The driver does not rebuild it.

## Wall

Device submit is not run here. Bypass in flight: set
`csne_load_program=1` on the M2 lane after the blob is installed at
`apple/ane/load_program.bin`. Missing or wrong-sized blob logs and
does not submit.

## Check

`python3 tools/h14_load_program.py` printed `h14_load_program: ok`.
Packed size `0x1b8`, id `0x0200`, doorbell word `0x1b8000100`.
