#!/usr/bin/env python3
"""Decode the two CSNE command-name tables in the pinned J700 H17 firmware.

This is an offline structural decoder, not a command-contract validator.
The table records are 16 bytes: cstring VA (u32), 0x200000 tag (u32),
command ID (u32), reserved (u32). The two known copies begin at the VAs below.
"""
import hashlib
import struct
import sys

FW = "/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho"
EXPECTED_SHA256 = "32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c"
TABLE_VAS = (0xC1C50, 0xC78A8)
DATA_FILE_OFFSET = 0x4000
CSTRING_VA = 0x9D298
CSTRING_FILE_OFFSET = 0xA1298
CSTRING_SIZE = 0x14967
RECORD_SIZE = 0x10
TAG = 0x200000


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else FW
    data = open(path, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"unexpected firmware SHA-256: {digest}")

    print(f"firmware_sha256={digest}")
    for table_va in TABLE_VAS:
        table_file = table_va + DATA_FILE_OFFSET
        print(f"\nCSNE table VA={table_va:#x} file={table_file:#x}")
        count = 0
        for i in range(256):
            off = table_file + i * RECORD_SIZE
            ptr, tag, command_id, reserved = struct.unpack_from("<4I", data, off)
            if tag != TAG or not CSTRING_VA <= ptr < CSTRING_VA + CSTRING_SIZE:
                break
            string_off = CSTRING_FILE_OFFSET + ptr - CSTRING_VA
            end = data.find(b"\0", string_off, CSTRING_FILE_OFFSET + CSTRING_SIZE)
            if end < 0:
                raise SystemExit(f"unterminated command name at {off:#x}")
            name = data[string_off:end].decode("ascii")
            print(f"{command_id:#010x} {name}")
            count += 1
        print(f"count={count}")


if __name__ == "__main__":
    main()
