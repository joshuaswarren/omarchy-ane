# 2026-10-02 — ANE T6021 DSID_SET decode + dcs-ps probe

Pre-registered work plan and receipt for the CSNE_CMD_DSID_SET (0x25) chain on
T6021 (M2 Max) under macOS 13.5 (22G74). Companion notebook entry:

  ~/.local/share/apple-silicon-lab/entries/AneDsidRe2/2026-10-02T02<rest>.md

All claims cite the file/byte-level evidence that produced them; everything not
in this README is INFERENCE.

## Decode

Source data on disk (not committed; sha256 below):

  - kernelcache.t6020.13.5-22G74.macho  96518144 B
    sha256 9615a486511c7a60b141d7f4291361c5212e908546d6568890029bb90b5431e7
  - 13v5-22G74-selene.macho             5004072 B  (ANE firmware)
  - AneDsidRe/dsid-decode/{fw135-dsid.txt,kext135-mcache.txt,mcc135-pmp.txt}

Image-base delta for the kernelcache, derived from its __TEXT__/__DATA segment
boundaries, is 0xfffffe0007004000 (consistent across all top-level segments):
every VA in the kext range (incl. the MCC kext at 0xfffffe0008c00000..0x8e00000)
maps to fileoff = va - 0xfffffe0007004000. ane135 only carries the AppleH11ANE
kext's subrange, which is why ane135.read() returned None for writePTD/copyDSIDs.

(a) DSID value(s) macOS passes.

  ANE_Init (H11ANEIn::ANE_Init) → McacheDriverClient::powerOnMcacheRequest
  → (aneType > 0x8f on T6021 with ane-type 0xa0) sends CSNE_CMD 0x2e
  ANE_DEFAULT_SETTING_SET with the literal `{ regId=4, value=0x33 }, { regId=3,
  value=0xe }` from 0xfffffe000739ffe0, then resets the per-stream state at
  H11ANE-inst +0x48..+0xc8, then falls into the dsid block at 0xfffffe00094f4cf8.
  There it:

    str xzr, [sp+0x38]                                  # dsid = 0
    ldr x8, [x19, #0x30]                                # strh-table base
    ldr x0, [x8, #8]                                   # MCDataStream* for slot 0
    add x1, sp, #0x38                                  # &out
    mov w2, #1                                         # count = 1
    bl  MCDataStream::copyDSIDs                        # hands out first id
    ...
    ldr w8, [sp+0x38]
    str w8, [sp+0x30]                                # request+8 = dsid
    mov w2, #0xc; bl H11ANEIn::aneCmdSend             # cmd 0x25, len 12

  PowerOnMcacheRequest is called by H11ANEIn::ANE_Init at 0xfffffe00094e2ce4,
  which the ANE_CALL macros dispatch during ANE start — before any program load
  and before any CSNE_CMD_PROG_LOAD/CREATE_PROCESS.

  MCDataStream::copyDSIDs (0xfffffe0008db5f34) walks the object's {first,
  last} range at +4/+8 and writes `first + i` for i in 0..count-1. The object
  is the kANE_Victims (stream id 8) MCDataStream created by the MCC enable
  call, with the dsid range drawn from the per-stream DSID:[first,last] field
  of the mcc driver's MCDataStreamConfig default table. The exact decimal value
  for kANE_Victims on T6021/13.5 is NOT derived on disk in this pass:

  - The AppleARMPlatform kbuild in the kernelcache uses LC_DYLD_CHAINED_FIXUPS
    (LC 0x80000034 at file offset 0x170, fixups at file 0x5c08000). The default
    MCDataStreamConfig array lives in __DATA_CONST (vm 0xfffffe0007990000,
    fileoff 0x98c000, size 0x84000) and references the stream-name cstrings
    (e.g. kANE_Victims at 0xfffffe000714acdc) via chained rebase/bind entries.
    Decoding the chains (dyld_chained_ptr_64_offset, format 7) was started and
    not finished; the pointer table that maps kANE_Victims to its MCDataStreamConfig
    entry would let us read the dsid first/last fields directly.
  - No on-disk macOS ioregdump/hv trace of cmd 0x25 was located; the kext log
    "kANE_Victims dsid: 0x%x" only prints under ANE kext debug (T1027).
  - The jw16MacWin8 dumps (fw-text-adt.bin/dump.bin) are firmware text dumps
    of a T6010/M1 Max ANE boot — they have the AFPP mcachConfig dsid sentinel
    strings (afppMCacheDsid, afppInterSpillDsid, 0xFF) but no live MCC heap
    with the resolved config table.

  Derivation gap named for completion to move later:

  missing_data = "dyld_chained_ptr_64_offset decode of the mcc kext's
                  MCDataStreamConfig pointer table in __DATA_CONST
                  (fileoff 0x98c000..0xA10000). One ~30-line capstone-format
                  sweep + the ptr lookup would yield the numeric kANE_Victims
                  dsid first/last. Not done in this pass; required to lift the
                  fw_dsid A/B from STAGED to LANDED."

(b) Firmware's virtual method at vtable+0x118 for the member at controller+0x130.

  Disassembled CANEController::CANEController(char const*, ...) at 0x26114
  (file scope, demangled from the selene macho symbol table). Around 0x265cc..0x26610:

    adr x1, "hwState";  bl 0x39f34                 ; hwState resource
    adr x21, "DATA_CHAIN_H2T"
    mov x1, x21; bl 0x14a34/0x14a40                 ; name + length
    adr x0, "AneEngine";  mov w2, #0xc; mov x3,x20; mov x4,x22
    bl 0x3a0c4 -> LoadAneEngine("AneEngine",0,12,...) [singleton at 0x4f87d0]
    bl 0x490b8 -> CAneEngineExeLoopH14::CAneEngineExeLoopH14(...)
        adr x8, #0xc7e30;  str x8, [x0]             ; vptr
    str x0, [x19, #0x130]                           ; dst->+0x130 = AneEngine

  So [CANEController + 0x130] is a CAneEngineExeLoopH14 instance, vtable at
  VA 0xc7e30. Slot +0x118 of that vtable is CAneEngineExeLoopH14::updateDSID
  (demangled, file at 0x51d58). That is exactly the function called by the
  CmdProcessor 0x25 DSID_SET tail (fw135-dsid.txt 028a70):

    028a70: ldr x0, [x20, #0x130]                   ; x0 = CAneEngineExeLoopH14*
    028a74: ldr w1, [x21, #8]                      ; dsid
    028a78: ldr x8, [x0]; ldr x8, [x8, #0x118]
    028a80: blr x8

  updateDSID(dsid) then calls [[exeLoop+0x680] vtbl+0x38](dsid, dsid, dsid) and
  also [[0x4fa358] vtbl+0x38]+vtbl+0x40 (TM_LTH tail). vtbl+0x38 is the
  H14TunableManager DSID setter at VA 0x32c00 (fw135-dsid.txt 032c00):

    mov x8, #0x46c; movk #0x85c2,lsl16; movk #2,lsl32 ; x8 = 0x2_85c2_046c
    ldr w9, [x8]                                    ; read
    bfi w9, w1, #0xa, #8                            ; insert w1 at [17:10]
    str w9, [x8]                                    ; write
    stp w1, w2, [x0, #0x40]; str w3, [x0, #0x48]   ; cache
    ret

  So the firmware's DSID_SET path writes the 8-bit dsid into bits [17:10] of a
  32-bit word at MMIO 0x285c2046c (engine_base 0x284000000 + 0x1c2046c on
  T6021; the matching TM_LTH setter writes 0x285c20424 directly). ctor
  default = 0.

  MMIO addresses 0x285c2046c and 0x285c20424 lie in the 0x285c2_xxxx range
  — consistent with the mcc/ane block on T6021. The dcs-ps probe words
  0x28e20c000 (fabric-ps) and 0x28e20c400 (dcs-ps) live in the same
  engine/pmgr window but in a different subsystem.

(c) Do the firmware or HWX task descriptors need DSIDs for correct operation?

  MEASURED: yes — both layers carry DSIDs.

  Firmware side (dump1-idle/fw-text-adt.bin, 13.5 selene-family):
    "[%s] CMD = %#04x [%s] at %lld : dsid=%d"
    "!isProgramLoaded"
    "afpp mcacheDsid[%d]: %d" / "afpp interspill dsid: %d"
    "afppMCacheDsid[idx] != (0xFF)"
    "afppInterSpillDsid != (0xFF)"
    "AFPP prefetch dsid at [%d] is invalid"
    "AFPP intermdate spill dsid is invalid"
    "number of mcache DSIDs exceed max (%d)"
    "MCACHE_INTERMEDIATE_SPILL_HI_DSID / _LO_DSID"
    "MCachePrefetchInformation dsid size / offset"
    "Inconsistent DSID offset among operations"
    "Inconsistent DSID size in bits among operations"
    "CSNE_CMD_DSID_SET" / "CSNE_CMD_PREFETCH_DSID_EVENT"
    "kANE_Prefetch DSID: %d is not valid"

  Kext side (kext-22G74-dsid-strings.txt, 13.5 22G74 / 25G83):
    "prgoramInfo: %p,procId: %d numDSID: %d"
    "procedure: %d, intermediate high mcache size"
    "procedure: %d, intermediate low mcache size"
    "procedure: %d, prefetch mcache size, numDsid: %d"
    "MCACHE_INTERMEDIATE_SPILL_HI/LO_DSID"

  So an ANE HWX program carries per-procedure mcache DSID fields
  (prefetch, intermediate-spill-hi, intermediate-spill-lo, with explicit
  offset/size-in-bits fields), the firmware validates them with 0xFF sentinels
  and "consistent across operations", and a 0x25 cmd-DSID_SET additionally
  writes the kANE_Victims SLC stream's id into the TM MMIO at boot. The
  boot-level DSID_SET is global; per-procedure DSIDs live in the HWX ops.

(d) Timing.

  CSNE_CMD 0x25 DSID_SET is sent in McacheDriverClient::powerOnMcacheRequest,
  which is called from H11ANEIn::ANE_Init at 0xfffffe00094e2ce4 — that is the
  ANE start path, strictly before any program load. The case-0x25 arm in the
  firmware asserts `!isProgramLoaded` at 0x27df4 (file
  ./sne/controller/target/CANEController.cpp:39e): the firmware only honours
  DSID_SET before any program has been loaded. For T6021 ane-type 0xa0 > 0x8f
  the sequence is: 0x26 MCACHE_SIZE_GET (expected reply 0x300000, fallback
  0x200000) → reset stream state → 0x2e ANE_DEFAULT_SETTING_SET {regId=4,
  value=0x33}, {regId=3, value=0xe} → 0x25 DSID_SET, all within
  powerOnMcacheRequest.

## Pre-registered A/B protocol (LANDED on agent/ane-dsid-probe @ 90b800c)

The numeric dsid was derived by AneDsidRe3 (kANE_Victims = 9, single id, from
the static stream→dsid table at kernelcache file 0xbbe8f8); the param ships in
ane_t6021_rtclient_main.c as `fw_dsid_set` (int, -1 = off = default, 0..255)
with `fw_dsid_defaults` (bool, default off). The AneDsidRe2 stash
(`AneDsidRe2: fw_dsid param, NOT landed`) is superseded by that commit and has
been dropped.

Arms (one arm per boot; reboot between arms; the module cannot be reloaded):
  - A default        = no param (fw_dsid_set = -1; no command sent;
                       identical to previous mainline behaviour)
  - B                = `fw_dsid_set=9` — sends 0x26 MCACHE_SIZE_GET (reply
                       logged), then 0x25 DSID_SET dsid=9
  - B2               = `fw_dsid_set=9 fw_dsid_defaults=1` — sends 0x26, then
                       0x2e ANE_DEFAULT_SETTING_SET {regId=4,0x33},{regId=3,
                       0xe}, then 0x25 DSID_SET dsid=9

Workload: whole encoder, 20 blocks × 16 calls of the production encoder
(severity matches the existing parity sweeps under `omarchy/tools`).

Boot gate per arm (record before loading):
  - settled boot: uptime > 25 min
  - load average < 0.5
  - PSI cpu avg10 = 0 (recorded)

Gate to record:
  - golden sha fca96f13 exact across all 20×16 outputs
  - the firmware's replies (each exchange logs its result) must not wedge the
    engine (ASC wedge ⇒ immediate reboot, never recover)

Abort rules:
  - any load > 0.5 or PSI cpu avg10 > 0 → abort, do not record
  - any encoder output sha ≠ fca96f13 → abort
  - any translation fault in dmesg → reboot immediately
  - any dmesg line "CSNE_CMD_DSID_SET Failed" (kext side) → abort
  - any ASC wedge (firmware reaches CmdProcessor 0x28/0x2a and stops
    replying, or `dcs-ps`/`fabric-ps` both read 0/0xffffffff while ane_sys
    ACTUAL = 0xf) → reboot immediately, log wedge time + last seq

## 0x285c2046c readback protocol (LANDED on agent/ane-dsid-probe)

ane_dsid_tm_probe (ane/t6021/probes, ane_dcs_ps_probe rules) reads the one
named TM word the 13.5 fw DSID_SET programs — updateDSID's read-modify-write
target 0x285c2046c, dsid at bits [17:10] — as an exact 4-byte ioremap_np map
under the PS guard (ane_sys ACTUAL=0xf + seven island words 0x3ff), address
logged before the read, no writes, no module_exit. No other window is read.

  - before (baseline): arm A boot (fw_dsid_set off — Linux never writes the
    word), `insmod ane_dsid_tm_probe.ko`, record the init line
  - after: arms B/B2 boot, `insmod ane_dsid_tm_probe.ko` any time after boot
    (the dsid sequence ran at driver probe), record the init line; extra
    re-reads with `echo 1 > /sys/module/ane_dsid_tm_probe/parameters/start`
  - pass shape: word unchanged except bits [17:10] = dsid << 10 (9 → 0x2400)
  - the read is evidence only; abort rules above are unchanged (a probe
    fault is a translation fault → reboot)

## dcs-ps probe protocol (LANDED on agent/ane-dsid-probe)

ane_dcs_ps_probe.c is a read-only kernel module. Rules:
  - non-posted maps (ioremap_np) only — two exact 4-byte maps
    0x28e20c000 (fabric-ps) and 0x28e20c400 (dcs-ps), plus the named pmgr
    PS-block reads at 0x260 / 0x4000..0x4030
  - PS guard before MMIO: ane_sys ACTUAL=0xf AND seven island words 0x3ff,
    checked at init, before each first read, and before every sample
  - named single-word registers only — no MMIO range walks
  - read-only: readl everywhere, no writes
  - no module_exit — maps stay until reboot; unload is not supported
    (consistent with the "never rmmod ane_t6021" ban; same reboot-only rule)
  - does not depend on or touch ane_t6021

Use:
  # idle sample (printed once at init, line ends " (idle)")
  insmod ane_dcs_ps_probe.ko
  # N samples 100 ms apart, taken in the writer's process
  echo 300 > /sys/module/ane_dcs_ps_probe/parameters/start
  # output lines: "ane_dcs_ps_probe: s=<seq> t=<ns> fabric-ps=0x<v> dcs-ps=0x<v>"

Each read is gated: if ane_sys or any island word flips off-state mid-sample,
the loop stops and logs the index it reached. Output is line-based (pr_crit);
one line per sample, suitable for `grep -E 'fabric-ps|dcs-ps'`.

## Build (this CT)

  cd ane/t6021/probes
  KERNELDIR=/var/tmp/ane-kbuild/out ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    PATH=/usr/bin:/bin make modules

  # produced (sha256):
  ane_dcs_ps_probe.ko b99124041951920ff73a4f4552c5d5cfa930c97b52a67721852b81bc5e73db6c
  ane_afbridge_probe.ko (rebuilt, sha unchanged from build-drv.log)
  ane_dart_probe.ko    (rebuilt, sha unchanged)

  Host tests:
    python3 tools/test_ane_m2.py  → "test_ane_m2: ok"

The documented chroot /alarmroot recipe (build-t6021-ko.sh, image
dg-alarm-py314:sep23) is available on macstudio. On this CT the in-tree
kernel at /var/tmp/ane-kbuild (7.1.12-ane-intree) builds the probes with
CROSS_COMPILE set; the upstream kernel build did the same.

fw_dsid_set A/B build (2026-10-02, AneDsidBuild) — the same chroot recipe
on macstudio, kernel build tree m2-headers/7.1.13-3-1-ARCH, sources =
agent/ane-dsid-probe clones at 90b800c (module) and b435a9d (probes);
clean build, only the documented pahole warning:

  ane_t6021.ko        584e7ac2b06069b5c4aeda3994dfc1b8b703ed182479fb7556e734b77373551c
  ane_dsid_tm_probe.ko 24bbaadb0e164a054dce31da516b042087d4772f368ca2653dc3415ec8f71167
  (vermagic both 7.1.13-3-1-ARCH SMP preempt mod_unload aarch64)

  Host tests: python3 tools/test_ane_m2.py → "test_ane_m2: ok";
  make -C tools test_ane_fwcmd && tools/test_ane_fwcmd → "test_ane_fwcmd: ok"

## Branch / commit

  branch agent/ane-dsid-probe @ ~/src/omarchy-ane-dsid-wt

  carried (committed, pushed):
    90b800c ane/t6021: default-off fw_dsid_set replicates the 13.5 power-on
            DSID sequence (ane_t6021_fwcmd.h, ane_t6021_rtclient_main.c,
            tools/test_ane_fwcmd.c, tools/Makefile)
    b435a9d ane/t6021: PS-guarded read-only probe for the TM dsid word
            0x285c2046c (ane_dsid_tm_probe.c, probes/Makefile)
    ane_dcs_ps_probe.c + its Makefile entry landed earlier on this branch.

  Superseded and dropped: the AneDsidRe2 stash (`AneDsidRe2: fw_dsid param,
  NOT landed`) — its param re-enters as `fw_dsid_set` in 90b800c after
  AneDsidRe3 derived the dsid value.

## Missing data / why

  1. mcc kext MCDataStreamConfig default table — CLOSED by AneDsidRe3
     (notebook entry 20261002T082000Z-ct-ane-dsid-final): the static
     stream→dsid table at kernelcache file 0xbbe8b8..0xbbeaa8 gives
     kANE_Victims = 9 (single id, first==last; copyDSIDs is called with
     count=1). The auth=1 (PAC-signed) chain slots of the default table
     itself remain undecoded, but the value is derived.
  2. Live boot trace of cmd 0x25 — only the ANE kext debug log
     "kANE_Victims dsid: 0x%x" or an iBoot/JT trace of the M$DS function
     dispatcher would give the runtime value without decoding the config
     table. Not present in any artifact on disk.
  3. Per-DSID validation: ANE programs reject dsid=0xFF ("invalid") but the
     MMIO field is 8-bit (dsid 0..255). Whether macOS guarantees dsid !=
     0xFF for the runtime value, and which stream id goes with which
     single dsid under load (multiple streams ⇒ consecutive ids), are runtime
     observations, not evidence-dependent on disk.

## Not verified

  - Live enc/dec throughput on the M2 with cmd 0x25 sent vs. not sent (no
    hardware access; the M2 is reserved for other lanes).
  - Whether the firmware actually rejects dsid=0xFF when written into the
    TM field; the MMIO field is 8 bits and a write through 0x32c00 will
    succeed regardless. The dsid is treated as a semantic per-stream id;
    the 0xFF invariant lives in the per-procedure HWX validation, not the
    TM MMIO.
  - Whether the M$DS handler actually rejects datasets when dsid_mgmt=0 (it
    logs "DSID management is disabled" and the strings there suggest it
    returns a no-op; an hv trace would be definitive).
  - Whether other cmd paths besides updateDSID write 0x285c2046c
    (TunableManager::ReloadTunables re-writes via vtbl+0x38 on tunables
    reloads; the cmd-0x2e path does not touch the DSID field, it writes
    regId 3 and 4 to the same block via the updateDefSetting handler at
    0x32c24 — those land at 0x285c20424 and 0x285c20428-ish, not the DSID
    field).