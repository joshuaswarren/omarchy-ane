# baremetal/ane-add — BMN-04 draft

Smallest bare-metal ANE compute for T8103 (M1): powers the ANE's PMGR
chain, programs the three Gen1 ANE DARTs, runs the h13-anec `add
[1,64,1,1]` program through the host task manager, and prints the
result on the debug uart. Chainloaded in RAM by the lab aura loader
over the KIS debug-USB path — no OS, no ASC firmware, no ESP writes.

**Draft row: build only, nothing runs on a Mac.** See ACCEPTANCE.md
for the register program, pass criteria and run notes.

```sh
make          # -> ane-add-payload.bin (needs aarch64-linux-gnu-gcc)
make check    # host checks: fp16 tables, fixture geometry, golden
./stage.sh    # build + stage on the kisd host; prints the run line
```

Sources: `ane/src/ane_tm.c` (TM/TQ protocol), `ane/src/ane_drv.c`
(submit semantics), `packaging/dt/t8103-ane.dts` (MMIO + power chain),
`packaging/omarchy-ane-smoke` (fixture vectors and golden).
