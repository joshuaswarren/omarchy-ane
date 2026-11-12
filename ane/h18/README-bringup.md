# ane/h18: OPT-IN EXPERIMENTAL H18 ANE0 bring-up for the T8152 (Mac mini M6, J873g) and the T8132 (14-inch M5 Max MacBook Pro, K114c)

Nothing in this directory is installed, autoloaded or shipped as working support. Stages dt, psd and power ran once on an M6 (see "Measured"). Stage boot has not run on any machine.
The module never writes IORVBAR or a mailbox. The ANE firmware is preloaded by iBoot (the ADT
`/arm-io/ane` carries `segment-ranges`) and iBoot programs IORVBAR.

## What it does

`ane_h18.ko` binds to the `apple,t8152-ane` node of `t8152-j873-ane.dtsi` and only when loaded with `optin=t8152`.

- `stage=dt` (default): check every device tree window and property against the SoC row (`ane_h18_soc.c`) and log
  them. No mapping and no MMIO.
- `stage=psd`: read only. Map the two PSD slot windows (slots 30 and 31) and read VALID, then STATUS when VALID is
  set. The proven PSD probe does the same two reads. No write.
- `stage=power`: needs `confirm_power=1`. Request PSD state `psd_state` (module default 0, the stage script passes 1) on the first `psd_slots`
  slots (default 1, slot 30 only) and wait for each acknowledge as `drivers/pmdomain/apple/pmgr-psd.c` does. Only
  after every requested slot acknowledged, read CPU_STATUS, ASC_STATUS and IORVBAR (and CPU_CONTROL with `read_ctrl=1`,
  and the Postoffice status word of three candidate channels with `read_mbox=1`). Then request state 6 again unless `keep_on=1`. Every write and
  read address is logged before the access, so a hang leaves the last address in the log.

The module reads CPU_CONTROL (ASC + 0x44) with `read_ctrl=1` and in stage boot. It writes CPU_CONTROL only in stage boot, and writes nothing
else in the ASC, IORVBAR or mailbox windows.

- `stage=boot`: needs `confirm_power=1` and `confirm_boot=1`. Powers the slots like `power`, then refuses unless CPU_STATUS says STOPPED
  (and not RUNNING), CPU_CONTROL.RUN is clear and IORVBAR is exactly 0x10000000001, the value the board showed. Then it writes
  CPU_CONTROL with RUN set (the one write to the ASC window), reads CPU_STATUS every 10 ms for `boot_wait_ms` (default 300) and the
  status words of the three candidate Postoffice channels, reads the outbox of any channel that holds a message (at most 8 messages,
  raw 16 bytes logged), writes CPU_CONTROL with RUN clear and waits for STOPPED. If the core does not stop, the PSD slots stay on
  and a reboot is required. The module sends no mailbox message unless `rtkit_hello=1` is given.
  - `alias_mbox=1`: also read the status word of the alias-bank Postoffice channel (window + 0x10000, channel 2 * mailbox number, 0x316010c20 for
    ANE0) once before RUN and on every poll, and read the message when its count is not zero. The channel layout is the one the kernel mailbox driver
    uses on this SoC: RX at +0x00, TX at +0x10, status at +0x20. A message moves as one paired 16 byte access. The raw words are logged.
  - `read_dart=1` (needs `psd_slots=2`): before RUN, read twelve words of the ANE DART (instance 0): config, version, the stream-enable word, and
    the stream configuration words of streams 0, 10 and 13 and the TTBR words of streams 0, 1, 10, 13 and 14. Read only, each address is
    logged before its read, so a hang names the word. It can wedge the machine: the block has manual availability, the macOS driver does not
    touch it before forcing it available, and a read of such a block hung the USB DARTs on this board before their power chain was up. The
    gate on both PSD slots is a hypothesis, not a measured safe condition. Only the first word is read before anything else runs.
  - `map_fw=1` (needs power-domains and iommus in the node): before RUN, attach the ANE to its DART through the iommu API and map the two
    firmware segments the boot loader left in DRAM at their IOVAs: TEXT at 0x10000000000 read only, DATA at 0x100000c4000 read and write
    (the ADT `segment-ranges`). The DART driver programs the DART at the attach. With `read_dart=1` the twelve DART words are read before and after.
    The mapping is released after a clean stop. If the core does not stop, it stays.
  - `rtkit_start=1` (needs `rtkit_hello=1`): after the last endpoint map message, start the system endpoints the firmware announced (STARTEP). When
    the firmware acknowledges the IOP power state ON, answer with AP power state ON. When the firmware acknowledges that, start the application endpoints
    it announced (endpoint 0x20 and above). A buffer request is logged and not answered.
  - `rtkit_stop=1` (needs `rtkit_start=1`): before RUN is cleared, send AP power state QUIESCED (0x10), then IOP power state SLEEP (0x1) when the AP
    acknowledgement arrives, as `apple_rtkit_shutdown` does, and wait up to 600 ms for the IOP acknowledgement. That pair can be restarted. Whether it lets
    the core reach STOPPED is not known. `rtkit_stop=2` sends OFF (0x0) on both sides instead, as `apple_rtkit_poweroff` does. The board stopped the
    core with that pair. The core cannot be restarted after it.
  - `read_scratch=1`: read the eight SCRATCH words of the engine block (engine + 0x18a0024 to +0x18a0040, the registers the shipped system uses for the
    boot-args handshake of this SoC) three times: before RUN, when AP power ON is acknowledged and before the shutdown. Read only, each
    address logged before its read. Whether the block can be read on this board is not known, and a read the block does not answer can hang or reset the machine, as the DART read can.
  - `handshake=1` (needs `map_fw=1`, `read_scratch=1`, `rtkit_start=1` and `rtkit_stop=1` or `2`): the first write to the SCRATCH words. Once AP power ON
    is acknowledged, the module reads the eight words, allocates the surfaces macOS allocates (an IPC surface of `SCRATCH1 + 1` bytes rounded up to a
    page, a HEAP surface of `SCRATCH3` bytes, and a 0x40000 byte pool surface that holds the 0x174 byte init block), fills the block with the layout of `ane/t6021/ane_t6021_boot.h`,
    writes the block address to SCRATCH0 (low) and SCRATCH1 (high), and then writes the wake word 0xf7fbdff9 to SCRATCH7. It waits up to `hs_wait_ms` for a new
    address in SCRATCH1:SCRATCH0 together with 0x08042006 in SCRATCH7, and dumps the first 0x800 bytes at that address (all-zero lines of 16 bytes are counted, not printed) when one of its own surfaces holds it.
    The firmware shows 0x08042006 before the wake, so the answer is judged by the new address. With this option the two firmware segments are mapped into the
    default DMA domain of the ANE (no own domain is attached), because the surfaces come from the DMA API. The config size at block +0x10 is 0x380000, the value the shipped system sets on the M6 (`hs_cfg_size`; T6021 uses 0x500000). Two words the shipped system
    does not give is zero: the template word. The pool qword at +0x60 is 0x40000 as in the T6021 sequencer. The handshake refuses unless SCRATCH7 already shows READY. SCRATCH6 is not
    written (macOS writes 1 there before RUN, the firmware showed 0). A hang, a reset or a DART fault is possible, and what the firmware does with the
    surfaces is not known. The surfaces are freed only after the core stopped. Nothing here has run on the board.
  - `hs_chan=1` (needs `handshake=1`): the step macOS does after it created its channels. The module checks the eight entries of the published table (type 0
    to 2, bit below 32, a ring inside the surface that holds the table) and writes nothing if one entry is wrong. Then it sets every host-to-firmware slot header to 1
    (host owned, as `ane/t6021/ane_t6021_boot.h` does before the wake), writes 0x08042006 to SCRATCH3 and waits up to `hs_wait_ms` for the firmware to clear it.
    The firmware's own restartable stop got no IOP acknowledgement after the handshake alone, and this step may be what it waits for. Not known.
  - `hs_cmds=1` (needs `hs_chan=1`): PRINT_ENABLE (0) and CONFIG_GET as 64 byte slots on the IO ring. The slot header is the DMA address of a command block of 0x100 bytes in the pool
    (id at +4, argument at +8), the doorbell is BIT(bit of the IO entry) written to engine + 0x18a4000, and the answer is the slot header with bit 0 set and a zero
    acknowledgement at +6. The doorbell address comes from macOS behaviour on the M6 and has never been written on this board. CONFIG_GET returns the tick rate at +8.
    START is not sent, because the firmware may ask for memory on the shared-memory ring and this module does not answer that ring.
  - `hs_start=1` (needs `hs_cmds=1`): macOS's order of commands, PRINT_ENABLE, START, CONFIG_GET and RESOURCE_INFO_GET. START has no payload. RESOURCE_INFO_GET is
    a 100 byte command, and its reply is logged in 16 byte lines (macOS copies 16 bytes from +0x20 and +0x50). START starts the firmware's services and can bring
    more memory requests, which the polling answers. On the M6 all four commands were answered in one run and CONFIG_GET returned 0x16e3600.
  - `hs_prog=1` (needs `hs_start=1`, file `prog_fw`, default `ane_h18_prog.bin` under /lib/firmware): run one program on the IO ring. The file is made by
    `tools/ane-prog-pack` from an ANEC with the section builder libane uses on the M2, and the module checks every size and offset in it and requests it before the
    first write. It allocates one DMA surface per section and per buffer, sends LOAD_PROGRAM (0x200, 0x1c0 bytes, six 0x30 byte section records, the program id comes
    back at +0x1b8), CREATE_PROCESS (0x202, process id at +0xc) and PROCEDURE_CALL (0x204, one 0x30 byte element per buffer at +0x60, cookie 0xADD0), reads the
    IO_T2H events (two per call, the second with state 1 when the procedure finished) and gives each slot back with the ring's doorbell bit, logs the buffers,
    then sends TERMINATE_PROCESS (0x203) and DESTROY_PROGRAM (0x201). The wire format is the T6021's, which the M2 driver uses and the shipped system's behaviour matches in command ids
    and sizes. Whether the M6 firmware accepts a program built for the M2's firmware has not been tried: a refusal is logged with the reply, an unanswered command
    or a call without a finish event ends the sequence with no unload, and the buffers are freed only after the core stopped.
    With `read_dart=1` the module also reads the exception state of the ANE DART (instance 0), read only, before LOAD_PROGRAM and after the call. On the first run on the M6 the firmware answered LOAD_PROGRAM, CREATE_PROCESS and PROCEDURE_CALL with acknowledgement 0 for the H14 add program. It posted one IO_T2H event with state 0. No finish event came, and the kernel logged an unhandled ANE DART interrupt.
    `dart_inst` (a mask of ANE DART instances 0 to 3, needs `hs_prog=1` and `read_dart=1`) also reads the instances read only, before LOAD_PROGRAM and after the call. It reads the stream enable, translation control and table base words, the first two exception bitmap words and the records of streams 0, 10, 11, 13 and 15. The ADT lists four DART instances behind one interrupt, and the kernel handler reads only the first. `hs_premap=1` sends PREMAP_BUFFER (0x20d, type 2) for one inference request pool before LOAD_PROGRAM, as the shipped system does at init. Neither has run on the board yet.
  - After the channel step the module polls the doorbell side every millisecond while it waits. That is 100 ms after SCRATCH3 and the whole wait of each command.
    It answers the firmware's requests on the SHAREDMALLOC ring. A MALLOC gets a DMA buffer of up to 2 MiB (at most 16 buffers and 64 MiB) and an ordinal. A FREE
    gets the answer the firmware waits for with no timeout. Each answer rings the ring's doorbell bit. A pending IPI value is written to the ack register at
    engine + 0x18b0000, as the shipped system does. When the pending value or a firmware-to-host ring changes, the module logs the nonzero lines of those rings. A silent
    firmware then still shows which ring moved. The first command on the M6 got no answer before this. The cause is open.
    The command block lives inside the pool surface, 0x400 bytes in, because the shipped system takes every command block from the pool and the first run with a separate block
    got no answer. The terminal ring is logged as text too.
  - `rtkit_hello=1` (needs `alias_mbox=1`): answer the firmware's RTKit management HELLO with version 12 and its endpoint map, nothing else. The
    reply goes to TX as one paired 16 byte store. No reply is sent without a free TX slot, for a message of another type or endpoint, or when the
    firmware's version range excludes 12. At most four replies per run.

With power-domains in the node (the overlay `t8152-j873-ane-dart.dtsi`), the kernel's PSD driver owns slots 30 and 31. The module then
requests no PSD state and no undo. It takes a runtime PM reference in stages power and boot, which powers the slots and fails if the power-on fails,
and releases it after a clean stop. Binding the module powers the domain, so stage dt is no longer free of hardware access with that overlay.
With `iommus` in the node, the iommu core also attaches a default domain to the device when the module binds, in every stage. On the board the DART's
first-use initialisation ran from that attach, at boot, while the PSD group was still at state 6, and the boot was fine. After a stage the group stays at state 1
if the DART holds a reference, so the stage script's check for a slot that did not return to state 6 cannot fire in this mode.

## Why the ANE is powered by PSD slots

The live M6 ADT lists `power-gates`, `clock-gates` and `service-gates` of `/arm-io/ane` as 30 and 31 (ANE1: 32 and
33). Gate values under 170 are PSD slot numbers, and the kernel tree already drives PSD slots for the USB ports and the
GPU (`t8152-j873-psd.dtsi`, `t8152-j873-psd-gpu.dtsi`). The ADT has no PMGR power state for the ANE. A slot window is
0x100 bytes at 0x303a10000 + (slot << 8): slot 30 is 0x303a11e00 and slot 31 is 0x303a11f00.

## Not known

- The PSD state each ANE slot needs. macOS behaviour recorded in the lab receipts names slot 30 ANE0_CPU_BLOCKIN and slot
  31 ANE0_SYS_0, reads "on" as request value 1 (or 2) from the shared threshold lists, and says which value the ANE
  driver passes is not known. The GPU uses 0 for its cluster slot and 2 for its ASC slots, the USB blocks 0. The stage
  script requests state 1 on slot 30 first; `psd_state` takes 0 to 5 without a rebuild.
- Whether the ASC core is running or halted, and what the bits of CPU_STATUS mean on this chip. The ANE also has `clock-ids` 329,
  328 and 375, which this module does not touch.
- The Postoffice channel of this ANE. Two sources on the macOS wrapper driver agree on the alias bank. The node has a postoffice
  property and no alias property. That puts the channel at window + 0x10000 + 2 * 12 * 0x80 = 0x316010c00, with status at +0x20. The plain-bank
  channels 12, 24 and 25 that `read_mbox=1` reads are other apertures, so their zero status said nothing. Which half is RX and which is TX comes from the kernel
  mailbox driver's T8152 layout (RX +0x00, TX +0x10). A third source on the macOS driver names the halves the other way round.
- Whether the core can fetch its firmware at all: nothing here programs the ANE DART. The macOS driver maps the two preloaded firmware
  segments through it before RUN. `read_dart=1` shows what iBoot left in it. If it holds no mapping, an iommu domain for the ANE
  has to map the two segments at the addresses in `segment-ranges`.
- Everything about the DART. It is `dart,gen3` with manual availability, and the first read of such a DART hangs the
  bus until its power chain is up. The overlay has no DART node.

## Measured

Mac mini M6, 2026-10-23, kernel 7.1.12-aurora-m6usb-ARCH. `stage=psd`: slots 30 and 31 both read VALID 0. `stage=power` with
`psd_state=1 psd_slots=1`: REQUEST=1 on slot 30 was acknowledged after 0.85 ms (valid 1, status 1). With only slot 30 on, the
ASC wrapper answered: CPU_STATUS 0xa, ASC_STATUS 0xa, IORVBAR 0x10000000001, which is the DART-side address of the preloaded
firmware text with the valid bit set. Then REQUEST=6 was acknowledged. No bus fault. This run did not read CPU_CONTROL, slot 31,
the mailbox or the DART.

Board run of stage boot (Mac mini M6, 2026-10-23, `psd_slots=2`, 300 ms): CPU_STATUS went from 0xa to 0x8 within 11 ms of the RUN write and stayed
0x8. The plain-bank status words stayed 0 and no message was read. Clearing RUN did not return the core to STOPPED, so the stage reported
core-not-stopped and left the slots on. That run watched the plain bank, not the alias bank.

Board runs with the DART overlay (Mac mini M6, 2026-10-24, `psd_slots=2`, payload with the ANE DART node): before the map the DART stream enable words
were 0 (the boot loader leaves no stream enabled). After the attach and the two maps they read 0xffff. After RUN the firmware sent the RTKit HELLO (version
12) and two endpoint map messages: endpoints 0 and 1, then endpoint 0x20 with the last flag. The module answered them. With `rtkit_start=1` it started endpoint 1
(the firmware answered with a buffer request for its own crash log, 12 pages at an address inside its data segment, which the module logs and does not answer).
The firmware then acknowledged IOP power state ON (0x20). The module sent AP power state ON, the firmware acknowledged it, and the module started endpoint 0x20.
Endpoint 0x20 sent nothing in the next 1.2 s. With `rtkit_stop=2` the AP and IOP OFF messages were acknowledged, and clearing RUN then returned the core to STOPPED.

### 2026-11-03: the command queue and the first program

With the four-DART device tree and the command sequence first, the firmware
answered the queue: CONFIG_GET, RESOURCE_INFO_GET and two more opcodes came back
in the same millisecond, and a clean stop followed ([issue #319](https://github.com/joshuaswarren/omarchy-ane/issues/319)).
The first program (a one-op fp16 add, 64 channels, 27,040 bytes) then ran:
LOAD_PROGRAM, CREATE_PROCESS and PROCEDURE_CALL acknowledged, PROCEDURE_CALL
finished after 1 ms, and the output buffer's first 4,096 bytes matched the
reference byte for byte (same SHA-256, `e7fa8c08`); y = a + b held for all 64
channels. No DART exception on any of the four instances, and the run repeated
cleanly with the module defaults. The earlier attempt that started but never
finished was the one-DART device tree: the ANE node now lists all four, as on
the M1 and M2. The K01 pack (a first real model layer) is not accepted yet.

## Build and sim

Build the module against the kernel tree of the Image that runs on the M6, on a build host (the vermagic must match):

    make -C ane/h18 KERNELDIR=<kernel tree> O=<its out dir> ARCH=arm64 LLVM=1 W=1 modules

The userspace sim needs only a C compiler:

    make -C ane/h18 sim-all

`sim-test` has the positive controls (zero bus accesses at `dt`; at `psd` no write; at `power` the exact write sequence,
the reads only after the acknowledge, three candidate mailbox addresses). The negative controls
(`sim-negative-psd-never`, `-psd-wrong`, `-dt`, `-args`) each exit 0 only when the module fails the induced fault
correctly: a slot that never acknowledges, a slot that acknowledges another state, a tampered window or property
(including mailbox number 0), a wrong opt-in key, an unknown stage or a bad argument. Five mutation targets delete
one check each from a copy of the module and must fail the harness. A sim is not silicon: it proves control flow and
gating, not any M6 behavior.

## Stage script

`m6-ane-stage.sh <1|2|3> <boot-id prefix> [GO [state]]` runs one stage as root (step 4 with chan=4 also installs the pinned program file into /lib/firmware) and collects the log in a new directory under
`/var/tmp/m6-ane`. It refuses on a kernel release other than 7.1.12-aurora-m6usb-ARCH, a boot id that does not match,
a missing ANE node, a module whose sha256 is not the pinned one, and a module that is already loaded. It enforces the
order per boot: step 2 needs a step 1 PASS, step 3 needs a step 2 PASS and the literal argument `GO`, and step 3 runs once
per boot. The boot id prefix has at least 8 characters. The script refuses when any of its override variables is set in
the environment. It reads the verdict only from RESULT lines that the run added to the kernel log, for its own stage. A
step 3 PASS with a `did not return to state 6` line exits 3. `test-stage-script.sh` checks all of this off device with fake `insmod`, `dmesg` and `lsmod`.

## Device tree

`t8152-j873-ane.dtsi` adds the `ane@419600000` node under `/soc` of `t8152-j873.dts`. Every window is a live ADT value
plus 0x200000000. `t8152-j873-ane.dts` includes both files, as `t8152-j873-gpupsd.dts` does for the GPU. The node has
no `power-domains` property, because attaching a PSD domain writes the slot request before any stage decides.

`t8152-j873-ane-dart.dtsi` adds the ANE DART (`dart,gen3`, instances 0 to 3 as four nodes on the one ADT interrupt, the ADT tunables of each instance, the DMA range of the ADT vm-base and vm-size, the shared TLBI doorbell, interrupt 789), a
PSD group for slots 30 and 31 (state 1 on both, as the board accepted) and the `iommus` and `power-domains` links of the `ane0` node.
`t8152-j873-ane-dart.dts` includes all three files. None of it has run: no ANE DART access has gone through the kernel's DART driver.

The ADT `dart-ane` has four translation instances (0x419800000 LLT, 0x419820000 BRD, 0x419840000 BRD, 0x419860000 BWR, 0xc000 each) and a DAPF window, and one interrupt, 789. On the M1 and M2 the engine's reads and writes go through the BRD and BWR instances (stream 0), and every working ANE overlay lists all instances in `iommus`; the first M6 program ran with only instance 0 in `iommus`: the firmware, which uses instance 0, worked, and the engine never finished while interrupt 789 stormed. The overlay now has one node per instance with `iommus` naming all four (stream 0), and each node carries its ADT tunables: the kernel's `apple-dart` parses `apple,tunable` on every Gen3 node and fails the probe without it (`failed to parse DART tunables`, -ENOENT), which leaves the ANE without an iommu group. The first version of this overlay left instances 1 to 3 without tunables and did not probe on a board kernel; that is fixed here. On the M2 the same words (0x20c above all) applied live to the T8110 DARTs broke translation; whether the Gen3 DART behaves the same is not known, and instance 0 already carries 0x20c. `t8152-j873-ane-dart-sid11.dtsi` adds stream 11 (`mapper-ane-mpm`) on every instance; `t8152-j873-ane-dart-full.dts` includes it. Build `t8152-j873-ane-dart.dts` first, so a run isolates the instances. `h18-dart-check.py ADT.bin DTB` compares the built DTB with the ADT of the same board (addresses, interrupt, tunable records on every instance, streams). It is a static check; no instance 1 to 3 register had been read when the first version was written.

## Bring-up order

Stop at the first unexpected result and keep the log. A hang costs one reboot: hold the power button and reload.

1. `insmod ane_h18.ko optin=t8152` (stage `dt`). Expect `RESULT stage=dt ... verdict=PASS reason=dt-windows-match`.
2. `insmod ane_h18.ko optin=t8152 stage=psd`. Expect one `psd0` and one `psd1` line with their valid and status words.
3. `insmod ane_h18.ko optin=t8152 stage=power confirm_power=1 psd_state=1`. Slot 30 only. Expect `psd0 ... state 1
   acknowledged`, then the ASC words, then the return to state 6.
4. Only after step 3 passed: `psd_slots=2`, then `read_mbox=1`.
5. Only after step 4 of the stage script's order (two slots, mailbox, CPU_CONTROL) passed: `m6-ane-stage.sh 4`, which runs stage boot once per boot.

## T8132 (14-inch M5 Max MacBook Pro, K114c): diagnostic row

The t8132 row is incomplete by design: asc, rvbar, mbox, scratch, mailbox, rvbar_expect and fw_seg are not yet mapped from a verified current-boot source, so this opt-in admits two stages and refuses `psd`, `power` and `boot`.

- `stage=dt` (t8132): applies an embedded overlay that creates `/soc/ane@5080000000` with `power-domains = <0xa2 0xa3 0xa4>` (the ps_ane_mpm, ps_ane_tdbase and ps_ane_cpu pmgr pwrstate phandles of one base-dtb build) and no PMP report cell. Before anything is pinned or applied, every raw-phandle cell must resolve to a pmgr pwrstate provider whose `reg` is the ANE offset in pmgr1; a shifted or dangling claim refuses. A base-dtb rebuild can shift the pins: re-derive them from the installed base dtb before a rerun.
- `stage=reads` (t8132, needs the overlay applied, `allow_reads=1`, `pmgr_ps_pa` and `pmgr_ps_mask=0xf`): the read-only register probe behind the pmgr power-state gate. Optionally up to two extra gate registers are re-read per step. Exactly one register is read per write of `next` to `/sys/kernel/debug/ane_h18_rd`: the seven original probe cells first, then Aurora's T6050 cold-boot cells (scratch file, CPU run and status, compute-engine enable, state and error). Every read logs its address at console priority before the readl. No writes, no clocks, no firmware start.

Measured on the machine (2026-11-11): with this overlay and `stage=reads`, the module read all 20 probe registers with no crash and never started the ANE firmware. The same kernel and base dtb with a PMP report cell in the overlay crashed the power-management firmware in both cell orders tried; that is why the overlay carries no PMP cell and the module refuses any power-domains cell that is not a pmgr pwrstate provider.

Host tests for the row: `make sim-dt-pmp sim-dt-lifecycle` (the phandle identity gate and the control lifecycle), `sim_h18 pos-reads-single pos-reads neg-reads` (the arm and step machine: three positive suites, 19 refusal cases) and the `nopwrstateid`, `novotecheck` and `nopdattach` mutation controls that must fail the harness.
