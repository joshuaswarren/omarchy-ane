#!/usr/bin/env python3
"""Sabotage ane_h15_overlay.c in distinct ways and require sim_overlay to FAIL for every one (exit 0 iff all are caught).

usage: sim/mutate_overlay.py        (from ane/h15; needs a C compiler)
Each mutant is compiled in a private temp copy; the real sources are never touched.
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

H15 = Path(__file__).resolve().parent.parent
MUTANTS = {
    "no-t8122-overlay": ('\t{ "t8122", "apple,t8122", "apple,t8122-ane", ane_h15_ovl_t8122_blob, sizeof(ane_h15_ovl_t8122_blob),\n\t  ane_h15_ovl_t8122_syms, ARRAY_SIZE(ane_h15_ovl_t8122_syms) },\n', ""),
    "wrong-blob-for-t8122": ('"apple,t8122-ane", ane_h15_ovl_t8122_blob, sizeof(ane_h15_ovl_t8122_blob)', '"apple,t8122-ane", ane_h15_ovl_t6031_blob, sizeof(ane_h15_ovl_t6031_blob)'),
    "no-phandle-assignment": ("targets[i]->phandle = next++;", "next++;"),
    "driver-before-overlay": ("\tif (apply_overlay)\n\t\tret = ane_h15_overlay_apply(optin);\n\tif (ret)\n\t\treturn ret;\n\tret = platform_driver_register(drv);",
                              "\tret = platform_driver_register(drv);\n\tif (apply_overlay && !ret)\n\t\tret = ane_h15_overlay_apply(optin);"),
    "skip-machine-check": ("!of_machine_is_compatible(o->board)", "false"),
    "skip-existing-node-check": ("\tif (np) {\n\t\tof_node_put(np);", "\tif (false) {\n\t\tof_node_put(np);"),
    "skip-target-check": ("if (!targets[i]) {", "if (false) {"),
    "skip-collision-check": ("strcmp(cur, o->syms[i].path)", "false"),
    "no-revert-on-overlay-failure": ("\t\t\tof_changeset_revert(&ane_ovl.cs);\n\t\t\tof_changeset_destroy(&ane_ovl.cs);\n\t\t\tane_ovl.symbols = false;\n\t\t} else {",
                                     "\t\t} else {"),
    "no-partial-overlay-remove": ("int cleanup = of_overlay_remove(&ane_ovl.id);", "int cleanup = 0;"),
    "no-overlay-remove-on-unload": ("if (ane_ovl.overlay) {", "if (false) {"),
    "no-symbols-revert-on-unload": ("\tif (ane_ovl.symbols) {\n\t\tof_changeset_revert(&ane_ovl.cs);\n\t\tof_changeset_destroy(&ane_ovl.cs);\n\t\tane_ovl.symbols = false;\n\t}\n}\n\nint ane_h15_register",
                                    "}\n\nint ane_h15_register"),
    "wrong-blob-for-t6030": ('"apple,t6030-ane", ane_h15_ovl_t6030_blob, sizeof(ane_h15_ovl_t6030_blob)', '"apple,t6030-ane", ane_h15_ovl_t6031_blob, sizeof(ane_h15_ovl_t6031_blob)'),
    "renumber-existing-phandles": ("\t\tif (targets[i]->phandle)\n\t\t\tcontinue;\n", ""),
    "apply-even-when-off": ("\tif (apply_overlay)\n\t\tret = ane_h15_overlay_apply(optin);", "\tret = ane_h15_overlay_apply(optin);"),
}


def run(mutated):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "sim").mkdir()
        shutil.copytree(H15 / "sim" / "ovl", root / "sim" / "ovl")
        shutil.copy(H15 / "sim" / "sim_overlay.c", root / "sim")
        for name in ("ane_h15_overlay.h", "ane_h15_overlay_data.h"):
            shutil.copy(H15 / name, root)
        (root / "ane_h15_overlay.c").write_text(mutated)
        build = subprocess.run(["cc", "-O0", "-std=gnu11", "-I", str(root / "sim" / "ovl"), "-o", str(root / "t"), str(root / "sim" / "sim_overlay.c")],
                               capture_output=True, text=True, check=False)
        if build.returncode:
            return "does not compile", build.stderr[-300:]
        return ("passes", "") if subprocess.run([str(root / "t")], capture_output=True, check=False).returncode == 0 else ("caught", "")


def main():
    source = (H15 / "ane_h15_overlay.c").read_text()
    bad = 0
    for name, (old, new) in MUTANTS.items():
        if source.count(old) != 1:
            print(f"  {name}: anchor found {source.count(old)} times, fix the mutant")
            bad += 1
            continue
        verdict, extra = run(source.replace(old, new))
        print(f"  {name}: {verdict} {extra}")
        bad += verdict != "caught"
    print("MUTATION:", "all caught" if not bad else f"{bad} not caught")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
