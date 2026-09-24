# Mailbox drain + carveout answers — 2026-09-24 ~14:00 UTC

## 1. No unread HELLO (offsets from ane_t6021.h + mailbox_poll.c)
- Drain: 0 words, EMPTY held, out114 0x000a0001.
- Lone I2A word: stale type-0 0x000a000000000000.
- SCRATCH0-7: all zero. Firmware did not boot; mailbox not the blocker.

## 2. Carveout reserved and SEG0 intact
- Reserves: SEG0 0xc4000 + SEG1 0x438000, apple,asc-mem no-map.
- SEG0 head: 8100001400000000... (iBoot image survived).
- Open: DATA bytes at 0x1000150c000 need a re-read (second range in
  iomem starts at 0x100013e4000, SEG1 offset inside it unconfirmed).
