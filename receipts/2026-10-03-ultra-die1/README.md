# 2026-10-03: Ultra die-1 ANE capture (T6002 live, T6022 from ADT)

Read-only captures for die-1 ANE planning. The live source is one M1 Ultra
desktop (Mac13,2, T6002, j375d, macOS 26.6.2), read over ssh with `ioreg` only.
No kext load, no dtrace, no sysctl write, no reboot, no sudo. The T6022 (M2
Ultra) rows come from the static 13.5 ADT decode in
[receipts/2026-10-01-ane-every-soc](../2026-10-01-ane-every-soc/README.md)
(j475d = j180d), which the live T6002 capture cross-checks.

Raw outputs stay outside the repo, in the lab store:
`~/.local/share/apple-silicon-lab/artifacts/UltraDie1/2026-10-03-ultra-die1-capture/`
(with SHA256SUMS). This receipt carries decoded values only: node names,
property names, reg windows, interrupt numbers, compatible strings. No
firmware or kernelcache bytes.

## Capture files and SHA-256

| File | Bytes | SHA-256 | Content |
| --- | --- | --- | --- |
| raw-adt-ioreg.txt | 1753051 | e7181f56194fecbf9c6cdc405f653595853c3b5791fee1e31a446d32e8e30b17 | `ioreg -p IODeviceTree -l -w0`, full plane |
| raw-adt-pmgr.txt | 122261 | 1addfad42d24d347ec3cc10c4bb9bcc543dbf5e61e319b3300327456c6ea2334 | pmgr node, DT plane |
| raw-adt-ane0.txt | 1968 | 78de0ccc3539877f4765a226a88b9713afa015ef635b4b6403aabba47e1dcd9b | ane0 node |
| raw-adt-ane2.txt | 1920 | 77f34afee69b26d8aba43a391142d331b0cf423b042812e6fdc54fd7848c5033 | ane2 node (die 1) |
| raw-adt-dart-ane0.txt | 3250 | 27b3bb3abccb426ffe2a69b896540e87bcb7ee2721a717883c07165b30c0b47a | dart-ane0 node |
| raw-adt-dart-ane2.txt | 3309 | 18335df034815bff55bdf7f190b2de73f7e5c6226a032187b25acf33d827177b | dart-ane2 node (die 1) |
| ioservice-ane-grep.txt | 878776 | afa3000df274d614fbcfc4797e2d88ae98d65895469f6e1b5158b455b13e725b | IOService lines naming ane |
| ioservice-H11ANEIn.txt | 2607 | b5fa86dbc01fc74386111afed88ec7afd345881943f792e2a40b00d57e074d8d | both ANE IOService devices |
| ioservice-H1xANELoadBalancer.txt | 1982 | 944f8ddb1ceb07a53009eba7d40465e3ec23e98d5af39af325e40db0ecd3fb89 | ANEDriverRoot balancer |
| raw-adt-ane1.txt, raw-adt-ane3.txt, raw-adt-dart-ane1.txt, raw-adt-dart-ane3.txt | 0 | e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 (empty files) | absence evidence: `-n <name>` matched nothing |

Full hashes: lab store `SHA256SUMS`. The zero-byte files are the measurement:
the live tree has no ane1, ane3, dart-ane1, dart-ane3 nodes.

## T6002 (M1 Ultra), live macOS 26.6.2 versus 13.5 ADT: die 0 vs die 1

MEASURED = this capture. Cross-check = 13.5 IPSW ADT j375d (2026-10-01
receipt). Every live value equals the 13.5 decode.

| Property | die 0 (`ane0`) | die 1 (`ane2`) | Relation |
| --- | --- | --- | --- |
| compatible | `ane,t8020` | `ane,t8020` | same |
| engine window | 0x284000000 + 0x2000000 | 0x2284000000 + 0x2000000 | +0x20_0000_0000 |
| pmgr window (reg[1]) | 0x28e080000 + 0xc02c | 0x228e080000 + 0xc02c | +0x20_0000_0000 |
| SET block (driver constant) | 0x28e08c000 | 0x228e08c000 | +0x20_0000_0000 |
| interrupt | 770 | 4866 | die-1 AIC space: 4096 + 770, same die line |
| die-id | 0 | 1 | measured |
| ane-id | 0 | 2 | global instance number |
| die-ane-id | 0 | 0 | per-die index, same |
| ane-subtype | 0 | 2 | differs; meaning unknown |
| clock-gates / power-gates | 0x000001cf | 0x100001cf | bit 28 = die flag |
| `pre-loaded` | 1 | 1 | iBoot preloads die-1 firmware too |
| segment-ranges | present (2 entries) | present (2 entries) | values not decoded here |
| firmware (13.5 BuildManifest) | `t600x_ane0_fw_eos_jc3x` | `t600x_ane2_fw_eos_jc3x` | one image per instance (ANE0..ANE3) |

DARTs:

| Property | die 0 (`dart-ane0`) | die 1 (`dart-ane2`) | Relation |
| --- | --- | --- | --- |
| compatible | `dart,t6000` | `dart,t6000` | same |
| reg windows | 0x285800000, 0x285810000, 0x285820000, 0x285804000, each +0x4000 | 0x2285800000, 0x2285810000, 0x2285820000, 0x2285804000, each +0x4000 | +0x20_0000_0000 |
| interrupt | 771 | 4867 | 4096 + 771, same die line |
| SIDs | one stream used (sid 0) | one stream used (sid 0) | same shape |

Power states (13.5 ADT names; the live macOS ioreg carries no ps entries, so
names come from the ADT decode; offsets identical per die):

| ps name | offset in die pmgr page | die 0 page | die 1 page | parent |
| --- | --- | --- | --- | --- |
| ANE_SYS | 0x268 | 0x28e080000 | 0x228e080000 | AFR (die's own) |
| ANE_SYS_CPU | 0x2c8 | 0x28e080000 | 0x228e080000 | ANE_SYS |
| ANE_SET0 | 0xc000 | 0x28e080000 | 0x228e080000 | ANE_SYS_CPU |
| ANE_BASE | 0xc008 | 0x28e080000 | 0x228e080000 | ANE_SET0 |
| ANE_SET1..4 | 0xc010..0xc028 | 0x28e080000 | 0x228e080000 | ANE_BASE chain |

AIC: the live tree has one `aic@8e100000` node (phandle 0xb1, "master"); both
ane0 and ane2 name it as interrupt-parent. The die-1 line numbers 4866/4867
live in that one flat macOS ADT numbering.

Absent from the live tree: ane1, ane3, dart-ane1, dart-ane3. The 13.5 IPSW ADT
j375d lists them (ane1 at 0x508000000 IRQ 797, ane3 at 0x2508000000 IRQ 4893,
DARTs dart-ane1/dart-ane3 with IRQ 798/4894). macOS boots with them pruned;
the aurora tree keeps their pmgr states `#if 0` ("seems to be disabled on
shipping hardware"). MEASURED: macOS drives one ANE per die on this desktop.

IOService plane (MEASURED):

| Object | Class | Count | Notes |
| --- | --- | --- | --- |
| H11ANE, H11ANE2 | `H11ANEIn` | 2 | one per die; both match `ane,t8020`; both `FirmwareLoaded=Yes`; 16 cores each; ANEVersion 96, ANEMinorVersion 17; ANEHWBoardSubType 0 vs 2 |
| ANEDriverRoot | `H1xANELoadBalancer` | 1 | `ANEDevicePropertyNumANEs=2`; serves `aned` and two direct-path clients |

macOS shape: two independent ANE devices plus one load balancer above them.

## T6022 (M2 Ultra), 13.5 ADT j475d/j180d: die 0 vs die 1

| Property | die 0 (`ane0`) | die 1 (`ane1`) | Relation |
| --- | --- | --- | --- |
| compatible | `ane,t8020` | `ane,t8020` | same |
| engine window | 0x284000000 + 0x2000000 | 0x2284000000 + 0x2000000 | +0x20_0000_0000 |
| pmgr window | 0x28e080000 + 0x4034 | 0x228e080000 + 0x4034 | +0x20_0000_0000 |
| SET window (reg[2]) | 0x28e08c000 + 0x4000 | 0x228e08c000 + 0x4000 | +0x20_0000_0000 |
| interrupt | 884 | 4980 | 4096 + 884, same die line |
| ane-type | 160 | 160 | same |
| die-id | 0 | 1 | ADT decode |
| firmware (13.5 BuildManifest) | `t602x_ane0_fw_selene_rc4x` | `t602x_ane1_fw_selene_rc4x` | one image per die |
| dart-ane | 0x285800000/810000/820000/804000, IRQ 885, `dart,t8110` | 0x2285800000/810000/820000/804000, IRQ 4981, `dart,t8110` | +0x20_0000_0000; 4096 + 885 |
| pmgr ps states | ANE_SYS 0x260 (parent AFNC0_LW0), ANE_CPU 0x2e0, ANE_SYS_MPM 0x4000, ANE_TD 0x4008, ANE_BASE 0x4010, ANE_SET1..4 0x4018..0x4030 | same names, same offsets | die-1 page 0x228e08xxxx |

## Rules the tables show

1. Die 1 is a pure +0x20_0000_0000 address translation of die 0, for every
   window, on both families. MEASURED on T6002 live; ADT-decoded on T6022.
2. Interrupts: same line number per die (ANE 770 T600x / 884 T602x; DART 771 /
   885), die-1 lines at +4096 in the flat macOS ADT numbering. MEASURED on 4
   node pairs across T6002, T6022, T8112 (die-1 dart 4981 = 4096 + 885).
3. Power-state names, order and offsets are identical per die; only the pmgr
   page moves by +0x20_0000_0000.
4. DART count (3), SID use (sid 0 per DART) and compatible (`dart,t6000` /
   `dart,t8110`) are identical per die. No die-specific DART driver data is
   visible in the node data.
5. Clock/power gates carry a die flag (bit 28) instead of separate names.
6. Each ANE instance has its own firmware image name (T6002: four images
   ANE0..ANE3; T6022: ANE, Ap,ANE1).
7. macOS exposes one ANE per die on T6002 (ane1/ane3 pruned) and drives two
   `H11ANEIn` devices behind one load balancer.
