# 2026-10-02: MacBook Neo (T8140, j700, H17) ANE feasibility (static)

## Verdict

The Neo ANE is feasible on the `ane_t6021` model, not as a new ASC-IOP
driver. The macOS 27.0 kernel binds the Neo's `ane,t8132exclave` node to
`H11ANEIn`, the same class that it binds to the `ane,t8020` node of every M1
and M2. The H17 firmware is a plain, unencrypted RTKit Mach-O that iBoot
loads, with the same 96 `CSNE_CMD` host commands as the 27.0 H14 image.
The work that is left is driver data and hardware discovery, not a new
protocol. Two items can still stop it: the ADT marks the ANE
`exclave-assigned`, and no H17 compiler target exists yet. No Neo has run
any ANE code. Everything here is static.

Data: [t8140.json](../../data/ane-soc/t8140.json),
[t8142.json](../../data/ane-soc/t8142.json),
[t6050.json](../../data/ane-soc/t6050.json), and the data-only overlay
[t8140-ane-dataonly.dts](../../packaging/dt/t8140-ane-dataonly.dts). Every
value has a source id. Each JSON file lists its sources with SHA-256 digests.

## Facts (MEASURED)

| Fact | T8140 j700 (Neo) | Source |
| --- | --- | --- |
| ANE node | `/arm-io/ane`, `ane,t8132exclave`, ane-type 512 | ADT j700 |
| Register windows | 0x400000000+0x2000000, 0x300700000+0x18000, 0x300724000+0x4000, 0x3003c0000+0x30000, 0x211000000+0xff4000, 0x3082c8000+0x4000 | ADT j700 |
| Interrupts | AIC 655, 668 (aic,3 at 0x301000000) | ADT j700 |
| IOMMU | `dart-ane` streams 0 (`mapper-ane`) and 11 (`mapper-ane-mpm`) | ADT j700 |
| Exclave | `exclave-assigned`, service `com.apple.service.ANEExclave`, `exclave-reg` 0x441c00000+0x88000 | ADT j700 |
| DART | `dart,t8110`; instances DARTLLT 0x401800000+0xc000, DARTBRD 0x401820000+0x20000, DARTBWR 0x401840000+0x20000, DAPFLLT 0x401810000+0x4000; IRQ 656 | ADT j700 |
| DART streams | sid 0, 11, 15, 10, 13; exclave-sid 1..7; sid-count 16 | ADT j700 |
| DART IOVA | vm-base 0x10000000000, vm-size 0x30000000000, page 16 KiB, flush-by-dva 1 | ADT j700 |
| DAPF ranges | 0x30070c000..0x30070c013, 0x300700290..0x300700293, and two others | ADT j700 |
| pmgr | ANE_SYS ps 0x300700290 (parent AFISOCNI2); ANE_CPU, ANE-SYS-V, ANE-SYS-V-PMP have no ps | ADT j700 |
| Firmware | `h17_ane_fw_theia_d9x.im4p`, 1,556,505 B, im4p sha256 `595562a9…`; IsLoadedByiBoot, IsFUDFirmware | BuildManifest, IPSW member |
| IM4P | four DER fields, no KBAG: not encrypted; payload is a raw Mach-O (arm64e, MH_PRELOAD), 0x17c000 B, sha256 `ddc97c20…`; `__TEXT` 0x0+0xc0000, `__DATA` 0xc0000+0x2ac000 | IPSW member |
| Firmware build | `RTKit_release-3514.0.15`, sources `AppleH16AneFW`, H17 parts `PowerControl_H17`, `CSneTMDrvH17`, `CSneCEDrvH17` | IPSW member |
| Kernelcache | `kernelcache.release.mac17p` (the j700ap BuildIdentity names it) | BuildManifest |
| macOS class | `H11ANEIn`, personality `t8132exclave`, IOProbeScore 10000, bundle `com.apple.driver.AppleH16ANEInterface` 10.19.2 | kernelcache |
| SoC HAL | `com.apple.driver.AppleT8140ANEHAL` 10.19.3 with `tunableh_*` tables | kernelcache |
| HWX | ANECCompile target h17: Mach-O cpusubtype 9 (h16: 7) | compiler oracle |

T8142 (j704, j813, j815) and T6050 (j714c, j714s, j716c, j716s, j775c, j775d,
j873s) have the same node shape (`ane,t8132exclave`, six or seven windows,
two interrupts, `exclave-reg` at engine + 0x41c00000). Their kernelcaches
(`mac17g`, `mac17j`) bind them the same way. Their HAL kexts are
`AppleT8142ANEHAL` and `AppleT6050ANEHAL`.

## Corrections to earlier claims

- README "Chip coverage" said every chip after M2 uses `iop,ascwrap-v6`.
  That is true for M3 (H15) only. M4 (H16) keeps `ane,t8020`, and H17 uses
  `ane,t8132exclave`. This branch corrects the README line.
- The M3 family firmware is not encrypted. `h15_ane_fw_themis_j51y.im4p` has
  four DER fields and no KBAG. Its payload is a raw Mach-O.
- AneAllSoc's fetch receipt gave the same size, 1,556,505 B, for all four
  H17 members and for H16. That is correct: the four H17 images differ in
  content (four different payload hashes) but not in size.

## What is new for H17 compared with H14 (T6021)

| Item | T6021 (H14) | T8140 (H17) |
| --- | --- | --- |
| Node | `ane,t8020`, 3 windows, 1 IRQ | `ane,t8132exclave`, 6 windows, 2 IRQs, exclave props |
| DART windows | 4 x 0x4000 | 0xc000, 0x20000, 0x20000, 0x4000 |
| DART streams | 0, 15 | 0, 11, 15, 10, 13 + exclave 1..7 |
| IOVA window | 0x10000000000 + 0x30000000000 | same |
| Host power states | ANE_SYS, ANE_CPU, MPM, TD, BASE, SET1..4 | ANE_SYS only; the rest have no ps. The DART DAPF ranges cover pmgr 0x30070c000..0x30070c013 and the ANE_SYS word (inference: the firmware sets its own power states, `PowerControl_H17`) |
| Firmware | selene, iBoot-loaded | theia, iBoot-loaded, unencrypted |
| Protocol | RTKit + CSNE_CMD | same 96 CSNE_CMD names (27.0 images); the macOS class has an exclave-mode path (`CSNE_CMD_EXCLAVE_MODE_START/STOP`) |
| Tunables | iBoot ASC tunables | per-SoC HAL kext tunable tables |

Not measured (inference only): the ASC and mailbox offsets inside the
engine window, the RTKit endpoint numbers, the iBoot runtime patches, and
whether Linux can reach the windows that the ADT marks `exclave-assigned`.
The aurora tree drives two j700 blocks that the same ADT marks
`exclave-assigned`: the AOP setup mailbox (`aop-exclave-mailbox`) and the
ISP (`isp,isp-generic-ex`). Thus a lockout is possible but not likely.

## Staged plan (each tester step needs only the power button to recover)

| Stage | Tester action | Risk | Estimate |
| --- | --- | --- | --- |
| a. Read-only probe | Run the userland probe and upload the row. It reads `/proc`, `/sys` and the phram `adt` region only; no MMIO. | None known | 1 agent-day; 1 run |
| b0. Static preparation | None | None | 6 to 10 agent-days: H17 table for `ane_t6021` (ASC offsets and power sequence from the H11ANEIn/HAL kexts), firmware fetch for `h17_ane_fw_theia_d9x`, iBoot patch list from the live ADT |
| b1. Firmware boot | Load the opt-in module by hand (no boot-chain or DT change), run the smoke script, upload the row. Pass = RTKit HELLO, EPMAP, then `CSNE_CMD_CONFIG_GET` and `PLATFORM_INFO` replies, no DART fault, clean unload. | Hang or SError: hold power, boot again (module does not autoload) | 3 to 6 agent-days; 2 to 6 runs |
| b2. First program | Same as b1 with the smoke program | Wrong results (caught by the golden), hang | 5 to 15 agent-days for an H17 compiler target (H16G base, oracle diffs); 2 to 4 runs |
| c. Promotion | One row with a ready check, the driver loaded, 20 bit-exact smoke calls, no ANE/DART/mailbox fault | A failing row blocks (CONFLICT) | 1 to 2 agent-days (t8140 entries in `promotion_check.py`: driver, golden, fault regex); 1 run |

## Aurora and the community row

- aurora-wip b3a55ee6 has `t8140.dtsi`, `t8140-pmgr.dtsi` and
  `t8140-j700.dts` with no ANE, ANE DART, ANE mailbox or ANE power domain.
  No aurora PR or branch adds ANE support for the Neo. The data-only
  overlay compiles with dtc 1.7.2 and applies with fdtoverlay to the
  aurora-wip j700 DTB; the result has the nodes disabled.
- The t8140 deep row (content sha256 `5efa0ec0…`, kernel
  7.1.12-2-11.16-sep-ARCH, m1n1 0a33275c) shows no ANE node, no ANE DART,
  no ANE power domain, no ANE interrupt, no ANE fault line, and omarchy-ane
  not installed. The row does not record `/sys/class/accel`.

## How

- HTTP range reads of the 27.0 (26A428) IPSW with the `RangeFile` reader of
  `packaging/omarchy-ane-firmware-fetch`: BuildManifest, three DeviceTree
  members, the six ANE firmware members named above, and three kernelcache
  members. The ADT and BuildManifest bytes equal the AneAllSoc fetch.
- ADT parse: AsahiLinux m1n1 proxyclient `adt.py`. IM4P parse: DER walk and
  pyimg4. Kexts: `ipsw kernel kexts`/`extract`, and `__PRELINK_INFO`
  personalities.
- HWX: Apple's compiler on an M1 Ultra (macOS 26.6.2) for targets h16 and
  h17, on an fp16 add and an fp16 mul. Both outputs are 65,536 B. The h17
  `__text` is 0x13c B against 0x130 B; 21 of 304 common bytes differ; the
  `__const` sections are equal.
- No Apple bytes are in this branch: only addresses, sizes, names, flags and
  SHA-256 digests.
