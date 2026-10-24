# ane_t6021 constants against T8152 (M6) and H15 (M3)

One row per chip-specific constant of `ane/t6021`, with the T8152 (Mac mini M6, board J873g, ANE0) value and the H15 value where one is known. `tools/test_ane_constants_table.py` re-reads every T6021 value from the source and every `[NAME]` reference from `ane/h18` or `ane/h15`, so a row that drifts from the code fails CI. Rows without a macro (`ane_t6021_soc.*`) are read from the struct initializer.

Evidence tags in the last column:
- **ADT**: read in the M6 ADT (`/arm-io/ane`, `/arm-io/dart-ane`) or the IPSW data file `data/ane-soc/t8152.json`.
- **kext**: decoded from the macOS 27.0 `mac18g` kernelcache (sha256 `09746d1f442153956ba00b385f49bc245460c1ebea92cae7e9e0e4f240745b85`), `AppleH16ANEInterface` for the engine words, `AppleA7IOP-ASCWrap-v8` for the mailbox. Static reading, nothing run.
- **board**: printed by `ane_h18` on the M6 (boot `ee8a53d1`, 2026-10-24; the log is not checked in).
- **I**: inferred, no source reads it. **?**: not decoded; the T6021 value must not be copied.

## What does not carry over (read this before the table)

1. **Engine words are not at the T6021 offsets.** On T8152 the ASC wrapper is at engine + `0x1600000` (ADT reg[0] `0x419600000`), not `0x1400000`. The T6021 probe reads `CPU_STATUS` at engine + `0x1400048` before it knows anything; on T8152 that offset is not the wrapper and nothing measured says it is safe to read.
2. **The SCRATCH, doorbell, pending and ack block moved.** T8152: SCRATCH0 at engine + `0x18a0024`, doorbell `0x18a4000`, pending `0x18ac000`, ack `0x18b0000`. The block base is `0x18a0000` (T6021 `0x1840000`) and SCRATCH sits at +`0x24` in it (T6021 +`0x48`). All of them lie inside `ANE_FATAL_READ_LO..HI`, the range the T6021 guard forbids. Board reads of SCRATCH at `0x4198a0024` did not fault; the range is not otherwise measured on T8152.
3. **There is no wrapper mailbox.** T8152 uses a Postoffice (`msgbox-using-postoffice`, mailbox number 12): channel 24, second 64 KiB half of reg[2], aperture `0x316010c00`. RX at +0x00, TX at +0x10, STATUS at +0x20 (kext, ADT). The T6021 `ANE_ASC_MBOX*` registers have no counterpart there.
4. **The channel layout is not the T6021 one.** The firmware's table on the board has TERMINAL with `0x200` slots (T6021 `0x300`), so every later ring offset and the total differ (second table below). `ane_t6021_chman_check` would reject the M6 table.
5. **`CPU_STATUS` after RUN.** Board: `0xa` before RUN, `0x8` after the write of RUN (bit 1 STOPPED clear, bit 0 RUNNING not set). The probe's `!(cpu_status & ANE_ASC_CPU_STATUS_RUNNING)` test is true on T8152 after RUN.
6. **iBoot loads the firmware** (`pre-loaded = 1`, BuildManifest `IsLoadedByiBoot`), so the loader rows (pinned image, ASC tunables, RTK patch addresses, alias windows) do not apply to T8152; IORVBAR is already `0x10000000001` (IOVA `0x10000000000` with the valid bit), which the T6021 entry fold accepts (checked by the test).
7. **The channel and handshake words match.** READY, wake, the 0x21 request limit, the 0x174 byte init block and the RTKit management types are the same constants.

## Constants

| constant | T6021 | T8152 | H15 (T6030) | basis |
|---|---|---|---|---|
| `ANE_PS_ON` | `0xf` | PSD state `0` [ANE_H18_PSD_P0_RUN]: slots 30 and 31 at `0x303a10000`, no pmgr ps words | `0xf` [ANE_H15_PS_ON] | ADT power-gates; board; H15: I |
| `ANE_ASC_CPU_CONTROL` | `0x1400044` | `0x1600044` | `0x1400044` | kext (ane-type 0x310, `initializeANESoCConfig`), board read of PA `0x419600044` = 0; H15: I |
| `ANE_ASC_CPU_STATUS` | `0x1400048` | `0x1600048` | `0x1400048` | kext, board read = `0xa`; H15: ADT reg[0] + 0x48, role I |
| `ANE_ASC_RVBAR` | `0x1050000` | `0x1050000` | `0x1050000` [ANE_H15_RVBAR_WIN_OFF] | ADT reg[1] `0x419050000`; board IORVBAR `0x10000000001` |
| `ANE_ASC_EDPRCR` | `0x1010310` | ? | ? | T6021 whitelist word; H15 treats `0x1010000` as forbidden |
| `ANE_ASC_VERS` | `0x1840000` | ? | ? | not translated: the block base moved, in-block offsets differ |
| `ANE_ASC_RTB_STATUS` | `0x1840088` | ? | ? | same |
| `ANE_ASC_RTB_STATUS_UNK7C` | `0x184007c` | ? | ? | same |
| `ANE_ASC_CPU_BASE` | `0x1000000` | ? | ? | firmware RTK_cpu_physical_address block; T8152 firmware is preloaded |
| `ANE_ASC_WRAPPER_BASE` | `0x1400000` | `0x1600000` | `0x1400000` [ANE_H15_WRAP_OFF] | ADT reg[0] minus engine base |
| `ANE_ASC_MBOX` | `0x1408000` | none: Postoffice, see below | `0x1408000` | H15: I (wrapper + 0x8000; CTRL words read on a T6030, PR 175) |
| `ANE_ASC_MBOX_A2I_CTRL` | `0x1408110` | none | `0x1408110` | H15: I |
| `ANE_ASC_MBOX_I2A_CTRL` | `0x1408114` | none | `0x1408114` | H15: I |
| `ANE_ASC_MBOX_A2I_SEND0` | `0x1408800` | none | `0x800` [ANE_H15_MBOX_A2I_SEND0] | mailbox-relative on H15; I |
| `ANE_ASC_MBOX_A2I_SEND1` | `0x1408808` | none | `0x808` [ANE_H15_MBOX_A2I_SEND1] | I |
| `ANE_ASC_MBOX_I2A_RECV0` | `0x1408830` | none | `0x830` [ANE_H15_MBOX_I2A_RECV0] | I |
| `ANE_ASC_MBOX_I2A_RECV1` | `0x1408838` | none | `0x838` [ANE_H15_MBOX_I2A_RECV1] | I |
| `ANE_FATAL_READ_LO` | `0x1854000` | ? (see 2 above) | `0x1854000` [ANE_H15_FATAL_LO] | H15 inherits the T6021 rule, no H15 evidence |
| `ANE_FATAL_READ_HI` | `0x1c04000` | ? | `0x1c04000` [ANE_H15_FATAL_HI] | same |
| `ANE_MBI_SCRATCH0` | `0x1840048` | `0x18a0024` | `0x1840050` [ANE_H15_SCRATCH_OFF] | kext (T8152 rodata table at `0xfffffe0007421850`; T6030 ane-type 0xc0 case); board reads |
| `ANE_MBI_SCRATCH6` | `0x1840060` | `0x18a003c` | `0x1840068` | stride 4 from SCRATCH0 |
| `ANE_MBI_SCRATCH7` | `0x1840064` | `0x18a0040` | `0x184006c` | stride 4 |
| `ANE_MBI_DOORBELL` | `0x1844000` | `0x18a4000` [ANE_H18_DOORBELL_OFF] | `0x1844000` [ANE_H15_DOORBELL_OFF] | kext, both; no H15 write yet |
| `ANE_IPI_OFF` | `0x1844000` | `0x18a4000` [ANE_H18_DOORBELL_OFF] | `0x1844000` [ANE_H15_DOORBELL_OFF] | same word as the doorbell |
| `ANE_MBI_MSG_A2I_RD` | `0x184c000` | `0x18ac000` [ANE_H18_IPI_PENDING_OFF] | ? | same offset from the doorbell (+0x8000). T6021 calls it host to firmware; the T8152 kext decode reads it as firmware-to-host pending. The T6021 direction label is unverified |
| `ANE_MBI_MSG_A2I_WR` | `0x1850000` | `0x18b0000` [ANE_H18_IPI_ACK_OFF] | ? | doorbell +0xc000; the kext writes back the value it read from pending |
| `ANE_MBI_TIMEBASE_LO` | `0x1170000` | `0x1370000` | ? | T8152: kext, lo/hi pair read as timestamps, I |
| `ANE_MBI_TIMEBASE_HI` | `0x1170004` | `0x1370004` | ? | as `ANE_MBI_TIMEBASE_LO` |
| `ANE_T6021_BOOT_REG_TICK` | `0x1160008` | ? | ? | T6021 progress observable |
| `ANE_MBI_CHAN_STRIDE` | `0x100` | `0x100` [ANE_H18_CH_ENTRY] | `0x100` [ANE_H15_CH_ENTRY] | channel table entry size; board table |
| `ANE_RTKIT_MGMT_HELLO` | `1` | `1` | `1` [ANE_H15_RTK_HELLO] | board: firmware HELLO v12 |
| `ANE_RTKIT_MGMT_HELLO_REPLY` | `2` | `2` | `2` [ANE_H15_RTK_HELLO_REPLY] | kext |
| `ANE_RTKIT_MGMT_STARTEP` | `5` | `5` | `5` [ANE_H15_RTK_STARTEP] | kext |
| `ANE_RTKIT_MGMT_SET_IOP_PWR_STATE` | `6` | `6` | `6` [ANE_H15_RTK_SET_IOP_PWR] | kext |
| `ANE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK` | `7` | `7` | `7` [ANE_H15_RTK_IOP_PWR_ACK] | kext |
| `ANE_RTKIT_MGMT_EPMAP` | `8` | `8` | `8` [ANE_H15_RTK_EPMAP] | kext, board |
| `ANE_RTKIT_MGMT_SET_AP_PWR_STATE` | `0xb` | `0xb` | `0xb` [ANE_H15_RTK_AP_PWR] | kext; H15: I |
| `ANE_RTKIT_PWR_STATE_ON` | `0x20` | `0x20` | `0x20` [ANE_H15_RTK_PWR_ON] | kext; H15: I |
| `ANE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK` | `0xb` | `0xb` | `0xb` [ANE_H15_RTK_AP_PWR] | kext; H15: I |
| `ANE_RTKIT_VER_MAX` | `12` | `12` [ANE_H18_RTK_VERSION] | `12` [ANE_H15_RTK_VERSION] | board: firmware offers v12; H15: version the driver answers |
| `ANE_RTKIT_VER_MIN` | `11` | `12` | ? | board: the firmware HELLO `0x100000000c000c` offers min 12, max 12 |
| `ANE_RTKIT_EP_CRASHLOG` | `1` | `1` | ? | board: endpoints 0x0, 0x1 and 0x20 discovered, no others |
| `ANE_RTKIT_EP_SYSLOG` | `2` | not in the board endpoint map | ? | board |
| `ANE_RTKIT_EP_DEBUG` | `3` | not in the board endpoint map | ? | board |
| `ANE_RTKIT_EP_IOREPORT` | `4` | not in the board endpoint map | ? | board |
| `ANE_RTKIT_EP_OSLOG` | `8` | not in the board endpoint map | ? | board |
| `ANE_RTKIT_EP_TRACEKIT` | `0xa` | not in the board endpoint map | ? | board |
| `ANE_FW_ALIAS_MAX_WIN` | `3` | n/a: iBoot maps 2 segments (TEXT IOVA `0x10000000000`, DATA `0x100000c4000`) | ? | ADT segment-ranges |
| `ANE_T6021_FW_ALIAS_PAGE` | `0x4000` | `0x4000` | ? | ADT dart-ane page-size 16384 |
| `ANE_CSNE_CMD_MAX_SIZE` | `0x1b88` | ? | ? | T6021 command block size |
| `ANE_T6021_LOAD_SEC_COUNT` | `9` | ? | ? | the H18 compiled-program format is not decoded |
| `ANE_PROGOBJ_MIN_ENTRIES` | `0x201` | ? | ? | same |
| `ANE_PROGOBJ_MAX_ENTRIES` | `0x400` | ? | ? | same |
| `ANE_PROGOBJ_TABLE_OFF` | `0x208` | ? | ? | same |
| `ANE_PROGOBJ_HDR_SZ` | `0x208` | ? | ? | same |
| `ANE_OPSEC_OP_REC_SIZE` | `0x40c` | ? | ? | same |
| `ANE_OPSEC_MAX_OPS` | `0x80` | ? | ? | same |
| `ANE_T6021_MAX_PROGRAMS` | `250` | ? | ? | T6021: measured; firmware table size on T8152 not read |
| `ANE_ASC_CPU_STATUS_RUNNING` | `BIT(0)` | `BIT(0)` not set after RUN (see 5 above) | `BIT(0)` [ANE_H15_ST_RUNNING] | board |
| `ANE_ASC_CPU_STATUS_STOPPED` | `BIT(1)` | `BIT(1)` set before RUN, clear after | `BIT(1)` [ANE_H15_ST_STOPPED] | board |
| `ANE_T6021_CPU_RUN_RELEASE` | `0x10` | `0x10` | `0x10` | kext write 0 then 0x10; H15: `ANE_H15_CPU_RUN` is BIT(4) |
| `ANE_T6021_RVBAR_ENTRY_BASE` | `0x0081000000000001` | `0x0081000000000001` | ? | kext reads IORVBAR with the T6021 constants |
| `ANE_T6021_RVBAR_ADDR_MASK` | `0xff7efffffffff800` | `0xff7efffffffff800` | ? | kext; iBoot's `0x10000000001` folds to `0x10000000000` |
| `ANE_T6021_BOOT_IPC_REQ_THRESHOLD` | `0x21` | `0x21` [ANE_H18_HS_REQ_LIMIT] | `0x21` [ANE_H15_HS_REQ_LIMIT] | kext both |
| `ANE_T6021_BOOT_WAKE_REQ` | `0xf7fbdff9` | `0xf7fbdff9` [ANE_H18_HS_WAKE] | `0xf7fbdff9` [ANE_H15_HS_WAKE] | kext both |
| `ANE_T6021_BOOT_ACK` | `0x08042006` | `0x08042006` [ANE_H18_HS_READY] | `0x08042006` [ANE_H15_HS_READY] | kext; T8152 SCRATCH7 already reads READY before the wake |
| `ANE_T6021_INIT_STRUCT_SIZE` | `0x174` | `0x174` [ANE_H18_HS_BLOCK_SIZE] | `0x174` | board block; kext |
| `ANE_T6021_INIT_FW_DVA_OFF` | `0x00` | `0x00` | `0x00` | board block +0x0 = `0x10000000000` (firmware IOVA) |
| `ANE_T6021_INIT_COUNT_OFF` | `0x68` | `0x68` | `0x68` | board block +0x68 = `0x40` |
| `ANE_T6021_INIT_COUNT` | `0x40` | `0x40` | `0x40` | board |
| `ANE_T6021_INIT_TEMPLATE_OFF` | `0x6c` | `0x6c` | `0x6c` | kext |
| `ANE_T6021_INIT_TEMPLATE_SIZE` | `0x100` | `0x100` | `0x100` | kext |
| `ANE_T6021_INIT_TBIT_OFF` | `0xc0` | `0xc0` | `0xc0` | board: +0x12c (= 0x6c + 0xc0) = `0x4` |
| `ANE_T6021_INIT_TBIT_VAL` | `0x4` | `0x4` | `0x4` | board |
| `ANE_T6021_BOOT_IPC_CEILING` | `0x100000` | `0x100000` [ANE_H18_HS_IPC_MAX] | `0x100000` [ANE_H15_HS_IPC_MAX] | driver limit, not a chip value; board IPC request was `0xc441` |
| `ANE_T6021_BOOT_HEAP_CEILING` | `0x2000000` | `0x4000000` [ANE_H18_HS_HEAP_MAX] | `0x4000000` [ANE_H15_HS_HEAP_MAX] | driver limit; the board heap request is `0xf00000`, which fits both |
| `ANE_T6021_BOOT_REG_TABLE0` | `0xb38` | ? | ? | T6021 register table, not decoded for T8152 |
| `ANE_T6021_BOOT_REG_TABLE1` | `0xb98` | ? | ? | same |
| `ANE_T6021_BOOT_REG_TABLE2` | `0xbf8` | ? | ? | same |
| `ANE_T6021_BOOT_TABLE_VALUE` | `0x01ff01ff` | ? | ? | same |
| `ANE_T6021_BOOT_REG_RVBAR` | `0x1050000` | `0x1050000` | `0x1050000` | as `ANE_ASC_RVBAR` |
| `ANE_T6021_BOOT_REG_CPUCTRL` | `0x1400044` | `0x1600044` | `0x1400044` | as `ANE_ASC_CPU_CONTROL` |
| `ANE_T6021_BOOT_REG_SCRATCH0` | `0x1840048` | `0x18a0024` | `0x1840050` | as `ANE_MBI_SCRATCH0` |
| `ANE_T6021_BOOT_REG_SCRATCH1` | `0x184004c` | `0x18a0028` | `0x1840054` | stride 4 |
| `ANE_T6021_BOOT_REG_SCRATCH6` | `0x1840060` | `0x18a003c` | `0x1840068` | stride 4 |
| `ANE_T6021_BOOT_REG_SCRATCH7` | `0x1840064` | `0x18a0040` | `0x184006c` | stride 4 |
| `ANE_T6021_CHMAN_ENTRY_SIZE` | `0x100` | `0x100` [ANE_H18_CH_ENTRY] | `0x100` [ANE_H15_CH_ENTRY] | board table |
| `ANE_T6021_CHMAN_NAME_LEN` | `0x40` | `0x40` [ANE_H18_CH_NAME_LEN] | `0x40` [ANE_H15_CH_NAME_LEN] | board table |
| `ANE_T6021_CHMAN_COUNT` | `8` | `8` [ANE_H18_CH_COUNT] | `8` [ANE_H15_CH_COUNT] | board table |
| `ANE_T6021_CHMAN_TOTAL` | `0x10440` | `0xc440` | ? | board SCRATCH1 `0xc440` (init block +0x50 = S1 + 1 = `0xc441`); equals the last ring end `0xc400` + 0x40, the slack T6021 has too; see 4 above |
| `ANE_FW_BUF_SIZE` | `0x500000` | n/a (iBoot loads); payload `0x180000` | ? | `data/ane-soc/t8152.json` firmware.ane0_payload_size 1572864 |
| `ANE_FW_NCMDS` | `5` | `7` | ? | json firmware.macho |
| `ANE_FW_SIZEOF_CMDS` | `0x960` | `0xb38` | ? | json firmware.macho |
| `ANE_FW_FLAGS` | `0x1` | `0x200001` | ? | json firmware.macho |
| `ANE_FW_NSEGS` | `2` | `3` | ? | json: three LC_SEGMENT_64 |
| `ANE_FW_ENTRY_PC` | `0x0` | `0x0` | ? | json: entry PC 0 |
| `ANE_FW_RTK_PATCHES` | `5` | ? | ? | the H18 payload has a 45 record patchbay instead; not mapped |
| `ANE_FW_DATA_BASE_VM` | `0x423c` | ? | ? | T6021 TEXT word holding the DATA IOVA; M6 DATA IOVA is set by iBoot |
| `ANE_FW_TUNABLES_MAX` | `0x24` | none | ? | M6 ADT has no ANE-level tunables blob; only `dart-tunables-instance-0..3` |
| `ANE_BOOT_ACK_POLL_US` | `1000` | `1000` | `1000` | kext polls SCRATCH7 1 ms x 1000; H15: I |
| `ANE_BOOT_POLL_MS` | `1000` | `1000` | `1000` | same |
| `ane_t6021_soc.soc` | `0x6021` | ? | n/a | the value iBoot writes to RTK_soc for T8152 is not known |
| `ane_t6021_soc.soc_revision` | `0x11` | ? | ? | not known; iBoot preloads on T8152 |
| `ane_t6021_soc.ps_cpu_off` | `0x2e0` | n/a (PSD slots 30, 31) | `0xc008` | H15 ANE_CPU ps word of T6030 (ADT, `ane_h15_ps_6030`) |
| `ane_t6021_soc.pmu_pa` | `0x28e084000` | ? | ? | H18 payloads carry InitPMUBaseAddress; the address is not decoded |
| `ane_t6021_soc.trace_td_off` | `0x1c20458` | ? | ? | the H18 payload names CSneTMDrvH18g; the TD word offset is not decoded |

Not chip constants, left out on purpose: allocation, buffer-object and hash limits (`ANE_LEGACY_*`, `ANE_T6021_BO_*`), trace sizes and magic (`ANE_TRACE_*`), the pmgr word offset `ANE_PMGR_PS_LAST_OFF`, the CALL cookie and its T2H layout (`ANE_CALL_COOKIE`, `ANE_T2H_CALL_*`; the T8152 INFERENCE_CALL payload is not decoded), the RTKit bit-field masks, which are the same RTKit message format on every chip here.

## Channel table (firmware's eight channels, 64 byte slots)

Ring offsets are from the IPC surface. T6021 is `ane_t6021_chman_layout`; T8152 is the table the firmware published on the board (IPC surface at IOVA `0x3fffffb0000`). Each ring starts where the previous one ends: offset + slots x 0x40, first ring at 0x800.

| channel | type | bit | T6021 slots | T6021 ring | T8152 slots | T8152 ring |
|---|---|---|---|---|---|---|
| TERMINAL | 2 | 0 | `0x300` | `0x800` | `0x200` | `0x800` |
| IO | 0 | 1 | `0x10` | `0xc800` | `0x10` | `0x8800` |
| DEBUG | 0 | 2 | `0x8` | `0xcc00` | `0x8` | `0x8c00` |
| BUF_H2T | 0 | 3 | `0x40` | `0xce00` | `0x40` | `0x8e00` |
| BUF_T2H | 1 | 4 | `0x40` | `0xde00` | `0x40` | `0x9e00` |
| SHAREDMALLOC | 1 | 5 | `0x8` | `0xee00` | `0x8` | `0xae00` |
| IO_T2H | 1 | 6 | `0x40` | `0xf000` | `0x40` | `0xb000` |
| DATA_CHAIN_H2T | 0 | 7 | `0x10` | `0x10000` | `0x10` | `0xc000` |

H15: the firmware's table has not been read on any T6030 (stage 7 has not run), so no H15 column.
