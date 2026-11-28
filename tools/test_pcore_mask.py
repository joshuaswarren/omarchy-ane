#!/usr/bin/env python3
"""Self-test for tools/pcore_mask.py: performance-core selection and masks.

Feeds p_core_cpus a fake sysfs tree (cpu[0-9]*/cpu_capacity files) and pins
the selection rules: the CPUs at the maximum capacity, None when every CPU
shares one capacity or a capacity file is unreadable. _target narrows the
mask to the performance cores the caller may already use, leaves an
E-core-only caller alone, and does nothing when the caller's mask already
equals the performance set. OMARCHY_ANE_NO_PIN=1 turns all of it off. The
command line prints the taskset list (4-7 or 4-5,7,9 style) or nothing.
"""
import importlib.util
import io
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("pcore_mask", REPO / "tools/pcore_mask.py")
pm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pm)


def fake_sysfs(tmp, capacities):
    for cpu, cap in capacities.items():
        d = tmp / f"cpu{cpu}"
        d.mkdir()
        (d / "cpu_capacity").write_text(str(cap))


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        fake_sysfs(root, {0: 400, 1: 400, 2: 400, 3: 400, 4: 800, 5: 800, 6: 800, 7: 800})
        check("p cores", pm.p_core_cpus(root), [4, 5, 6, 7])
        check("target narrows", pm._target(set(range(8)), root, {}), {4, 5, 6, 7})
        check("target subset already", pm._target({4, 5}, root, {}), None)
        check("target leaves e-only", pm._target({0, 1}, root, {}), None)
        check("target leaves exact", pm._target({4, 5, 6, 7}, root, {}), None)
        check("opt-out", pm._target(set(range(8)), root, {pm.OPT_OUT: "1"}), None)
        check("opt-out other value", pm._target(set(range(8)), root, {pm.OPT_OUT: "0"}), {4, 5, 6, 7})

        out = io.StringIO()
        with redirect_stdout(out):
            pm.main(root, {})
        check("cli list", out.getvalue().strip(), "4-7")
        out = io.StringIO()
        with redirect_stdout(out):
            pm.main(root, {pm.OPT_OUT: "1"})
        check("cli opt-out empty", out.getvalue(), "")

    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        fake_sysfs(root, {0: 800, 4: 800, 7: 800, 9: 800, 10: 400})
        out = io.StringIO()
        with redirect_stdout(out):
            pm.main(root, {})
        check("cli runs list", out.getvalue().strip(), "0,4,7,9")

    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        fake_sysfs(root, {0: 800, 1: 800})
        check("single capacity", pm.p_core_cpus(root), None)

    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        check("empty sysfs", pm.p_core_cpus(root), None)

    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "cpu0").mkdir()
        (root / "cpu0" / "cpu_capacity").write_text("junk")
        check("unreadable capacity", pm.p_core_cpus(root), None)

    # a pid that is gone gives 0, no exception
    check("pin_tasks dead pid", pm.pin_tasks(2 ** 22, Path("/nonexistent"), {}), 0)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_pcore_mask: {len(fails)} failed")
        return 1
    print("test_pcore_mask: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
