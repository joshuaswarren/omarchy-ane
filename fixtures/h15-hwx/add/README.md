# H15 add fixture

An fp16 add program for the M3 family chips t6030, t6031, t6034 and t8122, all in
the H15 module family. It gives the smoke a fixture to resolve on those chips.

## What it is

- model.hwx, a 65536 byte HWX program for one plain fp16 add over
  [1, 1, 128, 128] tensors a and b, output y.
- model.mil, the MIL source for that program. There are no weights.
- ports.json, the port list from the MIL graph. It names shapes and dtypes only.
- SHA256SUMS over model.hwx and model.mil. Check with `sha256sum -c` in this directory.

## How it was built

model.mil with fp16 inputs a and b and output y = add(a, b) went through the
ANE compiler tool ane-compile-hwx2 on a Mac Studio running macOS 26.6.2,
with target flag h15. The output is a legacy thread-state HWX: magic
0xbeefface, cpu type 0x80, subtype 6. The same capture compiled with target
h15g produced a different file, sha256
e92918ad56650c0bed72e9be5d0e9f65fb265b607653e461a847f1c4ae54f020. This
directory ships the h15 target, sha256
01d9439200cad6730a6c8755030ef325b2c4b84cabb0eb2aa0cadd2a44bd0fae.

The smoke golden for these chips equals the H14 add golden,
94041b7cc10a66dfd76ecfd469728ad9d66f68b508f4a9744ce41208d6a4de0b. The input
generator, the surface packing and the valid-plane check are shared, so the
expected output bytes are identical.

## What is not verified

This program has never run on any M3 chip. Compile success is not execution.
The 14.x firmware on an M3 stub may not accept a program from this compiler.
No driver for these chips can run a job yet, so omarchy-ane-smoke exits 2
with an unavailable note on every M3 chip. ports.json carries no bar slots,
buffer ids or channels, because no reader parses this HWX format yet.

## Host load construction

The `load/` directory contains five fixed-family reference sets: the original
fixture, changed extent, changed NE count, changed property and swapped input slots.
The host-only C builder owns six payload copies and constructs a 448-byte load
command from explicit address and capacity inputs. Constants precede TEXT in its
section records. Tests compare the command and payloads against these references.

Caller-declared addresses do not prove device residency. Zero initialization is
the Linux builder's policy. This adds no HWX parser, firmware transport or device
execution, and it does not establish firmware acceptance.
