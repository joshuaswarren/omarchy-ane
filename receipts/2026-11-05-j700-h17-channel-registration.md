# J700/H17 channel-registration rung receipt

Status: **device-observed registration acknowledgement; no task submitted and
no numerical execution.** The run stopped immediately after firmware cleared
SCRATCH3. This is not ANE compute qualification.

## Run identity

- Boot: `8ec803ee-85f7-434c-8a5e-19317934c9c9`
- Board/ANE: J700 / T8140, compatible `apple,t8140-ane`
- Kernel: `7.2.11-14-1-ARCH`, build ID
  `e6037e7a6a53ed1dfa9c1faa90efedc3d6b022bf`
- Source: `neo/h17-bringup` at
  `3d3b235dd4f4161e5028a61f140686191c0c2bfd`
- Module SHA-256:
  `5353f8c356d31bc5431b49e010591379343a02781f13d1f927e5d2833b10c91d`
- Firmware SHA-256:
  `32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c`
- UKI SHA-256:
  `df8d56d02043fff28a38bfb05c661e370dd6c0e5d57a9de8e4761b0b0aa1ab89`
- m1n1 SHA-256:
  `7a68ca9d93733dca6efb56ce03b3506a7f76eebf5f9e09f742dc2446d71b92f9`
- One-shot evidence directory:
  `/var/lib/omarchy-ane/h17-registration-attempts/8ec803ee-85f7-434c-8a5e-19317934c9c9/`
- Judge result: `PASS`, reason `channel-registration-acknowledged`.

## Observed protocol boundary

The runner's preflight matched the manifest, including the board, enabled ANE
node, all three DART links, kernel build, module/firmware/UKI/m1n1 hashes,
clean pushed source revision, and unused one-shot state. Device logs recorded:

- Reserved alias range `0x10000380000` for `0xf50000` bytes.
- Owned IPC at `0x100003c0000`, logical size `0xc441`; copied eight table
  entries (`0x800` bytes), SHA-256
  `05e5c12d7e24fc033f6eb0dd246ae4753004375426b0d07c8c211fab575e08ae`.
- All eight expected channel records matched the strict pinned verdict.
- Host initialization was limited to type-0 entries 1, 2, 3, and 7, with
  ranges `(0x8800, 0x400)`, `(0x8c00, 0x200)`, `(0x8e00, 0x1000)`, and
  `(0xc000, 0x400)` respectively.
- The driver wrote `0x8042006` to SCRATCH3 at engine offset `0x188002c`;
  firmware cleared it (`H17 CHANNEL_REGISTER_ACK value=0`). The driver then
  logged `H17 CHANNEL_REGISTER_STOP`; no doorbell, IRQ acknowledgement, or
  task followed in this rung.

## Raw evidence hashes

All files below remain in the root-owned attempt directory above:

| File | SHA-256 |
|---|---|
| `identity` | `b8820e3ea717b07486c98c2baa760321585cd36697d7233d145f400754c92124` |
| `preflight.json` | `da71335113aba35581f431cf392d7a79c254dd4b857bdf1056cb62fdc9d426e2` |
| `run.log` | `af3e15624919be844509741c5bff47a5950050abb695181019d0b0d9675cc98b` |
| `dmesg-rung.log` | `863275907e0eeba4aa68815101eaf89e3cb6a24c2ea5e8d26454b2947c16d9db` |
| `verdict.json` | `89129988f8f7779a6f1ed6e910e978a733829bb83d5fa42bd3b02014c10096df` |
| `pending.claimed` | `ac93b735fe3e4760dde3525145d4b3112f851561497def06174f335516ec8a23` |

The preflight result explicitly says `arms_rung=false`; it records identity
matching and one-shot claim, not device behavior. The registration PASS comes
from the captured kernel log plus the strict offline judge. No hardware access
was made after that saved log was reviewed, and the bound driver/module must
remain loaded until reboot.
