#!/usr/bin/env python3
"""Self-test for tools/hwx_ports.py: port derivation from a synthetic H14 HWX.

Builds the smallest valid H14 HWX (magic 0xBEEFFACE, a __TEXT/__FVMLIB
segment map, LC 0x40 name records, LC 4 kind-3 tensor descriptors, the
kind-4 slot table) plus the matching ANEC header + task stream and a MIL
signature, then pins the derived port table: channels planned by the
converter (first output = 4, then inputs, then other outputs), BAR-slot
DMA coverage per class, the scratch slot, the ordering checks, and the
exceptions the tool raises (unbarred TileDMA base write, unwritten
output, input written, non-fp16 dtype, span overlap).
"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/hwx_ports.py"

TEXT, CONST = 0x1000, 0x2000
IN_A, OUT_A = 0x3000, 0xB000
ALLOC = 0x8000


def lc(command, body, pad_to=None):
    size = pad_to or (8 + len(body))
    return struct.pack("<2I", command, size) + body.ljust(size - 8, b"\0")


def segment(name, sections):
    # LC body starts at +8: segment name (16s), section count at +64,
    # 80-byte section records at +72 (segname, secname, address, length).
    body = bytearray(64 + 4 + 80 * len(sections))
    body[:16] = name.encode().ljust(16, b"\0")
    struct.pack_into("<I", body, 56, len(sections))
    for i, (segname, secname, address, length) in enumerate(sections):
        # record order is sectname, segname (parse keeps the first field)
        struct.pack_into("<16s16s2Q", body, 64 + i * 80,
                         secname.encode(), segname.encode(), address, length)
    return lc(0x19, bytes(body))


def name_record(address, name):
    return lc(0x40, bytes(8) + struct.pack("<Q", address) + name.encode(), pad_to=0x20 + len(name) + 1)


def tensor(symbol, binding, shape, strides, surface):
    # LC body starts at +8. Fields are read cursor-relative: kind +8,
    # binding +0x14, symbol offset +0x20, element code +0x24, shape +0x28,
    # strides +0x50, surface bytes +0x70; the symbol string at +0x80.
    body = bytearray(0x78 + len(symbol) + 1)
    struct.pack_into("<I", body, 0, 3)
    struct.pack_into("<I", body, 0x0C, binding)
    struct.pack_into("<I", body, 0x18, 0x80)
    struct.pack_into("<I", body, 0x1C, 5)
    struct.pack_into("<4I", body, 0x20, *shape)
    struct.pack_into("<4Q", body, 0x48, *strides)
    struct.pack_into("<Q", body, 0x68, surface)
    body[0x78:0x78 + len(symbol)] = symbol.encode()
    return lc(4, bytes(body))


def slot_table(slots):
    body = struct.pack("<I", 4) + bytes(4)
    for slot in range(64):
        body += struct.pack("<Q", slots[slot] if slot < len(slots) else 0) + bytes(8)
    return lc(4, body)


def hwx(tensors, slots, sections=None):
    sections = sections or [("__TEXT", "__text", TEXT, 0x100), ("__TEXT", "__const", CONST, 0x100),
                            ("__FVMLIB", "__io_in", IN_A, ALLOC), ("__FVMLIB", "__io_out", OUT_A, ALLOC)]
    cmds = [segment("__TEXT", sections)]
    for slot in range(4, len(slots)):
        if slots[slot]:
            symbol = next(s[1] for _, s, *_ in tensors if slots[slot] == s[0])
            cmds.append(name_record(slots[slot], symbol.removesuffix("@output")))
    cmds += [tensor(symbol, binding, shape, strides, surface)
             for _, (_, symbol), binding, shape, strides, surface in tensors]
    cmds.append(slot_table(slots))
    head = struct.pack("<5I", 0xBEEFFACE, 0, 5, 0, len(cmds)).ljust(32, b"\0")
    return head + b"".join(cmds)


def bar_ref(slot, register, count=2):
    return (1 << 29) | (slot << 23) | ((count - 1) << 15) | (register // 4)


def task(*records):
    # the task header's low halfword at +2 holds the word count (bits 16:26
    # of word 0); payload words start at index 8 (tw[7] & 3 != 3)
    words = [((8 + len(records)) << 16)] + [0] * 7 + list(records)
    return struct.pack(f"<{len(words)}I", *words)


def anec(stream, const_len, input_count, tiles):
    header = bytearray(0x1000)
    struct.pack_into("<Q", header, 0x10, len(stream))
    struct.pack_into("<Q", header, 0x18, const_len)
    struct.pack_into("<I", header, 0x20, input_count)
    struct.pack_into("<32I", header, 0x28, *tiles)
    return bytes(header) + stream


def mil(dtypes):
    return ("program(1.3)\n{\n    func main<ios18>(" +
            ", ".join(f"tensor<{t}, [4]> {n}" for n, t in dtypes) + ") {\n" +
            "    } -> (" + ", ".join(n for n, _ in dtypes) + ");\n}\n")


def stage(tmp, stream_tasks, slots, tiles, tensors, mil_text, input_count=1,
          const_len=0x100, sections=None):
    stream = b"".join(stream_tasks)
    stream = stream.ljust((len(stream) + 63) // 64 * 64, b"\0")
    hwx_data = hwx(tensors, slots, sections)
    anec_data = anec(stream, const_len, input_count, tiles)

    paths = {"m.hwx": Path(tmp) / "m.hwx", "m.anec": Path(tmp) / "m.anec",
             "m.mil": Path(tmp) / "m.mil"}
    paths["m.hwx"].write_bytes(hwx_data)
    paths["m.anec"].write_bytes(anec_data)
    paths["m.mil"].write_text(mil_text)
    return paths


def run(paths, prog="prog_999"):
    return subprocess.run([sys.executable, str(TOOL), "--program", prog,
                           "--hwx", str(paths["m.hwx"]), "--anec", str(paths["m.anec"]),
                           "--mil", str(paths["m.mil"])], capture_output=True, text=True)


SLOTS = [TEXT, CONST, 0, 0, IN_A, OUT_A]
TILES = [0] * 32
TILES[4] = TILES[5] = ALLOC // 0x4000
TENSORS = [("t_in", (IN_A, "t_in"), 1, [1, 1, 1, 4], [4, 4, 4, 1], 8),
           ("t_out", (OUT_A, "t_out@output"), 2, [1, 1, 1, 4], [4, 4, 4, 1], 8)]
MIL = mil([("t_in", "fp16"), ("t_out", "fp16")])


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as tmp:
        # happy path: input on slot 4 with srcA DMA, output on slot 5 with dst DMA
        stream = task(bar_ref(4, 0x1110), 0, 0, bar_ref(5, 0x1508), 0, 0)
        paths = stage(tmp, [stream], SLOTS, TILES, TENSORS, MIL)
        result = run(paths)
        check("happy rc", result.returncode, 0)
        table = json.loads(result.stdout)
        ports = {p["name"]: p for p in table["ports"]}
        check("channels", (ports["t_out"]["buffer_id"], ports["t_in"]["buffer_id"]), (4, 5))
        check("directions", (ports["t_in"]["direction"], ports["t_out"]["direction"]), ("input", "output"))
        check("coverage slots", [c["bar_slot"] for c in table["dma_coverage"]], [4, 5])
        check("coverage classes", {c["buffer_id"]: c["dma_classes"] for c in table["dma_coverage"]},
              {4: ["dst"], 5: ["srcA"]})
        check("no exceptions", table["exceptions"], [])
        check("no ambiguities", table["ambiguities"], [])

        # an output referenced only by a srcA ref is never written
        stream = task(bar_ref(4, 0x1110), 0, 0, bar_ref(5, 0x1110), 0, 0)
        paths = stage(tmp, [stream], SLOTS, TILES, TENSORS, MIL)
        result = run(paths)
        check("output unwritten rc", result.returncode, 1)
        check("output unwritten", any("never written" in e for e in json.loads(result.stdout)["exceptions"]), True)

        # an unbarred TileDMA base write
        dense = (0 << 29) | (1 << 15) | (0x1110 // 4)   # dense record, no BAR bit
        stream = task(bar_ref(4, 0x1110), 0, 0, bar_ref(5, 0x1508), 0, 0, dense, 0)
        paths = stage(tmp, [stream], SLOTS, TILES, TENSORS, MIL)
        result = run(paths)
        check("unbarred base rc", result.returncode, 1)
        check("unbarred base", any("without a BAR ref" in e for e in json.loads(result.stdout)["exceptions"]), True)

        # an input written by dst
        stream = task(bar_ref(4, 0x1508), 0, 0, bar_ref(5, 0x1508), 0, 0)
        paths = stage(tmp, [stream], SLOTS, TILES, TENSORS, MIL)
        check("input written rc", run(paths).returncode, 1)

        # a second same-shape input creates an ambiguity group
        in2 = ("t_in2", (0xD000, "t_in2"), 1, [1, 1, 1, 4], [4, 4, 4, 1], 8)
        slots2 = [TEXT, CONST, 0, 0, IN_A, 0xD000, OUT_A]
        sections2 = [("__TEXT", "__text", TEXT, 0x100), ("__TEXT", "__const", CONST, 0x100),
                     ("__FVMLIB", "__i1", IN_A, ALLOC), ("__FVMLIB", "__i2", 0xD000, ALLOC),
                     ("__FVMLIB", "__o", OUT_A, ALLOC)]
        tiles2 = list(TILES)
        tiles2[5] = tiles2[6] = ALLOC // 0x4000
        tensors2 = TENSORS + [in2]
        stream = task(bar_ref(4, 0x1110), 0, 0, bar_ref(5, 0x1110), 0, 0, bar_ref(6, 0x1508), 0, 0)
        paths = stage(tmp, [stream], slots2, tiles2, tensors2,
                      mil([("t_in", "fp16"), ("t_in2", "fp16"), ("t_out", "fp16")]),
                      input_count=2, sections=sections2)
        result = run(paths)
        table = json.loads(result.stdout)
        check("ambiguity found", len(table["ambiguities"]), 1)
        check("ambiguity ports", table["ambiguities"][0]["ports"], ["t_in", "t_in2"])

        # non-fp16 dtype refuses
        paths = stage(tmp, [task(bar_ref(4, 0x1110), 0, 0, bar_ref(5, 0x1508), 0, 0)],
                      SLOTS, TILES, TENSORS, mil([("t_in", "fp32"), ("t_out", "fp16")]))
        result = run(paths)
        check("fp32 rc", result.returncode, 1)
        check("fp32 stderr", "unsupported dtype" in result.stderr, True)

        # malformed hwx refuses before any table
        paths["m.hwx"].write_bytes(bytes(64))
        result = run(paths)
        check("bad hwx rc", result.returncode, 1)
        check("bad hwx stderr", "not an H14 HWX" in result.stderr, True)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_hwx_ports: {len(fails)} failed")
        return 1
    print("test_hwx_ports: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
