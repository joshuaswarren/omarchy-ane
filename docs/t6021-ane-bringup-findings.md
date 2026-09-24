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
2. Map both segment-ranges entries at their remap IOVAs, iBoot pages in
   place. T6021 only; the T6001 runs were unmapped.
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

On T6021 the firmware also stores nothing: the 0x430000 readable bytes of
DATA are byte-identical before and after release. With both segments
mapped `IOMMU_CACHE`, the leaf PTE for IOVA `0x10000000000` is
`0x000fff1000084801` (valid, NO_CACHE clear, PA `0x10000848000`), and the
stall is unchanged over 60 s.

## 6. Ruled out (do not re-run without new evidence)

| Hypothesis | Evidence |
|---|---|
| Host-driven TM on T6021 | External abort on first touch; no kext TM sites |
| x1 = boot-args pointer for the 26/27 hv guest | x1 landed; cpu0 still spins at the same `cbz` |
| Kernel DART reset erases iBoot's map | The DART is powered off at handoff; there is no iBoot map |
| Unreserved TEXT tail | The fault IOVA is inside DATA; the gap holds an image-head copy |
| Wrong DART instance on SID 0 | All three instances have TCR0 `0x9` and the same TTBR0; the PTE resolves |
| Uncached firmware pages, or SID 15 not bypassed (fetch stream refused) | First release after a reboot with the TEXT/DATA map `IOMMU_CACHE` (leaf PTE bit 1 clear) and TCR15 `0x2` on all three instances: stall unchanged over 60 s (§12) |
| SID-0 stream enable | dart-ane0 ENABLE already `0xffff`; stall unchanged over 60 s |
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
- Never touch engine + `0x1010000` (CoreSight) except by the unlock order
  in §11. On T6021 from Linux it reads zero (see §12). Never use
  `/dev/mem`.
- On T6001, reading CPU_CONTROL is hostile; writing it is safe.
- A wedged M2 watchdog-resets in about 2 minutes. If it stays at the loader,
  reboot it over USB-C from the proxy host with the m1n1 proxy `p.reboot()`.

## 8. Tools and artifacts

- `tools/m2hv_diff.py`: diffs an m1n1 hv MMIO trace against the Linux
  baseline; prints the first missing write (`--selftest` built in).
- 13.5 hv traces (three runs, up to 3075 writes): all power and DART
  bring-up, then an ISP power-down walk and no CPU_CONTROL, SCRATCH, RVBAR
  or mailbox write. AneStaticStart proved why: the 13.5 kext starts the ASC
  lazily, only when a user client (aned/CoreML) powers it on, so a
  kernel-only guest never issues the start sequence. A longer kernel-only
  trace cannot capture the ASC start; reaching it needs a macOS 13.5
  userspace that opens the ANE (an aned/CoreML inference workload in the
  guest). `tools/m2hv_replay-trace-135.txt` (beb39fa): all 151 macOS writes
  to ANE engine/DART/pmgr registers from trace-135, with per-write flags
  for ps-off/SET-window/hook and the Linux-same mark.
- 13.5 IPSW members (kernelcache, ane0/ane1 firmware) are stored with
  SHA256SUMS in the fleet artifact store, not in git.

## 9. Next steps

1. (SUPERSEDED) Run a longer 13.5 hv trace that reaches the kext's
   CPU_CONTROL write: the kernel-only guest never issues it (lazy start),
   so longer runs of the same guest add nothing. What would work instead:
   a 13.5 guest whose userspace opens the ANE (aned/CoreML workload).
2. OPEN, strongest remaining pre-RUN candidate: the kext writes
   set+0x12cc <- 3 and set+0x13cc <- 0 before ANE_Init. Linux never makes
   those writes. The base is bound (§12): phys `0x28e08d2cc` / `0x28e08d3cc`
   read `0` / `0`, so the `3` is missing. Test it on a fresh boot, before
   the first release, with readback.
3. Reverse the 13.5 firmware reset path: find the first loop that waits on an
   external value (MMIO, SCRATCH, a DATA boot-args field, a mailbox bit).
4. Whichever answer comes first gets tested on T6001 as well, because the
   stall is shared.

## 10. Method lessons

- Use version-matched sources: the preloaded firmware decides the version.
- Verify IOVA and range arithmetic against the raw ADT table before you act.
- Take read-only measurements before writes, and ask one discriminating
  question per hardware run.
- A stall shared by two chips points at a shared missing step. Test it on the
  laptop that recovers cheaply.
- A hypervisor trace of a kernel driver only captures what the kernel does.
  A lazily started coprocessor needs its userspace trigger inside the guest,
  or the trace ends at device bring-up forever.
- One release per boot. CPU_CONTROL = 0 does not stop a released T6021
  core, and the vector handler is `b .`, so every setup change must be in
  place before the first release after a reboot. CPU_STATUS `0x2a` before
  the release proves a core that has not run yet.

## 11. T6001 core state, read via CoreSight (2026-09-24)

On T6001 the stalled core was halted through the Apple DBGWRAP register
(engine+0x1040000) and read through the external debug block
(engine+0x1010000). That block is readable on T6001 once the OS lock is
cleared; reading DBGDTRRX (engine+0x1010080) while the lock is set resets
the machine. A clean read, taken before any debug instruction stuffing,
gives ESR_EL1 = 0x86000010: EC 0x21, an Instruction Abort from the
current level, with fault status 0x10, a synchronous external abort not
on a translation table walk. FAR_EL1 and ELR_EL1 are both 0x10000a54200,
which is VBAR_EL1+0x200, and the word there is `b .` (0x14000000). The
core's instruction fetch of its own exception vector external-aborts, so
the handler never runs and the core faults on that fetch at every entry.
SCTLR_EL1.M = 0, so it is not a translation fault, and the CPU reads the
same word fine, so the abort is specific to the core's fetch. The
firmware did complete its EL3 to EL1 reset first (VBAR_EL1 reads back the
image base its prologue writes at 0x10000a54234). The ISP-style warm
reset (EDPRCR = 2) does not change the stall. Detail:
receipts/2026-09-24-t6001-asc-debug.

## 12. T6021 CoreSight and DART stream state (2026-09-24)

The same debug block on T6021, read from Linux with the islands on and
the core stalled (CPU_STATUS `0x28`), reads zero: EDPRSR (engine+0x1010314)
`0x00000000` and EDDEVARCH (engine+0x1010fbc) `0x00000000`, against
`0x2ab` and `0x09108a15` on T6001. No hang. The block is read-as-zero on
this path, so the T6021 PC could not be read, and no unlock, halt, or DTR
access was made.

The ASC fetch stream is the open question. Read-only state of the three
dart-ane instances after release, with the Linux domain attached:

| instance | PARAMS_C | TCR0 | TTBR0 | TCR15 | TCR1-14 | ENABLE |
|---|---|---|---|---|---|---|
| `0x285800000` | `0x00010010` | `0x9` | `0x100124d1` | `0x2` (set by hand) | 0 | `0xffff` |
| `0x285810000` | `0x00100010` | `0x9` | `0x100124d1` | 0 | 0 | `0xffff` |
| `0x285820000` | `0x00100010` | `0x9` | `0x100124d1` | 0 | 0 | `0xffff` |

ERROR (`+0x100`) has no FLAG on any instance. It carries SID-field bits
only: `0x00a00000`, `0x00f00000`, `0x10700000`. A fresh boot reads the same
values before any release, so they are reset residue. ERROR_ADDR also holds
residue. macOS writes TCR15 = `0x2` (bypass) on all three instances. Linux
leaves SIDs 1-15 at TCR 0.

CPU_CONTROL = 0 does not stop a released core. STATUS stayed `0x28` for
100 ms after the write. The T6021 image has `b .` at TEXT+0x200, the same
as T6001, so a parked core cannot show a later fix. Only the first release
after a reboot is a valid test.

**First-release test (17:30 CDT).** This run was a fresh boot with the
islands already on in genpd and CPU_STATUS `0x2a` (not yet run).
TCR15 = `0x2` was written on all three instances and read back. Both
segments were mapped `IOMMU_CACHE`: leaf PTE `0x000fff1000084801`, PA
`0x10000848000`. I2A bit 0 was set, then CPU_CONTROL 0 then `0x10`.
STATUS went `0x2a -> 0x28`. For 60 s, SCRATCH0-7 stayed 0, I2A stayed
`0x00020001`, the ERROR words did not move, and no apple-dart fault was
logged. **Negative.** It supersedes the IOMMU_CACHE first release earlier
that day (dart0 TCR15 only) and the SID-0 and SID-15 runs on an already
released core, which tested nothing.

With the fetch stream bypassed and SID 0 translating to the right
cached page, the core still stalls. If T6021 has the T6001 fetch abort,
the abort is not caused by a refused DART stream on SID 0 or SID 15.

**PWGATE 3/0 words, base bound (17:52 CDT), read-only.** In the macOS
registry, `H11ANEIn` is the direct child of `ane0@84000000`
(`IOProviderClass AppleARMIODevice`), so the `start()` provider is the
ane nub itself. Its `IODeviceMemory` index 2 is `0x28e08c000`, length
`0x4000`. That is the same range as the Linux DT `set` reg
(`/soc/ane@284000000`, parent `/soc`, `simple-bus`). The kext writes
therefore land at phys `0x28e08d2cc <- 3` and `0x28e08d3cc <- 0`. The
`0x28e092cc` in the first static derivation was an arithmetic slip. With
the islands on, the live words read `0x00000000` and `0x00000000`. **The
`3` at `+0x12cc` is missing on Linux.** An earlier 17:41 note in this doc
called the theory dead on 0/0. That was wrong, because the kext wants
3/0. Other words read 0: `0x28e0932cc`/`0x28e0933cc`, `0x28e09359c`, and
the RMW word `0x28e0922dc`. No write was made. The core had already been
released this boot (STATUS `0x28`), so the write must be tested on a
fresh boot before the first release. Risk: this is the ane nub's SET
window. A Linux write of `0x28e08c000 = 0x80000000` was applied and read
back without harm earlier today.
