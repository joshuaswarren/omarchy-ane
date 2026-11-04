# H17 pre-release ordering helper and warm/cold branch correction

Parent `1a31d8b`. Host-only helper: no production caller or MMIO adapter.
T8140 stage=boot remains refused. This does not replace provider votes.

`ane/h17/bringup_order.h` encodes CE status/safety checks, request toggle,
bounded acknowledgement poll, 64-bit acknowledgement, the 37 decoded HAL
masked writes, and PS/PWGATE writes with checked polls. Every read/write
requires a successful pending-intent callback first. Any logging, read,
write or delay failure stops the sequence. It stops before scratch/CPU
release. A callback's durable-log contract is not proof an adapter meets it.

The host model covers all four initial CE states, pre/post-ack errors,
the exact four-write PS/PWGATE order and width, masked-bit preservation,
every one of 88 intent failures and 88 I/O failures on the normal path,
and three stuck-status timeouts. All 37 triples match the previously
decoded hal_probe table numerically. `make -C ane/h17 sim-all` and
`make -C ane/h16 sim` pass. Complete local H17 output:
`/var/tmp/opencode/h17-order-sim-all.log`.

## Important correction: SCRATCH7 warm/cold writes are alternatives

The earlier firmware-load receipt incorrectly flattened two branches into
SCRATCH7:=1 then :=0. Retained kernelcache SHA256
7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe:

- 0xfffffe0008f3dd88 loads byte this+0xa2; the tbz at 0x8f3dd98
  selects the cold path at 0x8f3ddd4 when bit 0 is clear.
- The warm path writes 1 at 0x8f3de98, then branches through
  0x8f3df9c/0x8f3dfa0 to 0x8f3dfec, bypassing the cold write.
- The cold path writes 0 at 0x8f3df18, then joins at 0x8f3dfe8.

The branch structure—not merely the sorted write addresses—is the source
of truth. Local recheck:
`/var/tmp/opencode/h17-warm-boot-handshake-disasm.txt`.
The old fwboot probe's 1-to-0 sequence is not an exact Apple sequence;
its negative wake observation does not isolate the hardware blocker.
Do not use that prototype as the next hardware rung.

## Preload review

Read-only review by session ses_e593c2d98ffef6e9aneN3903I4 reports that
TEXT [0x10000774000,0x10000834000) and DATA
[0x100019ac000,0x10001c58000) have no reserved-memory overlap and lie below
the boot DT's first usable /memory range. These are metadata observations,
not verified firmware contents or proof of AP/ANE access permission.
Admission must check the full DRAM aperture, disjointness from Linux RAM
and other reservations, and quiescence; merely testing "inside RAM" is
not an ownership check. The generic preload rule live!=0 && file==0 is
also too broad and needs a position-bounded replacement.
