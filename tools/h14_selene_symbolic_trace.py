#!/usr/bin/env python3
"""h14_selene_symbolic_trace — machine-checked symbolic trace of the selene
reset vector with the runtime PC base = the latched RVBAR entry (1 TiB,
alias-mapped). Flags every computed memory access outside the mapped set:
  alias  [0x10000000000, +0x500000)  (our DART mapping, image bytes known)
  scratch [0x285840000, +0x1000)     (SCRATCH cells, genpd/runner-managed)
All arithmetic is done by Python (no human hex math). Registers start
SYMBOLIC-UNKNOWN except where the trace defines them; an access that
depends on an unknown is reported as unknown-dependency, never guessed.
"""
import struct, sys
import capstone

IMAGE = ("$HOME/src/ane-linux-experiments/receipts/"
         "2026-09-18-t6021-engine-layout-mined/fw-h14j-selene/"
         "t602x_ane0_fw_selene_rc4x.macho")
SHA_PIN = bytes.fromhex("9f7915c431d288a2bdc2132c399db8cf"
                        "5574716a3b1e94af76be6a291c2e665b")
ENTRY = 0x10000000000          # latched RVBAR entry (1 TiB)
ALIAS_LO, ALIAS_HI = ENTRY, ENTRY + 0x500000
SCR_LO, SCR_HI = 0x285840000, 0x285841000
TEXT_FILE = 0x4000             # __TEXT vm0 -> file offset
START = 0x810                  # reset-vector branch target
LIMIT = 400                    # instructions to trace

buf = open(IMAGE, "rb").read()
import hashlib
assert hashlib.sha256(buf).digest() == SHA_PIN, "selene sha256 pin failed"
assert buf[0x4000:0x4004] == bytes.fromhex("81000014"), "vector branch regressed"

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_LITTLE_ENDIAN)
md.detail = True

UNK = ("?", None)
regs = {i: UNK for i in range(31)}
regs[31] = (0, None)  # xzr defined 0

def rd(r):
    return regs.get(r, UNK)

def known(v):
    return v is not None and v[0] == 0

def val(v):
    return v[1] if known(v) else None

def mem_class(a):
    if ALIAS_LO <= a < ALIAS_HI:
        off = a - ALIAS_LO
        data = buf[TEXT_FILE + off: TEXT_FILE + off + 8]
        return ("alias+image", off, data)
    if SCR_LO <= a < SCR_HI:
        return ("scratch", a - SCR_LO, None)
    return ("UNMAPPED", a, None)

pc = START
log = []
for step in range(LIMIT):
    fo = TEXT_FILE + pc
    code = buf[fo:fo + 4]
    ins = next(md.disasm(code, ENTRY + pc), None)
    if ins is None:
        log.append(f"[{pc:#x}] UNDECODED {code.hex()}")
        break
    m, ops = ins.mnemonic, ins.op_str
    line = f"[{pc:#07x}] {m} {ops}"

    def rnum(s):
        return 31 if s in ("xzr", "sp") else int(s[1:].rstrip(","))
    tokens = ops.replace("[", " ").replace("]", " ").replace(",", " ").split()
    rds = [rnum(o) for o in ins.op_str.split(",")[0:1] if o and o[0] in "wx" and not o.startswith("#")]
    wregs = [o for o in ins.op_str.split(",") if o.strip() and o.strip()[0] in "wx"]
    defined = None
    if m in ("mov", "movz", "movk", "orr", "add", "sub", "adrp", "lsl", "eor", "and", "madd", "csel"):
        defined = ins.operands[0].reg if ins.operands else None
    elif m in ("ldr", "ldur", "ldrb", "ldrh") and ins.operands and ins.operands[0].type == capstone.arm64.ARM64_OP_REG:
        defined = ins.operands[0].reg

    # address computation for memory ops
    memop = None
    for op in ins.operands:
        if op.type == capstone.arm64.ARM64_OP_MEM:
            base = op.mem.base
            rn = 31 if base == capstone.arm64.ARM64_REG_SP else base - capstone.arm64.ARM64_REG_X0 if base >= capstone.arm64.ARM64_REG_X0 else None
            v = rd(rn) if rn is not None else UNK
            if known(v):
                a = v[1] + op.mem.disp
                cls = mem_class(a)
                memop = (a, cls, op.mem.base)
            else:
                memop = (None, ("unknown-base", None), base)

    if memop and memop[0] is None:
        log.append(line + "   ; MEM from UNKNOWN base -> cannot verify")
        break
    if memop:
        a, cls, _ = memop
        if cls[0] == "UNMAPPED":
            log.append(line + f"   ; *** ACCESS OUTSIDE MAPPED SET: {a:#x} ***")
            break
        if m in ("ldr", "ldur"):
            if cls[0] == "alias+image":
                v = int.from_bytes(cls[2][:4], "little")
                log.append(line + f"   ; load [{a:#x}] alias+{cls[1]:#x} -> {v:#x} (image bytes)")
                if defined:
                    regs[defined] = (0, v)
                pc += 4
                continue
            log.append(line + f"   ; load [{a:#x}] scratch cell")
        else:
            log.append(line + f"   ; store [{a:#x}] {cls[0]}")
        pc += 4
        continue

    # PC-relative adrp: rd = ((PC+pc) & ~0xFFF) + imm
    if m == "adrp" and ins.operands and defined:
        imm = ins.operands[1].imm
        page = ((ENTRY + pc) & ~0xFFF) + (imm << 12)
        regs[defined] = (0, page)
        log.append(line + f"   ; -> {page:#x}")
        pc += 4
        continue
    if m == "adr" and ins.operands and defined:
        imm = ins.operands[1].imm
        target = ENTRY + pc + imm
        regs[defined] = (0, target)
        log.append(line + f"   ; -> {target:#x}")
        pc += 4
        continue

    # register-register ops on known values
    if defined is not None and ins.operands and all(
            o.type in (capstone.arm64.ARM64_OP_REG, capstone.arm64.ARM64_OP_IMM)
            for o in ins.operands):
        srcs = []
        ok = True
        for o in ins.operands[1:]:
            if o.type == capstone.arm64.ARM64_OP_REG:
                rn = 31 if o.reg == capstone.arm64.ARM64_REG_SP else o.reg - capstone.arm64.ARM64_REG_X0 if o.reg >= capstone.arm64.ARM64_REG_X0 else None
                v = rd(rn) if rn is not None else UNK
                if not known(v):
                    ok = False
                    break
                srcs.append(v[1])
            else:
                srcs.append(o.imm)
        if ok and m in ("mov", "add", "sub", "eor", "and", "orr"):
            if m == "mov":
                r = srcs[0]
            elif m == "add":
                r = srcs[0] + srcs[1]
            elif m == "sub":
                r = srcs[0] - srcs[1]
            elif m == "eor":
                r = srcs[0] ^ srcs[1]
            elif m == "and":
                r = srcs[0] & srcs[1]
            else:
                r = srcs[0] | srcs[1]
            regs[defined] = (0, r)
            log.append(line + f"   ; -> {r:#x}")
            pc += 4
            continue

    # branches: b / cbz-correct jumps only when target computable
    if m == "b" and ins.operands and ins.operands[0].type == capstone.arm64.ARM64_OP_IMM:
        tgt = ins.operands[0].imm - ENTRY
        if 0 <= tgt < len(buf) - TEXT_FILE:
            log.append(line + f"   ; jmp {tgt:#x}")
            pc = tgt
            continue
        log.append(line + f"   ; jmp outside __TEXT -> {tgt:#x}")
        break

    if defined is not None:
        regs[defined] = UNK
    log.append(line + "   ; (symbolic)")
    pc += 4

print("\n".join(log))
print(f"\ntraced {len(log)} steps, ended at pc={pc:#x}")
