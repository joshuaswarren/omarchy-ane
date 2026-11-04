#!/usr/bin/env python3
"""Require the simulated real driver to catch ADT and DMA regressions."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    original = (root / "ane_h16_main.c").read_text()
    cases = [
        ("arm-io-length", 'size >= 7 &&', 'size > 7 &&', "pos-dt"),
        (
            "segment-u64-size",
            "le32_to_cpup(data + i * stride + 24) : le64_to_cpu(v[2])",
            "le64_to_cpu(v[3]) : le64_to_cpu(v[2])",
            "pos-boot",
        ),
        (
            "dma-handle-is-not-physical",
            "pa = iommu_iova_to_phys(ane->domain, source);",
            "pa = source;",
            "pos-boot",
        ),
        (
            "released-cpu-power-drop",
            "ane->boot_error = ret;",
            "pm_runtime_put_sync(&pdev->dev); ane->boot_error = ret;",
            "neg-mbox-silent",
        ),
    ]
    # Only copies of source are mutated. Never load a kernel module.
    with tempfile.TemporaryDirectory(prefix="h17-adt-mutations-",
                                     dir="/var/tmp/opencode") as tmp:
        dst = Path(tmp)
        (dst / "sim").mkdir()
        for name in ("ane_h16.h", "ane_h16_soc.c"):
            shutil.copyfile(root / name, dst / name)
        for name in ("shim.h", "sim_harness.c"):
            shutil.copyfile(root / "sim" / name, dst / "sim" / name)
        shutil.copytree(root / "sim/inc", dst / "sim/inc")
        for name, before, after, scenario in cases:
            if original.count(before) != 1:
                raise RuntimeError(f"{name}: mutation anchor is not unique")
            (dst / "ane_h16_main.c").write_text(original.replace(before, after))
            binary = dst / name
            subprocess.run([os.environ.get("CC", "cc"), "-g", "-O1",
                            "-I", str(dst / "sim"), "-I", str(dst / "sim/inc"),
                            "-o", str(binary), str(dst / "sim/sim_harness.c")],
                           check=True)
            result = subprocess.run([str(binary), scenario], capture_output=True,
                                    text=True)
            if result.returncode != 1 or "FAIL" not in result.stdout:
                raise RuntimeError(f"{name}: regression not caught\n{result.stdout}"
                                   f"\n{result.stderr}")
            print(f"PASS: {name} regression caught by {scenario}")


if __name__ == "__main__":
    main()
