# H13 add fixture

`program-0.anec` is a 20,992-byte H13 add program compiled by the MIT-licensed `mil-hwx-compiler` parity encoder (`h13-oracle-parity`, `add [1,64,1,1]`). The source manifest uses this exact encoder label. It is from `omarchy-mlx` fixture `h13-explicit-chain-add-mul`; the build output is our compiler output, not Apple-produced ANEC bytes. The source manifest calls this an H13 package and defines one task descriptor, input channels 5/6, output channel 4, and 64 logical fp16 elements.

The tensor shape is `[1,64,1,1]`: 64 valid fp16 planes in one 16,384-byte tile per channel. Each valid fp16 value is at the start of its 64-byte plane in little-endian order. Remaining bytes in the tile are zero padding. Do not apply H14's `surface_size / 2 / 32` lane rule or `omarchy-ane-run --check add`; the H13 smoke compares its own 64-lane integer fp16 model against the full 16 KiB result surface.

SHA-256: `9a6a6a9a701ce207d4a8ea80ee1ca6d5ade076be2115b73a9aed6f4236aa56ab`.
