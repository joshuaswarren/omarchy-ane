"""The run-time overlays are derived from the boot-time ones that were booted on the owners' Macs. Pin exactly how they differ."""
import difflib
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("gen", HERE / "gen-runtime-overlay.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

HAVE_DTC = shutil.which("dtc") is not None and shutil.which("fdtdump") is not None


class GenTests(unittest.TestCase):
    def test_t8122_power_graph_and_disabled_darts(self):
        if not HAVE_DTC or not shutil.which("fdtget"):
            self.skipTest("no DT tools")
        with tempfile.TemporaryDirectory() as tmp:
            blob = Path(tmp) / "t8122.dtbo"
            blob.write_bytes(gen.compile_blob((HERE / "t8122-ane-experimental.dts").read_text()))

            def prop(node, name):
                return subprocess.check_output(
                    ["fdtget", "-t", "x", str(blob), node, name], text=True
                ).split()

            power = "/fragment@2/__overlay__/"
            cpu = prop(power + "power-controller@c008", "phandle")
            td = prop(power + "power-controller@c010", "phandle")
            self.assertEqual(
                set(prop(power + "power-controller@c018", "power-domains")),
                set(td + cpu),
                "T8122 ADT: ANE_BASE requires both ANE_TD and ANE_CPU",
            )
            for addr in ("311800000", "311810000", "311820000", "311804000"):
                status = subprocess.check_output(
                    ["fdtget", str(blob), "/fragment@3/__overlay__/iommu@" + addr, "status"],
                    text=True,
                ).strip()
                self.assertEqual(status, "disabled")
            props = subprocess.check_output(
                ["fdtget", "-p", str(blob), "/fragment@3/__overlay__/ane@310000000"],
                text=True,
            ).splitlines()
            self.assertNotIn("iommus", props)

    def test_only_the_empty_label_fragments_are_removed(self):
        for soc, info in gen.SOCS.items():
            with self.subTest(soc=soc):
                source = (HERE / info["src"]).read_text().splitlines()
                derived, syms = gen.derive(soc)
                removed = [line for line in difflib.unified_diff(source, derived.splitlines(), lineterm="", n=0) if line.startswith("-") and not line.startswith("---")]
                added = [line for line in difflib.unified_diff(source, derived.splitlines(), lineterm="", n=0) if line.startswith("+") and not line.startswith("+++")]
                self.assertEqual(added, [], "the derived overlay adds nothing")
                body = "\n".join(removed)
                self.assertEqual(body.count("__overlay__"), len(syms))
                self.assertEqual(body.count("target-path"), len(syms))
                for label, path in syms:
                    self.assertIn(f'target-path = "{path}"', body)
                    self.assertIn(f"{label}: __overlay__", body)
                self.assertNotIn("compatible", body)
                self.assertNotIn("status", body)

    def test_the_labels_are_exactly_the_ones_the_overlay_refers_to_but_does_not_define(self):
        for soc in gen.SOCS:
            with self.subTest(soc=soc):
                derived, syms = gen.derive(soc)
                for label, _ in syms:
                    self.assertIn(f"&{label}", derived)
                    self.assertNotIn(f"{label}:", derived)

    def test_a_changed_overlay_source_is_refused_not_silently_derived(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "t6031-ane-dart-experimental.dts"
            src.write_text((HERE / "t6031-ane-dart-experimental.dts").read_text().replace("ps_ane_td: __overlay__", "ps_ane_xx: __overlay__"))
            old = gen.HERE
            gen.HERE = Path(tmp)
            try:
                with self.assertRaises(SystemExit):
                    gen.derive("t6031")
            finally:
                gen.HERE = old

    def test_the_committed_header_is_what_the_generator_writes(self):
        if not HAVE_DTC:
            self.skipTest("no dtc")
        self.assertEqual(subprocess.run(["python3", str(HERE / "gen-runtime-overlay.py"), "--check"], check=False).returncode, 0)

    def test_the_compiled_overlay_leaves_the_labels_external(self):
        if not HAVE_DTC:
            self.skipTest("no dtc")
        for soc, info in gen.SOCS.items():
            with self.subTest(soc=soc):
                derived, _ = gen.derive(soc)
                with tempfile.TemporaryDirectory() as tmp:
                    blob = Path(tmp) / "o.dtbo"
                    blob.write_bytes(gen.compile_blob(derived))
                    dump = subprocess.run(["fdtdump", str(blob)], capture_output=True, text=True, check=False).stdout
                fixups = dump[dump.index("__fixups__"):dump.index("__local_fixups__")]
                for label in info["labels"]:
                    self.assertIn(f"{label} =", fixups)
                if soc == "t6030":
                    self.assertNotIn("ps_ane_mpm =", fixups, "t6030 defines its other power controllers itself")


if __name__ == "__main__":
    unittest.main()
