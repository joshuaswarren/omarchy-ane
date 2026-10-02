# H18 / T8152 ANE research receipt

Status: static research only. No Apple firmware was decrypted. No fleet machine was accessed. No driver was written or exercised.

## Chip map and search scope

| Item | Evidence | Result |
| --- | --- | --- |
| H18 SoC | macOS 27.0 (26A428) IPSW `BuildManifest.plist`; `receipts/2026-10-01-ane-every-soc/firmware-27.0.txt`, row `0x8152 j873gap` | T8152 has two ANE firmware entries: `h18_ane0_fw_kirkland_j8xx.im4p`, `h18_ane1_fw_kirkland_j8xx.im4p`. This is the only H18 SoC id in the fetched 27.0 BuildManifest/ADT set. |
| Board identity | `DeviceTree.j873gap.adt` in macOS 27.0 IPSW; same BuildManifest row | `j873g` maps to `arm-io,t8152`. Aurora DTS calls it “Apple Mac mini (M6)” (`aurora-wip:arch/arm64/boot/dts/apple/t8152-j873g.dts`, blob `bd259e3f7e364aa6d9777d829c33548dd738c968`, `model` field). Therefore T8152 is M6, not M5. |
| Same product-family board `j873s` | 27.0 BuildManifest row `0x6050 j873sap`; `adt-27.0.txt` says `j873s: same as j714c` under `arm-io,t6050` | j873s is T6050/H17, not T8152. |
| Newer potential ids | Complete 27.0 extracted set in `fetch-27.0.txt`, `firmware-27.0.txt`, `adt-27.0.txt`; compare T8152 and all `arm-io,t*` sections. Also checked the macOS 13.5 (22G74) extracted BuildManifest/ADT receipts. | No T8160/T8162 or other H18/newer SoC id in those two IPSW extracts. This is not a claim about future Apple catalog entries. |

The previous AneAllSoc run fetched `BuildManifest.plist` and DeviceTree members from the macOS 27.0 (26A428) IPSW and 13.5 (22G74) IPSW using the range-fetch method in `receipts/2026-10-01-ane-every-soc/ipsw_ane.py` (only BuildManifest and DeviceTree members were extracted; no kernelcache was fetched). The full 27.0 member listing and board identifiers are in `receipts/2026-10-01-ane-every-soc/fetch-27.0.txt` and the hashed input list is in `~/.local/share/apple-silicon-lab/artifacts/AneAllSoc/every-soc/hashes/ipsw-members.sha256`. 27.0 members include `DeviceTree.j873gap.adt` (614,344 bytes) and `DeviceTree.j873sap.adt` (823,988 bytes). We did not query Apple catalog endpoints live in this offline pass; absence is only established for the two fetched restore images. Model identifiers checked from those BuildManifest/ADT members: `j873gap` (T8152), `j873sap` (T6050), and every other device class listed in the 27.0 receipt.

## T8152 per-SoC facts

All T8152 ADT values below are from macOS 27.0 `DeviceTree.j873gap.adt`, `/arm-io`, `ane`, `ane1`, `dart-ane`, `dart-ane1`, and pmgr. The exact extracted ADT SHA-256 is `3252a3f13dc025ffa8551c7a5da6a71501f76ea6c5d38aa35390f9268a6dc378` (AneAllSoc `hashes/ipsw-members.sha256`).

- `ane`: compatible `iop-ane,ascwrap-v8`, `ane-type=784`, `die-id=0`; 10 register tuples: `0x419600000+0x74000`, `0x419050000+0x4000`, `0x316000000+0x1c000`, `0x418000000+0x2000000`, `0x300700000+0x1c000`, `0x300724000+0x4000`, `0x3003c0000+0x40000`, `0x211000000+0xff4000`, `0x3082c8000+0x4000`, `0x301200000+0x8000`; interrupts `311 271 788 801 765`.
- `ane1`: same compatible, `ane-type=784`, `die-id=0`; first registers `0x481600000+0x74000`, `0x481050000+0x4000`, `0x316000000+0x1c000`, `0x480000000+0x2000000`, followed by manager windows matching `ane` reg[4..9]; interrupts `312 272 836 813 813`.
- `dart-ane`, `dart-ane1`: compatible `dart,gen3`. First has four `0xc000` windows at `0x419800000`, `0x419820000`, `0x419840000`, `0x419860000`, plus config `0x419810000+0x4000`; IRQ 789. Second uses corresponding `0x4818...` windows; IRQ 837. No `sid`, `vm_base`, or `vm_size` property appears in the extracted ADT.
- ANE gates are named DCS2/DCS3. No T6021-like ANE_SYS/ANE_CPU/TD/SET register sequence is present.
- No `segment-ranges` property is in the IPSW ADT. A live macOS IORegistry or booted m1n1 ADT is needed to determine iBoot's firmware placement. It is not safe to infer a firmware mapping from T6021.
- The 27.0 BuildManifest maps `j873gap` to two files: `h18_ane0_fw_kirkland_j8xx.im4p` and `h18_ane1_fw_kirkland_j8xx.im4p`. The range-fetch receipt reports each member as 1,572,889 bytes. Payloads are encrypted; they were not decrypted or copied. The current ipsw-members hash list does not include either firmware member hash.
- No H18 kernelcache was extracted. Thus the ANE kext class/protocol is unknown, not inferred.

## Design decision

**Do not bind H18 to an existing driver.** omarchy-ane's H13 `ane.ko` and H14 `ane_t6021` bindings do not match `iop-ane,ascwrap-v8`. H18 also uses `dart,gen3`, has two ANE IPs, ten register ranges per ANE node, five interrupts per IP, and only the DCS2/DCS3 named gates in the captured ADT. No H18 mailbox/RTKit or firmware mapping model has been measured. Keep the `t8152` record and overlay data-only. Do not add `omarchy,opt-in` as a bypass: the runtime's data-only state must refuse the overlay even with a key.

### Compiler gap

The measured 2026-09-05 cross-target receipt (`ane-research-mirror/mil-hwx-compiler/receipts/2026-09-05-ane-community/anecompile-cross-target.json`) lists CPU subtype results h13=4, h14=5, h15=6, h16=7, h17=9; H18 was not a target. H16G is a pre-existing compiler label for subtype 7 / ISA 17 (`mil-hwx-compiler/research/inspect_hwx.py`); it does not prove compatibility with the H18 IP. H18 subtype and ISA are unknown. Do not project a subtype/ISA sequence. Required before implementation: obtain a real H18 compile/decode oracle, then add a backend only if HWX supports a distinct H18 target.

### Aurora and kernel state

On aurora-wip, `arch/arm64/boot/dts/apple/t8152-j873g.dts` (commit blob `bd259e3f7e364aa6d9777d829c33548dd738c968`) is titled “Apple Mac mini (M6)” and is minimal RAM boot. It has no ANE/DART nodes. The checked `arch/arm64/boot/dts/apple/Makefile` includes `t8152-j873g.dtb`, but the source DTS has no ANE nodes. The aurora and linux-asahi trees have no H18 `iop-ane,ascwrap-v8` driver or `dart,gen3` binding in the checked sources. Existing kernel support for the T6021 mailbox/RTKit path does not establish H18 protocol compatibility.

`packaging/dt/t8152-ane-dataonly.dts` records the ADT ranges as disabled nodes, without a driver compatible, and carries the root `omarchy,data-only` marker. There is no base DTB in the current kernel tree with the matching T8152 ANE paths, so no fdtoverlay application result is claimed. DTC compilation is the only overlay smoke completed here.

## Staged plan and estimates

Estimates are engineering work estimates, not measured durations. They assume access to an M6/T8152 restore image and an M6 device when the stage requires runtime data.

1. **Resolve address-space and security facts (1–2 engineer days):** fetch only public j873g restore ADT/kernelcache with range requests; record hashes; inspect the kernelcache kext class without decrypting restricted payloads; capture live boot-time segment-ranges and DART sid/VM mapping on an authorized M6. Stop on encrypted payloads; no decrypt attempt.
2. **Driver architecture (2–4 days):** compare H18 ASC wrapper behavior to H15/H17 source and any freely readable kernelcache strings/symbols; identify the IPI/mailbox transport, RTKit compatibility, required DART-gen3 support, resets, power/clocks, and safe entry/exit. No code until each hardware window is powered and mapped by source.
3. **Compiler oracle (2–5 days, hardware-dependent):** compile/inspect real H18 HWX; establish subtype/ISA, target identifier, object layout, and an initial point in the parity envelope. If unavailable, leave H18 compiler support unknown.
4. **Driver and DT implementation (5–10 days):** add a separate H18 driver family and complete binding only after addresses/protocol are grounded; use per-SoC data from `data/ane-soc/t8152.json`; add disabled overlay where the matching kernel DTB exists. Keep runtime opt-in prohibited until end-to-end safe bring-up.
5. **Verification (3–7 days):** first smoke DART/mailbox/firmware startup with bounded timeout and automatic fallback, then run small deterministic compute jobs with output checks, power-cycle/recovery checks, and CI. Hardware access, not the current source record, sets the schedule ceiling.

## Validation and limits

- Schema validator: run `python3 tools/validate_ane_soc.py data/ane-soc/t8152.json` after the shared framework branch provides it. The generator branch did not include the framework's uncommitted script at authoring time; current validator from the shared checkout accepted the JSON with 3 sources and all referenced ids resolved.
- Overlay smoke: `dtc -@ -I dts -O dtb -o /tmp/t8152-ane-dataonly.dtbo packaging/dt/t8152-ane-dataonly.dts` returned 0 with no warnings after 64-bit ranges were encoded as separate high/low cells. No matching base DTB exists in the checked linux-asahi tree, and aurora's minimal DTS contains no ANE targets; fdtoverlay application was not possible.
- No hardware run, no H18 firmware decryption, no kext extraction, no H18 compile, and no H18 driver implementation. Those facts remain unknown.
