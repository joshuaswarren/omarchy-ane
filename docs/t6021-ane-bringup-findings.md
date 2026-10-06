# T6021 (M2 Max) ANE bring-up: findings as of 2026-09-27 (legacy ChMan section added; older sections 2026-09-25)

> **Update 2026-09-29.** The blocker sections below that say no command
> completes and no inference exists are superseded. The firmware runs from
> the autoloaded `ane_t6021` module and executes H14 programs (add, mul,
> relu, scalar ops, clip, matvec up to 2048x5120) on the valid lanes of
> each surface. Current record:
> [receipts/2026-09-29-t6021-installed-path](../receipts/2026-09-29-t6021-installed-path/README.md).
> The older sections stay as history until they are rewritten.

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
  IM4P (LZFSE), byte-identical to the IPSW member (sha256 `ea70fb77...`,
  25,883,676 B; re-verified on the ESP 2026-09-28); the decompressed fileset
  is sha256 `9615a486...` (local copy:
  `/var/tmp/t6021-kc/kernelcache.t6020.13.5-22G74.macho`, hash-verified). It
  contains `com.apple.driver.AppleH11ANEInterface`.
  **Filename trap:** `ane-linux-experiments-parakeet-perf/receipts/2026-09-18-t6021-engine-layout-mined/kernelcache.release.mac14j`
  is a different image (sha256 `5e11f97a...`, the 26A428 mining copy) — same
  name, not 13.5. Hash-check before use; this is the 09-28 firmware-mismatch
  failure mode.
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
   place. T6021 only: the T6001 runs were unmapped (the remap finding is
   documented in section 3 above).
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

### iBoot's runtime patches are the whole difference between the preload and the file

On 17 pre-Linux captures (2026-09-27) the preload differs from the 13.5
archive in exactly these places: the TEXT u64 at vm 0x423c (the DATA base
IOVA, 0x100000c4000), five `__rtk_patch` records (stack guard, `RTK_soc`,
`RTK_soc_revision`, `RTK_cpu_physical_address`,
`RTK_cpu_wrapper_physical_address`) and the first 0x1e8 bytes of
`__rtk_platform_asc_tunables_block` (24 ASC register tunables). Only the
stack guard changes between boots. The Mach-O vm layout already equals the
iBoot placement. So the earlier staged-copy trials ran an image with
`RTK_soc` 0xffffffff, zero ASC addresses and DATA base 0: they never ran
what iBoot runs. `ane_fw_apply_boot_patches()` writes these fields into the
driver's own copy; with them, the copy equals every capture byte for byte
(guard aside). `fw_alias_reserved=0` runs that copy and needs no preload
address or reserved memory. No T6021 has run it yet (receipt
`receipts/2026-10-01-t602x-independent/README.md`).

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
dropped by silicon. The unlock protocol (M2PreRunRE; m1n1): write 0x2 to
UNPROTECT (0x285800204), verify PROTECT
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
ane-linux-experiments:kext-re/FINDINGS.md (AppleH11ANEInterface 9.512.0,
macOS 25G83 build). T6001-derived.

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
ane-linux-experiments:kext-re/DART-DECODE.md. T6001-derived.

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
  RegMap/reg targets. Final decode (receipt final 4e221b34): the only
  ANE perf writes in the path are the DVFS pair — DVFS_CMD at PA
  0x400004A00, value = `0x80000000 | (prev << 4) | new`, driving a
  6-entry ladder with state 5 = 1500 MHz, and DVFS_ON at PA 0x400006000
  (0/1). Bridge arithmetic: raw ADT reg[113] 0x200004000 is pre-bridge;
  absolute IODeviceMemory[113] = 0x400004000, length 16384. The two
  hottest targets are not ANE perf hardware: map0 0x1e8 is the ps_afr
  pwrstate and map2 0x3c0 the ps_gfx pwrstate (x430 and x180) — plain
  pwrstate layout (AUTO_ENABLE 28, PS_AUTO 27:24, PS_MIN 19:16, target
  3:0). With the RegMap-to-PA map resolved, the Linux driver write set is
  fully determined (a scaffold exists; it needs a scoped PMGR-map
  extension, since the targets are PMGR-mapped).
- The DVFS pair is PMP-served (PmpDvfsResearch, d54d714b,
  `receipts/2026-09-25-pmp-dvfs-map113/`): PA 0x400004000 is the pmgr
  reg[113] window (raw ADT 0x200004000 pre-bridge) in the die-IO band —
  not inside any T6001 PMP PIO range (those are 0x282000000 /
  0x304000000 / 0x383000000 / 0x402000000; it is the T6021 PMP whose PIO
  range covers 0x400000000). The tie to the PMP is functional:
  /arm-io/pmp is an iop,ascwrap-v4 coprocessor (ASC
  0x28ec00000, SRAM 0x28e700000) whose iop-pmp-nub ADT `dvfs-domain`
  table manages DCS, FAB, AFR, SOC0, SOC0_ANE_SYS, SOC0_AVD, DISP and
  AVEMSR, with pmgr flagging ANE_SYS notify_pmp=1. DVFS_CMD and DVFS_ON
  act only while the PMP firmware runs: macOS boots ApplePMPFirmware
  (RTBuddy role PMP); Asahi leaves the pmp node disabled, so under Linux
  the window is dead (reads 0, writes no-op). The 22G74 side is decoded
  too: its BIT(29) ORR is a value flag on a 10-entry PS-reg table and
  never touches map113, and 22G74 setPerfState accepts 1-5/13 — domain 8
  is 25G83-only. T8103 has no PMP and no such window; T6021 has the same
  PMP subsystem but no 0x400004000 window (its nub table shows AFR at 6
  levels; the ladder is not located). Linux route, receipt section 7:
  probe the parked-PMP-firmware question, enable the pmp node with
  CONFIG_APPLE_PMP and the ADT tunable payloads, then replay the
  captured token stream idle-safe.
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

## 20. 13.5 (22G74) legacy ChMan transport — verified evidence (2026-09-27)

The T6021 ANE bring-up splits along the macOS-Asahi-stub's preloaded
firmware version. The 13.5 (22G74) selene image
(`a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc`,
sha-256) does NOT speak RTKit HELLO when brought up by the mainline
`apple_rtkit` path — its post-DONE contract is the legacy ChMan
sequence, which the macOS kext pairs with the IORegistry-started host
server (SHAREDMALLOC/TERMINAL). The driver exposes `legacy_only=1`
(default OFF, 0444) as the *experimental* path that takes the legacy
contract and excludes every RTKit surface. Verified state as of
`45dc9a7`.

### What the legacy ChMan probe path proves

The host ack (legacy P8: SCRATCH3 ← `0x08042006`, post-DONE) causes
the parent poll (6c80..6ca4) to EXIT. The discriminator run
(`artifacts/M2Runtime/legacy-discriminator-20260927T152309/`,
dmesg-ane-complete.log, 2185 lines) shows:

- **Pre-ACK POST** (217–218 s after module probe): CSharedMemory
  `0x4f86d8 = 0`, encode pair `0x4f86e8/f0 = 0`, CIPSynchro
  `0x4f8558 = 0`, CTaskPool `0x4f8710 = 0`. These zeros are
  *negative-ambiguous* (BSS sweep not yet executed); they cannot
  prove "ctor did not run".
- **ACK** at 218.6 s (LEGACY P8).
- **Post-ACK dump** at 227 s: CSharedMemory::instance
  `0x4f86d8 = 0x2000681660`; encode pair `0x4f86e8/f0 =
  {0x21bf0, 0x21c14}` — exactly the `EncodeLegacy` /
  `Encode5bPacking` slots M2Protocol decode predicted;
  CTaskPool::instance `0x4f8710 = 0x2000696148`.
- **Nonzeros are proven this-boot** (lean rule, M2Protocol): the
  parent poll EXITED and ctor1 COMPLETED after the ACK. First
  proven post-DONE fw progression on Linux.

A subsequent same-settings run
(`legacy-envfields-20260927T154327`) with the experimental
RESERVED-ENV dump row added narrowed the post-ACK park:

- **ENV+278** (CSharedMemory-returned ptr): `0x2000681660`,
  captured once at the post-ACK tagged ioread dump window —
  ctor1 returned, paired with the discriminator result.
- **ENV+290** (CDebugAgent ctor store): `0`, captured at the same
  tagged dump window — store at 6d64, before any CIPSynchro code
  path. This is the only ENV-window observation. There is no raw
  second ENV re-read; do not extrapolate the park across time from
  the dump window alone.
- **ENV+298** (validator): `0x1b3d4440`, positive control
  (unchanged pre-ACK vs post-ACK).
- **SCRATCH3** at the same dump window: `0x08042006`. A separate
  SCRATCH3 re-poll observed the same value at +10 min (raw second
  read, NOT an ENV-window read; it does not extend the env+290
  observation).

SUPERSEDED INTERPRETATION (2026-09-27, ring-owner run): the park was
NOT a timer/scheduler stall and NOT a ctor2 body wait. The firmware's
DebugTask was consuming the first slot of the zeroed `BUF_H2T` ring
as a real command and faulting (captured context: ELR `0x128c0`,
FAR `0x4`, x0 = 0; exception globals `0x4fab80 = 1`, `0x4fabb0 = 4`
post-ACK; SCRATCH3 stuck at `0x08042006` because the clear at
6d84..6d98 never runs after that fault). With the host initializing
every H2T ring slot to ownership `1` before the ACK
(`ane_t6021_chman_host_init()`, `dma_wmb()`-ordered before the
SCRATCH3 store), the fault does not occur: post-ACK exception globals
`0x4fab80/0x4fabb0/0x4fabb8 = 0` and **SCRATCH3 clears to
`0x00000000`** — the 13.5 post-DONE sequence completes end to end.
Receipt: `receipts/2026-09-27-t6021-ring-owner-h2t-init.md`.
Handshake-only: no CSNE command, no inference, no program load.

### Backing-envelope contract

- `fw_extra_ram = 0x200000` (2 MiB, 16 KiB-aligned at
  `ANE_T6021_FW_ALIAS_PAGE = 0x4000`) + DMA32 — only verified
  envelope.
- Both alias modes map the whole allocation at the entry: the
  reserved mode maps SEG0, SEGi and the owned tail; own memory
  (`fw_alias_reserved=0`) maps the whole staged copy. So the grant
  does not need `fw_alias_reserved=1` (corrected 2026-10-01, receipt
  `receipts/2026-10-01-t602x-independent/README.md`).
- The bound and alignment check is the probe-top predicate
  `ane_t6021_fwload_options_ok()`, which `ane_rtclient_probe` calls
  before `devm_kzalloc`/power; `ane_t6021_fwload_probe()` repeats it
  at the alloc site as defense in depth.

The earlier `0x1800000` (24 MiB) invocation was rejected at the
prior code's late fwload alloc site (after `devm_kzalloc` and after
the power raise had completed), NOT at probe top. The actual insmod
log for that boot was lost to a recovery cleanup
(M2Runtime, 2026-09-27). What the code PREDICTED then and predicts
now is the same -EINVAL; what was OBSERVED is a single box
hard-hang whose cause is undetermined and unrelated to the RAM
grant — DMA size is not implicated in this publication's evidence,
and the historical log cannot prove which side of the alloc
actually fired. Do not lower the 16 MiB cap or loosen the 16 KiB
alignment without independent verification on a new boot (with
the actual log captured this time).

### Receipts (in this repo)

- `receipts/2026-09-27-t6021-13_5-legacy-only-publish.md` —
  full provenance, param contract, run list, and what is NOT in
  this publication.
- `receipts/2026-09-27-t6021-ring-owner-h2t-init.md` — the
  ring-owner fix: H2T slots host-initialized before the ACK, the
  SCRATCH3-clear verification, and the superseded-park decode.
- Boot IDs and box identities are intentionally omitted; the runs
  were on the single T6021 testbed the program uses.

### Out of this publication

- The legacy ChMan host server (SHAREDMALLOC/TERMINAL). Root
  review flagged three defects: wrong acquire ordering
  (`dma_rmb` AFTER the `hdr+a1+a2` loads), unchecked ring
  offsets/size/bit before deref and modulo, and the TERMINAL
  cursor never returning the slot to the producer. It will land
  under a separate commit when those are addressed.
- Lowering `SZ_16M`, loosening the 16 KiB alignment, dropping the
  reserved-alias coupling when `fw_extra_ram > 0`. The probe-top
  predicate makes each one explicit; lowering it requires
  independent verification.
- Live inference (no CPU tensor fallback used here; the
  installation lifecycle for inference is untouched).
- The 14.x / 15.x / 26 / 27 firmware paths. The `legacy_only`
  contract is structural against the sha-pinned 13.5 (22G74)
  selene image only.

## 21. Stock kernel, packaged overlay, ANE mailbox (2026-09-30)

Record: [receipts/2026-09-30-t6021-stock-mailbox](../receipts/2026-09-30-t6021-stock-mailbox/README.md).

- The stock linux-asahi kernel `7.1.13-3-1-ARCH` runs the ANE; the poll-TX
  kernel is not needed. The mailbox node needs a second, never-firing
  `send-empty` IRQ (AIC2 1833) for stock apple-mailbox to bind.
- `packaging/dt/t6021-ane.dts` (omarchy-ane `9c925cd`) on the pristine
  package DTB gives the lab tree again (phandles normalized, only
  `__symbols__` differs), and the M2 passed on it: 20/20 gate loads and a
  60 s four-worker burst, 680/680 exact. dart0 sits in the pmp domain as in
  the lab tree; `ane_sys` was never booted.
- On the 13.5 firmware the host sends no mailbox message after probe (RTKit
  HELLO times out; commands use the ChMan ring and the IPI doorbell), so
  the A2I FIFO cannot fill.
- AIC2 884 (`mailbox-recv`) stormed at about 700,000/s while the mailbox was
  started (about one CPU of hardirq time), and it caused the episodic latency
  stalls. `hello_wait_ms` now defaults to 0 (omarchy-ane `4648648`): the
  mailbox never starts and 884 never fires. add p90 fell from 95-152 ms to
  1.29-1.42 ms; a 30 s loop ran 4,787 processes instead of 203. The I2A FIFO
  was empty during the storm, so 884 is not I2A-not-empty (inference: it is
  the ANE doorbell line).
- io BOs the firmware saw used to stay held until reboot, capped at 2 GiB
  (`ANE_T6021_BO_TOTAL_MAX`), so a boot ran out after about 14,500 add
  processes. They now go to a pool at the last reference and are reused
  after zeroing (omarchy-ane `b17f49b`); program sections stay held. 105,232
  add processes ran exact on one boot with used memory flat within 88 MiB
  ([receipts/2026-09-30-t6021-bo-pool](../receipts/2026-09-30-t6021-bo-pool/README.md)).
- The total BO cap is now the parameter `bo_total_max_mb` (12288 MiB by
  default, omarchy-ane `62f04c6`); `bo_total_bytes` shows the counted bytes.
  The 32-bit DMA mask still limits all BOs to 4 GiB of IOVA, so `BO_INIT`
  fails with `ENOMEM` before the cap. One boot loaded and ran all 38 Qwen
  programs once each, with 2.63 GiB counted at the end
  ([receipts/2026-10-01-t6021-bo-cap](../receipts/2026-10-01-t6021-bo-cap/README.md)).
- The disk boot path works on the test laptop since 2026-10-01 with the
  opt-in `uboot-serial-stdin-t6021` overlay and the lab m1n1 stage 2
  ([receipts/2026-10-01-t6021-disk-boot](../receipts/2026-10-01-t6021-disk-boot/README.md));
  see section 26 for the 0.4.0 module on that path.
- `ane-run --ports` binds the port table on the device and refuses a port
  whose surface is larger than its io BO (`49eb8ed`). A BAR slot is the
  HWX program-descriptor resource at `+0x10 + 0x10 * slot` (0 text, 1 kernel
  constants, 3 `__DATA` scratch, 4.. io surfaces), so a port larger than
  16 KiB is one slot and one BO. All 38 regenerated Qwen tables pass the
  size gate and `--dry-run`; the port build takes several outputs and the
  slot-3 scratch, and refuses a task-stream slot the table does not bind
  ([ports-sizes.md](../receipts/2026-09-30-t6021-qwen-chain/ports-sizes.md)).

## 22. CALL completion: the IO_T2H finish event (2026-09-30)

Record: [receipts/2026-09-30-t6021-call-wait](../receipts/2026-09-30-t6021-call-wait/README.md).

- The last-committed-TD word (TM +0x20458) holds the call's nid (bits
  23:16, +1 per call) and the index of the last task taken (bits 15:0; a
  task header carries its index in word 0 bits 15:0). It is not a per-TD
  counter: a 2-task matvec moved it from 0xe0000 to 0xf0001.
- That word, the eight TQ status words and the firmware's first IO_T2H
  event mark the dispatch of the last task, not its end. Qwen program 20
  (20 tasks, 84 MB of constants) showed them 0.22 ms after the ack and ran
  on for about 3.3 ms, so one call read an all-zero output.
- The firmware posts two IO_T2H events per CALL (0x28-byte payload: u32
  sequence, u32 0x300, u64 the CALL cookie from +0x20, u32 program id,
  u32 process id, u32 0, u32 state). State 0 marks the dispatch, state 1
  the finish: 3.5 ms after the ack for program 20, 0.22 ms for add. A CALL
  now waits for the state-1 event (omarchy-ane `e794c4a`); the wait reads
  no TM or pmgr register.
- Program 20 then matches the M1 golden with one call: relative L2
  0.00117, 50 of 50 runs identical.
- The finish event is also the output-landed signal. With no sleep after
  it, 6000 add calls with new inputs per call each read their own result
  right after the ioctl returned, program 20 (10 one-call processes) and
  the Parakeet encoder stayed bit-identical, and dmesg stayed clean. The
  1 ms post-call settle (`call_settle_us`) was the whole fixed cost: H14
  add went from 1.478 to 0.308-0.319 ms median per call, the ledger ANE
  cell from 674 to 3,206 jobs/s (boot `772d212d`, aurora 11.36).
  `call_settle_us` now defaults to 0
  ([receipts/2026-10-06-t6021-call-settle](../receipts/2026-10-06-t6021-call-settle/README.md)).

## 23. Qwen per-program conformance against the M1 (2026-10-01)

Record: [receipts/2026-09-30-t6021-qwen-chain/conformance.md](../receipts/2026-09-30-t6021-qwen-chain/conformance.md).

- All 38 Apple-compiled Qwen programs match the M1 outputs of the same
  decode step on boot `0e2c3743` (module `a584a967`). That covers six steps
  with the M1 inputs, and 0→1→2 and 11→12→13 with the resident state chained
  on the M2: 456 of 456 runs pass, and the largest relative L2 over 2,304
  outputs is 0.007. The pass bound for each output is twice the M1's own
  error against a float64 evaluation of the MIL program, and at least 0.02.
  The 18 DeltaNet programs are bit-exact.
- All 114 port-table ambiguity groups resolve on the device to the generated
  table order. The next permutation is at least 7.4 times the bound.
- Every Apple-compiled Qwen ANEC header records one output. libane checked
  the M2 send/read index against that header before cb5a8f1, so
  `ane-run --ports` read only output 0 and wrote heap garbage for the rest.
  Device writes, the kernel io binding and the BO pool were correct.

## 24. The whole Parakeet encoder as one H14 program (2026-10-01)

Record: [receipts/2026-10-01-t6021-parakeet-encoder](../receipts/2026-10-01-t6021-parakeet-encoder/README.md).

- Apple's h14 cross-compile accepts the encoder's ANE-segment MIL (the
  compile input of the H13 whole-encoder HWX) whole: one program, 3,597
  tasks, 448 MB of palettized constants, a 61 MB scratch at BAR slot 3.
- On the M2 it runs as one call. The output is bit-exact with the golden
  macOS capture (fp16 sha256 `fca96f13…`), and the greedy TDT decode gives
  the golden 104 tokens.
- Exec time is 254.4 ms per call. The M1 under Linux runs the h13 build in
  about 139.4 ms; CoreML on the M2 under macOS 27 runs its own build in
  90.6 ms. The numerics are correct, so the gap is in how the engine runs
  the program; section 25 splits the time by task family.
- libane's port-table build no longer caps the task count at 128; that bound
  belongs to the derived build only. An HWX LC 0x40 record holds the tensor
  name from +0x18 and grows in 8-byte steps with it (0x20, 0x28, 0x30 here),
  so a name longer than 8 bytes needs the whole record.

## 25. Per-task timeline of the encoder: trace_td (2026-10-01)

Record: [receipts/2026-10-01-t6021-trace-td](../receipts/2026-10-01-t6021-trace-td/README.md).

- `ane_t6021.trace_td` (runtime switch, default 0, omarchy-ane `7ae53e0`)
  samples the last-committed-TD word every 20-40 us during the completion
  wait, under the PS-word guard, into debugfs `ane_t6021/trace_td`
  (ane/t6021/ane_t6021_rtclient_main.c, `debugfs_create_blob("trace_td", ...)`). Trace on
  changes the encoder exec time by less than 0.1%.
- The TD word moves when the task manager takes a task. The manager keeps
  19 tasks in flight: task k+1 is taken when task k+1-19 finishes (r 0.985
  with the weight bytes of that task in Qwen prog_006, 0.871 with the
  activation bytes in the encoder). The IO_T2H state-0 event comes 0.2-0.5 ms
  after the ack whatever the program length.
- The encoder timeline is the same in every call; no firmware pacing shows.
  Linear and conv layers with weights take 41% of the call at about
  2.2 TMAC/s (estimate). The 120 attention-shape tasks (relative positions,
  inferred) take 30% at about 14 GB/s of activations. PE-only tasks take
  23%. The smallest tasks run in about 3 us, so a fixed cost per task is at
  most 5%.
- The clock (or core use) can still explain the linear layers; activation
  traffic explains the rest.

## 26. The 0.4.0 release module and the #23 overlay on a disk boot (2026-10-01)

Record: [receipts/2026-10-01-t6021-release-boot](../receipts/2026-10-01-t6021-release-boot/README.md).

- The release-built `ane_t6021.ko` (sha256 `54c1da56…`, 0.4.0) and a boot.bin
  built by the packaged path from the #23 overlay (j414c tree `c31a54c3…`)
  booted together from the disk. The three ANE DARTs read `status = "okay"`
  in the live tree and bind to `apple-dart`; the full device gate, the
  whole encoder (bit-exact, 254.3 ms) and a 60 s four-worker burst pass.
- On a kernel without ANE nodes, the #23 change adds only the three status
  properties to the tree. The kernel case #23 was written for (nodes shipped
  disabled) has not been booted.
- The module bytes depend on the build directory: the path is in the module
  10 times. A rebuild of the same source in another directory gives a
  different sha256 with the same srcversion and `.text`. A DKMS build will
  not reproduce the release sha; compare srcversion and `.text` instead.

## 27. The M2's native encoder program under Linux: H_driver (2026-10-01)

Records: [receipts/2026-10-01-t6021-native-macos](../receipts/2026-10-01-t6021-native-macos/README.md)
(macOS side) and [receipts/2026-10-01-t6021-native-vs-cross](../receipts/2026-10-01-t6021-native-vs-cross/README.md).

- The M2's own macOS 27 compile of the whole encoder (HWX `6430da18…`, 3,597
  tasks) runs under our driver in 253.10 ms. The Mac Studio cross-compile
  runs in 254.27 ms in the same session (20 interleaved blocks per arm, R =
  0.9954). Both are bit-exact with the golden. macOS runs the native build in
  89.3 ms.
- Qwen prog_020 and prog_006 give the same result: the native build (one
  task fewer) runs within 0.3% of the cross build, with byte-identical
  outputs.
- So the compiled program is not the cause of the 2.8x gap to macOS. The
  cause is on the Linux side. Untested candidates: the ANE operating point
  (the live tree of boot `65d832d5` has `pmp@28e700000` disabled, so no PMP
  runs), the DPE/PPT limit (the driver's only CH_PROPERTY_WRITE is property
  0x10aa, `ane/t6021/ane_t6021_rtclient_main.c:1565` at `9f37b47`; 0x1701 is
  never sent), and the firmware build (`asahi,os-fw-version` 13.5 under
  Linux, macOS 27.0 under macOS).

## 28. The macOS ANE0 AXI2AF bridge tunables: applied, no speed change (2026-10-01)

Records: [receipts/2026-10-01-t6021-af-bridge](../receipts/2026-10-01-t6021-af-bridge/README.md)
(table and probe) and [receipts/2026-10-01-t6021-af-bridge-run](../receipts/2026-10-01-t6021-af-bridge-run/README.md).

- Under Linux none of the 26 bridge registers at 0x284000000 + 0x000..0xa00
  holds the macOS value (`AppleT6020PMGR::applyBridgeTunables`). Two of them
  carry our P-1 values (0x000 = 0x10, 0x400 = 0x40010001); the other 24 read
  the same values on two boots before any write, so they are most probably
  power-on values.
- The iBoot state cannot be read: on a `fw_start=0` boot the seven ANE
  islands are off (PS word 0x300) and only `ane_sys`/`ane_cpu` are on.
- The test-only `ane_t6021` parameter `af_bridge_macos=1` (code `d3b8561`,
  removed from main after this result) writes the 26
  read-modify-writes at the start of P-1, in place of P-1a and P-1d. All 26
  read back as macOS writes them (0x400 = 0xc0f10010, as in the 13.5 hv
  trace), the firmware boots, and every output stays bit-exact.
- The encoder time does not change: 254.274 ms against 254.276 and 254.215
  ms on default boots before and after (prog_020 and prog_006 within 0.1%).
  The bridge tunables are not the cause of the gap to macOS. The section 27
  candidates remain.

## 29. macOS vs Linux MMIO footprint: the DART tunables (2026-10-01)

Record: [receipts/2026-10-01-t6021-macos-vs-linux-mmio](../receipts/2026-10-01-t6021-macos-vs-linux-mmio/README.md)
(offline; no device run).

- The only DMA-path setting in the 13.5 hv trace that Linux never makes is
  the DART tunables on dart-ane1 and dart-ane2 (ADT instance names DARTBRD
  and DARTBWR): 0x20c, 0x220, 0x224, the 0x300-0x310 DVA window and 16
  per-SID words at 0x800. The trace, the 26.6.2 j414c iBoot tables and the
  live macOS 27.0 ADT agree on the values. Linux `apple-dart` writes none of
  them. On T8103, m1n1 writes the same 0xf0f0f/0x80808 pair to the ANE DART.
- macOS translates the ANE stream the same way Linux does: TCR[0] = 0x9,
  16 KiB pages, bypass on sid 15 only. Each TLB entry covers the same 16 KiB
  on both systems. Of the DART state the trace shows, only these tunables
  differ for the ANE data stream (sid 0).
- Linux writes 10 bridge-page words at P-1 (P-1b, c, e-l; seven carry
  m1n1's T8103 values) that macOS never writes.
- The hv trace has no firmware start and no job (kernel-only guest, lazy ASC
  start), traces writes only, and does not cover the PMP, DPE, DAPF, DCS or
  fabric. It cannot test the PMP and DPE/PPT candidates of section 27.
- Next: read the DART tunables and PERF counters on Linux and on macOS, then
  an A/B of the BRD/BWR tunables with the NativeVsCross method.

## 30. The ANE DART tunables under Linux: read, applied, 0x20c breaks translation (2026-10-01)

Record: [receipts/2026-10-01-t6021-dart-tunables](../receipts/2026-10-01-t6021-dart-tunables/README.md).

- Under Linux, dart-ane1 (BRD) and dart-ane2 (BWR) keep their reset values:
  0 of the 22 ADT tunable words on each holds the macOS value. dart-ane0
  (LLT) already holds its 6 macOS values; a stage before Linux writes them.
- The m1n1-named DART PERF words (0x700-0x788) read 0 and stay 0 over 21
  encoder CALLs. The counters are not enabled, so there is no TLB miss count.
- `ane_dart_probe apply=1` wrote the 19 macOS words per bulk DART on the
  live DART (no flush), every readback equal. The next gate failed: `NO PTE
  FOR IOVA` faults on BRD and BWR stream 0, and every second process gave an
  all-zero output. A reboot restores the reset values.
- In the macOS order (streams off, TLB flush, words, flush, streams on), by
  group: 0x220/0x224 and the 32 SID words apply with 0 faults, alone and
  together, and do not change the speed (encoder 254.3 ms, drop +0.00% to
  +0.03%). Rejected.
- 0x20c alone breaks translation in that order too: `NO PGD FOR IOVA` on BRD,
  then the CALL hangs and every program load times out until a reboot, also
  after 0x20c is written back. It stays untested for speed.
- 0x20c can only go in at DART init, before TTBR and ENABLE_STREAMS: in
  `apple_dart_hw_reset()` (kernel build). apple-dart is built in with
  `suppress_bind_attrs`, so no unbind/rebind; m1n1 cannot help because
  `ane_cpu` is off at handover and the DART resets at power-up.

## 31. The ten Linux-only P-1 writes: skipped, no speed change; the DART DVA window (2026-10-01)

Record: [receipts/2026-10-01-t6021-p1-groups](../receipts/2026-10-01-t6021-p1-groups/README.md).

- P-1 writes 12 words in the engine page; the macOS 13.5 trace never writes
  ten of them (0x038, 0x03c, 0x600, 0x738, 0x798, 0x7f8, 0x900, 0x410,
  0x420, 0x430). The test-only parameter `p1_groups=0x1f` (code `46636d3`,
  branch `agent/m2-p1-groups`, not merged) skips all ten. The firmware still
  boots, every output stays bit-exact, and the encoder time does not change:
  254.147 ms against 254.349 and 254.121 ms on default boots before and
  after. Rejected; no per-group arm ran.
- Without P-1 the ten words read 0 (0x038), 0xffff (0x03c), 0x100
  (0x410-0x430) and 0 (the other five). 0x600, 0x738, 0x798, 0x7f8 and 0x900
  read 0 also after P-1 writes them: the m1n1 T8103 values leave no readable
  state there.
- DART 0x300-0x310 are ADT tunables of all three ANE DARTs: 0x300 bit 0 on,
  0x308/0x310 = the IOMapper range [0x100_0000_0000, 0x400_0000_0000) in
  4 KiB units. LLT holds them; on BRD and BWR bit 0 is off and the bound
  words change from boot to boot. BO IOVAs sit below 4 GiB (32-bit DMA
  mask); a 42-bit mask would put them inside the window.

## 32. Own memory is the default: device gate with the lab and the stock m1n1 (2026-10-01)

Record: [receipts/2026-10-01-t6021-default-on-gate](../receipts/2026-10-01-t6021-default-on-gate/README.md).

- The firmware runs from a driver-owned copy with iBoot's runtime patches
  replayed (`fw_alias_reserved=0`, the default since #49), aliased at the
  latched entry 0x10000000000 (448 DART pages). It starts like the reserved
  preload: READY at 4.2-4.3 s, DONE at 4.4 s, the same ane_t6021 kernel lines
  except the two fwalias lines.
- With the stock m1n1 1.6.1 stage 2 and the packaged boot.bin (no
  `ane-firmware` reservation), the M2 Max passes the 16 gates, the encoder is
  bit-exact (254.318 ms median of 20 calls), and all outputs are
  byte-identical to the reserved mode. Only the reserved mode still needs the
  lab m1n1.
- The marker contradiction of the 2026-09-27 staged trials did not occur in
  the three own-memory boots. [INFERENCE] Those trials ran a copy without
  iBoot's patches (receipt 2026-10-01-t602x-independent); the replay closes it.

## 33. The DART tunables at DART reset: 0x20c faults in the macOS order too (2026-10-02)

Record: [receipts/2026-10-01-t6021-dart-kernel](../receipts/2026-10-01-t6021-dart-kernel/README.md).

- `kernel/patches/apple-dart-ane-tunables.patch` (asahi-7.1.13-3, parameter
  `apple_dart.ane_tunables`, default off) writes the 19 macOS words per bulk
  DART in `apple_dart_hw_reset()`: TTBRs cleared, full TLB flush, the words,
  then streams and TTBR, as macOS does. The kernel is a cross build of the
  package source and config; with the parameter off it runs the encoder at
  the stock time (254.362 vs 254.378 ms minmin) and passes every gate.
- With the parameter on, both bulk DARTs read back all 19 words (0x20c
  0xe40000ff) and the firmware boots, but the first CALL gives `NO PGD FOR
  IOVA` on BRD stream 0 at 0xfd68be00, the CALL times out, and every later
  program load times out until a reboot. Same fault as the live write in
  section 30. 0x20c with the macOS value does not work with the Linux page
  tables in any write order. Rejected as a lever on this path; the only open
  test is 0x20c together with the DVA window and BOs above 4 GiB (E3).
- A stock Omarchy boot deletes every module tree under `/usr/lib/modules`
  that is not the running kernel's and that no package owns
  (`linux-modules-cleanup.service`, kernel-modules-hook): it is moved to
  `.old` and deleted at the next boot. A custom kernel needs its module tree
  copied in by the stock boot right before the reboot into it.
- GRUB on the M2 consumes a `grub-reboot` entry before the boot (tested with
  the stock kernel), and `systemd.watchdog_sec=120` on the kernel command
  line arms the SoC watchdog in the initramfs at about 1.0 s.
