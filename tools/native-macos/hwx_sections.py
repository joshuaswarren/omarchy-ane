#!/usr/bin/env python3
"""Fingerprint H14 HWX files so two compiles can be compared without both files on one host.

Apple's compiler output is byte-deterministic except for one provenance load command (cmd 0x8: the
compiler module version and the command line with the input/output paths). Its length moves the
relocation and symbol tables, so `normalized_sha256` hashes, in order: the header without sizeofcmds,
every other load command with the section reloff and symtab symoff/stroff fields zeroed, every
section's contents, its relocation entries, and the symbol and string tables. Per load command and
per section hashes locate any other difference.

  python3 hwx_sections.py prog_000=prog_000/model.hwx ... > fingerprints.jsonl
  python3 hwx_sections.py --compare a.jsonl b.jsonl
"""
import argparse, hashlib, json, os, re, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from hwx_h14_staged_to_anec import parse_hwx_segments  # noqa: E402

PROVENANCE = 0x8


def fingerprint(label, path):
    data = open(path, "rb").read()
    task_count = parse_hwx_segments(data)["task_count"]
    ncmds = struct.unpack_from("<I", data, 16)[0]
    norm = hashlib.sha256(data[:20] + data[24:32])
    lcs, sections, strings, tables, cursor = [], [], {}, [], 32
    for _ in range(ncmds):
        command, size = struct.unpack_from("<2I", data, cursor)
        body = bytearray(data[cursor:cursor + size])
        lcs.append({"cmd": command, "size": size, "sha256": hashlib.sha256(body).hexdigest()})
        if command == 0x19:
            for i in range(struct.unpack_from("<I", body, 64)[0]):
                at = 72 + i * 80
                sect, seg, _, length, offset, _, reloff, nreloc = struct.unpack_from("<16s16s2Q4I", body, at)
                part = data[offset:offset + length] if offset else b""
                name = b",".join(n.split(b"\0", 1)[0] for n in (seg, sect)).decode()
                sections.append({"name": name, "size": length,
                                 "sha256": hashlib.sha256(part).hexdigest() if part else "zerofill"})
                tables += [part, data[reloff:reloff + 8 * nreloc]]
                body[at + 56:at + 60] = bytes(4)
        elif command == 0x2:
            symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", body, 8)
            tables += [data[symoff:symoff + 16 * nsyms], data[stroff:stroff + strsize]]
            body[8:12] = body[16:20] = bytes(4)
        elif command == PROVENANCE:
            text = body.decode("latin-1")
            for key in ("ModuleVersion", "coremlc-version"):
                m = re.search(key + r":\s*([\w.]+)", text)
                if m:
                    strings[key] = m.group(1)
            m = re.search(r"-t (\w+)", text)
            strings["target"] = m.group(1) if m else None
        if command != PROVENANCE:
            norm.update(body)
        cursor += size
    for part in tables:
        norm.update(part)
    return {"label": label, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "normalized_sha256": norm.hexdigest(), "task_count": task_count, "compiler": strings,
            "load_commands": lcs, "sections": sections}


def compare(a_path, b_path):
    load = lambda p: {r["label"]: r for r in map(json.loads, open(p))}
    a, b = load(a_path), load(b_path)
    labels = sorted(set(a) | set(b))
    same = 0
    for label in labels:
        if label not in a or label not in b:
            print(f"{label}: missing in {'A' if label not in a else 'B'}")
            continue
        x, y = a[label], b[label]
        ok = x["normalized_sha256"] == y["normalized_sha256"]
        same += ok
        line = f"{label}: {'SAME' if ok else 'DIFFERENT'} bytes {x['bytes']}/{y['bytes']} tasks {x['task_count']}/{y['task_count']}"
        if not ok:
            lc = [f"{i}:0x{p['cmd']:x}" for i, (p, q) in enumerate(zip(x["load_commands"], y["load_commands"]))
                  if p != q and p["cmd"] != PROVENANCE]
            sc = [f"{i}:{p['name']}" for i, (p, q) in enumerate(zip(x["sections"], y["sections"])) if p != q]
            line += f" load commands {len(x['load_commands'])}/{len(y['load_commands'])} differing {lc or 'none'}" \
                    f" sections differing {sc or 'none'}"
        print(line)
    print(f"same after provenance normalization: {same}/{len(labels)}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"))
    ap.add_argument("hwx", nargs="*", help="LABEL=PATH")
    a = ap.parse_args()
    if a.compare:
        compare(*a.compare)
    else:
        for item in a.hwx:
            label, path = item.split("=", 1)
            print(json.dumps(fingerprint(label, path)), flush=True)
