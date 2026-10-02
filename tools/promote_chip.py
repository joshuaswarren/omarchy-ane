#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Flip an ANE chip between default-on and opt-in, exactly like the PR that
did it by hand (T6021: joshuaswarren/omarchy-ane#44).

  promote_chip.py --chip t8112 --to default-on --check   print the diff, write nothing
  promote_chip.py --chip t8112 --to default-on --apply   edit the tree
  promote_chip.py --chip t6021 --to opt-in --apply       the revert direction

A flip touches, and nothing else:

  packaging/dt/overlays             the chip's row: opt-in <-> enabled, and the
                                    "Tested SoCs are enabled (...)" header list
  packaging/dt/<chip>-ane.dts       the overlay's "omarchy,opt-in" marker
                                    (removed on default-on, restored on opt-in)
  packaging/omarchy-ane-check       the SoC moves between the case lists that
                                    decide whether UNTESTED prints
  packaging/omarchy-ane-dt          the docstring's opt-in key enumeration
  packaging/omarchy-ane-firmware-fetch   DEFAULT_ON (chips the install hook
                                    fetches firmware for; M2 family only)
  README.md                         the chip table row: State and Opt-in key
  CHANGELOG.md                      one Unreleased line (the direction log)

Driver qualification lives in the kernel (ane.ko ane_of_match, ane_t6021
ane_rtclient_of_match) and no flip needs it: T6000/T6002 are ANE_QUALIFIED in
ane.ko, and ane_t6021 binds T6020/T6022/T8112 with no gate. The tool refuses
a chip whose overlay is disabled.

--check is the default. Exit 0: done or nothing to do (idempotent). Exit 1:
refused, the tree is unchanged.
"""
import argparse
import difflib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

MARKETING = {"t8103": "M1", "t6000": "M1 Pro", "t6001": "M1 Max", "t6002": "M1 Ultra",
             "t8112": "M2", "t6020": "M2 Pro", "t6021": "M2 Max", "t6022": "M2 Ultra"}
# The State and Opt-in key cells a revert writes (README.md, "Chip coverage").
# The default-on chips have no opt-in row today: their revert cell is the plain
# opt-in form. A promote always writes ("on by default", "none").
README_OPTIN = {
    "t6000": ("opt-in, untested", "`ane-t6000`"),
    "t6002": ("opt-in, untested (die 0)", "`ane-t6002`"),
    "t8112": ("opt-in, untested", "`ane-t8112` + note"),
    "t6020": ("opt-in, untested", "`ane-t6020` + note"),
    "t6022": ("opt-in, untested (die 0)", "`ane-t6022` + note"),
    "t8103": ("opt-in", "`ane-t8103`"),
    "t6001": ("opt-in", "`ane-t6001`"),
    "t6021": ("opt-in", "`ane-t6021`"),
}
# Where each chip sits in packaging/omarchy-ane-check's opt-in case list, so a
# revert restores the original token order byte for byte.
CHECK_LIST_INDEX = {"t6000": 0, "t6002": 1, "t6020": 2, "t6022": 3, "t8112": 4,
                    "t8103": None, "t6001": None, "t6021": None}
# The seat of the chips that are on by default today, in the tested-header list
# and the check's on list, so a re-promote after a revert restores it.
FIRST_CLASS = {"t8103": 0, "t6001": 1, "t6021": 2}


class Refuse(Exception):
    pass


def read(rel):
    return (REPO / rel).read_text()


def overlay_rows():
    rows = {}
    for line in read("packaging/dt/overlays").splitlines():
        if line and line[0] != "#":
            prefix, src, state = line.split()
            if src == f"{prefix}-ane.dts":
                rows[prefix] = state
    return rows


def on_chips():
    return {p for p, state in overlay_rows().items() if state == "enabled"}


def opt_in_ane_chips():
    return sorted(p for p, state in overlay_rows().items() if state == "opt-in")


def join_names(names):
    return names if len(names) <= 1 else f"{', '.join(names[:-1])} and {names[-1]}"


def edit_overlays(text, chip, promote):
    row = re.search(rf"(?m)^{chip}\s+{chip}-ane\.dts\s+(\S+)\s*$", text)
    if not row:
        raise Refuse(f"packaging/dt/overlays: no row for {chip}")
    if row.group(1) == "disabled":
        raise Refuse(f"packaging/dt/overlays: {chip} is disabled; a flip needs the overlay enabled")
    want = "enabled" if promote else "opt-in"
    text = text[:row.start(1)] + want + text[row.end(1):]
    header = re.search(r"(?m)^# Tested SoCs are enabled \(([^)]*)\)\.", text)
    if not header:
        raise Refuse('packaging/dt/overlays: no "Tested SoCs are enabled (...)" header line')
    names = [n for n in header.group(1).split(", ") if n]
    up = chip.upper()
    if promote and up not in names:
        names.insert(min(FIRST_CLASS.get(chip, len(names)), len(names)), up)
    if not promote and up in names:
        names.remove(up)
    text = text[:header.start(1)] + ", ".join(names) + text[header.end(1):]
    return text


MARKER = ("\t/* dtb-overlays.sh: applies only when opted in. */\n"
          '\tomarchy,opt-in = "ane-{chip}";\n')


def edit_dts(text, chip, promote):
    marker = MARKER.format(chip=chip)
    if promote:
        if marker not in text:
            if f'omarchy,opt-in = "ane-{chip}"' not in text:
                return text  # already enabled
            raise Refuse(f"packaging/dt/{chip}-ane.dts: the opt-in marker is not in the expected shape")
        return text.replace(marker, "")
    if f'omarchy,opt-in = "ane-{chip}"' in text:
        return text  # already opt-in
    m = re.search(r'(?m)^\tomarchy,skip-if-compatible = "apple,[a-z0-9]+-ane";\n', text)
    if not m:
        raise Refuse(f"packaging/dt/{chip}-ane.dts: no skip-if-compatible line to anchor the marker")
    marker = MARKER.format(chip=chip)
    return text[:m.end()] + marker + text[m.end():]


def edit_check(text, chip, promote):
    m = re.search(r'(?m)^  ([a-z0-9|"]+)\) ;;\n  ([a-z0-9|]+)\)\n', text)
    if not m:
        raise Refuse("packaging/omarchy-ane-check: the UNTESTED case lists are not in the expected shape")
    on_list = [t for t in m.group(1).split("|") if t and t != '""']
    off_list = m.group(2).split("|")
    if promote:
        if chip in off_list:
            off_list.remove(chip)
            if chip not in on_list:
                on_list.insert(min(FIRST_CLASS.get(chip, len(on_list)), len(on_list)), chip)
        if not off_list:
            raise Refuse("packaging/omarchy-ane-check: the UNTESTED case arm would be empty "
                         "(every untested chip would be on); remove the arm by hand")
    elif chip in on_list:
        on_list.remove(chip)
        if chip not in off_list:
            index = CHECK_LIST_INDEX[chip]
            if index is None:  # a default-on chip reverting: it was never opt-in
                off_list.append(chip)
            else:
                off_list.insert(min(index, len(off_list)), chip)
    on_expr = ("|".join(on_list) + '|""') if on_list else '""'
    new = f'  {on_expr}) ;;\n  {"|".join(off_list)})\n'
    return text[:m.start()] + new + text[m.end():]


def edit_firmware_fetch(text, chip, promote):
    m = re.search(r"(?m)^DEFAULT_ON = \(([^)]*)\)\n", text)
    if not m:
        raise Refuse("packaging/omarchy-ane-firmware-fetch: no DEFAULT_ON line")
    chips = set(re.findall(r'"([^"]+)"', m.group(1)))
    (chips.add if promote else chips.discard)(f"apple,{chip}")
    body = ", ".join(f'"{c}"' for c in sorted(chips))
    if len(chips) == 1:
        body += ","
    return text[:m.start(1)] + body + text[m.end(1):]


def edit_dt_docstring(text, chip, promote):
    """The docstring's opt-in enumeration, regenerated from the post-flip set."""
    chips = [c for c in opt_in_ane_chips() if c != chip] if promote \
        else sorted(set(opt_in_ane_chips()) | {chip})
    m = re.search(r"\(the untested (?P<names>[^()]*?) nodes: (?P<keys>[^()]*?); the", text)
    if not m:
        raise Refuse("packaging/omarchy-ane-dt: the opt-in enumeration is not in the expected shape")
    names = join_names([c.upper() for c in chips]) or "none"
    keys = join_names([f'"ane-{c}"' for c in chips]) or "none"
    new = f"(the untested {names} nodes: {keys}; the"
    return text[:m.start()] + new + text[m.end():]


def edit_readme(text, chip, promote):
    lines = text.splitlines(keepends=True)
    hit = None
    for i, line in enumerate(lines):
        cells = line.split("|")
        if len(cells) > 7 and cells[2].strip() == chip.upper():
            hit = i
            break
    if hit is None:
        raise Refuse(f"README.md: no chip table row for {chip.upper()}")
    cells = lines[hit].split("|")
    if promote:
        cells[6], cells[7] = " on by default ", " none "
    else:
        state, key = README_OPTIN[chip]
        cells[6], cells[7] = f" {state} ", f" {key} "
    lines[hit] = "|".join(cells)
    return "".join(lines)


def changelog_line(chip, promote, note):
    tail = f" ({note})" if note else ""
    if promote:
        return f"- {chip.upper()} ({MARKETING[chip]}) ANE on by default.{tail}"
    return f"- {chip.upper()} ({MARKETING[chip]}) ANE back to opt-in.{tail}"


def edit_changelog(text, chip, promote, note):
    keep = changelog_line(chip, promote, note)
    drop = changelog_line(chip, not promote, note)
    kept = [l for l in text.splitlines(keepends=True) if l.rstrip("\n") not in (keep, drop)]
    h = next((i for i, l in enumerate(kept) if l.strip() == "## Unreleased"), None)
    if h is None:
        raise Refuse("CHANGELOG.md: no Unreleased section")
    at = h + 2 if h + 1 < len(kept) and kept[h + 1].strip() == "" else h + 1
    kept.insert(at, keep + "\n")
    return "".join(kept)


def files(chip, promote, note):
    edits = [("packaging/dt/overlays", edit_overlays),
             (f"packaging/dt/{chip}-ane.dts", edit_dts),
             ("packaging/omarchy-ane-check", edit_check),
             ("packaging/omarchy-ane-dt", lambda t, c, p: edit_dt_docstring(t, c, p)),
             ("README.md", edit_readme),
             ("CHANGELOG.md", lambda t, c, p: edit_changelog(t, c, p, note))]
    if f"apple,{chip}" in re.search(r'(?m)^FETCH = \{([^}]*)\}', read("packaging/omarchy-ane-firmware-fetch")).group(1):
        edits.insert(3, ("packaging/omarchy-ane-firmware-fetch", edit_firmware_fetch))
    return edits


def plan(chip, promote, note):
    planned = []
    for rel, fn in files(chip, promote, note):
        old = read(rel)
        new = fn(old, chip, promote)
        if new != old:
            planned.append((rel, old, new))
    return planned


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--chip", required=True, choices=sorted(MARKETING))
    ap.add_argument("--to", required=True, choices=("default-on", "opt-in"), dest="to")
    ap.add_argument("--check", action="store_true", default=False)
    ap.add_argument("--apply", action="store_true", default=False)
    ap.add_argument("--note", help="appended to the CHANGELOG line in parentheses (e.g. a row sha)")
    args = ap.parse_args(argv)
    if args.apply == args.check:
        ap.error("give exactly one of --check and --apply (--check is the safe default, but be explicit)")
    promote = args.to == "default-on"
    state = on_chips()
    if not promote and args.chip not in state:
        print(f"promote_chip: {args.chip} is already opt-in; nothing to do")
        return 0
    if promote and args.chip in state:
        print(f"promote_chip: {args.chip} is already on by default; nothing to do")
        return 0
    try:
        planned = plan(args.chip, promote, args.note)
    except Refuse as e:
        print(f"promote_chip: refused: {e}", file=sys.stderr)
        return 1
    if not planned:
        print(f"promote_chip: {args.chip} is already {args.to}; nothing to do")
        return 0
    if args.check:
        for rel, old, new in planned:
            diff = "".join(difflib.unified_diff(old.splitlines(keepends=True), new.splitlines(keepends=True),
                                                fromfile=f"a/{rel}", tofile=f"b/{rel}"))
            sys.stdout.writelines(diff)
        print(f"promote_chip: would flip {args.chip} to {args.to} ({len(planned)} file(s))")
        return 0
    for rel, _, new in planned:
        (REPO / rel).write_text(new)
    print(f"promote_chip: {args.chip} -> {args.to} ({len(planned)} file(s) written)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
