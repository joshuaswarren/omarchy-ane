#!/usr/bin/env python3
"""Board-key resolution test for data/ane-soc records (G304 follow-up).

For every board row of a SoC record, verify the board's device tree resolves
in an aurora dts checkout: arch/arm64/boot/dts/apple/<soc>-<board>.dts exists,
its root compatible names the board (apple,<board>), and the record still
carries the ladder keys the G304 m3air first-boot ticket reads (ANE
compatible, ane-type, reg windows).

Needs no hardware. Skips (exit 0) when no dts checkout is available;
fails when a dts checkout exists and a requested board does not resolve.

Default scope is t8122 (this record's boards were dts-verified in the same
commit). Pass SoC names for others; other records still carry legacy board
labels and unresolved boards, so sweeping them is opt-in:

  python3 tools/test_ane_soc_boards.py            # t8122 only
  python3 tools/test_ane_soc_boards.py t8122 t8140
  python3 tools/test_ane_soc_boards.py --all      # every record; loud

Environment:
  AURORA_DTS_DIR   dts directory (default ~/src/aurora-linux/arch/arm64/boot/dts/apple)
"""
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DTS_DIR = Path(os.environ.get("AURORA_DTS_DIR",
                              os.path.expanduser("~/src/aurora-linux/arch/arm64/boot/dts/apple")))

# The keys the G304 m3air first-boot ladder reads from the record before it
# will trust a board: the ANE compatible, the ane-type, and the first reg window.
LADDER_KEYS = ("compatible", "ane_type", "reg")


def board_from_dts(path: Path) -> str | None:
    """apple,<board> from the root compatible line, or None."""
    text = path.read_text(errors="replace")
    m = re.search(r'compatible\s*=\s*"apple,([a-z0-9]+)"\s*,\s*"apple,t[0-9a-z]+"', text)
    return m.group(1) if m else None


def has_value(node) -> bool:
    """True when a record branch carries at least one measured 'v' leaf."""
    if isinstance(node, dict):
        if "v" in node:
            return node["v"] is not None
        return any(has_value(v) for v in node.values())
    if isinstance(node, list):
        return any(has_value(v) for v in node)
    return False


def board_ids(doc) -> list[str]:
    ids = []
    for b in doc.get("boards", []):
        if isinstance(b, str):
            ids.append(b)
        elif isinstance(b, dict):
            v = b.get("v")
            if isinstance(v, dict):
                ids.append(str(v.get("value")))
            elif isinstance(v, str):
                ids.append(v)
    return [i for i in ids if i]


def check_record(path: Path, dts: Path, failures: list[str]) -> int:
    doc = json.loads(path.read_text())
    soc = doc["soc"]
    boards = board_ids(doc)
    if not boards:
        return 0
    # the ladder's record keys must exist for every board of this SoC
    ane = doc.get("ane", {})
    missing_keys = [k for k in LADDER_KEYS if not has_value(ane.get(k))]
    if missing_keys:
        failures.append(f"{path.name}: ladder keys missing from record: {', '.join(missing_keys)}")
    resolved = 0
    for board in boards:
        dts_path = dts / f"{soc}-{board}.dts"
        if not dts_path.exists():
            failures.append(f"{soc}-{board}: no {dts_path.name} in {dts}")
            continue
        got = board_from_dts(dts_path)
        if got != board:
            failures.append(f"{soc}-{board}: dts root compatible names {got!r}, expected apple,{board}")
            continue
        print(f"ok  {soc}-{board} -> {dts_path.name}")
        resolved += 1
    return resolved


def main(argv: list[str]) -> int:
    dts = DTS_DIR
    if not dts.is_dir():
        print(f"SKIP: no aurora dts checkout at {dts} (set AURORA_DTS_DIR); nothing to resolve against")
        return 0
    sweep_all = "--all" in argv
    wanted = {a for a in argv if not a.startswith("--")} or ({"t8122"} if not sweep_all else set())
    records = sorted((ROOT / "data/ane-soc").glob("t*.json"))
    if not records:
        print("SKIP: no SoC records found")
        return 0
    failures: list[str] = []
    resolved = 0
    for path in records:
        if wanted and path.stem not in wanted:
            continue
        resolved += check_record(path, dts, failures)
    for f in failures:
        print(f"FAIL {f}")
    print(f"{resolved} board(s) resolved, {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
