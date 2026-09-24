# T6021 (M2 Max) ANE bring-up: findings as of 2026-09-24

This is the canonical record of what the T6021 ANE work has proven, what it has
ruled out, and what is still open. Read it before starting any T6021 ANE
experiment. Update it when a finding changes, and delete lines that go stale.

## 1. Architecture: why the M2 needs firmware and the M1 does not

- T8103 (M1) and T6001 (M1 Max) run the ANE under Linux through the
  host-driven task manager (`ane_tm_enable`: TQ enables, push, poll IDLE).
  Linux never starts the ANE coprocessor (ASC) firmware on those chips.
- T6021 has no host TM path. The first host touch of the H13-style TM/TQ
  window external-aborts (two runs, 2026-09-18). The macOS ANE kext has zero
  TM/TQ sites for this chip. The firmware owns the TM.
- Therefore the T6021 ANE works under Linux only after its ASC firmware boots,
  sends the RTKit HELLO, and completes the handshake.
- eiln/ane, the upstream of this driver, never went past the M1. No public
  M2-family ANE Linux work exists (searched 2026-09-24).

## 2. Firmware version: match the version, not the installed macOS

- Under Linux, iBoot preloads the ANE firmware from the Asahi stub's macOS
  version, not from the main macOS install. On the test laptop the stub is
  **macOS 13.5 (22G74)** (`/sys/firmware/devicetree/base/chosen/asahi,os-fw-version`).
- Firmware: `Firmware/ane/t602x_ane0_fw_selene_rc4x.im4p` from the 13.5 IPSW
  (decompressed payload: Mach-O arm64, sha256 `a9c4b771...`).
- Kernelcache: the stub ESP holds `asahi/kernelcache.release.mac14j`. It is an
  IM4P (LZFSE), byte-identical to the IPSW member (sha256 `ea70fb77...`);
  the decompressed fileset is sha256 `9615a486...`. It contains
  `com.apple.driver.AppleH11ANEInterface`.
- Consequence: static diffs and hypervisor traces must use the 13.5
  kernelcache. A 26/27 kernelcache under m1n1 hv spins at `_start+0x10` on
  `[x1+8] == 0` (new entry ABI). The 13.5 kernelcache enters with the stock
  x0-only `hv_start`.

## 3. Firmware memory map (proven)

- The ANE DART power domain is **off at the iBoot handoff**: an m1n1 proxy
  read of the DART MMIO takes an L2C error. iBoot leaves no ANE DART mapping,
  so the kernel's apple-dart probe reset destroys nothing.
- The map comes from the ADT `/arm-io/ane` `segment-ranges`
  entries `{phys, iova, remap, size}`. The ASC sees the **remap** address.
  - T6021 entry 0 (TEXT): PA `0x10000848000` -> IOVA `0x10000000000`, size `0xc4000`.
  - T6021 entry 1 (DATA): PA `0x10001400000` -> IOVA `0x100000c4000`, size `0x438000`.
- The historic fault `NO PGD FOR IOVA 0x100000dca10` is DATA + `0x18a10`.
  It came from not mapping DATA at its remap IOVA. It was not a TEXT overrun.
  Check IOVA arithmetic against the table before you build a theory on it.
- Both ranges are reserved `no-map` `apple,asc-mem`. The preloaded image
  survives Linux boot (TEXT head reads `81 00 00 14`, `b +0x204`).
- PA `0x1000090c000-0x100009fbfff` is outside `/proc/iomem` and holds a second
  copy of the image head. It is not the TEXT tail.

## 4. Release sequence (proven on T6021 and T6001)

1. Gate: all eight ANE pmgr islands read ACTUAL = `0xf` (bits [7:4]).
2. Map both segment-ranges entries at their remap IOVAs, iBoot pages in place.
3. Set bit 0 of the I2A control register (engine + `0x1408114`).
4. Write CPU_CONTROL (engine + `0x1400044`) = 0, barrier, then `0x10`.
5. RVBAR (engine + `0x1050000`) is latched (bit 0 set, `0x10000000001`).
   Never write it.

Result on both chips: CPU_STATUS goes `0x2a -> 0x28`. The ASC runs.

## 5. The open blocker

After the release above, on **both** T6021 and T6001:

- no word ever arrives in the I2A outbox (it stays armed and empty),
- SCRATCH0-7 stay zero (the macOS H13 path polls SCRATCH7 after RUN),
- no fault latches on any of the three ANE DARTs, on any SID.

The firmware runs but stalls before RTKit init. Because both chips stall the
same way, the missing piece is shared and is not T6021-specific.

## 6. Ruled out (do not re-run without new evidence)

| Hypothesis | Evidence |
|---|---|
| Host-driven TM on T6021 | External abort on first touch; no kext TM sites |
| x1 = boot-args pointer for the 26/27 hv guest | x1 landed; cpu0 still spins at the same `cbz` |
| Kernel DART reset erases iBoot's map | The DART is powered off at handoff; there is no iBoot map |
| Unreserved TEXT tail | The fault IOVA is inside DATA; the gap holds an image-head copy |
| Wrong DART instance or SID | All three ANE DARTs read clean after RUN |
| Missing pre-RUN writes from the 26/27 kext | Every write is present, lawfully skipped, or provider-owned |
| pmgr `ps_ane_cpu` TARGET (kext `0x2e0 = 0xf`) | Already on; it is a pmgr write, not an engine write |
| PWGATE `0x28e09359c = 0` | Already reads 0 |
| `0x28e08c000 = 0x80000000` (first 13.5 trace write) | Applied, read back, no change |
| Firmware + legacy TM coexisting on T6001 | With the firmware running, the TM path stops serving jobs; a reboot restores it |

## 7. Hazards (each one wedged or reset a laptop)

- Reading the ANE engine window while the islands are off wedges the fabric.
  Gate every engine read on the pmgr ACTUAL words.
- The engine-window mirror of pmgr registers reads 0 while the pmgr window
  reads on. Trust the pmgr window. Never write through the engine mirror.
- Raw pmgr ps writes can reset the laptop. Use the kernel power-domain path.
- Never touch engine + `0x1010000` (CoreSight). Never use `/dev/mem`.
- On T6001, reading CPU_CONTROL is hostile; writing it is safe.
- A wedged M2 watchdog-resets in about 2 minutes. If it stays at the loader,
  reboot it over USB-C from the proxy host with the m1n1 proxy `p.reboot()`.

## 8. Tools and artifacts

- `tools/m2hv_diff.py`: diffs an m1n1 hv MMIO trace against the Linux
  baseline; prints the first missing write (`--selftest` built in).
- Power-gated observer module: refuses engine access unless every island is on.
- Pre-RUN write TSV and the 13.5/26 static diffs: ane-linux-experiments main.
- 13.5 hv trace (first run): 3075 writes, all power and DART bring-up. The
  kext's CPU start was not reached, so the run is too short to show the missing
  step.
- 13.5 IPSW members (kernelcache, ane0/ane1 firmware) are stored with
  SHA256SUMS in the fleet artifact store, not in git.

## 9. Next steps

1. Run a longer 13.5 hv trace that reaches the kext's CPU_CONTROL write and
   the first mailbox exchange. Diff it with `m2hv_diff.py`.
2. Reverse the 13.5 firmware reset path: find the first loop that waits on an
   external value (MMIO, SCRATCH, a DATA boot-args field, a mailbox bit).
3. Whichever answer comes first gets tested on T6001 as well, because the
   stall is shared.

## 10. Method lessons

- Use version-matched sources: the preloaded firmware decides the version.
- Verify IOVA and range arithmetic against the raw ADT table before you act.
- Take read-only measurements before writes, and ask one discriminating
  question per hardware run.
- A stall shared by two chips points at a shared missing step. Test it on the
  laptop that recovers cheaply.
