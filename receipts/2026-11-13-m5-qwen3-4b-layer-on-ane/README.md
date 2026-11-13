# One Qwen3-4B layer on the M5 Max Neural Engine (14-inch MacBook Pro)

This record holds one run, on 2026-11-13 at about 02:47 CST. The Neural Engine of a 14-inch M5 Max MacBook Pro ran every matrix projection of layer 0 of Qwen3-4B-Instruct-2507-4bit for two tokens. The kernel was the Aurora tree with [aurora-silicon/linux #1790](https://github.com/aurora-silicon/linux/pull/1790) applied.

## Split of the work

- The Neural Engine ran 8 programs, compiled by Apple's compiler for the H17 engine: q/k/v, o, gate/up and down.
- The host CPU ran the rest in pure Python: the two norms, rope, attention, softmax, silu and the residual add.
- The two tokens were at positions 0 and 1.

## Result

The output was compared with an fp32 numpy reference of the same layer.

| Measure | Value |
|---|---|
| Largest absolute error | 2.9e-3 (largest reference value 2.2) |
| Relative L2 error | 1.5e-3 |
| Cosine similarity | 0.999999 |
| NaN values | 0 |
| Neural Engine time per program (p50, 50 calls each) | 1.1 ms to 2.6 ms |
| Sum of the 8 programs, per token, per layer | 18.75 ms |

The 18.75 ms is Neural Engine time only. It leaves out the host time for the Python steps. This record does not measure tokens per second.

## Limits found

- A single program with 49.8 MB or more of weights fails to load with ENOMEM. The kernel reserves 64 MiB of contiguous memory (CMA) for these loads.
- Only 5 programs of about 25 MB stay loaded. The sixth load fails with ENOSPC.
- So the layer runner loads one program for each call.
- A program of about 25 MB takes about 2.6 ms. That is about 9.6 GB/s of weight streaming.

## What is not in this record

The compiled programs are Apple data, so they are not published here. The run covers layer 0 only, not the 36 layers of the model.
