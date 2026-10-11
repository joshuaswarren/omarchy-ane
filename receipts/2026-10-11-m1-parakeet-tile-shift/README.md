# 13-inch M1 (T8103): Parakeet whole encoder with `ane-run --tile-shift 9`, 2026-10-11

Eight single-process runs on one boot. The raw `exec.txt` and `results.txt` of each run are in `runs/`. Each of the six successful runs also wrote two output files of 480,256 bytes. These are not stored. Their hashes are in `results.txt`.

## Setup
- Chip: Apple M1 (T8103), 13-inch laptop.
- Kernel: 7.1.12-2-12.6-sep-ARCH, boot id starts 30178ff6. The `ane` module is the one in the kernel package.
- Program: the whole-encoder ANEC from the hwxv2 converter, 458,018,816 bytes, sha256 13c744231524d440b0a774155343df9ade0bbcbc37edc4b1ccf9698e580d5453.
- Reference: the macOS output, sha256 fca96f1355485ec3e72f314c9e44f72c968f8eb2b88e101043a818114a752063 over the first 240,000 fp16 words.
- Runner before: `ane-run` built from omarchy-ane main 75c8829, sha256 0465fd67d4b97577ede54615659356d70e449e80cfb089e95a16b1c7f6f17db0.
- Runner after: the same source plus the `--tile-shift` change, sha256 480040b5f700f407f11563f5349e7614df9fefe0efe57327c68ebc35b9779805.
- Each run: 60 s idle, then one `ane-run` process with 20 calls.

## Without the option (2 runs)
Both runs failed with exit code 1 and `DRM_IOCTL_ANE_BO_INIT failed with 0xffffffff`. No job ran (`jobs delta 0`).

The program header counts tiles in 512-byte units. The default unit is 0x4000 bytes, 32 times larger. So the default sizes the program at 458,018,816 x 32 = 14,656,602,112 bytes. The kernel log line for these two runs was not saved, so this receipt quotes no kernel message.

## With `--tile-shift 9` (6 runs)
All six runs exited with code 0. In each, 240,000 fp16 words were compared with the reference, 0 words were mismatched and the maximum ulp difference was 0.

|Run (UTC)|Median of 20 calls|Min|Max|
|---|---|---|---|
|01:39:39|137.419 ms|137.166 ms|140.357 ms|
|01:42:02|141.980 ms|137.194 ms|151.987 ms|
|01:43:43|158.179 ms|157.714 ms|160.693 ms|
|01:52:40|151.145 ms|149.575 ms|152.971 ms|
|01:55:02|137.853 ms|137.587 ms|140.405 ms|
|01:56:45|137.878 ms|137.700 ms|141.321 ms|

The medians range from 137.4 to 158.2 ms. Four of the six are between 137.4 and 142.0 ms. Two are slower (151.1 and 158.2 ms). The cause of the spread was not isolated.

## Limits
- The room, the temperature and the other load on the machine were not recorded for these runs.
- This is one program on one machine. The M2 path (`ane_m2_open`) does not read the tile shift, so these runs say nothing about it.
- No kernel log was saved for the two failed runs.
