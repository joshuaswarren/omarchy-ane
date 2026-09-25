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
   place. T6021 only: the T6001 runs were unmapped (T6001 map staged in
   agent/ane-boundaries `receipts/2026-09-24-ane-perf-mode-h13` §10).
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
The uncached-map hypothesis is ruled out. On 2026-09-24 the two
segment-ranges were mapped with `IOMMU_READ|IOMMU_WRITE|IOMMU_CACHE`
forced. `iommu_iova_to_phys(0x10000000000)` returned `0x10000848000`.
The leaf PTE was `0x000fff1000084801`: valid, bit 1 (NO_CACHE) clear,
PA the TEXT page. dart-ane0 TCR[15] read back `0x2` (BYPASS). After
the usual release, SCRATCH7 stayed 0 and the I2A outbox stayed empty
(`recv0=0`, `i2a=0x00020001`, CPU_STATUS `0x28`) for 5 s.
dart-ane0 ENABLE at `0xc00` already read `0xffff` on that boot (bit 0
set). A 60 s poll with that bit set still showed SCRATCH7 0 and an
empty outbox. Writing `1` to DISABLE at `0xc20` cleared bit 0; the
readback was `0xfffe`.

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
| Firmware pages mapped uncached, so the ASC cannot fetch | Leaf PTE `0x000fff1000084801` has bit 1 clear and the TEXT PA; sid 15 TCR is `0x2`; SCRATCH7 and the outbox still empty |
| Zero `armv8_timer_frequency` keeps the core asleep | `patch_timer_freq=0x016e3600` wrote PA 0x10001406880 and read back, then release: still parked, no READY (2026-09-25, 340f3a4, receipt 8004cfe) |
| Coproc IRQ masks 0x1400a00-a14 left closed after the park | Read 0 post-park; 0xffffffff stuck 60 s; no READY, doorbell undrained (receipt 8004cfe). Status did move 0x28 -> 0x08, see section 14 |
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
- Every 13.5 hv run loses the proxy ACM 34-36 s after launch, whatever the
  trace set and whether the guest ADT keeps the ATC/USB nodes. The guest
  boots the Asahi stub, whose System volume has no macOS root filesystem
  (asahi-installer `src/stub.py`), so XNU cannot mount root, and the
  13.5 RELEASE kernel panics on that. The leading cause of the link loss
  is the SoC reset after that panic; the panic text is not captured yet.
  `tools/m2hv_catch_and_run.sh` logs the guest console from the hv vuart
  and passes the Asahi guide's macOS boot-args with both XNU debug gates
  opened (`-d` for `/chosen/debug-enabled`, `tools/m2hv_guest_debug.py`
  for `/chosen/asmb lp-sip0`), so a panic parks in the debugger with the
  link up. Receipt:
  `receipts/2026-09-23-m2-hv-trace/2026-09-25-usb-death-root-cause.md`.
- 13.5 IPSW members (kernelcache, ane0/ane1 firmware) are stored with
  SHA256SUMS in the fleet artifact store, not in git.

## 9. Next steps

1. (SUPERSEDED) Run a longer 13.5 hv trace that reaches the kext's
   CPU_CONTROL write: the kernel-only guest never issues it (lazy start),
   so longer runs of the same guest add nothing. What would work instead:
   a 13.5 guest whose userspace opens the ANE (aned/CoreML workload). That
   needs a full macOS 13.5 install in its own APFS volume on the M2, with
   m1n1 as that volume's boot object (Asahi m1n1-hypervisor guide:
   `bputil -nkcas`, `kmutil configure-boot`); the Asahi stub cannot host it.
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
- A hypervisor trace of a kernel driver only captures what the kernel does.
  A lazily started coprocessor needs its userspace trigger inside the guest,
  or the trace ends at device bring-up forever.
- Open the hv vuart (the second ACM port) on every hypervisor run. m1n1
  drops guest console bytes until the host opens that port, so without it
  a guest panic looks like a bare USB disconnect.

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

## 12. T6021 VENC leg: firmware reaches its service loop (2026-09-24)

With the VENC leg up (VENC_SYS→VENC_DMA→PIPE4/PIPE5/ME0), the T6021 core
executes to its main service loop and waits on host input; no HELLO yet.
The DATA footprint proves execution past fetch: the RTKit canaries at
SEG1+0x167d0 are overwritten with headers and ring words, a 319-pair
dispatch table is built at SEG1+0x1c000, and ~46 flag rows sit at
SEG1+0x5d42. The earlier "no stores" result (byte-identical DATA) was
the VENC-down stall; with the leg up the core runs code. Vehicle:
`ane/h13/ane_t6021_venc.c` (same shape as the T6001 `ane_pdraise.c`
approach in 8756868, T6021-gated on `apple,t6021-ane`; ported from
c9b2324, which was built on a side line and never landed on main).

A mailbox audit on the pre-VENC boot state (`receipts/2026-09-23-m2-hv-trace/
2026-09-24-mbox-carveout.md`, ~14:00 UTC, before the VENC leg): the I2A
outbox drains 0 words and holds EMPTY (out114 `0x000a0001`), the only I2A
word is a stale type-0 `0x000a000000000000`, SCRATCH0-7 read zero, and the
SEG0/SEG1 carveout reservations plus the iBoot image head are intact — with
the firmware not booted, the mailbox was not the blocker. With the VENC leg
up the core reaches its service loop and the mailbox FIFO still never
drains; no RTKit HELLO has arrived in any state so far.

## 13. T6021 CoreSight is fused off: no live PC path (2026-09-24)

EDPRSR (engine+0x1010314) and EDDEVARCH (engine+0x1010fbc) both read
`0x00000000` from Linux (vehicle `ane_ascdbg`, islands on, core stalled)
and from the m1n1 proxy (no guest, all nine ANE islands verified at
ACTUAL=f after direct TARGET writes to the seven `0x4000` islands that
`pmgr_adt_power_enable` does not raise). Neither path hung. A debug
block that reads RAZ with power on is fused off, not gated: no unlock,
halt, or DTR sequence can work from either side, and none was attempted.
No further CoreSight runs on T6021. The T6001 PC result stands on its
own; the T6021 PC is unconfirmable. Log:
`jwm1:~/m2proxy/hvlogs/proxy-coresight/capture.txt`.

## 14. Why the parked core takes no interrupt (2026-09-25)

Decode of the 13.5 ANE payload (`t602x_ane0_fw_selene_rc4x.payload`, sha256
`a9c4b771`, TEXT vm 0, DATA vm 0xc4000) against the live iBoot-patched image
(`/tmp/m2kstart/data_before.bin`, `data_venc.bin`) and the 13.5 kernelcache
(sha256 `9615a486`). The core parks in `wfi` at payload vm 0x71bc and neither
its own timer nor a mailbox doorbell wakes it.

### The timer fires FIQ, and the firmware does arm and unmask it

- Two vector tables. The reset stub (vm 0x234) sets VBAR_EL1 to the image base;
  its slots at 0x80/0x100/0x180 capture ESR/FAR/ELR in x28/x29/x30 and spin.
  After MMU bring-up the firmware installs `__rtk_arch_vectors` (vm 0x63800,
  payload 0x65914). The core runs with SPSel=0 (payload 0x50c), so IRQs and
  FIQs land on the Current-EL SP0 slots: IRQ at +0x80 (branches to 0x63d58),
  FIQ at +0x100 (branches to 0x63eac). The SPx IRQ/FIQ slots are the fatal
  capture spin, so a FIQ taken on the wrong stack pointer hangs the core.
- The core timer is PPI 30 and Apple routes it to FIQ, not IRQ. The payload
  keeps the two masks separate: `_RTK_enable_fiq` (vm 0x656ac) is `msr
  daifclr, #1`, while IRQ unmask is `daifclr, #2` (`_RTK_enable_all_interrupts`,
  vm 0x65694). Platform init unmasks both (vm 0x6fdc, 0x6fe4) before the idle
  loop, and the timer driver arms the comparator (vm 0x654cc writes
  CNTP_TVAL_EL0, 0x654d8 sets CNTP_CTL_EL0 = 1). So the firmware arms the
  timer and unmasks FIQ. The interrupt is generated; it is not delivered.

### The zero timer frequency is a real gap, and not the cause

iBoot patches the RTKit patchbay on the Asahi path too (live image differs
from the file in the stack guard, `RTK_soc` = 0x6021, `RTK_soc_revision` =
0x11, `RTK_cpu_physical_address` = 0x285000000, `RTK_cpu_wrapper_physical_address`
= 0x285400000, and the tunables block). It leaves one field the timer path
consumes at zero: `armv8_timer_frequency`, value at vm 0xca880 (PA
0x10001406880), 0x00000000 in both live captures. The firmware writes
CNTFRQ_EL0 from it only when nonzero (payload 0x65ff4-0x66010), so CNTFRQ_EL0
stays 0 and the frequency-derived tick rate (payload 0x6a028, frequency /
1000000) is 0. That does not stop the timer: the arm routine clamps every
interval to at least one tick (payload 0x654a8-0x654b8), so a zero frequency
makes an armed timer fire sooner, not never.

Tested 2026-09-25 (M2FwStart-2, `patch_timer_freq=0x016e3600`, omarchy-ane
340f3a4; receipt ane-linux-experiments 8004cfe on lane/m2-fwstart): PA
0x10001406880 before=00000000 wrote=016e3600 readback=016e3600. After the
release CPU_STATUS read 0x28, SCRATCH7 (+0x1840064) and +0x184006c stayed 0,
no HELLO, through poll A, a 30 s READY poll, and a 60 s doorbell poll. The
frequency is not what keeps the core asleep.

### What the host must do, from the working drivers

- Asahi's ISP driver (`isp-fw.c`) writes the coprocessor IRQ mask registers
  0x1400a00-0x1400a14 to 0xffffffff and polls the coprocessor status word at
  +0x818 for zero before it releases the CPU. The ANE kext does neither.
- The ANE mailbox is the ASC variant: Asahi's `mailbox.c` gives it
  `has_irq_controls = false`. There is no host-side mailbox IRQ-enable
  register to write; the doorbell is the inbox write itself, and the
  coprocessor's own controller decides whether that raises a core interrupt.
- The 13.5 kext `ANE_Init` (0xfffffe00094e0cfc) writes, before and at the
  release: eight scratch clears (offsets from the per-version tables at
  0xfffffe00073a0148, all inside the 0x18400xx GPIO block), RVBAR only if its
  lock bit is clear (it is set, so skipped), and CPU_CONTROL = 0 then 0x10.
  No interrupt, AIC, timer, or FIQ-route register is written on this path.
  Linux already matches every one of these writes.

### The delivery gate is not host-writable

The physical-timer FIQ is gated by `S3_5_C15_C1_3`
(`SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2` in m1n1 `cpu_regs.h`), bit 1 = enable
physical timer. The payload never writes it. The firmware drops EL3 to EL1 at
reset (payload 0x214-0x224, SPSR_EL3 = 0x3c5) and runs at EL1, so it cannot
set an EL2 register, and T6021 CoreSight is fused off (section 13), so the
host cannot set it either. On macOS, iBoot sets it before the firmware runs;
the Asahi stub's iBoot does not touch the ANE (its DART is powered off at
handoff). No Linux register write reaches this bit. Confirming it is the
difference needs the hypervisor trace of a macOS boot, not another register
guess.

### Coprocessor IRQ masks, post-park: null, but the core left IDLE

Keep `patch_timer_freq=0x016e3600` on every later run. It is not the fix, but
macOS iBoot fills this field, and firmware that wakes with CNTFRQ_EL0 = 0
would compute every timeout from a zero rate. The module writes it only when
the 36 bytes at PA 0x10001406870 still match the pinned pattern (tag
`76384671`, length `00000004`, value `00000000`, next tag `4453524c`).

Measured 2026-09-25, same boot as the frequency test (receipt 8004cfe): after
the park all six masks engine+0x1400a00-0x1400a14 read 0, so the firmware
never sets them. The tunables block does not touch them either: its records
name offsets +0x1401xx, +0x145010, +0x14a008 and +0x14a010 (live words at vm
0xdcf24, 0xdcf38, 0xdcf4c). Writing 0xffffffff to all six stuck, still
0xffffffff 60 s later. With the masks open, a 30 s READY poll stayed 0 at
status 0x28. A doorbell then sat queued for 60 s (A2I_CTRL 0x00020001, I2A
0x00020001, recv 0, SCRATCH7 0), and at the end CPU_STATUS read 0x08.

0x28 to 0x08 is bit 5 clearing, the bit m1n1 names IDLE (its CPU_STATUS
map is partly guessed). It is the first state change seen after a park. It
came only after masks-open plus doorbell. Masks alone held 0x28 for 30 s.
A read-only discriminator comes next:

1. On a 0x08 core, take the same SEG1 + BSS snapshot as section 12 and diff
   it against a snapshot taken before the doorbell. New stores mean the core
   took the interrupt and ran. A byte-identical image means it left WFI but
   stores nothing, which fits a vector capture spin (section 9: the capture
   slots only move ESR/FAR/ELR into x28-x30 and branch to self).
2. Doorbell without the mask write, 60 s, then read CPU_STATUS. If it holds
   0x28, the masks gate the doorbell's path to the core.

The interrupt entry is `__rtk_arch_interrupt` (payload vm 0x656ec). It
dispatches through the interrupt-controller object at vm 0xca148, vtable
+0x18. `_irqc` (vm 0x7384), which reads 0x285178000 into `gIrqcTimestamp`
(vm 0x4f84e8, PA 0x100018344e8), is only a per-line handler installed by
`CPlatformISRManager::Unmask` (vm 0x757c). A zero `gIrqcTimestamp` therefore
does not prove that no interrupt was taken.

### The doorbell wakes the core, and the handler never runs

Measured 2026-09-25 (receipt bb6bff2, lane/m2-fwstart, unmerged): a doorbell
with the six masks left at 0 still moved CPU_STATUS 0x28 to 0x08 after 60 s,
and the message never drained. The masks do not gate the doorbell. The diff
window used (SEG1+0x3b2000 and +0x3b6000) is inside `_rtk_page_tables` (vm
0x476000), so it cannot show a handler frame.

The IRQ entry, decoded from the payload:

- Runtime VBAR is vm 0x63800 (set at 0x65914). The Current-EL SP0 IRQ slot
  (+0x80) saves a frame and calls `__rtk_arch_interrupt` (vm 0x656ec), which
  dispatches through the controller object at vm 0xca148, vtable +0x18 (vm
  0x682ec).
- The handler reads the event word at MMIO base +0x818 (vm 0x68344). The
  base is the wrapper, 0x285400000, set at payload 0x6f64, so the register
  is engine+0x1400818. That read is a pop. Init drains the queue by reading
  +0x818 until the read returns 0 (vm 0x682e0-0x682e4) and never writes the
  register. The companion is +0x820, a status word the poll path reads
  (vm 0x68314). The firmware never writes that either, and a pop is not
  proven for it. There is no pointer register.
- Type is bits [18:16], the low 3 bits of the Apple AIC event type in bits
  [23:16] (`AIC_EVENT_TYPE`, irq-apple-aic.c). Source is bits [9:0], the low
  part of `AIC_EVENT_NUM`. Type 4 is the AIC IPI type. The decoder
  (vm 0x686a0) accepts type 4 only for source <= 0xb, type 1 for source
  <= 0xbf, and type 7 for source <= 0xf. Types 2, 3 and 5 return an error
  at once. A rejected event is not acknowledged. The dispatch loop repeats
  while ISR_EL1 bit 7 stays set (vm 0x683fc).
- Host samples of this word showed type 4 with the low field rising through
  7, 9, 0xb, 0xd, and an earlier dump showed 1 then 3. Odd values, step 2:
  two readers splitting one queue. Sources 7, 9 and 0xb are accepted; 0xd
  is rejected. Each host read retired an event the firmware did not see.

Host reads of engine+0x1400818 are banned while the core is running,
including wrapper-page dumps. A dump that includes +0x818 steals events.
+0x820 is not sampled in a loop either, until a pop is ruled out for it.

The IRQ frame lands on the IRQ stack, not the thread stack. SP_EL1 is set to
`_rtk_irq_stack` + 0x1000 (payload 0x658ac-0x658d0), vm 0xdba10, PA
0x10001417a10. The entry pushes 0x2e0 bytes before the handler call, so a
frame occupies vm 0xdb730-0xdba10, PA 0x10001417730-0x10001417a10. The file
image there is the `RTKSTACK` canary, `52 54 4b 53 54 41 43 4b`.

### IRQ stack: no frame, handler never ran

M2FwStart-2, 2026-09-25. One read of 0x400 bytes at PA 0x10001417610
(vm 0xdb610-0xdba10). The tail, which is where a frame would be, still
reads `53 4b 54 52 4b 43 41 54` at PA 0x10001417a08. That is the file
canary `52 54 4b 53 54 41 43 4b`. No register was saved. The handler
never ran.

The core left WFI without taking an exception. The idle loop is
`wfi; b wfi` (vm 0x71bc). A wake that is not delivered as an interrupt
completes the wfi, takes the branch, and enters wfi again, so IDLE stays
clear. Status 0x08 is that loop running. The doorbell is a wake event,
not an interrupt the core takes.

What remains is delivery into the core, not a handler bug. The
physical-timer FIQ enable (`S3_5_C15_C1_3` bit 1) is still the register
nobody sets, and it is still not host-writable. No further host read or
write is ranked. The next evidence has to come from a macOS boot trace,
or from a core register read this hardware cannot do.
