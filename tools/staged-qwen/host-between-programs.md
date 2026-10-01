# Host work between the 38 staged Qwen programs

Reference path: ANEForge `lane/deltanet-split-decode` 2ea941c on the Mac Studio (M1 Ultra),
`LlamaPrefill.generate(..., batched_prefill=False)` and `_decoder` in `aneforge/llm.py`,
weights from `qwen35.load_gguf(resid_scale=1.0)`, lm_head on the host (`ane_lm_head=False`).
Every tensor op of the 24 layers and the final norm runs inside the 38 programs. The host
only gathers, builds position tables, copies lanes, and does the lm_head plus argmax.

## Once per generate()

`set_input(zeros)` on every resident state port: 18 conv states `[6144,3]`, 18 DeltaNet
states `[16,128,128]`, 12 KV caches `[2,50,256]`. At build time `share_buffer` aliased each
state output onto its own input, so after the reset the host never reads or writes a state
again. Each state belongs to exactly one program and stays on the device across steps.

## Per step (position p, token t)

1. Embedding: row t of GGUF `token_embd`, dequantized to fp16 (times 1/resid_scale = 1).
   This is lane `x` `[1,2048]` of program 0.
2. Context tables, fed to the six attention programs (6, 12, 18, 25, 31, 37):
   `oh [1,50,1]` one-hot at p; `inv = 1 - oh`; `mask [1,1,50]` 0 for 0..p, -1e4 after;
   `cosp`/`sinp [1,256]`: row p of the rope tables (base and rotary dim 64 from GGUF
   metadata, columns 64..255 padded with cos 1, sin 0).
3. Programs 0..37 in order. For each one: `set_input` its lane ports from the host lane
   dict, `set_input` its ctx ports, `execute`, then `read_output` its lane outputs into the
   lane dict by name. At a group start (0, 21) lane `x` is the running hidden: the
   embedding for group 0, program 20's `h` for group 1. At a group end (20, 37) hidden
   becomes `h`.
4. After program 37 (its `h` already has the final RMSNorm): logits =
   float32(h) @ float32(token_embd).T (tied head, every embedding row), greedy argmax.
   Only steps p >= len(prompt)-1 use the logits; prefill steps drop `h`.

There is no host residual add, norm, conv or gate math. Prefill is the same 38-program
step, once per prompt token.

## Lanes and program classes

| programs | inputs | outputs | content |
|---|---|---|---|
| 0, 21 (group start) | x, conv state | q k v [16,128], beta gt [16,1,1], z [16,128] | first DeltaNet layer's input block (norm, projections, causal conv, gates) |
| odd 1..19, even 22..36 | q k v beta gt, state [16,128,128] | o [16,128] | DeltaNet recurrence |
| 2 4 8 10 14 16 23 27 29 33 35 | x o z, next layer's conv state | x, q k v beta gt z | readout + MLP of one DeltaNet layer, input block of the next |
| 6 12 18 25 31 | ctx, x o z, K V caches, conv state | x, q k v beta gt z | readout + MLP, then one full attention layer (3 7 11 15 19) with its MLP, then the next input block |
| 20 (group end) | x o z | h | readout + MLP of layer 12 |
| 37 (group end) | ctx, x o z, K V caches | h | readout + MLP of layer 22, attention layer 23 + MLP, final norm |

The layer mapping comes from the `_decoder` flush points and the manifest shapes; it was
not separately measured. Residual `x` is not re-emitted by the class-A and class-B
programs: a consumer reads the last `x`/`h` produced in the step, or the embedding.

## What a replay host needs

- The per-program weights are baked into each program; the programs need nothing from the
  GGUF.
- The host needs the GGUF `token_embd` (embedding gather and the tied lm_head) and its rope
  metadata. A tokenizer is not needed to replay p001: `chunk_00.json` carries the prompt ids.

## Goldens

`dump_step_ports.py` writes the exact fp16 arrays per execution (resident state inputs are
read back from the aliased state port right before `execute`); `check_step_dump.py`
verifies a dump offline. The max_len 50 `goldens.json` is NOT the first step: its lane,
ctx and output hashes are step 11 of p001 (position 11, the last prompt token, the step
that yields the first generated id), and its state-input hashes are the reset-time zero
fill. The exporter version that wrote it kept the last `set_input` per port
(`dict(ins)`); the current `staged_qwen_manifest.py` keeps the first one.
