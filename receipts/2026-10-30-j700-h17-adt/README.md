# J700 H17 stage 0: live boot ADT

2026-10-30. MacBook Neo J700/T8140, Linux `7.2.11-6-1-ARCH`.
Repository baseline `063ae8e`; aurora device-tree source
`68df87370abac4e227936552dd9be2252972e551`. The running tree has
`/soc/ane@400000000` and three `iommu@4018*` nodes, all `disabled`;
`omarchy-ane-probe` reported `reachability.verdict=not-exposed` and
`driver_loaded=false`. No ANE MMIO, power-domain request or task submission
occurred in this receipt.

`make -C ane/h17` against the running kernel's headers exited 0. GCC 16
reported warnings in kernel headers because that kernel was built with
Clang 18; there were no source warnings from `ane_h17_adt_probe.c` in the
final build. `make -C ane/h15 check` exited 0 (`test: ane_h15_adt — ok`).

The first stage 0 attempt found a 64-byte `segment-ranges` property but
refused it because the H15 helper preserves the flags in the upper 32
bits of the TEXT size word. The corrected T8140 probe splits the flag
and byte count. Its built module SHA-256 was
`3f76396d38a3c03f2ae6f73a727474908040b960825ed692c7e5cbc354738f08`
for the loaded build. The next `insmod ... optin=1`
exited 0 and printed:

```text
ane_h17_adt_probe segment-word[0]=0x10000774000
ane_h17_adt_probe segment-word[1]=0x0
ane_h17_adt_probe segment-word[2]=0x10000000000
ane_h17_adt_probe segment-word[3]=0x1000c0000
ane_h17_adt_probe segment-word[4]=0x100019ac000
ane_h17_adt_probe segment-word[5]=0xc0000
ane_h17_adt_probe segment-word[6]=0x100000c0000
ane_h17_adt_probe segment-word[7]=0x2ac000
ane_h17_adt_probe TEXT flags=0x1 DATA flags=0x0
ane_h17_adt_probe TEXT phys=0x10000774000 iova=0x10000000000 size=0xc0000
ane_h17_adt_probe DATA phys=0x100019ac000 iova=0x100000c0000 size=0x2ac000
ane_h17_adt_probe RESULT stage=0 soc=t8140 verdict=PASS reason=segments
```

`sudo rmmod ane_h17_adt_probe` exited 0, and `lsmod` showed no probe
module afterward. This establishes that iBoot left ANE segment metadata
in the live ADT. It does not establish that the preload bytes are valid,
that the ANE can power up, or that inference runs.

## Stage 1: reserved-RAM preload read

The first `inspect_preload=1` attempt refused `reason=outside-dram` before
mapping a segment. Linux's `memory@10000000000/reg` begins at
`0x10003438000` on this boot, after the iBoot-reserved firmware area. The
probe was corrected to admit only ADT-named ranges in the first 64 MiB of
the J700 DRAM aperture that end before the first Linux RAM range. The
corrected build had SHA-256
`dc4f00b7a3235a906654182ada4ecaac6b463fe9fc65cebf98fbe3f33c1f6eae`
when loaded. `sudo insmod ... optin=1 inspect_preload=1` exited 0 and
printed:

```text
ane_h17_adt_probe preload segment=TEXT sha256=cda09ec1aee68b88b47edc0eebc810452532a855563fbd650b95e2846c345310 nonzero=1
ane_h17_adt_probe preload segment=DATA sha256=ceea79eadab680c3a849dc93442a8d5bec6d43b5e304fa88673e89969c721f5c nonzero=1
ane_h17_adt_probe RESULT stage=1 soc=t8140 verdict=PASS reason=preload-readable
```

`sudo rmmod ane_h17_adt_probe` exited 0. This proves the two named ranges
hold readable, nonzero bytes on this boot. It does not compare them with
the pinned firmware payload or prove that ANE hardware can run them.

After the empty-segment refusal was added, the final module built without
source warnings (SHA-256
`0cebd48a202ed52689bf29f570212568abfb746512e48447783b4d9c882bbaf0`).
The same stage 0 and stage 1 outputs passed again, with the same two
segment digests. `sudo rmmod` exited 0. An `insmod` without `optin=1`
then refused with `reason=board-or-optin`, and no module remained loaded.

## Stage 2: the host's 26.6.2 payload and preload diff

The host's `asahi,os-fw-version` is 26.6.2. The earlier T8140 pin in
`data/ane-soc/t8140.json` is for macOS 27.0 and must not be used on this
boot. A range request to Apple's
[`UniversalMac_26.6.2_25G83_Restore.ipsw`](https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw)
extracted `Firmware/ane/h17_ane_fw_theia_d9x.im4p`. The ZIP member's CRC
passed. The IM4P is 1,572,889 bytes, SHA-256
`1694f0dd3157dc887e58cdb09b01c874fcf2ec3b5c201f4a08d6564afb7f669c`;
its raw Mach-O payload is 1,572,864 bytes, SHA-256
`32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c`.
This extraction did not download or hash the entire IPSW archive.

The pinned Mach-O has TEXT vm `0+0xc0000`, file `0x4000+0xc0000`, DATA vm
`0xc0000+0x2ac000`, file `0xc4000+0xbc000`. Its `ce_env` section is at vm
`0xb8280+0x4000`; `_rtk_patchbay` at `0xcbdc8+0x261`; and
`_rtk_tunables` at `0xde030+0x6a0`. These are file-layout facts from the
26.6.2 payload, not observed register behaviour. The source records them
in `ane/h17/ane_h17_fw.h` and the fetcher pins this file only for a
26.6.2 stub.

`python3 tools/test_ane_firmware_fetch.py` passed. On this host,
`sudo python3 packaging/omarchy-ane-firmware-fetch` installed the pinned
payload under `/usr/lib/firmware/apple/ane/`, and `--check` passed.
The stage 2 module build was W=1-clean for its own source, SHA-256
`fcfe45553bf650b9070ddcc096c20ef491da56a74126bbe613b8fee620a761b9`.
The `compare_preload=1` run exited 0 and reported:

```text
ane_h17_adt_probe firmware name=apple/ane/h17_ane_fw_theia_d9x.macho bytes=1572864 sha256=32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c pin=PASS
ane_h17_adt_probe comparison segment=TEXT differing_bytes=1730 allowed=1730 forbidden=0 first=0xb8280 last=0xb8ff0 first_forbidden=0x0 file_bytes=0xc0000
ane_h17_adt_probe comparison segment=DATA differing_bytes=336 allowed=336 forbidden=0 first=0xbdf0 last=0x1e650 first_forbidden=0x0 file_bytes=0xbc000
ane_h17_adt_probe RESULT stage=2 soc=t8140 verdict=PASS reason=preload-whitelist
```

`sudo rmmod ane_h17_adt_probe` exited 0. The code and zero-fill matched
the 26.6.2 file outside the three named environment sections. This result
does not prove that the patched values are correct, that ANE power is
available, or that any program executed.
