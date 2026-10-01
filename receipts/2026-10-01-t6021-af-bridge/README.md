# T6021: the ANE0 AXI2AF bridge tunables, read-only comparison prepared (2026-10-01)

Status: prepared, not run. The probe module is built. No register on the M2
was read or written for this receipt. Running it needs an owner go.

## Question

macOS 27.0 (`AppleT6020PMGR::applyBridgeTunables`, the same table in the
T6020 and T6021 kexts) programs 26 registers on the ANE0 AXI2AF bridge.
Each is a 32-bit read-modify-write: `reg = (reg & ~mask) | value`. m1n1
v1.6.1 has no T6020/T6021 case in its static tunables. Do these registers
hold the macOS values under Linux, that is `(read & mask) == value`? If
they do not, the bridge runs on other settings than macOS uses, a
candidate cause of the 254 ms vs 90 ms encoder gap
([2026-10-01-t6021-parakeet-encoder](../2026-10-01-t6021-parakeet-encoder/README.md)).

## The table (offset from the bridge base, mask, value)

| offset | mask | value |
|---|---|---|
| 0x000 | 0x00000003 | 0x1 |
| 0x00c | 0x000fffff | 0xd |
| 0x010 | 0x000fffff | 0xc |
| 0x014, 0x018 | 0x00000fff | 0x1 |
| 0x01c-0x034 (7 words) | 0x00000fff | 0x3 |
| 0x108 | 0x00000003 | 0x1 |
| 0x10c | 0x000fffff | 0xd |
| 0x110 | 0x000fffff | 0xc |
| 0x114, 0x118 | 0x00000fff | 0x1 |
| 0x11c-0x134 (7 words) | 0x00000fff | 0x3 |
| 0x400 | 0xc00103ff | 0xc0010010 |
| 0xa00 | 0x01ffffff | 0x01ffffff |

## Findings

All addresses here are absolute physical addresses. Linux DT `reg` values
and m1n1 hv MMIO addresses are already absolute.

1. **The bridge base is the ANE engine base, 0x284000000.** The kext
   takes it from RegMap 4 = ADT `/arm-io/pmgr` reg[38]. The Linux DT
   cannot give that index: each of the seven pmgr nodes in the live DT has
   one or two `reg` entries. The macOS 13.5 hv trace gives the answer
   another way ([../../tools/m2hv_replay-trace-135.txt](../../tools/m2hv_replay-trace-135.txt),
   events 2816-2841). The guest writes exactly 26 engine-low words, at
   0x284000000 plus exactly the 26 offsets above. It writes them right
   after `ps_ane_sys` goes T=0, T=f, AUTO (events 2813-2815) and before
   `ps_ane_cpu` goes on (event 2842). All 26 written values pass the mask
   test above. Three of them carry bits outside their mask (eng+0x000 =
   0x11, eng+0x108 = 0x11, eng+0x400 = 0xc0f10010), so macOS read those
   registers before it wrote them. The ADT value of reg[38] is still
   unconfirmed. From an m1n1 proxy: `u.adt["/arm-io/pmgr"].get_reg(38)`.
   Expected: 0x284000000, size at least 0xa04.
2. **PAs:** 0x284000000, 0x28400000c-0x284000034 (stride 4),
   0x284000108, 0x28400010c-0x284000134 (stride 4), 0x284000400,
   0x284000a00. All are in one 4 KiB page, inside the DT `ane` reg[0]
   "engine" window (0x284000000, size 0x2000000). ane_t6021 maps that
   whole window non-posted (`ane_t6021_rtclient_main.c`, `ioremap_np` of
   the engine resource). The pmgr node does not cover it.
3. **Power:** macOS programs the bridge right after ane_sys comes up, so
   the bridge depends on ane_sys (pmgr +0x260). The DT makes ane_sys the
   parent of ane_sys_mpm and ane_cpu, and the ANE node holds the seven
   compute islands (+0x4000..+0x4030) and ane_cpu.
4. **Our driver already overwrites two of the 26.** Each ane_t6021 load
   with `fw_start=1` (the default) runs P-1, which writes eng+0x000 = 0x10
   and eng+0x400 = 0x40010001 (`ane_t6021_boot.h`, P-1 table). The M2 read
   those back as written (2026-09-21, non-posted map). Under the macOS
   masks both are NOT-applied: 0x10 & 0x3 = 0, not 1, and 0x40010001 &
   0xc00103ff = 0x40010001, not 0xc0010010. P-1 is m1n1's T8103 ANE bridge
   table. On T6021, macOS writes only two of its twelve offsets, with other
   values, and never writes 0x038, 0x03c, 0x410, 0x420, 0x430, 0x600,
   0x738, 0x798, 0x7f8 or 0x900.

## The probe

`ane/t6021/probes/ane_afbridge_probe.c` (commit `ba6ad14`):

- It reads the 26 words and writes nothing. Nothing stays mapped after
  init, and the empty exit lets `rmmod` remove it.
- It maps with `ioremap_np` only, since posted maps of Apple fabric
  windows froze this machine before.
- It refuses unless ane_sys ACTUAL = 0xf and the seven island PS words
  (0x28e084000-0x28e084030) read 0x3ff, the trace_td guard of
  ane_t6021. It then waits 3 s, and checks the guard again before each
  read.
- It logs `read 0x<PA>` at KERN_CRIT, waits 50 ms for the nbcon
  netconsole thread, then reads. The M2 boots with `loglevel=3`, so
  KERN_CRIT is the lowest level the console prints. After a fault, the
  last line names the address.
- Output lines are `ane_afbridge_probe: 0x<PA> = 0x<value>`.

Build, against a copy of the stock `7.1.13-3-1-ARCH` headers package
build tree, in an Arch Linux ARM chroot with gcc 16.1.1:

    make -C <headers> M=$PWD/ane/t6021/probes CONFIG_DEBUG_INFO_BTF_MODULES= W=1 modules

The build gave no warnings. The `.ko` is 179,464 bytes, sha256
`5a235996a517624673921f0f9e48366b1f250e5214b5dcced2cc27706f31d2ac`.
vermagic `7.1.13-3-1-ARCH SMP preempt mod_unload aarch64` equals the
running kernel and the installed ane_t6021.ko. The undefined symbols are
`__ioremap_prot`, `iounmap`, `_printk`, `msleep`,
`of_machine_compatible_match` and `arm64_use_ng_mappings`.

## Risk

Read classes already proven on this M2:

- the pmgr PS words, read by every driver guard;
- the TM word 0x285c20458 under the seven-word guard;
- the engine-low page through `ioremap_np`: 12 words read back, at 0x000,
  0x038, 0x03c, 0x400, 0x410, 0x420, 0x430, 0x600, 0x738, 0x798, 0x7f8 and
  0x900. Five of them read 0 and did not fault.

New in the probe: 24 offsets in that same page that Linux has never read.

Known fault classes that the probe stays away from:

- posted maps;
- engine reads with the islands off;
- TM +0x20420, which hung even non-posted;
- ASC wrapper reads;
- CoreSight (engine+0x1010000).

The residual risk is low but not zero. Auto-gating could drop ane_sys
between the guard and a read, or a bridge register could answer a read
with a bus error. If either happens, the M2 hard-resets and the watchdog
reboots it from disk in about 2 minutes. Run `sync` first, and run it
with no GPU job active.

## Run procedure (owner go required)

1. Confirm ADT reg[38] = 0x284000000 (finding 1), or accept the trace
   evidence in its place.
2. Check the module hash on the M2: `sha256sum ane_afbridge_probe.ko` must
   print `5a235996...31d2ac`.
3. Run:

       cat /proc/sys/kernel/random/boot_id; sync
       sudo flock /var/tmp/ane-run.lock insmod ane_afbridge_probe.ko; echo rc=$?
       sudo rmmod ane_afbridge_probe
       journalctl -k -b | grep ane_afbridge_probe > afb-run.log
       cat /proc/sys/kernel/random/boot_id

   insmod takes about 4.3 s.
4. If the log shows a `refused` line, no engine read happened. Repeat
   step 3 without `flock`, while an encoder process runs.
5. Compare each line with the table: applied if `(read & mask) == value`.
   Keep the netconsole copy of the log as well.

Pass/fail:

- PASS: 26 lines and `done, 26 of 26 read`, with the boot_id unchanged.
- SAFE-STOP: a `refused` or `stopped before` line. This is not a result;
  retry it.
- FAIL-HARD: a reset. The last line names the address. Do not retry.

Expected result: 0x000 and 0x400 read NOT-applied (finding 4).

- If the other 24 are all applied, the bridge is already set, and the
  only difference is our P-1 overwrite.
- If any of the 24 is NOT-applied, the hypothesis stays open.

The next step is a separate go: a write A/B. It applies the 26 RMWs in
macOS order, after P-1 and before the ASC start, behind a default-off
parameter. It measures encoder exec ms with the bit-exact golden check.

Limit: because the bridge depends on ane_sys, a read shows the current
power epoch only. P-1 runs at each load, so a pristine pre-Linux value
needs a load with `fw_start=0`.
