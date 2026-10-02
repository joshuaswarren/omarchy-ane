# H18 ANE research receipt: T8150 and T8152

Status: static research only. No Apple firmware was decrypted. No fleet machine was accessed. No driver was written or exercised.

## Chip map and search scope

| Item | Evidence | Result |
| --- | --- | --- |
| H18 SoCs | macOS 27.0 (26A428) and iOS 27.0.1 (24A446) public Restore IPSWs; compiler-generation table | The macOS image maps T8152 / j873gap to Mac mini M6. The iPhone 17 Pro image maps T8150 / v53ap to iPhone 17 Pro (A19 Pro). The H18 generation source associates T8150 with A19/A19 Pro; it does not establish the H18 HWX target for T8152. M5-family T8142 and T6050 are H17, not new H18 ids. |
| Board identity | `DeviceTree.j873gap.adt` in macOS 27.0 IPSW; same BuildManifest row | `j873g` maps to `arm-io,t8152`. Aurora DTS calls it “Apple Mac mini (M6)” (`aurora-wip:arch/arm64/boot/dts/apple/t8152-j873g.dts`, blob `bd259e3f7e364aa6d9777d829c33548dd738c968`, `model` field). Therefore T8152 is M6, not M5. |
| Same product-family board `j873s` | 27.0 BuildManifest row `0x6050 j873sap`; `adt-27.0.txt` says `j873s: same as j714c` under `arm-io,t6050` | j873s is T6050/H17, not T8152. |
| Search boundary | Complete macOS 27.0 and 13.5 (22G74) BuildManifest/ADT extracts; iOS 27.0.1 iPhone18,1 Restore IPSW; public https://api.ipsw.me/v4/devices catalog response (57,790 bytes, SHA-256 in t8150.json S3) | No T8160/T8162 appeared in the checked macOS manifests/ADTs or the inspected iPhone18,1 BuildManifest/ADT. Catalog entries map iPhone18,1–5 to T8150; only iPhone18,1 Restore IPSW was inspected. M5 catalog models map to T8142 or T6050. This is not an exhaustive search of every iOS restore image or future catalog release. |

The previous AneAllSoc run fetched `BuildManifest.plist` and DeviceTree members from the macOS 27.0 (26A428) IPSW and 13.5 (22G74) IPSW using the range-fetch method in `receipts/2026-10-01-ane-every-soc/ipsw_ane.py` (only BuildManifest and DeviceTree members were extracted; no kernelcache was fetched). The full 27.0 member listing and board identifiers are in `receipts/2026-10-01-ane-every-soc/fetch-27.0.txt` and the hashed input list is in `~/.local/share/apple-silicon-lab/artifacts/AneAllSoc/every-soc/hashes/ipsw-members.sha256`. 27.0 members include `DeviceTree.j873gap.adt` (614,344 bytes) and `DeviceTree.j873sap.adt` (823,988 bytes). The public IPSW catalog was queried for model identifiers. `Mac18,5` is listed as Mac mini (M6), with board configuration `J873gAP`; its public 27.0 restore IPSW is on Apple CDN. The extracted BuildManifest/ADT join maps `J873gAP`/`j873gap` to chip 0x8152 and `arm-io,t8152`. `j873sap`/`J873sAP` is the separate 0x6050/T6050 entry. This establishes T8152 as M6, not M5. Searches of the complete fetched 27.0 and 13.5 BuildManifest/ADT sets find no T8160/T8162 and no other T8152-family ID. This does not establish future catalog availability.

## T8152 per-SoC facts

All T8152 ADT values below are from macOS 27.0 `DeviceTree.j873gap.adt`, `/arm-io`, `ane`, `ane1`, `dart-ane`, `dart-ane1`, and pmgr. The exact extracted ADT SHA-256 is `3252a3f13dc025ffa8551c7a5da6a71501f76ea6c5d38aa35390f9268a6dc378` (AneAllSoc `hashes/ipsw-members.sha256`).

- `ane`: compatible `iop-ane,ascwrap-v8`, `ane-type=784`, `die-id=0`; 10 register tuples: `0x419600000+0x74000`, `0x419050000+0x4000`, `0x316000000+0x1c000`, `0x418000000+0x2000000`, `0x300700000+0x1c000`, `0x300724000+0x4000`, `0x3003c0000+0x40000`, `0x211000000+0xff4000`, `0x3082c8000+0x4000`, `0x301200000+0x8000`; interrupts `311 271 788 801 765`.
- `ane1`: same compatible, `ane-type=784`, `die-id=0`; first registers `0x481600000+0x74000`, `0x481050000+0x4000`, `0x316000000+0x1c000`, `0x480000000+0x2000000`, followed by manager windows matching `ane` reg[4..9]; interrupts `312 272 836 813 813`.
- `dart-ane`, `dart-ane1`: compatible `dart,gen3`. First has four `0xc000` windows at `0x419800000`, `0x419820000`, `0x419840000`, `0x419860000`, plus config `0x419810000+0x4000`; IRQ 789. Second uses corresponding `0x4818...` windows; IRQ 837. No `sid`, `vm_base`, or `vm_size` property appears in the extracted ADT.
- ANE gates are named DCS2/DCS3. No T6021-like ANE_SYS/ANE_CPU/TD/SET register sequence is present.
- No `segment-ranges` property is in the IPSW ADT. A live macOS IORegistry or booted m1n1 ADT is needed to determine iBoot's firmware placement. It is not safe to infer a firmware mapping from T6021.
- The 27.0 BuildManifest maps `j873gap` to two files: `h18_ane0_fw_kirkland_j8xx.im4p` and `h18_ane1_fw_kirkland_j8xx.im4p`. The range-fetch receipt reports each member as 1,572,889 bytes. A supplement range-fetched both firmware members and `kernelcache.release.mac18g` from the public macOS 27.0 IPSW. Both H18 IM4P members have four DER fields and no KBAG. Their OCTET STRING payloads begin with Mach-O `cffaedfe` and are freely readable, so they are not encrypted. No decryption was performed. The exact IPSW-member sizes and SHA-256 values, unwrapped payload hashes, and BuildManifest flags are in the JSON and `h18-firmware-kext.txt` private artifact.
- The macOS kernelcache is a public LZFSE-compressed Mach-O fileset. Its ANE personality and HAL evidence are detailed below; no firmware payload was copied into the repository.

## H18 firmware and kernelcache supplement

The artifact receipt is `~/.local/share/apple-silicon-lab/artifacts/AneGenH18/ane-gen-h18/h18-firmware-kext.txt` (SHA-256 listed in that directory's `SHA256SUMS`). The source IPSW is macOS 27.0 (26A428), catalog device `Mac18,5`, board `J873gAP`, URL `https://updates.cdn-apple.com/2026FallFCS/59241d50-5d51-4ca8-9df4-31624b9a4aec/UniversalMac_27.0_26A428_Restore.ipsw`.

- BuildManifest row `0x8152 j873gap`: ANE0 and ANE1 are `h18_ane0_fw_kirkland_j8xx.im4p` and `h18_ane1_fw_kirkland_j8xx.im4p`. Both are `IsFUDFirmware=true`; ANE0 is `IsLoadedByiBoot=true`, ANE1 is false. `Ap,RestoreANE1` is separately marked `IsLoadedByiBoot=true`. These are loader flags, not proof of runtime behavior.
- Each firmware member is 1,572,889 bytes. Four DER fields encode three IA5 strings and one OCTET STRING. Neither member has a KBAG field. Each extracted payload is 1,572,864 bytes and begins with Mach-O 64-bit little-endian magic `cffaedfe`. Both are unencrypted; nothing was decrypted.
- ANE0 names `RTKSTACKRTKit_release-3514.0.15.release`, `PowerControl_H18g`, `./sne/drivers/tm/CSneTMDrvH18g.cpp`, and `./sne/drivers/tm/CSneTMDrvHx.cpp`. ANE0 and public H14/T602x Selene firmware contain 97 unique `CSNE_CMD_*` identifiers each; their identifier sets have no differences. This does not establish that firmware protocols or parameter semantics are interchangeable.
- `kernelcache.release.mac18g` is 32,759,747 bytes, has an IM4P `bvx2` LZFSE payload, and expands to a Mach-O fileset. The `com.apple.driver.AppleA7IOP-ASCWrap-v8` kext registers personality `AppleASCWrapV8ANE`, matching `iop-ane,ascwrap-v8` on `AppleARMIODevice`. This confirms an Apple ASCWrap-v8 path, not a Linux implementation.
- The same kernelcache has `com.apple.driver.AppleH16ANEInterface` with H14 `H11ANEIn` personalities (`ane,t8020`, `ane,t8132exclave`), and contains the string `AppleT8150ANEHAL`. The H18 HAL string is not a separate IOKit personality in the parsed metadata.
- The aurora-wip T8152 DTS identifies Mac mini (M6); its minimal RAM boot DTS has no ANE node. T8152 is M6, not M5. The separate j873s board is T6050/H17. No T8160/T8162 appears in the complete macOS 27.0 or 13.5 fetched identity/ADT sets.

## T8150 / A19 Pro per-SoC facts

The inspected iOS 27.0.1 (24A446) iPhone18,1 Restore IPSW maps ApChipID 0x8150, ApBoardID 0x0C, and DeviceClass v53ap. The public catalog maps iPhone18,1 to iPhone 17 Pro / V53AP / platform T8150. The H18 generation table associates A19/A19 Pro and T8150 with H18, CPU subtype 10 and ANE ISA 20; this is a reverse-engineered medium-confidence mapping, not a measured compile for T8150 or T8152. Catalog entries iPhone18,2–5 also report T8150, but their Restore IPSWs and ADTs were not individually inspected.

- `DeviceTree.v53ap.adt` has one ANE node, compatible `ane,t8132exclave`, `ane-type=768`, IRQs 607 and 620, six register tuples, gate ids 331/414 (`ANE-SYS-V`, `ANE-SYS-V-PMP`), clock ids 318/317/357, and `exclave-edk-service=com.apple.service.ANEExclave_EDK`. Its DART is `dart,t8110`, with four register tuples, IRQ 608, SID bytes `000000000a0000000b0000000d0000000f000000`, count 16, VM base `0x10000000000`, size `0x30000000000`, and page size 16384. No ANE `segment-ranges` or mailbox node was present. Full tuples and citations are in `data/ane-soc/t8150.json`.
- BuildManifest maps T8150 / `v53ap` to `h18_ane_fw_apollo_v5x.im4p`. The member is 1,556,505 bytes (SHA-256 in the JSON); the 1,556,480-byte OCTET STRING is readable ARM64 Mach-O (`cffaedfe`) with no KBAG. No decryption was performed.
- The readable, unencrypted `kernelcache.release.v53` member expands from its `bvx2` payload to a Mach-O fileset containing `com.apple.driver.AppleH16ANEInterface`, personality `t8132exclave / H11ANEIn`, matched to `ane,t8132exclave` on AppleARMIODevice with `IOExclaveProxy=true`. This is an Exclave/H11ANEIn path, not the M6 ASCWrap-v8 path. Exact hashes and metadata are in `data/ane-soc/t8150.json`.


## Design decision

**Keep both H18 SoCs data-only and treat them as separate driver-family candidates.** T8152/M6 uses `iop-ane,ascwrap-v8`, `dart,gen3`, two ANE IPs, and Apple's `AppleA7IOP-ASCWrap-v8` / `AppleASCWrapV8ANE` path. T8150/A19 Pro uses `ane,t8132exclave`, `dart,t8110`, and Apple's `AppleH16ANEInterface` / `H11ANEIn` ExclaveProxy personality. The shared H18 generation label does not show compatible register, DART, mailbox, or firmware protocols. A scoped source search found no Linux driver matching either ANE compatible. Keep the T8152 overlay disabled; do not create a T8150 overlay without a matching base device tree. Do not bind either path to the H14 driver or allow opt-in before driver implementation and hardware validation.

### Compiler gap

The measured 2026-09-05 cross-target receipt (`ane-research-mirror/mil-hwx-compiler/receipts/2026-09-05-ane-community/anecompile-cross-target.json`) reports h13=4, h14=5, h15=6, h16=7, h17=9; it did not request h18. A separate reverse-engineered compiler table in `mil-hwx-compiler/docs/ane/generations.md` associates H18 with A19/A19 Pro and T8150, CPU subtype 10 and ANE ISA 20. That does not map H18 to T8152/M6, and the table labels its mapping medium evidence. The T8152/M6 subtype and ISA therefore remain unknown. Do not project the sequence or transfer the A19/T8150 mapping. Before implementation, obtain and decode an actual T8152/M6 compile oracle.

### Aurora and kernel state

The aurora-wip `arch/arm64/boot/dts/apple/t8152-j873g.dts` (blob `bd259e3f7e364aa6d9777d829c33548dd738c968`) is titled “Apple Mac mini (M6)” and is a minimal RAM boot tree with no ANE/DART nodes. A scoped search found no `iop-ane,ascwrap-v8` or `ane,t8132exclave` string under the current kernel `drivers/` source tree or this repository's `ane/` source tree. The aurora DTS set contains no T8150 board target. Existing T6021 mailbox/RTKit support does not establish either H18 protocol.

`packaging/dt/t8152-ane-dataonly.dts` records the M6 ADT ranges as disabled nodes, without an ANE compatible or phandle, and carries root marker `omarchy,data-only`. IRQ values remain in JSON because the checked AIC node does not export a symbol. No matching T8152 ANE base path exists, so no overlay application result is claimed. No T8150 overlay was created because the checked tree has no matching iPhone base DT.

## Staged plan and estimates

Estimates are engineering work estimates, not measured durations. The runtime stages assume access to an authorized A19 Pro/T8150 phone and M6/T8152 system.

1. **Resolve address and security facts (1–2 engineer days per SoC):** capture live segment ranges, DART SID/VM mappings, and boot-time firmware placement on each target. Keep the T8150 Exclave path and T8152 ASCWrap path separate. Stop on encrypted payloads; do not decrypt them.
2. **Establish protocols (2–4 days per driver family):** determine IPI/mailbox, RTKit or Exclave service, DART setup, resets, clocks, power domains, and safe entry/exit from source and bounded hardware observations. Do not reuse H14 protocol assumptions.
3. **Compiler oracle (2–5 days per target, hardware-dependent):** compile real T8150 and T8152 HWX independently; establish subtype/ISA, target id, and object layout. The measured H11–H17 receipt is not an H18 oracle.
4. **Driver and DT implementation (5–10 days per family):** only after the protocol is grounded, implement separate compatible paths if evidence still requires them. Add an overlay only where a matching base DT exists; keep runtime opt-in prohibited until safe bring-up.
5. **Verification (3–7 days per supported device family):** smoke DART/mailbox or Exclave service and firmware startup with timeouts and fallback; run deterministic output checks and power-cycle recovery tests, then CI. Hardware access sets the schedule ceiling.

## Validation and limits

- Schema validation: the restored shared `tools/validate_ane_soc.py` reports `ok t8150 state=data-only sources=7 leaves=7` and `ok t8152 state=data-only sources=10 leaves=10`. `tools/gen_coverage_table.py --write` generated the H18 rows in the repository README.
- Overlay smoke: `dtc -@ -I dts -O dtb` compiled `packaging/dt/t8152-ane-dataonly.dts`, rc 0, no stderr, output 1015 bytes. Decompilation confirmed `omarchy,data-only = "true"`, four disabled nodes, no ANE compatible, and no external fixups. FrameworkFinish reported that the previous overlay revision failed application to aurora-wip because its base did not export `&aic`; the revision removes that unresolved reference and keeps IRQ facts in JSON. No application test was run against a matching T8152 ANE base path because none exists in the checked trees. `packaging/dt/overlays` is the package manifest file, owned by the framework agent; no manifest edit was made here.
- No hardware run, H18 compile target/oracle, or Linux driver implementation. Public firmware and kernelcache records were inspected without decrypting payloads; these facts do not establish a Linux runtime path.
