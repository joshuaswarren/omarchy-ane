# Qwen decoder on the M2: host requirements

`tools/qwen_m2_decode.py` runs the 38 Qwen3.8-2B programs on the ANE and does the rest on the CPU. It needs:

- an Apple Silicon Mac running Linux with the `ane_t6021` module (M2 Max and the other H14 chips) and the `ane-run` and `ane-session` tools from this repository;
- Python 3 with `numpy` and the `gguf` package (`gguf-py` from llama.cpp);
- **numpy on OpenBLAS.** On Arch: `sudo pacman -S blas-openblas python-numpy`.

Why OpenBLAS: Arch's `blas`, `cblas` and `lapack` packages are the reference implementation. Their matvec runs on one thread at any `OPENBLAS_NUM_THREADS`. The decoder's output projection (`head @ hidden`, float32, 248320 x 2048) took 604 ms per generated token on the M2 Max with them and 20 ms with OpenBLAS, which is most of the difference between 1.21 and 4.6 tokens per second
([receipt](../receipts/2026-10-10-m2-max-resident-decode-openblas/README.md)). `blas-openblas` provides `blas`, `cblas`, `lapack` and `lapacke`, so packages that depend on them (numpy, opencv, omarchy-mlx) stay installed; pacman removes the reference packages. To go back: `sudo pacman -S blas cblas lapack` (they conflict with `blas-openblas`).

Check which one numpy uses: `readlink -f /usr/lib/libcblas.so.3` ends in `libopenblas.so.0.3` with OpenBLAS and in `libcblas.so.3.12.0` with the reference. The decoder also checks at the start of a run and prints a warning on stderr when numpy maps the reference BLAS (`tools/test_qwen_blas_check.py`).

OpenBLAS sums the 2048-term dot products in a different order, so the float32 logits are not bit-identical to a run on the reference BLAS (largest difference 4.0e-5 on logits near 26, 1.5e-6 relative). The generated tokens were identical. A logits hash taken on the reference BLAS (the earlier receipt's `7384c10e...`) will not match; the OpenBLAS hash on the same decode is `5c359d36...` and was the same in two runs.
