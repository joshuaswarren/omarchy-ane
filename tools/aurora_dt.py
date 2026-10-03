#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Enable one chip's ANE in an aurora-silicon/linux tree: the device-tree half
of a PROMOTE verdict whose passing row ran the kernel's own driver
(driver_source=intree). tools/promote_from_verdict.py aurora opens the PR.

  aurora_dt.py --tree LINUX --chip t6000 --check   print the diff, write nothing
  aurora_dt.py --tree LINUX --chip t6000 --apply   edit the tree

It builds every board device tree of the chip (arch/arm64/boot/dts/apple/
CHIP-*.dts) the way kbuild does, with the dtc on PATH (the tree's own:
tools/asahi-dtbs OUT/bin), and collects the ANE node and every node it reaches
through iommus, mboxes, power-domains, resets, memory-region and
interrupt-parent, then through theirs, and their parents. The change enables
the disabled ones, in CHIP.dtsi only:

  switch  CHIP.dtsi defines APPLE_ANE_UNTESTED (T6000, T6020 and T8103 at
          #155): the define and the comment above it go, and the tree's own
          #ifndef block enables the nodes.
  blocks  otherwise (T6002, T6022 and T8112 at #155): one
          `&label { status = "okay"; };` per disabled node, appended.

Then it builds the boards again from the changed tree and refuses unless every
collected node is enabled. It also refuses when another file includes CHIP.dtsi
(the change would reach another chip) or when no board has an apple,*-ane node.
Exit 0: done, or nothing to do (every collected node is already enabled).
Exit 1: refused; the tree is unchanged.
"""
import argparse
import difflib
import re
import shutil
import subprocess
import sys
import tempfile
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
_spec = spec_from_loader("oadt", SourceFileLoader("oadt", str(REPO / "packaging/omarchy-ane-dt")))
OADT = module_from_spec(_spec)
_spec.loader.exec_module(OADT)

DTS = "arch/arm64/boot/dts/apple"
INC = "scripts/dtc/include-prefixes"
SWITCH = "#define APPLE_ANE_UNTESTED"
PLAIN = ("interrupt-parent", "memory-region")
MARKETING = {"t8103": "M1", "t6000": "M1 Pro", "t6001": "M1 Max", "t6002": "M1 Ultra",
             "t8112": "M2", "t6020": "M2 Pro", "t6021": "M2 Max", "t6022": "M2 Ultra"}


class Refuse(Exception):
    pass


def build(dts_dir, inc, board, work):
    """The board's device tree with __symbols__ (dtc -@), as an omarchy-ane-dt Tree."""
    pre, out = work / f"{board.stem}.pre", work / f"{board.stem}.dtb"
    for cmd in (["gcc", "-E", "-nostdinc", "-I", str(inc), "-undef", "-D__DTS__", "-x", "assembler-with-cpp",
                 "-o", str(pre), str(board)],
                ["dtc", "-@", "-q", "-O", "dtb", "-o", str(out), "-b", "0", "-i", str(dts_dir), "-i", str(inc),
                 str(pre)]):
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode:
            raise Refuse(f"{cmd[0]} failed on {board.name}: {p.stderr.strip()[:400]}")
    return OADT.Tree(out.read_bytes())


def reached(tree, start):
    """START and every node it reaches by phandle, transitively, and their parents."""
    seen, todo = set(), list(start)
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        props = tree.nodes[path]
        targets = [tree.phandles.get(p) for prop in PLAIN for p in OADT.cells(props.get(prop, b""))]
        for prop, count in OADT.REFS.items():
            words, i = OADT.cells(props.get(prop, b"")), 0
            while i < len(words):
                target = tree.phandles.get(words[i])
                targets.append(target)
                i += 1 + (OADT.cells(tree.nodes[target][count])[0] if target and count in tree.nodes[target] else 0)
        if path != "/":
            targets.append(path.rsplit("/", 1)[0] or "/")
        todo += [t for t in targets if t]
    return seen


def ane_paths(tree):
    return [p for p in tree.nodes if any(OADT.ANE.fullmatch(c) for c in tree.strings(p, "compatible"))]


def labels(tree):
    """{path: label}, the first label in sorted order for each node."""
    out = {}
    for label in sorted(tree.nodes.get("/__symbols__", {})):
        out.setdefault(tree.strings("/__symbols__", label)[0], label)
    return out


def drop_switch(text):
    lines = text.splitlines(keepends=True)
    i = next(n for n, l in enumerate(lines) if l.rstrip("\n") == SWITCH)
    j = i
    if i and lines[i - 1].rstrip().endswith("*/"):
        j = i - 1
        while not lines[j].lstrip().startswith("/*"):
            j -= 1
    del lines[j:i + 1]
    if j < len(lines) and not lines[j].strip() and (j == 0 or not lines[j - 1].strip()):
        del lines[j]
    return "".join(lines)


def add_blocks(text, chip, refs):
    blocks = "".join(f"\n{ref} {{\n\tstatus = \"okay\";\n}};\n" for ref in refs)
    return (text.rstrip("\n") + f"\n\n/* The ANE has run on the {MARKETING.get(chip, chip.upper())}: "
            f"enable it and the nodes it uses. */" + blocks)


def plan(tree_dir, chip):
    """{file, old, new, how, enabled, boards}, or None when the ANE is already on."""
    tree_dir = Path(tree_dir)
    dts_dir, inc = tree_dir / DTS, tree_dir / INC
    soc_file = dts_dir / f"{chip}.dtsi"
    boards = sorted(dts_dir.glob(f"{chip}-*.dts"))
    if not soc_file.is_file() or not boards:
        raise Refuse(f"{DTS} has no {chip}.dtsi or no {chip}-*.dts board")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        before = {b.name: build(dts_dir, inc, b, work) for b in boards}
        want = {}
        for name, t in before.items():
            ane = ane_paths(t)
            if not ane:
                raise Refuse(f"{name} has no apple,*-ane node at this tree")
            want[name] = sorted(reached(t, ane))
        off = {p for name, t in before.items() for p in want[name] if not t.enabled(p)}
        if not off:
            return None
        others = [f.name for f in dts_dir.glob("*.dts*") if f'#include "{chip}.dtsi"' in f.read_text()
                  and f not in boards]
        if others:
            raise Refuse(f"{', '.join(sorted(others))} include {chip}.dtsi, so a change there reaches another chip")
        old = soc_file.read_text()
        first = before[boards[0].name]
        if any(l.rstrip("\n") == SWITCH for l in old.splitlines()):
            how, new = "switch", drop_switch(old)
        else:
            names = labels(first)
            ane = set(ane_paths(first))
            refs = [f"&{names[p]}" if p in names else f"&{{{p}}}"
                    for p in sorted(off, key=lambda p: (p not in ane, names.get(p, p)))]
            how, new = "blocks", add_blocks(old, chip, refs)
        changed = work / "tree" / DTS
        shutil.copytree(dts_dir, changed)
        (changed / soc_file.name).write_text(new)
        enabled = set()
        for b in boards:
            after = build(changed, inc, changed / b.name, work)
            still = [p for p in want[b.name] if not after.enabled(p)]
            if still:
                raise Refuse(f"{b.name}: still disabled after the {how} change: {', '.join(still)}")
            enabled |= {p for p in after.nodes if after.enabled(p) and not before[b.name].enabled(p)}
        names = labels(first)
        return {"file": f"{DTS}/{soc_file.name}", "old": old, "new": new, "how": how, "boards": [b.name for b in boards],
                "enabled": sorted(names.get(p, p) for p in enabled)}


def diff(p):
    return "".join(difflib.unified_diff(p["old"].splitlines(keepends=True), p["new"].splitlines(keepends=True),
                                        fromfile=f"a/{p['file']}", tofile=f"b/{p['file']}"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tree", required=True, type=Path, help="aurora-silicon/linux checkout (arch/arm64/boot/dts/apple, "
                                                               "include/dt-bindings, scripts/dtc)")
    ap.add_argument("--chip", required=True, choices=sorted(MARKETING))
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="print the diff, write nothing (default)")
    mode.add_argument("--apply", action="store_true", help="write the change")
    args = ap.parse_args(argv)
    try:
        p = plan(args.tree, args.chip)
    except Refuse as e:
        print(f"aurora_dt: refused: {e}", file=sys.stderr)
        return 1
    if p is None:
        print(f"aurora_dt: {args.chip}: the ANE and every node it uses are already enabled; nothing to do")
        return 0
    if args.apply:
        (args.tree / p["file"]).write_text(p["new"])
    else:
        sys.stdout.write(diff(p))
    print(f"aurora_dt: {args.chip}: {p['how']} in {p['file']}; enables {', '.join(p['enabled'])} on "
          f"{', '.join(p['boards'])}" + (" (written)" if args.apply else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
