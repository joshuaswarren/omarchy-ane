# 2026-10-01 — T8112 capture kit, validated by replay on the T6021 captures

Host work on an x86_64 container. No Mac was used: no boot, no USB, no MMIO.
The kit is [tools/t8112-kit](../../tools/t8112-kit/README.md).

## Result

| Check | Result |
| --- | --- |
| `collect-m1n1.py --replay` + `ingest.py` on the 17 pre-Linux T6021 captures (2026-09-27) | 17 of 17 pass. 176 to 178 bytes differ from the archive (the stack guard's random bytes sometimes equal the archive bytes), all in iBoot fields; 0 bytes outside them. Each gives `RTK_soc` 0x6021, revision 0x11, cpu 0x285000000, wrapper 0x285400000, DATA base 0x100000c4000 (entry IOVA 0x10000000000), 24 tunables records, PMU base 0x28e084008 |
| The same 17, against the driver | each matches `ANE_T602X_SOC_REVISION`, `ane_t602x_asc_tunables` (the generated C rows equal the header rows), `ANE_T6021_PMU_PA` (0x28e084000) and the `t602x-ane.dtsi` alias IOVA (0x10000000000) |
| Live path, fake proxy (13.5 j413 IPSW ADT with injected `segment-ranges`, a patched bia preload, m1n1 proxyclient a878095) | ANE off: only the nine PS words are read, no engine or DART read. ANE on: RVBAR, CPU_STATUS and 3 × 3 DART reads. The proxy has no write method. `ingest.py` accepts the result (3 or 4 entry sources agree) and refuses an RVBAR that disagrees. A `segment-ranges` address outside DRAM is refused before any read |
| Generated diffs | `git apply` clean on 9c1c932. The patched overlay compiles (dtc 1.7.2-g53373d13) and applies to `t8112-j413.dtb`: mailbox interrupts `0 520 4 0 1000 4`, ane `mboxes` = the mailbox phandle. `tools/h14_boot_regression.c` builds with the patched header: 112 checks pass |
| `omarchy-ane-firmware-fetch` on a fake `apple,t8112` root, live CDN | installed `h14_ane_fw_bia_j4xx.macho`, 4938552 B, sha256 `af587dfa96b1e01d2b2e0f9f776e5dbefebb44c7968b86fff7d2b19c4cbef0dd`, 1.3 s |
| `tools/test_t8112_kit.py`, `test_ane_firmware_fetch.py`, `test_ane_m2.py`, `test_ane_dt.py`, `test_ane_overlays.py` | ok |
| `collect-macos.sh` | `bash -n` and shellcheck clean; run on Linux with stand-in `ioreg`, `sysctl`, `sw_vers`, `system_profiler`, `shasum` (the T6021 macOS 27 `segment-ranges` bytes): `ingest.py --macos` gives entry 0x10000000000; the serial number, hardware UUID and `unique-chip-id` lines are not in the tarball. Not run on macOS |
| Device run | NOT RUN. No T8112 is in the lab |

## The pmgr page comes from the image

Item 3 of the T8112 receipt needs no device. Both 13.5 images build the
PMU base with one `mov`/`movk`/`movk` sequence in five places
(capstone 5.0.7; symbols from each payload's `LC_SYMTAB`). The two
`SetPMUBaseAddress` bodies have the same instructions apart from the constant
and the addresses. Each stores the constant when the host's base argument is
not zero; it only logs the argument.

| Function | selene (T602x) vm | value | bia (T8112) vm | value |
| --- | --- | --- | --- | --- |
| `CPowerControlServiceAneH14::SetPMUBaseAddress`+0x6c (`str x8, [x19, #0x58]` after it) | 0x62700 | 0x28e084008 | 0x621e8 | 0x23b70c010 |
| `PowerUp`+0x24 | 0x627d4 | 0x28e084008 | 0x622bc | 0x23b70c010 |
| `GetPowerStatus`+0x20, +0x44 | 0x62abc, 0x62ae0 | 0x28e084008 | 0x625a4, 0x625c8 | 0x23b70c010 |
| `PowerDown`+0x2c | 0x63038 | 0x28e084008 | 0x62b20 | 0x23b70c010 |

0x28e084008 is the T6021 `ANE_TD` power state. The T6021 firmware programs the
power states through its DART at IOVA = PA, and without that page mapped it
faults and halts (`ane/t6021/ane_t6021_fwload.c`, `ANE_T6021_PMU_PA`
0x28e084000). 0x23b70c010 is the T8112 `ANE_TD` power state. The page is
0x23b70c000, inside the DAPF range 0x23b70c000..0x23b70c03b. `GetPowerStatus`
reads the word at base + 8 × domain id.

## What the macOS route gives

The T6021 macOS 27 `ioreg` of `ane0` (ane-linux-experiments d3dc1aef,
`receipts/2026-09-23-m2-macos-denominator/m2-macos-window/ane-evidence/ane0-devicetree.txt`)
has `segment-ranges`: TEXT phys 0x1000092c000, iova 0, remap 0x10000000000,
size 0xe8000; DATA phys 0x1000150c000, iova 0xe8000, remap 0x100000e8000,
size 0x284000. The sizes are the macOS 27 image, but the TEXT remap equals the
13.5-stub m1n1 ADT remap and the latched RVBAR entry (0x10000000000). So the
macOS route gives the entry IOVA. It gives no tunables (the node has none) and
no preload. `/arm-io` `chip-revision` is 0x11 on T6001 (macOS "B1"), and the
T6001 preload `RCOS` is also 0x11; no T6021 record shows `chip-revision`, so
it stays a candidate for `RTK_soc_revision`.

## How

```
tools/t8112-kit/collect-m1n1.py --replay CAPTURE/bytes --soc 0x6021 \
    --archive t602x_ane0_fw_selene_rc4x.macho --out OUT
tools/t8112-kit/ingest.py OUT --archive t602x_ane0_fw_selene_rc4x.macho
ANE_KIT_BIA=h14_ane_fw_bia_j4xx.macho ANE_KIT_ADT=DeviceTree.j413ap.adt \
ANE_KIT_M1N1=m1n1/proxyclient ANE_KIT_SELENE=t602x_ane0_fw_selene_rc4x.macho \
ANE_KIT_CAPTURES='preload-*/bytes' python3 tools/test_t8112_kit.py
```

The private notebook entry `entries/T8112Kit/20261001T224856Z-ct-t8112-kit.md`
keeps the replay outputs, the logs and the hashes.
