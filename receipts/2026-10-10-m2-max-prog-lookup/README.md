# M2 Max (T6021): loading a held program from a second process, 2026-10-10

This is one run on one boot. The raw files are in this directory. In `percall-delta-results.txt` the host name in the first line is replaced with `m2-max`. Nothing else in that file changed.

## Setup
- Chip: Apple M2 Max (T6021).
- Kernel: 7.1.12-2-12.6-sep-ARCH.
- Driver module: `ane_t6021` build `bd34a12`, srcversion C133FDFAB7C937204AE2186.
- Module file sha256: f068aaf1a2f86b28c8ffe53237347945752b6245660466e6fc1480834e3528f5.
- Source: the signed commit bd34a1261db417e79e46595ed8b77954b41f4eb4.
- Allocator: `bo_total_bytes` was 0 at the start, after a fresh reboot.
- Userspace: `ane-run` and `ane-session` built from the same commit with gcc 16.1.1 and `-O3 -Wall -Werror`.

## What changed
The change adds `DRM_IOCTL_ANE_PROG_LOOKUP`. A process asks the driver for a program by the SHA-256 digest of its sections. If the driver already holds that program, the process skips the section allocation.

## Why
On the module before this change (`ca09ce8`), one process per program worked for the 38 Qwen3.8-2B programs once. A second pass failed 5 times with `DRM_IOCTL_ANE_BO_INIT failed for 222980416 bytes`. Each repeat load asked for a new section-sized buffer. About 2.6 GB of sections were already held in the 4 GiB DMA window.

## Result 1: two processes, different inputs
The files are `twofd-*.jsonl` and `twofd-*.txt`.

Sessions A, B and C load the same program. A loads first. B loads while A still holds it. A exits. C loads after A has exited. The two inputs give different baseline outputs.
- `prog_002` (section 117,994,432 B): B's load grew `bo_total` by 1,146,880 B. C's load grew it by 0 B.
- `prog_006` (section 223,006,144 B): B's load grew `bo_total` by 851,968 B. C's load grew it by 0 B.
- Each program had 10 interleaved calls across A, B and C. Every output equals the single-process baseline for its own input.
- All sessions exited with code 0.

## Result 2: three passes over 38 programs
The files are `percall-delta-*`. Each program runs in its own process.
- Pass 1 (programs not yet held): 38 of 38 calls succeeded. `bo_total` went from 344,686,592 to 2,761,310,208 B. The calls took 5.21 s in total.
- Pass 2: 38 of 38 succeeded. `bo_total` grew by 196,608 B. The calls took 2.50 s.
- Pass 3: 38 of 38 succeeded. `bo_total` grew by 0 B. The calls took 2.63 s.
- Each call time covers process start, the lock, the file read, the hash, the load and one call. The inputs were zero-valued tensors.

## After the run
`bo_total_bytes` was 2,761,506,816, which is the 38 held sections. The pool was 0. No dmesg line matched `ane_t6021.*(fault|error|EXCH)`, `DART fault`, `quarantin` or `BO_INIT failed`.

## Limits
- This is one run. The inputs were zeros, not decode data. No model decode ran through this path.
- The times leave out the host logits head and host packing. They are not a tokens-per-second figure.
- The per-call decode step on module `37ffb57` took 6.398 s (p50). That run used a different boot and module, so it is context and not a controlled comparison.
- Not tested: the resident session on this module, the whole Parakeet encoder on this module, and other chips.
