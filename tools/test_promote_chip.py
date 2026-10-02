#!/usr/bin/env python3
"""Offline checks for tools/promote_chip.py.

Per chip: a flip edits exactly the flip file set, a second apply changes
nothing, --check writes nothing, and the revert round-trips the tree
byte-identically (the one CHANGELOG direction-log line excepted). One flipped
tree must also pass the offline packaging suites (test_ane_dt, test_ane_m2).
No network and no module loads; needs dtc and fdtoverlay (test_ane_dt).
"""
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
repo = Path(__file__).resolve().parents[1]

spec = spec_from_loader("fetch", SourceFileLoader("fetch", str(repo / "packaging/omarchy-ane-firmware-fetch")))
fetch = module_from_spec(spec)
spec.loader.exec_module(fetch)

UNTESTED = ["t6000", "t6002", "t6020", "t6022", "t8112"]
DEFAULT_ON = ["t8103", "t6001", "t6021"]


def flip_files(chip):
    files = ["packaging/dt/overlays", f"packaging/dt/{chip}-ane.dts",
             "packaging/omarchy-ane-check", "packaging/omarchy-ane-dt",
             "README.md", "CHANGELOG.md"]
    if f"apple,{chip}" in fetch.FETCH:  # the install hook fetches its firmware
        files.append("packaging/omarchy-ane-firmware-fetch")
    return files


def make_tree():
    tmp = Path(tempfile.mkdtemp())
    archive = subprocess.run(["git", "-C", str(repo), "archive", "HEAD"],
                             check=True, capture_output=True).stdout
    subprocess.run(["tar", "-x", "-C", str(tmp)], input=archive, check=True)
    # the script is live even before it is committed
    shutil.copy(repo / "tools/promote_chip.py", tmp / "tools/promote_chip.py")
    return tmp


def tree(root):
    return {p.relative_to(root).as_posix(): p.read_bytes()
            for p in Path(root).rglob("*") if p.is_file() and ".git" not in p.parts}


def run(root, chip, to, mode):
    return subprocess.run([sys.executable, str(root / "tools/promote_chip.py"),
                           "--chip", chip, "--to", to, mode],
                          capture_output=True, text=True)


for chip in UNTESTED + DEFAULT_ON:
    base = make_tree()
    before = tree(base)
    to = "default-on" if chip in UNTESTED else "opt-in"
    back = "opt-in" if chip in UNTESTED else "default-on"

    p = run(base, chip, to, "--apply")
    assert p.returncode == 0, (chip, p.stderr)
    after = tree(base)
    changed = {k for k in after if before.get(k) != after[k]}
    assert changed == set(flip_files(chip)), (chip, sorted(changed))

    p = run(base, chip, to, "--apply")  # idempotent
    assert p.returncode == 0 and tree(base) == after, (chip, p.stdout, p.stderr)
    p = run(base, chip, to, "--check")  # writes nothing
    assert p.returncode == 0 and tree(base) == after, (chip, p.stderr)

    p = run(base, chip, back, "--apply")
    assert p.returncode == 0, (chip, p.stderr)
    rt = tree(base)
    residue = {k for k in rt if rt[k] != before.get(k)} | {k for k in before if k not in rt}
    assert residue <= {"CHANGELOG.md"}, (chip, sorted(residue))
    if "CHANGELOG.md" in residue:
        added = [l for l in rt["CHANGELOG.md"].decode().splitlines()
                 if l not in before["CHANGELOG.md"].decode().splitlines()]
        assert len(added) == 1 and f"{chip.upper()} (" in added[0], (chip, added)
    print(f"promote_chip test: {chip} {to} -> {back}: round trip ok")

# One flipped tree (t8112: the widest flip, it also touches DEFAULT_ON) must
# pass the offline packaging suites.
flipped = make_tree()
p = run(flipped, "t8112", "default-on", "--apply")
assert p.returncode == 0, p.stderr
for suite in ("test_ane_dt.py", "test_ane_m2.py"):
    q = subprocess.run([sys.executable, str(flipped / "tools" / suite)],
                       capture_output=True, text=True, cwd=flipped)
    assert q.returncode == 0, (suite, q.stdout[-2000:], q.stderr[-2000:])
    print(f"promote_chip test: {suite} green on the flipped tree")

print(f"promote_chip test: ok ({len(UNTESTED) + len(DEFAULT_ON)} chips)")
