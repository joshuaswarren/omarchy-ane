#!/usr/bin/env python3
"""Self-test for tools/gen_coverage_table.py: the README data-only table.

Stages a fake repo root (tmp/tools/gen_coverage_table.py plus data/ane-soc,
packaging/dt and a README with the marker block) and pins: the row format
(soc upper-cased, generation.v or 'unknown', board count, overlay yes/no),
the no-data fallback line, --write rewriting only the marked block,
--check exiting 1 on a stale block and on a missing marker.
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/gen_coverage_table.py"

BEGIN = "<!-- BEGIN DATA-ONLY TABLE -->"
END = "<!-- END DATA-ONLY TABLE -->"


def stage(tmp):
    root = Path(tmp) / "repo"
    (root / "tools").mkdir(parents=True)
    (root / "data/ane-soc").mkdir(parents=True)
    (root / "packaging/dt").mkdir(parents=True)
    shutil.copy(TOOL, root / "tools/gen_coverage_table.py")
    return root


def soc(root, name, gen, boards, overlay=True):
    (root / "data/ane-soc" / f"{name}.json").write_text(json.dumps(
        {"soc": name, "generation": {"v": gen}, "boards": [{"b": i} for i in range(boards)]}))
    if overlay:
        (root / "packaging/dt" / f"{name}-ane-dataonly.dts").write_text("// dts")


def readme(root, body="stale\n"):
    (root / "README.md").write_text(f"top\n{BEGIN}\n{body}{END}\nbottom\n")


def run(root, *args):
    return subprocess.run([sys.executable, str(root / "tools/gen_coverage_table.py"), *args],
                          capture_output=True, text=True)


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as d:
        root = stage(d)
        soc(root, "t6030", "H15J", 2)
        soc(root, "t8112", "H13G", 11, overlay=False)
        soc(root, "t9999", None, 0)

        result = run(root)
        check("print rc", result.returncode, 0)
        lines = result.stdout.splitlines()
        check("header", lines[:2], ["| SoC | Internal | Boards | Overlay |", "| --- | --- | --- | --- |"])
        check("row t6030", "| T6030 | H15J | 2 | yes |" in lines, True)
        check("row t8112 no overlay", "| T8112 | H13G | 11 | no |" in lines, True)
        check("unknown gen", "| T9999 | unknown | 0 | yes |" in lines, True)
        check("sorted by soc", [l.split("|")[1].strip() for l in lines[2:]], ["T6030", "T8112", "T9999"])

        # no data files at all
        empty = stage(Path(d) / "e")
        check("no data line", run(empty).stdout, "No chip has a data file yet.\n")

        # --write rewrites only the marked block, then --check passes
        readme(root)
        check("write rc", run(root, "--write").returncode, 0)
        text = (root / "README.md").read_text()
        check("write keeps head", text.startswith(f"top\n{BEGIN}\n"), True)
        check("write keeps tail", text.endswith(f"{END}\nbottom\n"), True)
        check("write put rows", "| T6030 | H15J | 2 | yes |" in text, True)
        check("check fresh rc", run(root, "--check").returncode, 0)

        # --check fails on a stale block and on a missing marker
        readme(root)
        check("check stale rc", run(root, "--check").returncode, 1)
        (root / "README.md").write_text("no markers here\n")
        check("check no marker rc", run(root, "--check").returncode, 1)

        # --write on a README without markers fails too
        check("write no marker rc", run(root, "--write").returncode, 1)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_gen_coverage_table: {len(fails)} failed")
        return 1
    print("test_gen_coverage_table: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
