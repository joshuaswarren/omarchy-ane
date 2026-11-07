#!/usr/bin/env python3
"""Conservative H17 firmware text scan for obvious PING dispatch references.

This is a negative-search aid, not a firmware control-flow proof. It scans the
known __TEXT code candidate before the cstring pool for ADRP references to the
PING name and command-name table pages, plus CMP-immediate 0x12 instructions.
"""
import hashlib
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from capstone.arm64_const import ARM64_OP_IMM

FW = "/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho"
EXPECTED_SHA256 = "32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c"
TEXT_FILE = 0x4000
CODE_CANDIDATE_VA_END = 0x9D298
PING_NAME_VA = 0xA225A
COMMAND_TABLE_VAS = (0xC1C50, 0xC78A8)


def main():
    data = open(FW, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"unexpected firmware SHA-256: {digest}")
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    # Firmware __text contains sparse code and zero/data islands. Continue
    # after undecodable gaps instead of silently stopping at the first one.
    md.skipdata = True
    ins = list(md.disasm(data[TEXT_FILE:TEXT_FILE + CODE_CANDIDATE_VA_END], 0))
    print(f"firmware_sha256={digest}")
    print(f"code_candidate_va=0x0..0x{CODE_CANDIDATE_VA_END:x}")
    valid = [inst for inst in ins if inst.mnemonic != ".byte"]
    print(f"decoded_instructions={len(valid)}")
    print(f"skipped_data_items={len(ins) - len(valid)}")

    targets = (PING_NAME_VA, *COMMAND_TABLE_VAS)
    for target in targets:
        hits = []
        for i, inst in enumerate(ins):
            if inst.mnemonic == ".byte":
                continue
            if inst.mnemonic != "adrp" or inst.operands[1].imm != (target & ~0xFFF):
                continue
            reg = inst.operands[0].reg
            for nxt in ins[i + 1:i + 6]:
                if (nxt.mnemonic == "add" and nxt.operands[0].reg == reg
                        and nxt.operands[1].reg == reg
                        and nxt.operands[2].type == ARM64_OP_IMM):
                    value = inst.operands[1].imm + nxt.operands[2].imm
                    if value == target:
                        hits.append((inst.address, nxt.address))
                    break
        print(f"adrp_add_refs_to_{target:#x}={[(hex(a), hex(b)) for a,b in hits]}")

    cmp12 = [inst.address for inst in valid
             if inst.mnemonic == "cmp" and len(inst.operands) >= 2
             and inst.operands[1].type == ARM64_OP_IMM
             and (inst.operands[1].imm & 0xFFFFFFFF) == 0x12]
    print(f"cmp_immediate_0x12={[hex(a) for a in cmp12]}")

    for target in targets:
        occurrences = []
        pattern = struct.pack("<Q", target)
        start = 0
        while (offset := data.find(pattern, start)) >= 0:
            occurrences.append(offset)
            start = offset + 1
        print(f"raw_u64_{target:#x}_file_offsets={[hex(x) for x in occurrences]}")


if __name__ == "__main__":
    main()
