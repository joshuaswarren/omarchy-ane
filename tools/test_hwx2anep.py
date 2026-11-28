#!/usr/bin/env python3
"""Self-test for tools/hwx2anep.py: HWX -> ANEP program file.

Builds a synthetic H18 HWX (LC_SEGMENT_64 map, LC 0x40 buffer references)
plus a manifest and checks the ANEP the tool emits: header fields, the
section-2 layout (const window first, weights behind), section-3 task
stream, io records in loader order, zero-filled buffer data sizes, and
every refusal (bad magic or subtype, short const, ref without a manifest
entry or a surface, size or address-ladder mismatch, bad direction name).
"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/hwx2anep.py"

MAGIC, SUBTYPE_H18 = 0xBEEFFACE, 10
TEXT_OFF, CONST_OFF, IN_OFF, OUT_OFF, KERN_OFF = 0x1000, 0x2000, 0x4000, 0x8000, 0xA000


def segment(segname, sections):
    # one LC_SEGMENT_64 per segment name: the parser keys sections by the
    # command-level segment field, not the per-record segname
    body = bytearray(72 + 80 * len(sections))
    struct.pack_into("<2I16s4Q4I", body, 0, 0x19, len(body),
                     segname.encode(), 0, 0, 0, 0, 0, 0, len(sections), 0)
    for i, (seg, sec, addr, size, off) in enumerate(sections):
        struct.pack_into("<16s16s2Q8I", body, 72 + i * 80,
                         sec.encode(), seg.encode(), addr, size, off,
                         0, 0, 0, 0, 0, 0, 0)
    return bytes(body)


def ref(address, name):
    return struct.pack("<2I", 0x40, 0x20) + bytes(8) + struct.pack("<Q", address) + name.encode().ljust(8, b"\0")


def hwx(subtype=SUBTYPE_H18, sections=None, refs=(), payloads=None):
    sections = sections or []
    groups = []
    for seg, sec, addr, size, off in sections:
        if groups and groups[-1][0] == seg:
            groups[-1][1].append((seg, sec, addr, size, off))
        else:
            groups.append((seg, [(seg, sec, addr, size, off)]))
    cmds = [segment(name, group) for name, group in groups] + [ref(a, n) for a, n in refs]
    blob = b"".join(cmds)
    head = struct.pack("<8I", MAGIC, 0, subtype, 0, len(cmds), len(blob), 0, 0)
    data = bytearray(32 + len(blob))
    data[:32] = head
    data[32:] = blob
    for off, content in (payloads or {}).items():
        data += bytes(off - len(data)) + content
    return bytes(data)


def base_sections(const_size=0x4000, buffers=((IN_OFF, 0x4000), (OUT_OFF, 0x8000)), kern=False):
    sections = [("__TEXT", "__text", 0x40000000, 0x100, TEXT_OFF),
                ("__TEXT", "__const", 0x40001000, const_size, CONST_OFF)]
    for i, (addr, size) in enumerate(buffers):
        sections.append(("__FVMLIB", f"__buf{i}", addr, size, {IN_OFF: IN_OFF, OUT_OFF: OUT_OFF}.get(addr, addr)))
    if kern:
        sections.append(("__KERN_0", "__kern_0", 0x50000000, 0x80, KERN_OFF))
    return sections


def stage(tmp, sections=None, refs=None, manifest=None, subtype=SUBTYPE_H18, payloads=None):
    sections = base_sections() if sections is None else sections
    refs = [(0x4000, "input0"), (0x8000, "output0")] if refs is None else refs
    payloads = payloads or {TEXT_OFF: b"TASKSTREAM".ljust(0x100, b"\x11"),
                            CONST_OFF: b"CONST".ljust(0x4000, b"\x22")}
    manifest = manifest or {"inputs": [{"index": 1, "allocationBytes": 0x4000, "name": "input0"}],
                            "outputs": [{"index": 4, "allocationBytes": 0x8000, "name": "output0"}]}
    hp, mp = Path(tmp) / "p.hwx", Path(tmp) / "m.json"
    hp.write_bytes(hwx(subtype, sections, refs, payloads))
    mp.write_text(json.dumps(manifest))
    return hp, mp


def run(hp, mp, extra=()):
    return subprocess.run([sys.executable, str(TOOL), "--hwx", str(hp),
                           "--manifest", str(mp), *extra], capture_output=True, text=True)


def parse_anep(data):
    magic, version, nsec, nio = struct.unpack_from("<4I", data, 0)
    secs = [struct.unpack_from("<2I3Q", data, 0x20 + i * 0x20) for i in range(nsec)]
    ios = [struct.unpack_from("<2I4Q", data, 0x20 + nsec * 0x20 + i * 0x20) for i in range(nio)]
    return magic, version, secs, ios


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as tmp:
        hp, mp = stage(tmp)
        out = Path(tmp) / "p.bin"
        result = run(hp, mp, ("-o", str(out)))
        check("build rc", result.returncode, 0)
        data = out.read_bytes()
        magic, version, secs, ios = parse_anep(data)
        check("magic", magic, 0x50454E41)
        check("version", version, 1)
        check("nsec ids", [s[0] for s in secs], [2, 3])
        check("sec2 size", secs[0][2], 0x4000)
        check("sec3 size", secs[1][2], 0x100)
        check("sec offsets aligned", all(s[3] % 16 == 0 for s in secs), True)
        check("sec2 payload", data[secs[0][3]:secs[0][3] + 5], b"CONST")
        check("sec3 payload", data[secs[1][3]:secs[1][3] + 10], b"TASKSTREAM")
        check("io recs", [(i[0], i[1], i[2]) for i in ios], [(1, 0, 0x4000), (4, 1, 0x8000)])
        check("io data sizes zero", all(i[4] == 0 for i in ios), True)

        # with a __KERN_0 section, section 2 carries the weights behind the const
        sections = base_sections(kern=True)
        payloads = {TEXT_OFF: b"TASKSTREAM".ljust(0x100, b"\x11"),
                    CONST_OFF: b"CONST".ljust(0x4000, b"\x22"),
                    KERN_OFF: b"WEIGHTS".ljust(0x80, b"\x33")}
        hp, mp = stage(tmp, sections=sections, payloads=payloads)
        out = Path(tmp) / "p2.bin"
        result = run(hp, mp, ("-o", str(out)))
        check("kern rc", result.returncode, 0)
        magic, version, secs, ios = parse_anep(out.read_bytes())
        check("sec2 with kern", secs[0][2], 0x4000 + 0x80)
        check("kern payload", out.read_bytes()[secs[0][3] + 0x4000:secs[0][3] + 0x4007], b"WEIGHTS")

        # --print-plan writes nothing and lists the buffers
        result = run(hp, mp, ("--print-plan",))
        check("plan rc", result.returncode, 0)
        check("plan buffers", "buffer id=4 dir=1 size=32768 addr=0x8000 name=output0" in result.stdout, True)
        check("plan kern", "prog_kern_slot=1" in result.stdout, True)

        # refusals
        bad = Path(tmp) / "bad.hwx"
        bad.write_bytes(hwx(subtype=5))
        check("subtype rc", run(bad, mp, ("-o", "/tmp/x.bin")).returncode, 1)
        check("subtype reason", "not H18/H18G" in run(bad, mp, ("-o", "/tmp/x.bin")).stderr, True)

        bad.write_bytes(b"\x00" * 64)
        check("magic rc", run(bad, mp, ("-o", "/tmp/x.bin")).returncode, 1)

        hp, mp = stage(tmp, sections=base_sections(const_size=0x100),
                       payloads={TEXT_OFF: b"T".ljust(0x100, b"\x11"),
                                 CONST_OFF: b"C".ljust(0x100, b"\x22")})
        check("short const rc", run(hp, mp, ("-o", "/tmp/x.bin")).returncode, 1)

        # a ref that has no manifest entry (two inputs, one manifest input)
        hp, mp = stage(tmp, refs=[(0x4000, "input0"), (0x8000, "input1")],
                       manifest={"inputs": [{"index": 1, "allocationBytes": 0x4000, "name": "input0"}],
                                 "outputs": [{"index": 4, "allocationBytes": 0x8000, "name": "output0"}]})
        check("ref count rc", run(hp, mp, ("-o", "/tmp/x.bin")).returncode, 1)

        # surface size disagrees with the manifest
        hp, mp = stage(tmp, manifest={"inputs": [{"index": 1, "allocationBytes": 0x4000, "name": "input0"}],
                                      "outputs": [{"index": 4, "allocationBytes": 0x4000, "name": "output0"}]})
        check("size mismatch rc", run(hp, mp, ("-o", "/tmp/x.bin")).returncode, 1)

        # a second buffer off the address ladder
        sections = base_sections(buffers=((IN_OFF, 0x4000), (0x9000, 0x8000)))
        sections = [(s, n, a, z, 0x9000 if a == 0x9000 else o) for s, n, a, z, o in sections]
        hp, mp = stage(tmp, sections=sections, refs=[(0x4000, "input0"), (0x9000, "output0")])
        result = run(hp, mp, ("-o", "/tmp/x.bin"))
        check("ladder rc", result.returncode, 1)
        check("ladder reason", "does not follow" in result.stderr, True)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_hwx2anep: {len(fails)} failed")
        return 1
    print("test_hwx2anep: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
