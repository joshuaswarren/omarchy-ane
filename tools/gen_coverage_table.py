#!/usr/bin/env python3
"""The README's data-only chip table, from data/ane-soc/*.json and
packaging/dt/*-ane-dataonly.dts. It sits between the two marker lines below.

  gen_coverage_table.py           print the table
  gen_coverage_table.py --write   rewrite the README block
  gen_coverage_table.py --check   exit 1 when the README block is stale (CI)

Run tools/validate_ane_soc.py first: this reads only valid data files."""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
README = ROOT / "README.md"
BEGIN = "<!-- BEGIN DATA-ONLY TABLE -->"
END = "<!-- END DATA-ONLY TABLE -->"


def table():
    rows = []
    for path in sorted((ROOT / "data/ane-soc").glob("*.json")):
        doc = json.loads(path.read_text())
        soc = doc["soc"]
        overlay = ROOT / f"packaging/dt/{soc}-ane-dataonly.dts"
        rows.append(f"| {soc.upper()} | {doc['generation'].get('v') or 'unknown'} | {len(doc['boards'])} "
                    f"| [{path.name}](data/ane-soc/{path.name}) | "
                    + (f"[{overlay.name}](packaging/dt/{overlay.name})" if overlay.is_file() else "none")
                    + " |")
    if not rows:
        return "No chip has a data file yet.\n"
    return "\n".join(["| SoC | Internal | Boards | Data | Data-only overlay |",
                      "| --- | --- | --- | --- | --- |", *rows]) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    args = ap.parse_args(argv)
    if not (args.check or args.write):
        print(table(), end="")
        return 0
    text = README.read_text()
    head, begin, rest = text.partition(f"{BEGIN}\n")
    _, end, tail = rest.partition(END)
    if not begin or not end:
        print(f"gen_coverage_table: README.md has no {BEGIN} ... {END} block", file=sys.stderr)
        return 1
    new = f"{head}{BEGIN}\n{table()}{END}{tail}"
    if args.write:
        README.write_text(new)
        return 0
    if new != text:
        print("gen_coverage_table: the README data-only table is stale. Run: python3 tools/gen_coverage_table.py --write",
              file=sys.stderr)
        return 1
    print("gen_coverage_table: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
