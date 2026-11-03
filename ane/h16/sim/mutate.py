#!/usr/bin/env python3
"""Mutation controls for the h16 probe and the run-time overlay.

Each mutant changes one line in a COPY of the sources, rebuilds the sims and requires at least one scenario to fail.
A mutant that still passes means the sims do not cover that line. A mutant that does not compile is an error in this
script, not a kill.
  make sim-mutation
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
SCENARIOS = [
    "pos-dt", "pos-dt-noadt", "pos-defaults", "pos-status", "pos-status-scratch", "pos-pds", "pos-one-pd", "pos-boot",
    "pos-boot-hello", "neg-ps-stuck", "neg-mbox-silent", "neg-bad-pin", "neg-foreign-preload", "neg-dart-map",
    "neg-unknown-stage", "neg-no-confirm", "neg-pd-fail", "neg-pd-link-fail",
]

# (name, file, old text, new text)
MUTANTS = [
    ("confirm_read ignored", "ane_h16_main.c", "if (!confirm_read) {", "if (0) {"),
    ("SCRATCH always read", "ane_h16_main.c", "if (!read_scratch)\n\t\treturn 0;", "if (0)\n\t\treturn 0;"),
    ("default stage is status", "ane_h16_main.c", 'static char *stage = "dt";', 'static char *stage = "status";'),
    ("attach loop one short", "ane_h16_main.c", "for (i = 0; i < count; i++) {\n\t\tstruct device *pd", "for (i = 0; i < count - 1; i++) {\n\t\tstruct device *pd"),
    ("a single domain is attached by the module", "ane_h16_main.c", "if (count <= 1)\n\t\treturn 0;", "if (count < 1)\n\t\treturn 0;"),
    ("no detach after a failed link", "ane_h16_main.c", 'dev_err(dev, "link power-domain %d\\n", i);\n\t\t\tane_h16_detach_pds(ane);', 'dev_err(dev, "link power-domain %d\\n", i);'),
    ("no detach after a failed attach", "ane_h16_main.c", 'dev_err(dev, "attach power-domain %d: %d\\n", i, err);\n\t\t\tane_h16_detach_pds(ane);', 'dev_err(dev, "attach power-domain %d: %d\\n", i, err);'),
    ("remove leaves the domains attached", "ane_h16_main.c", "ane_h16_unmap(ane);\n\tane_h16_detach_pds(ane);\n}", "ane_h16_unmap(ane);\n}"),
    ("remove leaves the windows mapped", "ane_h16_main.c", "pm_runtime_disable(&pdev->dev);\n\tane_h16_unmap(ane);\n\tane_h16_detach_pds(ane);\n}", "pm_runtime_disable(&pdev->dev);\n\tane_h16_detach_pds(ane);\n}"),
    ("overlay: status never written", "ane_h16_overlay.c", "if (!ret && !of_device_is_available(ane))", "if (0)"),
    ("overlay: machine check removed", "ane_h16_overlay.c", 'if (!of_machine_is_compatible("apple,t6040")) {', "if (0) {"),
    ("overlay: parent availability check removed", "ane_h16_overlay.c", "if (!ok) {", "if (0) {"),
    ("overlay: wrong ane-type", "ane_h16_overlay.c", '"apple,ane-type", 0x110', '"apple,ane-type", 0x100'),
    ("overlay: domain order reversed", "ane_h16_overlay.c", "pd_val[i] = cpu_to_be32(ps[i]->phandle);", "pd_val[N_PS - 1 - i] = cpu_to_be32(ps[i]->phandle);"),
    ("overlay: iommus always removed", "ane_h16_overlay.c", "if (!ret && iommus && ane_h16_darts_unavailable())", "if (!ret && iommus)"),
    ("overlay: unload does not revert", "ane_h16_overlay.c", "of_changeset_revert(&ane_ovl.cs);\n\tof_changeset_destroy", "of_changeset_destroy"),
    ("overlay: optin check removed", "ane_h16_overlay.c", 'if (!optin || strcmp(optin, "t6040")) {', "if (0) {"),
    ("overlay: wrong pmgr window size", "ane_h16_overlay.c", "cpu_to_be32(0x18000)", "cpu_to_be32(0x4000)"),
    ("overlay: complete node is applied again", "ane_h16_overlay.c", 'if (!ret && of_device_is_available(ane) && of_find_property(ane, "apple,ane-type", NULL)) {', "if (0) {"),
    ("overlay: phandles are not minted", "ane_h16_overlay.c", "if (ps[i]->phandle)\n\t\t\tcontinue;", "continue;"),
    ("overlay: no revert after a registration failure", "ane_h16_overlay.c", "if (ret)\n\t\tane_h16_overlay_remove();", "if (ret)\n\t\t;"),
]


def build_and_run(work):
    """Return (compiled, failed): did both sims compile, and did anything fail."""
    cc = ["cc", "-g", "-O1", "-Wno-unused-parameter", "-I", "sim", "-I", "sim/inc", "-o"]
    r = subprocess.run(cc + ["sim/sim_h16", "sim/sim_harness.c"], cwd=work, capture_output=True, text=True, check=False)
    if r.returncode:
        return False, r.stderr[-300:]
    r = subprocess.run(["cc", "-g", "-O1", "-Wno-unused-parameter", "-I", "sim/ovl", "-o", "sim/sim_overlay", "sim/sim_overlay.c"],
                       cwd=work, capture_output=True, text=True, check=False)
    if r.returncode:
        return False, r.stderr[-300:]
    for sc in SCENARIOS:
        if subprocess.run(["./sim/sim_h16", sc], cwd=work, capture_output=True, check=False).returncode:
            return True, sc
    if subprocess.run(["./sim/sim_overlay"], cwd=work, capture_output=True, check=False).returncode:
        return True, "overlay sim"
    return True, None


def main():
    survived = 0
    for name, fname, old, new in MUTANTS:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            shutil.copytree(ROOT, work, dirs_exist_ok=True, ignore=shutil.ignore_patterns("sim_h16*", "sim_overlay", "*.o", "*.ko"))
            path = work / fname
            text = path.read_text()
            if text.count(old) != 1:
                print(f"SCRIPT-ERROR {name}: pattern found {text.count(old)} times in {fname}")
                return 2
            path.write_text(text.replace(old, new))
            compiled, detail = build_and_run(work)
            if not compiled:
                print(f"SCRIPT-ERROR {name}: the mutant does not compile: {detail}")
                return 2
            if detail:
                print(f"  killed   {name}  (by {detail})")
            else:
                print(f"  SURVIVED {name}")
                survived += 1
    print("MUTATION: all caught" if not survived else f"MUTATION: {survived} survived")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
