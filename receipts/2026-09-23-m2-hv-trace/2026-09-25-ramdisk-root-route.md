# macOS userspace under the hv, no-hands route ranking (2026-09-25)

## (1) Ramdisk root — staged, fastest viable
The 22G74 IPSW (`UniversalMac_13.5_22G74_Restore.ipsw`, 12893195726 B, still
signed per ipsw.me) contains the restore root `022-15462-082.dmg` (1.8 GB
APFS, sealed "macOS Base System" 13.5/22G74). Its
`System/Library/dyld/dyld_shared_cache_arm64e` covers IOKit,
CoreFoundation and libSystem, so a small arm64e IOKit binary runs there
with no userspace install. XNU 13.5 (xnu-8796) supports exactly this path:
`IOKitBSDInit.cpp` turns `/chosen/memory-map RAMDisk = (base, size)` into
`/dev/md0`, `rd=md0` selects it as root, and with `-rootdmg-ramdisk
rp=file:///ane-root.dmg` `imageboot_setup_new` reads the inner dmg off the
ramdisk root and mounts it (`bsd/kern/{bsd_init.c,imageboot.c}`). The
boot-args string is 170 chars, inside m1n1's 1024-char BootArgs_r3 field.
`tools/m2hv_ramdisk.py` (new hv `-m` module) uploads the dmg in 64 MB
chunks to the first 1 GB-aligned address above the m1n1 heap top inside
mapped guest RAM and sets the guest ADT RAMDisk/RDRamDisk/RDSize entries;
validated off-box against the decoded J414c ADT (chunking math plus an ADT
rebuild/reparse round-trip of the new entries). `tools/m2hv_catch_and_run.sh`
takes `M2HV_BOOTARGS` (full string override) and `M2HV_PREMOD` (extra `-m`
module) so the same launcher runs the ramdisk boot.

Staged on macstudio `~/src/ane-artifacts/hvramdisk/ane-root-22G74.dmg`
(sha256 `da372082fc1148f352a7433e546f7cabd6b24fe3df7d3d3d0b9299724403699d`,
1.8 GB): the restore root with `/sbin/launchd` replaced by `ane_open`
(arm64e, 52 KB, `com.apple.ane_open` adhoc signature, one entitlement;
verified byte-identical inside the rebuilt dmg, md5
`1362469b03a37a93fecef190ed2839e0`). `ane_open` matches `H11ANEIn` (falls
back to `H11ANE`), calls `IOServiceOpen`, prints the return, sleeps 25 s
with the client held, then exits. Kernel pid 1 is `/sbin/launchd`
unconditionally on RELEASE (`bsd/kern/kern_exec.c`), so no init override
is needed. Open risks, in order: (a) the modified APFS volume fails its
seal check at mount — the restore root is a sealed volume and the file
swap breaks the seal; the `-rootdmg-ramdisk` read path may refuse it;
(b) AMFI/launchd trust on the adhoc binary (`amfi_get_out_of_my_way=1
cs_enforcement_disable=1` are in the boot-args for this); (c) the ANE
kext's firmware source in the ramdisk-rooted guest.

## (2) The 26/27 install under the hv — disproven as a no-hands route
The 26/27 kernelcache spins at `_start+0x10` on `[x1+8]==0`. m1n1's
`hv_start` passes `(entry, bootargs_phys)` and the 13.5 kernel reads x1
as the boot-args pointer, so x1 itself is fine; what 26/27 wants at
`boot_args+8` is a new second entry word its entry ABI requires, and no
source available off-box defines it. Decoding it needs the 26/27 kernel
entry disassembly plus the SPTM-era boot contract, which is a separate
research task, not a run. Ranked below (1): unknown scope, no staged
artifact.

## (3) 13.5 volume install — fallback, needs hands
Per the Asahi m1n1-hypervisor guide (13.5 is a supported target on M2):
on the M2 in 1TR, `diskutil apfs addVolume`, install 13.5 from the
archived InstallAssistant.pkg (archive.org), `bputil -nkcas` on the new
volume UUID, `csrutil disable`, `kmutil configure-boot -c m1n1.bin --raw
--entry-point 2048 --lowest-virtual-address 0 -v <volume>`. Requires
Joshua in 1TR. Only if (1) fails on the seal/trust path.
