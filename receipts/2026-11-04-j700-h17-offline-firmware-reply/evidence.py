#!/usr/bin/env python3
"""Reproduce the bounded firmware reply-address evidence, without a device."""
import contextlib
import json
from pathlib import Path
import subprocess

import trace as fw

HERE = Path(__file__).resolve().parent


def require(va, mnemonic, operands):
    ins = next(fw.md.disasm(fw.d[fw.fileoff(va):fw.fileoff(va) + 4], va))
    if (ins.mnemonic, ins.op_str) != (mnemonic, operands):
        raise ValueError(f"instruction mismatch at {va:#x}: {ins.mnemonic} {ins.op_str}")


def align64(value):
    return (value + 63) & ~63


def model(dm, ipc, ipc_bytes=0xc441):
    # This models the traced successful address path, not MMU/device behavior.
    alias = ipc & 0xFFFFFFFFF
    table = align64(alias)
    channel = align64(ipc) + 0x800
    narrowed = channel & 0xFFFFFFFF
    reply = (dm & ~0xFFFFFFFF) | (table & 0xFFFFFFFF)
    return dict(dm=hex(dm), ipc=hex(ipc), ipc_cpu_alias=hex(alias),
                ipc_bytes=hex(ipc_bytes),
                logical_ipc_alias_fits_low32=0 < ipc_bytes <= (1 << 32) - alias,
                table_cpu_alias=hex(table), channel0_full_pointer=hex(channel),
                channel0_narrowed_pointer=hex(narrowed),
                channel0_store_operands=[hex(narrowed + 8), hex(narrowed)],
                reconstructed_reply=hex(reply),
                alias_survives_narrowing=(channel & 0xFFFFFFFFF) == narrowed,
                reply_equals_original_table=reply == align64(ipc))


def main():
    anchors = [
        (0x718c, 'mov', 'w24, #0xdff9'),
        (0x7194, 'movk', 'w24, #0xf7fb, lsl #16'),
        (0x73f0, 'lsl', 'x10, x0, #0x20'),
        (0x7458, 'orr', 'x1, x20, x21'),
        (0x748c, 'ldp', 'x25, x8, [x0]'),
        (0x749c, 'str', 'x8, [sp, #0x38]'),
        (0x76e0, 'ldr', 'x20, [sp, #0x38]'),
        (0x76fc, 'mov', 'x1, x20'),
        (0x7704, 'bl', '#0x21564'),
        (0x7748, 'mov', 'x0, x20'),
        (0x774c, 'mov', 'x1, x19'),
        (0x7750, 'bl', '#0x52e4'),
        (0x2159c, 'and', 'x19, x1, #0xfffffffff'),
        (0x52fc, 'add', 'x29, sp, #0x40'),
        (0x5300, 'add', 'x8, x1, #0x3f'),
        (0x5308, 'and', 'x19, x8, #0xffffffffffffffc0'),
        (0x530c, 'add', 'x8, x0, #0x3f'),
        (0x5314, 'and', 'x20, x8, #0xffffffffffffffc0'),
        (0x5324, 'add', 'x22, x20, #0x800'),
        (0x533c, 'stp', 'x8, x22, [x19, #0x48]'),
        (0x5354, 'str', 'w24, [x19, #0x40]'),
        (0x6e0c, 'ldr', 'w8, [x27, #0x50]'),
        (0x6e18, 'str', 'x8, [x24, #0x10]'),
        (0x6a60, 'ldr', 'x4, [x23]'),
        (0x6a68, 'bl', '#0xdbcc'),
        (0xdc04, 'mov', 'x20, x4'),
        (0xdc20, 'mov', 'x1, x20'),
        (0xdc44, 'bl', '#0x298fc'),
        (0x29920, 'mov', 'x19, x1'),
        (0x29988, 'tbz', 'w21, #0, #0x299d8'),
        (0x299b8, 'stp', 'xzr, xzr, [x19, #8]'),
        (0x299bc, 'str', 'x8, [x19]'),
        (0x77c4, 'mov', 'w2, w19'),
        (0x77c8, 'mov', 'x1, xzr'),
        (0x77f0, 'mov', 'w1, #7'),
    ]
    for args in anchors:
        require(*args)
    windows = {
        'handshake-descriptor': (0x7320, 0x7520),
        'ipc-table-publish': (0x76e0, 0x7834),
        'channel-table-builder': (0x52e4, 0x5364),
        'channel-pointer-narrowing': (0x6ddc, 0x6e88),
        'group-constructor': (0x69d4, 0x6aa4),
        'channel-constructor': (0xdbcc, 0xdc68),
        'channel-header-writes': (0x298fc, 0x299e0),
        'cpu-alias-helper': (0x21564, 0x215a4),
        'scratch-registers': (0x878c, 0x8844),
        'channel-type-transform': (0x54a4, 0x54c8),
        'channel-type-to-flags': (0xdc10, 0xdc48),
        'mapper-alias-vs-original': (0x20e70, 0x20f04),
        'pte-physical-mask': (0x8c93c, 0x8c954),
    }
    for name, (start, end) in windows.items():
        with (HERE / f'{name}.txt').open('w') as out, contextlib.redirect_stdout(out):
            print('Firmware SHA256', fw.SHA)
            fw.dis(start, end)
    with (HERE / 'llvm-crosscheck.txt').open('w') as out:
        print('Firmware SHA256', fw.SHA, file=out, flush=True)
        for start, end in ((0x6e0c, 0x6e1c), (0x299b4, 0x299c0)):
            subprocess.run(['/usr/lib/llvm18/bin/llvm-objdump', '--disassemble',
                            f'--start-address={start:#x}', f'--stop-address={end:#x}',
                            str(fw.FW)], stdout=out, check=True)
    result = {'firmware_sha256': fw.SHA, 'checked_instruction_anchors': len(anchors),
              'capstone_version': fw.capstone.__version__,
              'rung1_prediction': model(0x3ffffbc0000, 0x3ffffbb0000),
              'arithmetic_only_compatible_example': model(0x10000300000, 0x10000400000),
              'hardware_qualification': False}
    (HERE / 'address-model.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
