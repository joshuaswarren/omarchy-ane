# J700 / T8140 (H17): boot-wake reproduced on a third cold boot, RTKit poll skipped

**RESULT PASS boot-wake.** Boot `91129a82-5ead-4669-9473-b96eb3f15f73`,
kernel `7.2.11-14-1-ARCH`, run automatically at t+15 s by the gated one-shot
`omarchy-ane-h17-bootwake.service` (pending-file gate, self-clearing), which
ran `ane/h17/run-bootwake.sh`:

```
insmod ane/h16/ane_h16.ko optin=t8140 stage=boot hello_wait_ms=1000
```

This is the first device run of the poll-skip build (`91eb462`, script and
unit committed at `181eb7f`). The pass criteria were all met:

```
SCRATCH7 wake word 0x8042006: firmware is running (CPU_STATUS 0x6c)
H17 post-wake RTKit management poll skipped: H16 mailbox offset is unqualified; RESULT covers wake only
ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-wake
CPU release attempted: boot_error=0; retaining binding, mappings and power; do NOT unload; reboot required
```

`grep -c 'RTKit poll done'` over the rung's dmesg slice is 0: with
`hello_wait_ms=1000` the H17 path now waits for the wake word only and never
reads the unqualified H16-template mailbox at `0x1608000`. The reason string
is `boot-wake`, so this line no longer overstates what was observed.

## Identities (recorded by the run itself, `/var/tmp/opencode/h17-bootwake-91129a82-*/run.log`)

| Item | Value |
|---|---|
| Boot id | `91129a82-5ead-4669-9473-b96eb3f15f73` |
| Kernel | `7.2.11-14-1-ARCH` |
| Module file SHA-256 | `edfbb50414376362b3f454576513e447fd81337176dd12736e9190d58d542e29` (rebuilt from `91eb462`; matches the pre-reboot record `/var/lib/omarchy-ane/h17-bootwake-pre-shas.txt`) |
| Firmware SHA-256 | `32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c` (`h17_ane_fw_theia_d9x.macho`) |
| Staged image | accepted_diff_bytes=2065, SHA-256 `3c0524b95ac3b32ae0a83f4353d909e9ca547efd496baf0a24dba0c352ba9e5b` |
| Rung verdict file | `/var/tmp/opencode/h17-bootwake-91129a82-*/verdict.txt`: `rung=PASS reason=wake-and-poll-skipped` |

The run log shows `repo_head=` empty: root's git refused the user-owned repo
(`dubious ownership`). Cosmetic only — the module identity above is a file
hash, not a git claim; the script now passes `safe.directory` explicitly.

## Observation: the staged preload image is not bit-stable across boots

The same admission path accepted 2065 diff bytes with stage SHA `63edf11b…`
on boot `638bf8a4`, 2066 bytes / `4a91d2c3…` on boot `55f72e30`, and 2065
bytes / `3c0524b9…` here. The iBoot preload therefore carries per-boot bytes
inside the whitelisted patchbay/CE-env regions; wake does not depend on a
fixed stage hash. Which bytes vary is not established by these three runs.

## What this proves and what it does not

- Proves: the power-on → firmware-load-path → CPU-release → SCRATCH7
  `0x08042006` wake sequence reproduces on cold boot, third consecutive
  time, and the driver no longer performs an unqualified mailbox read after
  wake.
- Does not prove: anything about the command path. No mailbox traffic was
  attempted or observed, no ring, doorbell, task, or model ran. The module
  pins itself and holds power on this boot (`refcount 1`); any further
  hardware rung starts from a fresh reboot.

Raw evidence: `dmesg-rung.log` (the insmod-to-RESULT slice) and
`verdict.txt` alongside this file; full dmesg at
`/var/tmp/opencode/h17-bootwake-91129a82-5ead-4669-9473-b96eb3f15f73/dmesg-full.log`.
