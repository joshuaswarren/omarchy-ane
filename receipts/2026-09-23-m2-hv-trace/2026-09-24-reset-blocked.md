# Reset fallback blocked, stop point — 2026-09-24 ~11:00 UTC

## Attempt
Fresh boot, mailbox bound, rtclient fw_load=1 fw_start=1
fw_start_venc_gates=1 fw_alias_reserved=1, platform bind.

## Outcome
- Same HELD negative (no READY). cpu_reset sysfs absent: DT carries
  no resets property the driver consumes.
- Reset vehicle does not exist without a DT change. No more variants
  fired per standing order.

## Measured handoff state for the macOS-side trace
- RUN accepted, STATUS 0x28, clocks all on (VENC_SYS + leaves 0x3ff,
  ps 0x1f0003ff), SID0 translate-enabled, no DART fault ever latched,
  single type-0 I2A word, SCRATCH zero, wake unanswered.
- Placement eliminated (staged DMA and reserved SEG0/SEGi both negative).
- Open: what AppleA7IOP does at ANE start between RUN and HELLO that
  Linux does not (clocks beyond VENC, reset line, SCRATCH/boot-arg
  publication the RTBuddy path skips).
