# M2 Max (T6021): program lookup across processes, 2026-10-10

Facts from one run on one boot. Raw files are in this directory. In `percall-delta-results.txt` the host name in the first line was replaced with `m2-max`; nothing else was changed.

## Setup
- Chip: Apple M2 Max (T6021). Kernel 7.1.12-2-12.6-sep-ARCH. Driver module `ane_t6021` build `bd34a12` (srcversion C133FDFAB7C937204AE2186, file sha256 f068aaf1a2f86b28c8ffe53237347945752b6245660466e6fc1480834e3528f5, built from the signed commit bd34a1261db417e79e46595ed8b77954b41f4eb4).
- Fresh reboot: `bo_total_bytes` 0 at the start of the run.
- Userspace: `ane-run` and `ane-session` built from the same commit with gcc 16.1.1 (`-O3 -Wall -Werror`).
- The change under test: `DRM_IOCTL_ANE_PROG_LOOKUP`. A process that loads a program the driver already holds asks for it by the SHA-256 digest of its sections and skips the section allocation.

## Why
On the module before this change (`ca09ce8`), one process per program over the 38 Qwen3.8-2B programs worked once; a second pass failed 5 times with `DRM_IOCTL_ANE_BO_INIT failed for 222980416 bytes`. A repeat load allocated a new section-sized buffer while about 2.6 GB of sections stayed held in the 4 GiB DMA window.

## Result 1: two processes, distinct inputs (`twofd-*.jsonl`)
Sessions A, B and C load the same program. A loads first. B loads while A holds it. A exits. C loads after A exited. Two different inputs give different baseline outputs.
- `prog_002` (section 117,994,432 B): B's load grew `bo_total` by 1,146,880 B; C's load grew it by 0 B.
- `prog_006` (section 223,006,144 B): B's load grew `bo_total` by 851,968 B; C's load grew it by 0 B.
- 10 interleaved calls per program across A, B and C: every output equals the single-process baseline for its own input. All sessions exit 0.

## Result 2: three passes over 38 programs, one process per program (`percall-delta-*`)
- Pass 1 (all new programs): 38 of 38 calls OK; `bo_total` 344,686,592 to 2,761,310,208 B; wall time summed over the pass 5.21 s.
- Pass 2: 38 of 38 OK; `bo_total` grew 196,608 B; wall 2.50 s.
- Pass 3: 38 of 38 OK; `bo_total` grew 0 B; wall 2.63 s.
- Wall time is the sum of each call's wall time (process start, lock, read, hash, load, one call). Inputs were zero-valued tensors.

## After the run
`bo_total_bytes` 2,761,506,816 (the 38 held sections), pool 0. dmesg lines matching `ane_t6021.*(fault|error|EXCH)`, `DART fault`, `quarantin` or `BO_INIT failed`: 0.

## Limits
- One run. Zero-valued inputs, not decode data. No real model decode went through this path.
- The wall time excludes the host logits head and host packing, so it is not a tokens-per-second figure.
- The earlier per-call decode step on module `37ffb57` was 6.398 s (p50) on a different boot and module; that number is context, not a controlled comparison.
- Not shown: the resident session on this module, the whole Parakeet encoder on this module, other chips.
