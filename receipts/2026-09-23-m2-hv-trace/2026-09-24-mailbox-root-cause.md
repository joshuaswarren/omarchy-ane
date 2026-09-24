# Mailbox root cause + observer milestone — 2026-09-24 ~08:55 UTC

## Root cause: apple-mailbox never binds 285408000.mailbox
- Manual bind: `No such device`, rc=1.
- dmesg at boot and at bind: `apple-mailbox 285408000.mailbox:
  error -ENXIO: IRQ send-empty not found`.
- DT node `mailbox@285408000` carries interrupt-names = ["recv-not-empty"]
  only; the driver requires `send-empty` as well.
- Consequence: ANE consumer link stays dormant, every platform bind of
  284000000.ane parks its writer in D state before probe body.
- Installed-path fix: DT overlay adding the send-empty IRQ, or a driver
  fallback when absent. Not a compatible change.

## Observer milestone: ane_obs.ko read path
- insmod rc=0, no platform bind, no power calls.
- First read: ctl=0 status=0x2a rvbar=0x10000000001 out=0x20001/0x20001
  ps_cpu=0x1f0003ff, mailbox silent, SCRATCH zero.
- Source: receipts/2026-09-23-m2-hv-trace/ane_obs.c (+ Makefile).
