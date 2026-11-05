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
        (
            "table-start-pointer-only",
            "address, ANE_H17_TABLE_BYTES))",
            "address, 1))",
            "neg-table-overrun",
        ),
        (
            "descriptor-publish-without-dma-barrier",
            "\tdma_wmb();",
            "\t/* missing DMA barrier */;",
            "pos-h17-table",
        ),
        (
            "registration-too-early",
            '"H17 TABLE_STOP before SCRATCH3 registration; buffers retained; no doorbell, IRQ ack or task\\n");',
            '"H17 TABLE_STOP before SCRATCH3 registration; buffers retained; no doorbell, IRQ ack or task\\n"); ane_h16_wr32(ane, s->scratch0 + 12, ANE_H16_WAKE_ACK);',
            "pos-h17-table",
        ),
        (
            "advertised-fw-padding-unmapped",
            "return chanreg && s == 1 ? ANE_H17_FW_BYTES - 0xc0000 : ane->segs[s].size;",
            "return ane->segs[s].size;",
            "pos-h17-table",
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
        (dst / "h17").mkdir()
        for name in ("ane_h17_soc.c", "ane_h17_soc.h", "ane_h17_fw.h",
                     "bringup_order.h", "chanreg.h"):
            shutil.copyfile(root.parent / "h17" / name, dst / "h17" / name)
        # ane_h17_soc.h includes "../h16/ane_h16.h"
        (dst / "h16").mkdir()
        shutil.copyfile(root / "ane_h16.h", dst / "h16" / "ane_h16.h")
        shutil.copytree(root / "sim/inc", dst / "sim/inc")
        for name, before, after, scenario in cases:
            if original.count(before) != 1:
                raise RuntimeError(f"{name}: mutation anchor is not unique")
            (dst / "ane_h16_main.c").write_text(original.replace(before, after))
            binary = dst / name
            subprocess.run([os.environ.get("CC", "cc"), "-g", "-O1",
                            "-I", str(dst / "sim"), "-I", str(dst / "sim/inc"),
                            "-o", str(binary), str(dst / "sim/sim_harness.c")],
                           check=True, timeout=60)
            result = subprocess.run([str(binary), scenario], capture_output=True,
                                    text=True, timeout=20)
            if result.returncode != 1 or "FAIL" not in result.stdout:
                raise RuntimeError(f"{name}: regression not caught\n{result.stdout}"
                                   f"\n{result.stderr}")
            print(f"PASS: {name} regression caught by {scenario}")


if __name__ == "__main__":
    main()
