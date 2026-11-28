# H15 mul fixture

An fp16 mul program for the M3 family chips t6030, t6031, t6034 and t8122, all in
the H15 module family. It gives the smoke a fixture to resolve on those chips.

## What it is

- model.hwx, a 65536 byte HWX program for mul (see ports.json for shapes).
- model.mil, the MIL source for that program.
- ports.json, the port list from the MIL graph. It names shapes and dtypes only.
- golden-channels.json, the channel binding the staged-to-ANEC converter emits
  for this file (Q-87 golden).
- SHA256SUMS over model.hwx and model.mil. Check with `sha256sum -c` in this directory.

## How it was built

model.mil went through ANECCompile via tools' compile_target helper on a Mac
Studio running macOS 26.6.2, with target h15 (the same capture procedure the
add README records). The output is a legacy thread-state HWX: magic
0xbeefface, cpu type 0x80, subtype 6.

## What is not verified

This program has never run on any M3 chip. Compile success is not execution.
No driver for these chips can run a job yet.
