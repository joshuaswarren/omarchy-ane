# T8112 (M2) ANE capture kit

The `ane_t6021` driver can run the T8112 ANE firmware when it knows the
values that iBoot writes into the firmware at boot. Apple's IPSW files do not
contain them ([receipt](../../receipts/2026-10-01-t8112-ane/README.md)). This
kit gets them from one T8112 Mac and turns them into driver and overlay
diffs. All device access is read-only.

| Item | macOS only (`collect-macos.sh`) | m1n1 proxy (`collect-m1n1.py`) |
| --- | --- | --- |
| 1a. `RTK_soc_revision` | A candidate only: `/arm-io` `chip-revision`. On T6001 both values are 0x11. No record links them on T6021. | Yes: the `RCOS` record of the preload. The same run saves `chip-revision`, to test the link. |
| 1b. ASC tunables block | No. iBoot writes it from its own table. The live macOS ADT has no ANE tunables. | Yes: `__rtk_platform_asc_tunables_block` of the preload. |
| 2. Firmware entry IOVA | Yes: the ANE `segment-ranges` TEXT remap. On T6021, macOS 27 (`ioreg`) and the 13.5 stub (m1n1 ADT) give the same value, 0x10000000000. That value is also the latched RVBAR entry. | Yes, from two or three sources: the DATA base that iBoot writes into the preload, the ADT `segment-ranges`, and RVBAR (only when the ANE is powered). |
| 3. pmgr page that the firmware writes | Not needed. The image itself gives it: `CPowerControlServiceAneH14::SetPMUBaseAddress` stores 0x23b70c010 (the `ANE_TD` power state), so the page is 0x23b70c000. `ingest.py` reads it from the image bytes. | Same. |

So macOS alone gives item 2. Item 1 needs the m1n1 proxy, because the
values exist only in the firmware copy that iBoot preloads into DRAM. macOS
gives no access to that memory, and it boots its own firmware version, not
13.5.

## Who can run it

- The owner of a T8112 Mac: MacBook Air 13" (Mac14,2, j413), MacBook Air
  15" (Mac14,15, j415), Mac mini (Mac14,3, j473) or MacBook Pro 13"
  (Mac14,7, j493).
- macOS only: nothing else.
- m1n1 proxy: also a second computer (Linux or macOS) with Python 3.9 or
  later, and a USB-C cable. The owner must know the macOS administrator
  password, because the Asahi installer changes boot policy in recoveryOS.

**Safety.** The m1n1 route installs a second boot volume with Permissive
Security. This changes the policy of that volume only, not of macOS. The
installer shrinks the macOS APFS container. Make a backup first. Do not use
a Mac that you cannot erase and restore. The lab lead decides who runs the
m1n1 route.

## Route A: macOS only

On the T8112 Mac, from a checkout of this repository or a copy of
`collect-macos.sh`:

```sh
bash collect-macos.sh
```

It needs no `sudo` and no network. It writes
`t8112-macos-<model>-<UTC>.tar.gz` in the current directory. The tarball
holds the `ioreg -a` plists of the ANE, ANE DART and pmgr device-tree nodes,
four `/arm-io` and four `/chosen` properties, `sw_vers`, `sysctl hw.model`,
the model and chip lines of `system_profiler`, and `SHA256SUMS`. It does not
hold the serial number, the hardware UUID or `unique-chip-id`. Send it to the
lab.

## Route B: m1n1 proxy

### 1. Put m1n1 in proxy mode on the T8112, with the macOS 13.5 firmware

The driver pins the macOS 13.5 (22G74) firmware. iBoot preloads the firmware
of the boot volume's stub, so the stub must be 13.5. Use one of these:

- **No Asahi install yet.** In macOS, run `curl https://alx.sh | EXPERT=1 sh`.
  Answer `y` to "Enable expert mode?". Give the new volume the minimum
  space. For the OS, select "Tethered boot (m1n1, for development)". For "the
  macOS version to use for boot firmware", press Enter (the default, 13.5, on
  T8112). Follow the installer through its recoveryOS step. This m1n1 always
  starts in proxy mode.
- **Asahi Linux is installed.** In that Linux, `cat
  /proc/device-tree/chosen/asahi,os-fw-version` must print `13.5`. Then make
  Asahi the default boot volume, start recoveryOS (hold the power button,
  then Options), open Terminal, run `csrutil disable && nvram boot-args=-v`,
  select the Asahi volume, and shut down. At each boot, m1n1 now waits 5
  seconds for a USB host. To undo, run `csrutil enable` and `nvram -d
  boot-args` in recoveryOS for the same volume.

The Asahi documentation describes both routes:
<https://asahilinux.org/docs/sw/tethered-boot/>.

### 2. Prepare the second computer

```sh
pip3 install --user pyserial construct
git clone https://github.com/AsahiLinux/m1n1
git clone https://github.com/joshuaswarren/omarchy-ane
```

On Linux, install m1n1's `udev/80-m1n1.rules` or run the script as root. On a
macOS host, the device is `/dev/cu.usbmodem*`
(<https://asahilinux.org/docs/sw/tethered-boot-macos-host/>).

### 3. Run the capture

Connect the cable to a Thunderbolt port of the T8112. For the backdoor route,
run `m1n1/proxyclient/tools/picocom-sec.sh` first, so that m1n1 sees the host
during its 5 seconds. Start the T8112 into the m1n1 volume. Then:

```sh
cd omarchy-ane
M1N1DEVICE=/dev/ttyACM0 PYTHONPATH=$HOME/m1n1/proxyclient \
  python3 tools/t8112-kit/collect-m1n1.py \
  --archive $HOME/h14_ane_fw_bia_j4xx.macho \
  --out $HOME/t8112-result-$(date -u +%Y%m%dT%H%M%SZ)
```

When `--archive` does not exist, the script gets it first from Apple's CDN
with `packaging/omarchy-ane-firmware-fetch` (about 5 MB, SHA-256
`af587dfa…`). Exit 0 means that every byte that differs from the archive is
in a known iBoot field. When it is done, hold the T8112 power button to turn
it off. Start macOS from the startup picker.

Send the result directory to the lab privately. It holds Apple firmware
bytes, so do not publish it.

### What the script reads

- The ADT: `/arm-io/ane*`, `/arm-io/dart-ane*`, `/arm-io/pmgr` with their
  children, `/` `compatible` and `target-type`, `/arm-io` `compatible`,
  `chip-revision` and `fuse-revision`, `/chosen` `chip-id`, `board-id`,
  `firmware-version` and `system-firmware-version`.
- The pmgr power-state words 0x23b7004a8 (`ANE_SYS`) and 0x23b70c000..
  0x23b70c038 (`ANE_MPM`, `ANE_SYS_CPU`, `ANE_TD`, `ANE_BASE`,
  `ANE_SET1..4`).
- RVBAR (engine + 0x1050000), CPU_STATUS (engine + 0x1400048), and TCR, TTBR
  and ERROR of the three ANE DARTs, **only** when all of `ANE_SYS` and
  `ANE_SYS_CPU`..`ANE_SET4` read ACTUAL = TARGET = 0xf. An engine read while
  the islands are off hangs the SoC (it wedged the lab M2 twice).
- The preloaded firmware, from DRAM at the two ADT `segment-ranges`
  addresses, after a check that both ranges are in DRAM. This is how the lab
  captured the T6021 preload 17 times. It does not touch the engine.

It writes no register. It does not import `m1n1.setup`, because that module
also writes the PMU panic counter.

## Lab side

```sh
python3 tools/t8112-kit/ingest.py --macos t8112-macos-*.tar.gz
python3 tools/t8112-kit/ingest.py RESULT_DIR --archive h14_ane_fw_bia_j4xx.macho --send-empty-irq N
```

`ingest.py` checks `SHA256SUMS` and the JSON, decodes the segments again,
refuses differing bytes outside the iBoot fields, and requires that all
entry IOVA sources agree. For a T8112 result, it prints the per-SoC values for
`ane_t6021` and unified diffs for `ane/t6021/ane_fw_validate.h` (the chip
revision and the tunables table, in the T6021 table format) and for
`packaging/dt/t8112-ane.dts` (the mailbox node and `mboxes`). Apply them
with `git apply`. The send-empty line `N` is a lab choice: no Apple data names
one (T6021 uses the unused AIC2 line 1833). There is no `memory-region`:
`ane_t6021` runs the firmware from its own memory. The overlay stays
disabled until the driver binds `apple,t8112-ane`.

## Validation without a T8112

`collect-m1n1.py --replay` runs the same decode on a lab T6021 capture.
On the 17 pre-Linux captures, it reports only the iBoot fields, and
`ingest.py` finds the driver's T6021 constants: chip revision 0x11, the 24
tunables (byte for byte, in the same C rows), entry IOVA 0x10000000000 and PMU
page 0x28e084000. `tools/test_t8112_kit.py` checks the decoder and the diffs,
the live path against a fake proxy that has no write method, and the replay.
See [receipts/2026-10-01-t8112-kit](../../receipts/2026-10-01-t8112-kit/README.md).
