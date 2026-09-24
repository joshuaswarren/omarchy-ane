# PWGATE set-window words (2026-09-24, 17:41 CDT)

Main asked for a read of phys 0x28e092cc and 0x28e093cc and said: if they
already read 3 and 0, the theory is dead without a write.

That literal address is not on the derivation. The static receipt
(ane-linux-experiments agent/ane-static-start-r,
receipts/2026-09-24-t6021-macos-start-sequence §19) derives:
PWGATE+0x12cc <- 3, PWGATE+0x13cc <- 0, PWGATE = the set window
0x28e08c000, so literal 0x28e092cc / 0x28e093cc. But 0x28e08c000+0x12cc is
0x28e08e2cc, not 0x28e092cc. The literal is 0x052cc past the set window,
past its stated 0x4000 length. The same receipt elsewhere uses a PWGATE
window of 0x28e092000, which makes PWGATE+0x12cc = 0x28e0932cc — yet
another address. I did not guess a pmgr write.

## Read-only result (vehicle unloaded)

Offsets are from pmgr-window base 0x28e080000. `pwgate` vehicle
(staged, this worktree): `ane_ascdbg_pwgate.c`.

- 0xd2cc = 0x00000000
- 0xd3cc = 0x00000000
- 0x132cc = 0x00000000
- 0x133cc = 0x00000000
- 0x1359c = 0x00000000 (pre-RUN write target)
- 0x122dc = 0x00000000 (the RMW row)

Islands were on, STATUS 0x28, no watchdog. Both pairs are 0/0, so no 3/0
was seen anywhere two static derivations can point. Theory dead without
a write. No release was run on this already-released boot.

Full access log: ascdbg.log.
