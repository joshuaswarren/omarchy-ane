# T6021 (M2 Max) ANE bring-up: findings as of 2026-09-25

This is the canonical record of what the T6021 ANE work has proven, what it has
ruled out, and what is still open. Read it before starting any T6021 ANE
experiment. Update it when a finding changes, and delete lines that go stale.

## Keeping this record current

Every ANE agent lands its findings in this file on main in the same step as
its receipt. A finding that lives only in a receipt or an agent branch is
lost. Cite the source repo and the commit SHA next to every number.

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
  kernelcache. The 13.5 kernelcache enters with the stock x0-only
  `hv_start`. A 26/27 kernelcache does not. Its entry dispatches on x0, and
  the boot CPU needs x0 = 0, x1 = the boot_args pointer, x2 = a handoff
  struct (magic `0xd00f000000000000`, version >= 7, bit 6 set).
  `tools/m2hv_entry_abi.py` supplies that, loaded through `M2HV_PREMOD`.
  The earlier "spins at `_start+0x10` on `[x1+8] == 0`" reading was the
  secondary-CPU path, taken because x0 was still the boot_args pointer.

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

The macOS working-state dump (section 17) narrows the open question "which
macOS pre-RUN step is missing" to three write-form candidates: ane_sys_mpm
left off, the VENC rails left off, and the single-DART-stream form, plus
the dart-ane0 DAPF windows, which macOS programs at dart-probe time and
Linux never opens (section 18). The fourth dump difference, mailbox CTRL
bit 19, is decoded as the UNDERFLOW
status latch and macOS never writes it, so it is replicated only as a test,
not treated as a required host write. Everything else the macOS dump reads
matches Linux. The staged Linux test forms are omarchy-ane
`agent/t6021-macos-ps-form` 265bb63 (section 18) and
`agent/t6021-mbox-dart-form` dd66d27. The stable wrapper words Linux never
writes are closed as candidates: kext evidence shows they are reset
defaults, hardware mirrors, or unused channels (end of section 17; data
ane-linux-experiments abb63dd, closure omarchy-ane ee14b27).

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
| RVBAR mode bits or a different latch keep the core from starting | macOS runs the firmware with the identical latch `0x0000010000000001` and no mode bits (section 17, ane-linux-experiments b7c7bc5) |
| CPU_STATUS 0x28 marks a parked core | The working macOS run reads 0x20 and 0x28 (section 17); 0x28 only says the core left its boot path |

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
- The stage-1 proxy falls through to U-Boot the moment the proxy client
  disconnects. On the first macOS-to-Omarchy boot of 2026-09-25, U-Boot hung
  after "Hit any key to stop autoboot: 0"; the cause is not isolated. One
  cold reset recovered it, and the box came up clean from m1n1's own NVMe
  boot, ANS2 included. That is the same rails-drop-cures-it shape as
  section 15. If a boot after a macOS session hangs at the autoboot prompt,
  do one cold reset before deeper repair.
- After two consecutive stub boot failures, every reset — including a
  USB-PD VDM reboot from the proxy host's macOS — lands in the Omarchy
  stub's paired recoveryOS 13.5 (22G74). `remotectl show` then reports
  `OSInstallEnvironment => true`, the RemoteXPC services refuse
  connections, and no remote shell exists. Getting out takes one action at
  the M2's own screen (Restart, or Startup Options -> Omarchy). Only a
  booted OS or m1n1's `PMU.reset_panic_counter()` clears the boot-failure
  counter that drives this routing.
- The escape that needs no screen: `tools/m2_linux_boot.py` with
  `m2_boot_loop*.sh` chainloads grub's first menuentry (kernel, initrds,
  devicetree, args, `panic=30`) over the proxy in one unbroken session and
  resets the PMU panic counter. U-Boot is not involved.
  Receipt: ane-linux-experiments `lane/m2-fwstart` 91dbab3,
  `receipts/2026-09-25-m2-bootpath-recovery/README.md`; the 43ec stage-1
  lineage is identified in 58b0917
  (`receipts/2026-09-25-m2-43ec-provenance.md`).

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
- Every 13.5 hv run loses the proxy ACM 34-36 s after launch when booted
  against the stub, whose System volume has no root filesystem (asahi-installer
  `src/stub.py`), so XNU cannot mount root and panics. `tools/m2hv_catch_and_run.sh`
  logs the console from the opened hv vuart (`serial=3`).
- v7 ramdisk route: the 22G74 restore root (`022-15462-082.dmg`) was staged as
  `ane-root-22G74.dmg` with `/sbin/launchd` replaced by `ane_open` (ad-hoc signed,
  calls `IOServiceOpen("H11ANEIn")`, holds 25 s). First contact reached AMFI,
  which panicked with `"can't has cs_enforcement_disable"` at
  `AppleMobileFileIntegrity.cpp:5463`. Disassembly of AMFI in `kernelcache.release.mac14j.macho`
  isolates `cs_enforcement_disable` as the ONLY boot-arg calling `csr_check(0x08)`
  and panicking. Bypasses with zero gates on RELEASE: `amfi_get_out_of_my_way=1`,
  `amfi_allow_any_signature=1`, `amfi_unrestricted_local_signing=1`. Default
  profile omits `debug=0x14e` and `wdt=-1` so panics reset the SoC and auto-recover
  to Linux. Receipt: `receipts/2026-09-23-m2-hv-trace/2026-09-25-amfi-boot-args-analysis.md`.
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

## 15. ANS2 fails the same way on the first macOS boot after Linux (2026-09-25)

Receipt 1ff5328 (ane-linux-experiments). macOS 27.0 on the M2 panics on the
first boot after a Linux session:
`RTBuddy(ANS2)::_setManagedStateGated "No response received in 20s, ANS2 not
started? (4)"`. Mailbox state at the panic: IDLE_STATUS 0x0000000a,
INBOX0_CTRL 0x00100101, OUTBOX0_CTRL 0x00020001, one TX word
0x0060000000000220, zero RX. The immediate retry boots clean. Source for the
kext side: the 27.0 kernelcache (xnu-13432.1.9, RELEASE_ARM64_T6020,
/tmp/anestatic/kc.raw).

### What the AP does before the first TX

`_performPowerStateChangeGated` (0xfffffe000b6aff40) calls the slave's
`startCPU` (vtable slot +0x888, at 0xfffffe000b6b0034) before the first
endpoint send (slot +0x1e8, at 0xfffffe000b6b0128). For an ASC that is
`AppleA7IOP-ASCWrap-v4::_runCPU` (0xfffffe0008bc5180): read CPU_CONTROL at
wrapper+0x44, write it back with bit 4 set. `stopCPU` clears bit 4, then bit
5. No interrupt, AIC, or power register is written on this path. The ANE
kext's own `ANE_Init` is the same shape (section 14). iopStatus 4 is the
value this function stores (0xfffffe000b6b018c, `mov w1, #4`) when the IOP
has been started and the host is waiting for the firmware's version reply.
The panic prints that 4. It means the host sent its word and got nothing
back.

### What a panic reset clears that a warm reboot does not

A panic reset is a PMU reset: the rails drop, so every coprocessor power
domain and every wrapper register returns to its power-on value, and iBoot
re-runs its full coprocessor setup. A Linux reboot through macsmc or the
m1n1 proxy resets the application processors and re-enters the boot chain,
but it does not drop the coprocessor rails. Wrapper registers, AIC routing,
and a latched RVBAR survive it. That is the only reset-scope difference the
two paths have, and it is the difference between the failing first boot and
the working second boot.

### Does that explain the ANE park

Partly, and the part that does not is the useful one. The ANS2 failure is
specific to the first boot after Linux and is cured by the panic reset. The
ANE park is not: it reproduces after watchdog resets and after power-button
boots (section 5, the 06:11 watchdog reset). So the ANS2 case names a
residual that a PMU reset clears, but the ANE case survives resets that
should clear the same class of state. The two share the park shape. They do
not share the reset behaviour, and that rules out "a stale wrapper register"
as the whole explanation for the ANE.

### Tests, from Linux, ranked

1. ANS2 register dump, read only. From the ADT, take the ANS2 ASC wrapper
   base and read CPU_CONTROL, CPU_STATUS, both mailbox control words, and
   the event word at +0x818 once. Do this on a normal Linux boot and record
   it. The ANE reads 0x28 or 0x08 with the outbox armed and empty; if ANS2
   reads the same shape, the coprocessors are parked alike and the ANS2
   panic is the same park seen from macOS.
2. The dump again after a warm reboot (m1n1 `p.reboot()`), before any ANE
   experiment. If the values survive the reboot unchanged, the warm path
   preserves coprocessor state and test 1's reading stands.
3. The discriminating reset. One run where the machine is powered off at
   the PMU (SMC shutdown, then power button) rather than rebooted, then the
   same dump. If ANS2 and the ANE both come up differently after that and
   only after that, the residual lives in a domain only a PMU reset clears.
   If the ANE park survives that too, the residual is not reset state at all
   and this section's hypothesis is exhausted.

## 16. macOS power-state form: ane_sys_mpm stays off (2026-09-25)

Source: ane-linux-experiments `lane/m2-fwstart` 511acc6,
`receipts/2026-09-25-macos-ane-pstable`. macOS 27.0 (26A428) on T6021. A kext
register capture read the pmgr power-state block (PA `0x28e080000`) 14 times
around a Parakeet encoder run: 3 idle samples, 10 during the run at 4.8 W ANE
power (powermetrics), 1 after. Field layout per Linux `pmgr-pwrstate.c`:
TARGET [3:0], ACTUAL [7:4], PS_AUTO [27:24], AUTO_ENABLE bit 28.

| domain (offset) | macOS idle | macOS under load | Linux raised |
|---|---|---|---|
| ane_sys @0x260 | 0x0f000300 | 0x1f0003ff | 0x1f0003ff |
| ane_cpu @0x2e0 | 0x0f000300 | 0x1f0003ff | 0x1f0003ff |
| ane_sys_mpm @0x4000 | 0x00000300 | **0x00000300** | 0x000003ff |
| ane_td/base/set1-4 @0x4008-0x4030 | 0x00000300 | 0x000003ff | 0x000003ff |

- ane_sys and ane_cpu run in hardware auto-gating mode. Under load the word
  is 0x1f0003ff: AUTO_ENABLE set, PS_AUTO 0xf, ACTUAL 0xf, TARGET 0xf.
  Between jobs ane_cpu reads 0x1000030f: TARGET and AUTO_ENABLE hold, ACTUAL
  and PS_AUTO fall to 0. No software write moves it. Idle, macOS clears both
  domains to 0x0f000300. ane_sys can read its idle word between jobs too.
- ane_td, ane_base and ane_set1-4 use a plain target: 0x3ff under load,
  0x300 idle.
- ane_sys_mpm reads 0x300 (off) in all 14 samples, including the 4.8 W ones.
  The stock Linux DTB marks it `apple,always-on`, so the pmgr probe raises it
  at boot and genpd never lowers it. This is the largest power-form
  difference between the two systems.
- The ASC domain comes up before the compute islands: ane_cpu read ACTUAL 0xf
  two samples before any compute island left 0x300.
- This table also explains the earlier capture refusals: the old kext gate
  required ane_sys_mpm at ACTUAL 0xf, and macOS never raises it.

## 17. macOS working-state engine dump (2026-09-25)

Source: ane-linux-experiments `lane/m2-fwstart` b7c7bc5,
`receipts/2026-09-25-macos-ane-engine-dump`. Same T6021 laptop and kext tool,
two gated passes (load3, load4) while the ANE firmware ran under the encoder.
Engine base 0x284000000. Linux column: lane receipts
`2026-09-25-m2-handshake` and `2026-09-23-m2-fwstart` s9.

| register | macOS working | Linux |
|---|---|---|
| RVBAR +0x1050000 | 0x0000010000000001 | same value |
| CPU_CONTROL +0x1400044 | 0x10 (RUN) | 0x10 after release |
| CPU_STATUS +0x1400048 | 0x20 / 0x28 | 0x28 |
| SCRATCH0-7 +0x1840048 | 0 0 0 0 0 0 1 0 | SCRATCH6=1 (S1), SCRATCH7=0 |
| +0x184006c | 4 | not logged |
| A2I_CTRL +0x1408110 | 0x000a0001 | 0x00020001 |
| I2A_CTRL +0x1408114 | 0x000a0001 | not logged |
| IRQ masks +0x1400a00-a14 | 0 | 0xffffffff (our test wrote them) |
| DART ENABLE, all three instances | stream 0 only | stream 0; earlier runs enabled sid15 |
| DART sid0 TCR / TTBR | 0x9 / 0x1004102d, one shared table on all three | TCR 0x9; TTBR equalized |
| dart0 PROTECT +0x200 | 0x6 | not logged |
| VENC_SYS 0x2902803e0 | 0x0f000300 (off) in every sample | raised to 0x1f0003ff |
| VENC_DMA, PIPE4/5, ME0/1 | 0x300 (off) in every sample | raised to 0x3ff |

What this changes:

- **RVBAR is falsified as the blocker** (section 6). macOS runs the firmware
  with the same locked latch and no mode bits.
- **CPU_STATUS 0x28 is not a park signature** (section 6). The working macOS
  run reads 0x20 and 0x28.
- **SCRATCH7 is 0 while the firmware serves.** A READY poll on SCRATCH7 can
  only catch a transient. +0x184006c reads 4 in the working state.
- **The mailbox control words differ in bit 19** (0x000a0001 vs 0x00020001).
  Bits 16-17 are FULL and EMPTY. Bit 19 is now decoded (M2PreRunRE static
  RE): bit 19 is UNDERFLOW (read from an empty FIFO) and bit 18 is
  OVERFLOW. AppleA7IOP tests both words against mask 0xc0000 and panics
  (27.0 site 0xfffffe0008bca008, 13.5 site 0xfffffe0008bc61fc), and the 13.5
  ANE firmware runs the same test (payload text.dis 0x6d32c). macOS
  software never writes bit 19 — the only control write is the outbox
  enable, bit 0. The 0x000a0001 in this dump is a latched underflow flag,
  not a mode the host must set.
- **macOS enables one DART stream.** The dart1/dart2 sid1-14 words change
  between the two passes and are not a live TCR/TTBR layout.
- **macOS keeps every VENC rail off** while it uses the ANE. Linux raises
  them.
- The six IRQ masks read 0 in the working macOS state. This confirms section
  14: the masks are open only where our tests wrote them.

Caveat: both gated passes fell in the encoder's model-load phase. The 500 ms
powermetrics sample before each read showed 0 mW, and no engine read landed
in the steady 4.8 W reps. Treat the table as "firmware running, compute
idle", not as a mid-inference state.

### Wrapper map (receipt abb63dd)

Load3 vs load4 comparison of the same dump. These stable words read set in
the macOS working state, and Linux's fw_start sequence writes none of them.
They are further pre-RUN candidates, pending kext evidence:

| wrapper offset | macOS working | note |
|---|---|---|
| +0x8 | 0x12345678 | test-pattern-shaped, stable |
| +0x40 | 0x000a0000 | stable |
| +0x444 | 0x10 | a second CPU_CONTROL-shaped word, RUN bit set |
| +0xb80..+0xb94, +0xbfc | 0xffffffff | stable; candidate mask/enable bank |
| +0x4110..+0x4140 | 0x00020001 / 0x1 | mailbox-shaped CTRL block, no bit 19 |
| +0xc110, +0x10110 | 0x000a0001 | two more mailbox-shaped blocks |
| +0x481c..+0x497c (stride 0x20), +0x881c, +0x883c, +0xc81c, +0x1081c | 0x000a0000 | per-channel status words carrying bit 19 |

Bit 19 is set only in the CTRL and status words the macOS driver polls, and
absent from the +0x4110 block it does not poll. That matches the UNDERFLOW
decode above: bit 19 is a symptom of host reads, not a configuration value.
+0x1008 flips 1 -> 0 between the passes (transient), and the words at
+0x4150../+0x8150../+0xc150../+0x10150.. change between samples: FIFO SRAM.

Kext evidence then closed the list (M2PreRunRE static RE of the 27.0 and
13.5 kexts, omarchy-ane `agent/t6021-mbox-dart-form` ee14b27). ANE_Init,
EnableANEClocksAndPower and AppleASCWrapV4 write none of these words:

- +0x0 = 1 and +0x8 = 0x12345678 are silicon power-on reset defaults (m1n1
  prores.py shows the same pair).
- +0x40 = 0x000a0000 is a read-only wrapper status word; bits 19:16 = 0xa is
  UNDERFLOW + EMPTY.
- +0x444 = 0x10 is the Core 1 slice (+0x400 stride) mirrored by hardware
  when Core 0 RUN (+0x44) is written. No kext write to +0x444 exists.
- The 0xffffffff bank at +0xb80..+0xb94, +0xbfc is KIC interrupt registers
  at reset default; ASCWrapV4's interrupt functions are empty stubs.
- The mailbox-shaped blocks at +0x4000, +0xc000 and +0x10000 are unused
  extra ASC channels.

So the wrapper map holds no missing host write. ee14b27 adds BOOT-REPORT
baseline logging of the six locations plus opt-in `fw_start_core1_run` and
`fw_start_wrapper_b80_unmask` (default off) to force the two hardware
behaviours if a run ever needs them.

## 18. macOS capture tool and the staged Linux tests (2026-09-25)

The capture tool behind sections 16-17 is `tools/macos-regdump/` on
omarchy-ane main (commits cee3dd1 through 210a1e3). The kext
(`com.warren.ANERegDump`) is frozen at binary sha256 `932d3b9b...`; a staged
copy ships with SHA256SUMS. Each rebuilt kext costs an Allow click plus a
reboot, so the address table, pmgr base, island offsets, gate mask and poll
budget are runtime data in `ranges.txt` (all numbers hex).

The kernel side enforces the safety net whatever the file says
(`ane_regdump_filter.h`, `ane_req_acceptable`):

- a gate predicate needs ACTUAL bits [7:4] = 0xf and must reject the idle
  words 0x300 and 0x0f000300;
- every engine-window range is gated even if the file omits the flag;
- a range touching engine+0x1010000 (CoreSight) is refused outright;
- the engine+0x1400818/81c/820 event words and the mailbox RECV words are
  never read (pop-on-read, section 14);
- `poll_us` is capped at 10 s;
- `start()` only registers the service; all work happens on the CLI call.

Building it gave the macOS 27 idle signature: 0x300 per island and
0x0f000300 for ane_cpu, where the 0xf is bits [27:24] (PS_AUTO), not ACTUAL.
The 2 s poll catches the short ACTUAL-0xf windows; one workload sample still
read idle. Known gap: the kext asks the ADT for node `ane0`, the live node is
`ane0@84000000`, so ADT ranges never land. `capture.sh` saves the ADT
properties with ioreg and the boot args with sysctl, and the fw-text/fw-data
reads are ungated DRAM reads (the section 3 segment-ranges).

Linux test form, staged on omarchy-ane `agent/t6021-macos-ps-form` 265bb63.
Result pending:

- `fw_start_mpm_off` (default on, under `fw_start=1`) powers ane_sys_mpm down
  with the `apple_pmgr_ps_set()` PWRGATE write before the boot sequence's
  first engine write, and refuses the sequence unless ane_sys/ane_cpu read
  the macOS AUTO_ENABLE form and td/base/set1-4 read ACTUAL 0xf. A refusal
  unwinds cleanly, because no CPU has started.
- `fw_start_venc_gates=0` leaves the VENC rails off (the macOS form; the
  rtclient default raises them).
- Fix that came with it: the probe G1 gate tested TARGET where its comment
  said ACTUAL, so an auto-gated ane_cpu (0x1000030f) passed it. It now tests
  ACTUAL, and a failed pmgr map fails the probe instead of reading engines.

Two more opt-in params sit on omarchy-ane `agent/t6021-mbox-dart-form`
dd66d27 (off 265bb63, default off) for candidates 3 and 4 before CPU
release: `fw_start_mbox_ctrl_bit19` writes 0x000a0001 to both mailbox
control words, and `fw_start_dart_single_stream` sets the macOS DART form
(stream 0 only via DISABLE_STREAMS at +0xc20, dart0 PROTECT 0x6; Linux
apple-dart enables all streams and never touches PROTECT, and with
PROTECT bit 0 clear it will not fight) on all three DARTs. Both log before
and after reads. Clean build verified against the 7.1.13 tree. First
hardware leg: the single-stream writes landed — ENABLE went 0xffff to 1
and dart0 PROTECT went 0x2 to 0x6 (receipt caeb09cf).

A fourth staged form opens the dart-ane0 DAPF: omarchy-ane
`agent/t6021-leg-baseline` e8411ac adds `fw_start_dapf` (default off),
which writes the five ADT `dapf-instance-0` windows in m1n1
dapf_init_t8110a register order before CPU release — the first window is
the ANE pmgr ps block, 0x28e084000..0x28e084033 — logging each entry
before and after plus the first unused slot. Why this is a candidate
(M2PreRunRE static RE): `AppleT8110DART::start` calls `_apfSetupInstance`
at dart probe (27.0 site 0x9ffb74c, 13.5 site 0x9bfdc98), reads the 52
byte / 0x34-slice ADT property, and programs the DAPF instance at reg[3],
PA 0x285804000 — strictly before ANE_Init. Linux apple-dart has no DAPF
code, and m1n1's dapf_init_all skips dart-ane0, so nothing opens those
windows under Linux. Why the first writes were ignored: DART8110 PROTECT
(0x200) bit 1 (LOCK_REG_4xx) write-protects the +0x4000 DAPF aperture,
and Linux boots with dart0 PROTECT = 0x2 — every write to 0x285804000 was
dropped by silicon. The unlock protocol (M2PreRunRE; m1n1
`hw/dart8110.py`): write 0x2 to UNPROTECT (0x285800204), verify PROTECT
bit 1 clear, write the DAPF slices (+0x04 r4, +0x08 start, +0x10 end,
+0x00 r0 enable, +0x20 r20, stride 0x40), then re-protect by writing 0x6
to PROTECT (0x285800200) — the macOS working-state value (section 17).
Receipt: ane-linux-experiments `lane/m2-fwstart`
22b208b, `receipts/2026-09-25-t6021-ane-dapf/` (ADT extract and m1n1
entry list included). First hardware leg failed (M2FwStart-2, receipt
`lane/m2-fwstart` caeb09cf, `receipts/2026-09-25-t6021-dapf-lock/`): the
five writes did not land — every entry stayed r0=0, start=0, end=3.
PROTECT_LOCK read 0x2: bit 1 is set-once, and a live UNPROTECT could not
clear it (the bit was set before this Linux session; Linux apple-dart
never writes PROTECT_LOCK). A stage-1 read of 0x285800200 also aborted
(FAR 0x285800200, ESR 0x96000010) — the aperture was unpowered. Next
attempt, not yet run: power /arm-io/dart-ane0 in the catcher, then
unprotect and program.

## 19. Cross-SoC: T6001 and T8103 (2026-09-25)

Short notes from the M1 Max and M1 lanes. Each item says what it is derived
from; carry it to T6021 only after a check on that chip.

### T6001: firmware cold start as macOS does it

Source: ane-linux-experiments `lane/jw16-lsink2-clean` 82ede28 (merged
caabc0f), `receipts/2026-09-24-launch-sink2/asc-capture/run-205806/`. From a
T6001 macOS boot log.

- The kext starts the firmware lazily, on the first power-on client (the ISP
  peer here), not at boot. The capture window shows six cold starts; the
  `ColdStarts` counter reaches 6.
- One cold start is fast: "FW App image..." at 20:52:11.940, command buffer
  0x1fb08000 logged at .941, first firmware command accepted
  (CSNE_CMD_SET_SNE_PMU_BASE2, res=0) at .955. About 15 ms from image to a
  live firmware. Capability discovery follows at once: fNumANEs 1,
  fANEMaxCacheRequests 16, fANEMaxGlobalWaitEvent 32.
- Surfaces: ANE aperture 0x284000000, length 0x2000000; PS registers
  0x28e080000, length 0xc02c; fw TEXT phys 0x10000a5c000 (identity remap,
  size 0xf4000); fw DATA phys 0x10001684000, remap 0x1f0000f4000, size
  0x5f8000; command buffer IOVA 0x1fb08000.

### T6001: CSNE wire protocol (static kext RE)

Source: ane-linux-experiments `lane/kext-re-clean` d4b9b3e,
`kext-re/FINDINGS.md` (AppleH11ANEInterface 9.512.0, macOS 25G83 build).
T6001-derived.

- Command packet: u32 0 at +0, u16 opcode at +4, u16 0 at +6 (zeroed by the
  sender), opcode payload from +8. Copied verbatim into the command-buffer
  slot.
- Cold-start order: PRINT_ENABLE (0x4, 12 B) -> TRACE_ENABLE (0x21, 12 B,
  only when the unit-test flag dev+0xC8 is nonzero) -> START (0x0, 12 B) ->
  SET_SNE_PMU_BASE2 (0x29, 16 B: `00000000 29000000` + u64 LE pmu_base) ->
  CONFIG_GET (0x3, 16 B) -> CH_PROPERTY_WRITE (0x1F, 20 B: 0, 0x10A4, 1) ->
  RESOURCE_INFO_GET (0x22, 100 B).
- Doorbell: memcpy the packet into the slot, then IOProcessorChannelSend
  writes a 64 B ring entry {ep | seqtoggle^1, dst pointer, respSize}, dsb,
  ring. EP is the runtime IOP id for channel name "IO" (dev+0xDFC8), not a
  kext constant. The kext holds no raw doorbell MMIO store; XNU's
  IOProcessor layer owns the register.
- HELLO strictly comes first (M2PreRunRE static RE of the 27.0 kext). ANE_Init
  registers its RTBuddy endpoints, polls RTBuddy until the boot and handshake
  complete, and only then sends the CSNE cold-start commands over the
  established app channels (ANE_InitFirmwareConfigurationEv, after
  0xfffffe00095e9b60). The firmware cannot receive CSNE commands before
  HELLO/EPMAP. A Linux run that reaches the CSNE sequence without a
  completed handshake is testing nothing.

### T6001: DART, CTRR and RTKit decode

Source: ane-linux-experiments `lane/kext-re-clean` dfd628e,
`kext-re/DART-DECODE.md`. T6001-derived.

- The kext never writes DART PTEs. IODARTVMAllocator and the DART framework
  fill them. Bare-metal code must do that work itself.
- All three ANE DARTs (0x285800000 / 0x285810000 / 0x285820000 on T6001)
  need the same TTBR0 plus UNK_CONFIG_68/0x6c. m1n1's ane driver programs
  all three ("DMA fails w/o").
- Three-dart topology is the shared h13 shape, not a T6001 quirk: T8103
  also has three ANE DARTs (0x26b800000 / 0x26b810000 / 0x26b820000, 16K
  pages; d733cd1), and T6021's three DARTs are on record above (section
  17).
- DART8020 field map: TCR at 0x100 + 4*sid, TTBR at 0x200 + 16*sid + 4*bank,
  ENABLED_STREAMS 0xfc, REMAP 0x80..0x8c.
- The CTRR remap window (the 0x1f0000f4000 class) comes from the DT
  `segment-ranges` property {phys, virt, remap, size}. The kext only creates
  the DART translations that make remap -> phys true.
- RTKit: after boot the firmware sends HELLO on the management endpoint and
  waits. With no reply it never announces endpoints, and every later CSNE
  write is ignored. That is the exact stall symptom on both chips.
  Constants: HELLO=1, HELLO_REPLY=2, STARTEP=5, SET_IOP_PWR_STATE=6,
  EPMAP=8.

### T8103: ANE clock lead

Source: ane-linux-experiments fedd4da (section 7) and 887ba0e (section 8),
`receipts/2026-09-25-m1-ane-clock-macos/`. The a90b9a9 section 9 PA
reading is retracted (fix 8637d99); the history bullet below records it.

- Same Parakeet encoder on the same M1 (T8103): macOS 112.99 ms median,
  Linux 141.4-141.5 ms. The gap is real; no clock measurement explains it
  yet. The 1.258 ratio fit against the ladder steps is an inference.
- ADT decode history: two readings retracted. 87f86ab's PA 0x23d2b4140
  was an inference; the probe built on it was removed in omarchy-ane
  `agent/ane-clock-m1` 38beae6 before any run. a90b9a9's PA 0x23b778000
  read byte 2 of the `PMGRClocks` row as the perf block; byte 2 is a type
  field (3 on every PLL, 1 on muxes). The T6001 row proves the layout:
  PLL_ANE0 is `13 08 03 15` -> perf block 8 = `perf-regs[8]` =
  0x28e070000, size 0x64, idx 0x13, matching the 09-22/09-23 receipts.
  Standing result (887ba0e): T8103 `PLL_ANE` is perf block 1, idx 0x48 —
  `perf-regs[1]` = 0x23b734000, size 0x100, the same block as ANE_SYS
  (devices[99], idx 0x31) and the T8103 twin of the T6001 perf-regs[1]
  region next to the read that hard-reset the M1 Max. (`perf-domains`
  byte 1 is not a perf-regs index either: it takes 4/1/4/1/0/4, and the
  T8103 `perf-regs` has entries 0-3.) On both chips the ANE perf regions
  are forbidden-class and the idx-to-offset layout inside the block is
  unsourced, so there is no candidate PA on either chip. Do not write
  either region as part of a clock experiment. Receipt fix: 8637d99.
- T8103 kernelcache (mac13g, sha256 `861adca1`): `ApplePMGRNub::
  requestPerfState` maps enum 2 to internal domain 8 (ANE) and tail-calls
  `_handlePerfStateRequest`, which accepts domains 8 and 14 only. The
  apply routine writes an 8-bit state `(old & ~0xf) | (new & 0xf)` through
  the device register accessor. Unlike M2, the T8103 PMGR has an ANE
  path — but no kext imports `requestPerfState` statically: H11ANEIn
  reaches perf control through its IOPerfControlClient token path, and
  AppleT8103CLPCv3 owns the PMGR perf imports (`aneWorkBegin`/`aneWorkSubmit`
  compute the state from submitted work). ApplePMGR builds the accessor
  base from `perf-regs` at start, so the static PA is not independently
  confirmed.
- The kext's only host write in its private PMU window (clear mask 0x8 at
  0x23b110100) is ruled out: Linux already reads 0x0 there.
- New kext engine-write path, mac13g kernelcache (AneClockM1 static RE,
  receipt pending): `AppleT8103PMGR::writeReg32` (vtable slot +0xd20) has
  an ANE branch on top of the base write (+0xd30). When the register is
  0x470 — the ANE_SYS power-state word at pmgr reg[0]+0x470, PA
  0x23b700470 — with a nonzero low nibble, and the ADT `ane-acg-hack`
  flag is set (T8103 has it = 1), it does a physical read-modify-write at
  PA 0x26b868a04 (macOS ANE window 0x26a000000 + 0x1868a04):
  `(old & ~0x1000) | 0x80001000`; power-down to 0 clears bit 12. No
  Linux code writes that engine word. Executed, in two halves
  (AneAcgT8103, ane-linux-experiments 9288833,
  `receipts/2026-09-25-jwm1-t8103-acg-hack/`; omarchy-ane
  `agent/t8103-acg` eb4cf4c). Read half: Linux's power-up value at the
  word is 0x80000000 — bit 12 clear where macOS sets it; the two systems
  genuinely disagree. Write half, falsified: applying macOS's RMW
  hard-resets the machine (PMU-logged reset during probe, auto-recovery
  to stock, no filesystem damage). Bit 12 gates the idle engine's clocks,
  and the hack rides on CLPC/pmgr clock state Linux never configures. No
  retry — each attempt is an unclean reset on a btrfs root. Consequence:
  acg alone cannot close the 112.99 vs 140 ms encoder gap; the missing
  prerequisite is Linux-side ANE clock management.
- T6001 ACG question closed: `AppleT6000PMGR::writeReg32` (22G74 cache)
  has no ANE branch — its special case (map 2, reg 0xc00, die 0) is
  workaroundPSRegsForceWakeUP and the general path is a plain BIT(29)
  RMW, with none of the acg constants, and the T6001 ADT carries no
  ane-acg-hack (Jw16Levers5, ane-linux-experiments e5e82aa,
  `receipts/2026-09-25-jw16-levers5/receipt-step3-4.md`). The ACG lever is
  T8103-only; a Linux acg_hack fix does not transfer. Whether the T6021
  ADT carries the flag is unchecked — that decides whether this path can
  matter for the M2 park.
- Next measurement is staged, not run: dtrace `ane-perfstate.d` probes
  `_handlePerfStateRequest` and the apply routine during an encoder run.
  The capture moved to the M1 Max macOS window (8637d99), since the M1
  laptop keeps the armed M2 catcher. No Linux device read of either
  chip's perf block until then.

### T6021: the macOS pmgr has no ANE perf-state path

Source: ane-linux-experiments 887ba0e, `receipts/2026-09-25-m1-ane-clock-macos/`
section 8 (`t6020-setPerfState-full.asm.txt`). Decoded from the macOS 13.5
kernelcache (mac14j, sha256 `9615a486`). T6021-specific as a negative: the
same check on T8103 has since run and differs — its PMGR accepts ANE
domain 8 (see the T8103 clock lead above).

`AppleT6020PMGR::setPerfState` (0xfffffe0009b7ef14-0xfffffe0009b7f684)
dispatches on the domain ID. Only IDs 2, 5 and 13 reach the write path: the
CPU-cluster DVFS command word at block+0xe20020, written as
`(old & ~0x1f) | BIT(25) | (state & 0x1f)` — Asahi's
`apple-soc-cpufreq.c` shape. Every other ID, including the ANE perf-domain
index 8, branches to an assert panic. The host's only perf-state entry
point therefore has no ANE path on M2: the host never sets the ANE clock
through pmgr. That fits the firmware setting its own operating point after
the `CH_PROPERTY_WRITE` "FW PERF MODE" command. Sibling PMGRs differ, and
static reads can mislead: T8103 accepts domains 8 and 14 (the T8103 clock
lead above); T6001's static 22G74 read said 1-5/13, but the live capture
(a3641215, below) shows `ApplePMGR::_setPerfState` driving ANE domain 8 —
the same domain as T8103. Each chip's decode is its own.

### T8103: Qwen ANE layout gate closed

Source: ane-linux-experiments f5a8d4b (landing d99d9d4 on
`agent/jwm1-parity5-gpu-parity-m64`),
`receipts/2026-09-25-jwm1-qwen-ane-layout-gate/`. Stack: installed module at
omarchy-ane a9a5f60, libane rebuilt from the same commit, 38 staged Qwen
programs (one per layer slot), guard-checked load.

- Verify PASS 10/10. Every prompt matches the host reference from the first
  compared token (first_diff=32 on all 10). Guard proof: with the guard
  removed, libane refuses the load at init, before any device open. No
  silently wrong layout reaches the hardware.
- Bench n=100 against the committed macOS denominator (fedd4da json sha
  `410dc4f7`): decode 8.23 vs 5.625 tok/s = 1.466x [1.430, 1.503] PASS;
  e2e 5.314 vs 6.718 s = 0.787x [0.770, 0.806] PASS; TTFT 1.534 vs 1.189 s
  = 1.347x [1.236, 1.480] FAIL. The ~345 ms TTFT gap was this lane's live
  lever; it is closed by the next bullet.
- Same merge carries `receipts/2026-09-25-jwm1-parakeet-golden-rerun`:
  Parakeet golden PASS bit-exact on the installed a9a5f60 stack. The earlier
  post-reboot hang left no kernel trace, did not reproduce under an
  instrumented re-run.
- TTFT lever closed; the first root cause did not survive. b93397a
  (`receipts/2026-09-25-jwm1-qwen-ane-ttft-rt/`) called the ~154 ms slow
  steps (16% of steps) scheduler wakeup stalls in the kernel `ane_exec`
  completion poll and passed the cell with `chrt -f 50`. Per-step schedstat
  falsified that: inside a slow step every program runs at about 2x its
  normal wall while the submitter's CPU time and run-queue delay stay
  unchanged. The engine's memory path runs at half speed. On T8103
  apple-soc-cpufreq writes `DVFS_CMD PS2 = PS1`, so the memory-side
  performance state follows the CPU clusters; a mostly-sleeping submitter
  lets schedutil park both clusters low, and the bandwidth-bound Qwen step
  (~1.2 GB of Q4 weights per 65 ms) streams at the low memory p-state.
  SCHED_FIFO had helped only because it holds a CPU at max frequency.
  ff9ac7c (`receipts/2026-09-25-jwm1-ane-dvfs-boost/`): omarchy-ane main
  5a22ee3 adds `ane/src/ane_boost.c` — from the first submit until
  `boost_idle_ms` (default 100, 0 = off) after the last, the driver holds
  a `FREQ_QOS_MIN` at `cpuinfo.max_freq` on every cpufreq policy. The
  completion poll was never the cause and is unchanged. Pinned clusters
  gave 0 slow steps in 489, twice. Installed module sha256 `57ceaddd`.
  Qwen ANE cell at normal priority, no wrapper: TTFT 0.8449x [0.7750,
  0.9283], decode 1.4718x [1.437, 1.509], e2e 0.6993x [0.683, 0.716],
  100/100 tokens exact; golden bit-exact x3; battery 34/34. The
  compute-bound Parakeet encoder does not move (about 140 ms either way),
  so the 1.243x encoder gap stays with the ANE perf-state lever (the
  T8103 clock lead above).
- Same merge decomposed the GPU TTFT at RT: 219.6 ms fixed cost +
  1.19 ms/token (the old 4.81 ms/token slope was jitter;
  `receipts/2026-09-25-jwm1-gpu-ttft-fixed-cost/`). A Mesa barrier-batch
  candidate is pending a rare-race battery.

### T6001: tm/tq retention blocks in-place recovery

Source: omarchy-ane a9a5f60 (merge of df23ca9), `fix/t6001-tm-recovery`.
Validated on T6001.

- The set0/base islands hold the tm/tq register file in retention through
  any genpd cycle, so a power cycle clears nothing and in-place reset is
  unavailable.
- A timed-out task also leaves per-queue error latches: TM_ERROR1/2 read
  0x22222222 and TM_ERROR3 reads 0x2222. The latches are firmware-held;
  write-through and zero clears are both ignored.
- The working cure is the module-reload door: rmmod + insmod after a wedge
  (kill-race 10/10 reopen-clean; two wedge->reload cycles followed by the
  full Parakeet contract, all green twice, no reboot). The reload works
  because the wedge pin drops at postclose and the probe purges stale DART
  mappings.
- T8103 keeps its full-POR recovery path; that path is unchanged.

### T6001: ane_boost validated

Source: Jw16Levers3 validation of omarchy-ane 5a22ee3 on T6001, built
on-device (v0.1.0-605-g5a22ee3, module sha256 `cf1d4bf1...`), installed
persistently with `boost_idle_ms=100`. Receipt: ane-linux-experiments main
3120fe1 (commit a5a1095), `receipts/2026-09-25-jw16-levers3/receipt.md`
with artifacts/ane and artifacts/ane-ab.

- Kill-race x10: PASS 10/10 reopen-clean. The awk source-anchor half of
  test/guard fails identically on a9a5f60 — a stale anchor, not a
  regression. As in the retention finding above, a kill-race parks the TM
  (in-place recovery fails, later opens get -28) until rmmod + modprobe;
  after the reload the full Parakeet const-cache contract is all-green
  bit-exact.
- Boost A/B, interleaved 3 reps, all gates bit-exact in both arms:
  whole-encoder single submit does not move (440.0/440.8/441.4 vs
  441.0/440.7/441.0 ms/iter) — one 14.7 s submit is kicked once and the
  QoS request drops 100 ms later, and the job is compute-bound. The 440
  vs macOS 158 ms whole-encoder gap stays open; it is engine-side, not
  host overhead (see the encoder-anomaly subsection below). The island-submit
  Parakeet pipeline does move: encoder_ane 1437.5-1439.5 vs
  1640.3-1768.0 ms (-12.4% on medians), decoder_load 54 vs 77 ms, total
  2227-2250 vs 2466-2612 ms (-9.5%).
- cpufreq sampled at 50 ms: with boost the P-clusters sit at 3036 MHz in
  55% of samples (median 3036); without, 2% (median ~1056).

### T6001: the 440 ms encoder gap is engine-side; the tuning values are unreachable

Source: ane-linux-experiments d733cd1,
`receipts/2026-09-25-jw16-levers6/receipt-encoder-anomaly.md`.

- kprobe on the whole-encoder submit: dispatch is 13-139 us, execute is
  431.5-438.8 ms — the 440 ms run is 99.97% engine window (gold
  `fca96f13` bit-exact x3). Host-side work is not the gap.
- Why T6001 differs: m1n1's `tunables_apply_static` seeds ANE op-point,
  DPE and perf tables for T8103 only (AsahiLinux/m1n1 2abf3af3, from
  eiln). T6001's iBoot-preloaded firmware self-manages from a low default
  op point. The T6001 ADT ladder tops at 1500 MHz (300-1500, with mV
  values); 1500/540 = 2.78, and with a T8103-class 1.25x residual that is
  roughly the observed ~3.1x — labeled inference, not measured.
- The T6001 values are in no reachable artifact: m1n1 ships T8103 units
  only, the live ADT ane0 node has no tunables (checked against
  EmbeddedDeviceTrees), and a literal scan of the mac13j kernelcache
  matched none of 617 candidates. Unblock is one captured write set:
  either a recoveryOS SIP window with `ane-perfstate.d` on the M1 Max
  macOS side, or a physical USB serial for an m1n1 trace.

### T6001: ANE perf-state live capture decoded

Source: ane-linux-experiments a3641215,
`receipts/2026-09-25-t6001-perfstate-capture/`.

- Live fact from a 25G83 capture: `ApplePMGR::_setPerfState` is the live
  ANE path on T6001, with ANE domain 8 (36 probe hits with a2=8) — the
  same domain as T8103. This supersedes the 22G74 static read of accepted
  domains 1-5/13 recorded in the sibling-PMGR note above; static reads of
  the accepted-domain set can mislead.
- 913 `writeReg32` writes captured inside the domain-8 gate, across 10
  RegMap/reg targets: map0 0x1e8 ladder x430 (the ANE PS/PLL ladder
  steps), map2 0x3c0 ladder x180, map2 0x64000 power gate, and map113
  0xa00 writes = BIT31|N perf-controller tokens. RegMap-to-PA decode is
  assigned to the clock lane; once it lands, the Linux driver write set
  is fully determined (a scaffold exists; it needs a scoped PMGR-map
  extension, since the targets are PMGR-mapped).
- The T6001 ANE tunable-table lead is closed: not statically present in
  any obtainable container (26.6.2 KernelCollections are x86 stubs; the
  boot payload is transient) — 0865be7/0af98f6.
- The cache sweep is now systematic (Jw16Levers5, ane-linux-experiments
  0865be7; interpretation amended in 0af98f6,
  `receipts/2026-09-25-ane-tunables-static/`): the T8103 13.5 cache
  `__DATA` holds 179 tunable tables in a 16-byte-record
  `{offset, clear, set, pad}` + zero-separator format, and they belong to
  `com.apple.ApplePMGR` (kmod_info at the region head), not the ANE kext.
  Five of m1n1's eight ANE sequences are present (pmgr x2, dpe_sys x15,
  perf x4, dpe_soc x3 copies); m1n1's ane_dart/ane_dapf ANE sequences are
  absent entirely. Value semantics, corrected: the perf and DPE set
  values in the cache are zero runtime-fill templates (perf 45/45,
  dpe_sys 16/16, dpe_soc 96/96 zero) — m1n1's nonzero published values
  are runtime captures of one machine's state, origin unexplained, not
  normalizations of static data. Only the ane_pmgr sequences carry
  static set values, with copy-to-copy variance.
- Consequence for T6001 and T6021: neither chip shows any ANE tunable
  table in any segment (T6001 22G74 stub + 25G83 base; T6021 13.5 +
  27.0). That is consistent with m1n1's T8103-only ANE coverage and the
  firmware-driven ANE on T600x/T602x, and it closes the static route: a
  static {base, offset, clear, set} list for those chips cannot come
  from these containers.
