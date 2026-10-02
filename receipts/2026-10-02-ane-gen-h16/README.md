# H16 (M4 family) ANE static survey

## Result

The 27.0 (26A428) IPSW confirms three H16 ANE SoC identifiers: T8132, T6040,
and T6041. T8132 ADTs use `arm-io,t8132`; both T6040 BuildManifest rows and
T6041 rows use `arm-io,t6041`. The T6040 / T6041 ADTs inspected here expose
the same ANE and DART register layout. Preserve the BuildManifest chip IDs;
do not treat the ADT compatible as proof that the two chip IDs are identical.

All three new DTS overlays are **data-only** and disabled. They add only a
`omarchy,ane-data-only` record with source values. They do not create a live
ANE node, enable a DART, bind a driver, or make the ANE usable. The branch
has no H16 driver.

## Source and chip map

Primary source: macOS 27.0 26A428 IPSW:

`https://updates.cdn-apple.com/2026FallFCS/afcfc88e-bbe6-44bf-a5da-07c56eebc06c/UniversalMac_27.0_26A428_Restore.ipsw`

AneAllSoc already range-read this IPSW's `BuildManifest.plist` and all ADT
members. I range-read and decompressed the following twelve H16/T604x ADTs
again. No full IPSW was downloaded. IPSW facts and decoded ADT fields are
summarized in the public AneAllSoc source receipt
`~/.local/share/apple-silicon-lab/artifacts/AneAllSoc/every-soc/receipt/`;
private run hashes and extracted files remain outside this repo.

| BuildManifest chip ID | DeviceClass | ADT `target-type` / root model | ADT arm-io compatible | ANE firmware |
| --- | --- | --- | --- | --- |
| `0x8132` | `j604ap`, `j623ap`, `j624ap`, `j713ap`, `j715ap`, `j773gap` | J604/Mac16,1; J623/Mac16,2; J624/Mac16,3; J713/Mac16,12; J715/Mac16,13; J773g/Mac16,10 | `arm-io,t8132` | `h16_ane_fw_leto_j7x.im4p` |
| `0x6040` | `j614sap`, `j616sap`, `j773sap` | J614s/Mac16,8; J616s/Mac16,7; J773s/Mac16,11 | `arm-io,t6041` | `t604x_ane_fw_aether_brvx.im4p` |
| `0x6041` | `j575cap`, `j614cap`, `j616cap` | J575c/Mac16,9; J614c/Mac16,6; J616c/Mac16,5 | `arm-io,t6041` | `t604x_ane_fw_aether_brvx.im4p` |

 Aurora's `aurora-wip` `arch/arm64/boot/dts/apple/t8132.dtsi:3` explicitly names T8132 as M4. Board DTS strings give J604 = MacBook Pro 14-inch M4 (Mac16,1); J623/J624 = iMac 24-inch M4 (Mac16,2/3); J713/J715 = MacBook Air M4 13/15-inch (Mac16,12/13); and J773g = Mac mini M4 (Mac16,10). T8132 is the base M4 in these Mac models. The 27.0 Mac IPSW lists no iPad board for T8132.
 The T6040/T6041 BuildManifest rows give `ApChipID`, `DeviceClass` and `Mac16,x` identifiers, but no marketing tier. The inspected Aurora `aurora-wip` tree has no T6040/T6041 DTS model strings. Their M4 Pro/Max attribution and per-tier mapping are **unverified** here. The requested shorthand “T604x Pro/Max” does not support assigning either ID to a particular tier.

### T8132 ADT values (J604, J623, J624, J713, J715, J773g)

`/arm-io/ane`: `ane,t8020`; `ane-type=256`; IRQs 629 and 642; registers
`0x500000000+0x2000000`, `0x380700000+0x18000`,
`0x380724000+0x4000`, `0x3803c0000+0x30000`,
`0x211000000+0xff4000`, `0x3882c8000+0x4000`.
`/arm-io/dart-ane`: `dart,t8110`; registers
`0x501800000+0xc000`, `0x501820000+0xc000`, `0x501840000+0xc000`,
`0x501810000+0x4000`; IRQ 630; SID `0x0a00000000`, `0x0f0000000b`;
page size 16384; VM base `0x10000000000`, size `0x30000000000`.
Pmgr lists ANE_SYS `0x380700570`, ANE_MPM `0x38070c000`, ANE_CPU
`0x38070c008`, ANE_TD `0x38070c010`, and ANE_BASE `0x38070c018`.
The `ANE-SYS-V` and `ANE-SYS-V-PMP` clock-gates have `no_ps` and are not
pmgr power-state entries.

### T6040 and T6041 ADT values

T6040 J614s/J616s/J773s and T6041 J575c/J614c/J616c share the decoded ANE
layout. `/arm-io/ane0`: `ane,t8020`; `ane-type=272`; IRQ 1054; registers
`0x484000000+0x2000000`, `0x502280000+0x18000`, `0x5022a0000+0x4000`,
`0x5003c0000+0x30000`, `0x212000000+0xff4000`,
`0x522000000+0x2000000`, `0x508374000+0x4000`.
`/arm-io/dart-ane`: `dart,t8110`; registers
`0x485800000+0xc000`, `0x485820000+0xc000`, `0x485840000+0xc000`,
`0x485810000+0x4000`; IRQ 1055; SID `0x0a00000000`, `0x0f0000000b`;
page size 16384; VM base `0x10000000000`, size `0x30000000000`.
Pmgr lists ANE_SYS `0x5022803c0`, ANE_MPM `0x50228c000`, ANE_CPU
`0x50228c008`, ANE_TD `0x50228c010`, ANE_BASE `0x50228c018`.
`ANE-SYS-V` and `ANE-SYS-V-PMP` are no-PS gates. T6040's `arm-io,t6041`
compatible is an observed alias; firmware chip IDs remain separate.

## Firmware, kext, and driver verdict

| Fact | H16 evidence |
| --- | --- |
| T8132 ANE firmware | `h16_ane_fw_leto_j7x.im4p`, 1,556,505-byte wrapper, SHA-256 `594fb62ef07a628c37e22c9d91b1d96855c72a6471eb9d25a260998c9496904d`. HTTP range-read IM4P ASN.1 is `SEQUENCE { IM4P, anef, 1, OCTET STRING }`; payload is 1,556,480 bytes and begins with Mach-O `MH_MAGIC_64` (`cffaedfe`). No KBAG or extra keybag field; no LZFSE marker. `encrypted=false`.
| T6040/T6041 ANE firmware | Shared `t604x_ane_fw_aether_brvx.im4p`, 1,556,505-byte wrapper, SHA-256 `976c31a5c0ea3d3110293cb99b30342d085e0b1bf1d5f015d811938b0981d4a3`. Same IM4P ASN.1 structure; its 1,556,480-byte Mach-O payload begins `cffaedfe`. No KBAG or extra keybag field; no LZFSE marker. `encrypted=false`. Both wrappers were read by HTTP ranges from the Apple IPSW; no Apple payload is included in the repository or artifacts.
| H16 kext class/protocol | Not established. This pass did not extract or inspect the 27.0 kernelcache. No class/protocol claim is made. |
| Driver family | Unresolved. The ADT says `ane,t8020`, as do older ANE blocks; this alone does not establish H13's host-TM driver path. The H14 `ane_t6021` RTKit/ASC path is a plausible probe target, not a demonstrated H16 match. |
| Mailbox | No mailbox node or mailbox property was found in the H16 ANE/DART ADT fields. This does not prove that no mailbox exists elsewhere in the ADT or that the firmware needs none. |
| Linux mailbox support | The project tree contains an apple-mailbox TX-poll fix used for the T6021 lane. H16 mailbox compatibility is unknown until the actual H16 mailbox resource, protocol and interrupt behavior are identified. |
| HWX compiler | `mil-hwx-compiler` emits H16G (CPU subtype 7, ISA 17); its H16G target code and tests exist. H16G is the M4 compiler target. This closes neither firmware startup nor runtime protocol and is not proof that arbitrary H16 programs work. |

H16 differs from H14 by the firmware names/versions and by its H16-specific
ADT register topology. Relative to the M3 H15 `iop,ascwrap-v6` blocks, H16
reports `ane,t8020` and a single ANE node in these boards. Relative to M2
T6021, it has seven ANE `reg` entries on T604x and six on T8132, two
interrupts on T8132, `ANE-SYS-V-PMP`, and the T604x-specific AUX regions.
These are source-observed differences; their functional meanings are not
inferred here. No H16 evidence supports reusing H13 TM/TQ or H14's exact boot
sequence.

## Aurora kernel state

At the inspected aurora-wip ref `b3a55ee68bfb`, the T8132 DTS family exists
(`t8132.dtsi`, `t8132-j604.dts`, `t8132-j623.dts`, `t8132-j624.dts`,
`t8132-j713.dts`, `t8132-j715.dts`, `t8132-j773g.dts`), but the T8132 dtsi
contains no ANE node. No T6040/T6041 DTS or board file was present in the tree.
The public linux-asahi 7.1.13 tree does not establish the complete H16 ANE
node. This pass did not fetch or inspect a newer independent upstream branch.
The kernel prerequisite is therefore not met: add/split the missing DT before
attempting any driver bind. No H16 RTKit/mailbox driver support was found or
proved by this survey.

## Data-only overlays

`packaging/dt/t8132-ane-dataonly.dts`, `t6040-ane-dataonly.dts`, and
`t6041-ane-dataonly.dts` contain extracted ANE/DART source values under an
`omarchy,ane-data-only` node with `status = "disabled"`. They deliberately do
not claim a kernel compatible or set a live ANE node. The current packaging
manifest does not include these files. Keep them uninstalled until a kernel
driver exists and the shared data-only overlay framework recognizes the
marker. No overlay was tested against a kernel DTB or hardware in this run.

## Staged plan and estimates

Estimates are engineering work time, not calendar commitments. Each step has
a stop condition; do not skip the prior proof.

1. **Resolve T604x model attribution (1–2 days):** find a first-party source linking the IPSW `DeviceClass` / `Mac16,x` identifiers to M4 Pro or M4 Max. Keep each ID unverified if no source exists.
2. **Read-only kernel/firmware probe (2–4 days):** range-extract only the H16 kernelcache. Identify the ANE kext class, mailbox register/interrupt, loader and boot messages; verify the clear Mach-O payload's startup requirements. Never decrypt a payload marked encrypted. Require evidence from at least one T8132 and one T604x board before generalizing.
3. **Kernel DT / mailbox groundwork (3–7 days):** submit upstream-compatible
   T8132 DT node(s), add T6040/T6041 board descriptions or a proven shared
   include, and establish DART and power sequencing. Add mailbox support only
   after identifying a concrete resource and protocol. Keep all new ANE
   nodes disabled and avoid reads from an unpowered engine.
4. **Driver design and implementation (3–6 weeks):** port only after step 2
   identifies the protocol. Reuse H14 RTKit plumbing only if H16 firmware
   proves the same handshake. Implement H16-specific DART, pmgr, PMP and
   firmware handling, without claiming H13/H14 compatibility prematurely.
5. **Data-only promotion to opt-in (1–2 weeks after driver binding):** update
   the common framework, promote one SoC/board at a time, and keep nodes
   disabled by default. Require a real firmware startup, mailbox response,
   DART mapping, safe power cycle, and successful userland inference before
   advancing from data-only.
6. **Cross-board qualification (1–3 weeks):** repeat on T8132 and T604x
   silicon, including the T6040 and T6041 manifest classes. Confirm identical
   behavior before sharing a driver descriptor. Keep unsupported boards
   gated.

The complete end-to-end effort is not complete. This assignment delivers the
static records and data-only entries only. Driver implementation, firmware
interpretation, mailbox protocol, boot, and inference remain unverified.

## Restored framework-base validation

The H16 files were replayed onto `bbc41a6` after the data-only framework was restored. The SoC records now use the framework’s plain-string `soc` and `state` fields and contain full SHA-256 source digests. The scoped JSON validator accepted all three records, and `dtc` compiled all three disabled data-only overlays. This verifies schema and DTS syntax only; it does not verify a kernel DTB or hardware.

The restored-base packaging check also completed: `packaging/build-dtbo /tmp/h16-package-check` compiled all three H16 data-only overlays without installing them and installed all three H16 JSON records under `/usr/share/omarchy-ane/soc/`. `python3 tools/gen_coverage_table.py --check` returned `gen_coverage_table: ok` after the generated README table was committed.
