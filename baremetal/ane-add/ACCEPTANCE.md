# BMN-04 ane-add payload — acceptance criteria and run notes

**Status: DRAFT, build-only.** Nothing in this directory has run on a
Mac. The chainload is a lab-Mac test boot and needs Main's same-hour
word under the 2026-11-28 boot-change rule.

## What the payload is

A freestanding aarch64 image for the lab's KIS RAM-chainload path
(`docs/lab-hardware/bare-metal-node-first-step.md` in
omarchy-mplus-private: the aura/m1n1 loader hands control to a raw
binary, then the loader leaves the proxy). The payload is the ANE
variant of the doc's milestone two: one real computation, no OS, no
ASC firmware — on T8103 the ANE runs through the host task-manager
path (`ane_tm_enable`, TQ push, IDLE poll in `ane/src/ane_tm.c`).

Program: `fixtures/h13-anec/add/program-0.anec` (mil-hwx-compiler
`h13-oracle-parity`, `add [1,64,1,1]`), embedded whole. Inputs are the
canonical smoke vectors (`packaging/omarchy-ane-smoke`): 64 fp16
quarter values per input tile, lanes at 64-byte planes, channels
5/6 in, 4 out.

## Register program (order)

1. PMGR power chain, parents first: `0x470` (ane_sys), `0xc000`
   (ane_sys_cpu), `0xc008` (ane_base), `0xc010..0xc030` (ane_set1..5);
   each: clear flags+target, write target `0xf`, poll ACTUAL `0xf`.
   **Never write the 0x8cxxx mirror window** — firmware-locked,
   external-aborts the SoC (`ane_tm.c` bisect note).
2. DART tables (Gen1, 4 KiB pages, u64 ptes):
   `ttbr[iova>>30]` → `mid[iova>>21&0x1ff]` → `leaf[iova>>12&0x1ff]`.
   pte = `pa & 0xffffff000 | (0xfff<<40) | 1`; ttbr = `0x80000000 | pa>>12`.
   Per dart (0x26b800000/0x26b810000/0x26b820000), sid 0: TTBR write,
   TCR(0x100)=`0x80` (translate enable), STREAMS_ENABLE(0xfc)|=`1`,
   STREAM_SELECT(0x34)=1, STREAM_COMMAND(0x20)=`1<<20`, poll !BUSY(bit2).
3. TM enable (`ane_tm_enable`): TM_TQ_EN(0x2000c) |= `0x1000`;
   TQ_PRTY[q]=`{1,2,3,4,5,6,0x1e,0x1f}`; TM_IRQ_EN1=`0x4000000`,
   TM_IRQ_EN2=`0x6`.
4. Buffers: btsp page = first 504 B of the anec payload with word0
   bits [23:16]=0x40 (nid); cmd buffer = the 16896 B payload; input
   tiles = canonical vectors; output tile zeroed. `dc cvac` all ranges.
5. Enqueue q4 (`ane_tm_enqueue`): TQ_STATUS=1; BARs: [0]=cmd iova,
   [1]=cmd iova+`round_up(504,16)`=+0x200, [4]=out, [5]=in0, [6]=in1;
   TQ_SIZE1=`((504>>2)-1)<<16`=0x7d0000; TQ_ADDR1=btsp iova;
   TQ_NID1=`(0x40<<8)|1`=0x4001.
6. Push (`ane_tm_push_tq`): TM_ADDR=btsp iova; TM_INFO=0x7d0000|1;
   TM_PUSH=`5|(4&7)<<8`=0x405.
7. Poll: drain both IRQ event lines (info `0x05400000` for td_count 1),
   TM_IRQ_ACK|=2, until both lines finished and TM_STATUS&1 (IDLE).
   Bounded; DART error latch checked each pass.

## Pass criteria

Serial (s5l uart, build knob `UART_BASE`, default 0x235200000):

- `pmgr 0x...` lines all show ACTUAL nibble `f`, no `rc=`
- `pushed`
- `tm_status=0x........` with `finished=3`
- `lane0 out=0x3e00 want 0x3e00` — the expected output word is
  fp16(1.5) = `0x3e00`
- `mismatched lanes: 0/64`
- `RESULT_VALID=<256 hex>` — sha256 of those 128 bytes must equal
  `5ad7eccd2977a88a375f123fe42c6780855953a1e9df4ac442cae383bc240dd6`
  (the packaged H13 golden)
- `ANE_ADD_RESULT PASS`

## Abort/inspect paths

- Fixture geometry mismatch → early abort, header fields printed.
- PMGR word not reaching ACTUAL `0xf` → abort with the word printed.
- DART invalidate timeout or mid-run DART fault latch → abort with
  `DART_ERROR`, `ERROR_ADDR_LO/HI`.
- Event overflow or poll timeout → abort with TM_STATUS + TM_ERROR1..3.
- Silent console: first suspect is `UART_BASE` (try 0x235204000,
  0x235208000), then `LOAD_ADDR` colliding with the loader.

## Known-unverified points for the first window

- Chainload load address and the kisd runner name on the jwm1 path —
  `stage.sh` env knobs `KIS_RUN`, `LOAD_ADDR`.
- Whether m1n1's early `dapf_init_all` leaves the ANE DAPF windows
  open for our buffers (believed yes; first failure mode would be a
  DART fault at push).
- Gen1 DART attach order vs upstream's (ttbr → tcr → streams →
  invalidate is what this draft uses).
- s5l uart base on j274.

Rollback: power-cycle; the chainload is RAM-only and the ESP loader
is untouched.
