"""Validate run-time overlays derived from the per-SoC boot-time sources."""
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
    def test_every_blob_is_8_byte_aligned(self):
        """of_overlay_fdt_apply() -> fdt_check_header() refuses a blob not 8-byte aligned in memory
        (the t6030 boot blob did on the M3 Pro). The sim cannot see it on x86-64, so guard the
        declarations: every blob array in the generated header carries __aligned(8)."""
        import re
        text = (HERE / "ane_h15_overlay_data.h").read_text()
        decls = re.findall(r"^static const u8 (\w+_blob)\[\](.*?)= \{", text, re.M)
        self.assertEqual(len(decls), 6)
        for name, attrs in decls:
            self.assertIn("__aligned(8)", attrs, name)

    def test_t6030_boot_iommus_three_streams_and_runtime_drops_iommus(self):
        """The t6030 boot variant names dart0/dart1/dart2 stream 0 (the contract since PR 386);
        the runtime variant keeps the PR 382 contract: no iommus at all, not a leftover
        specifier naming a disabled provider. Guards both shapes after the reverted sid-16
        experiment (99a0e124): the strip regex must keep matching the bare three-specifier
        line."""
        if not HAVE_DTC or not shutil.which("fdtget"):
            self.skipTest("no DT tools")
        with tempfile.TemporaryDirectory() as tmp:
            boot = Path(tmp) / "t6030-boot.dtbo"
            boot.write_bytes(gen.compile_blob((HERE / "t6030-ane-dart-experimental.dts").read_text()))
            darts = {}
            for addr, want in (("309800000", "okay"), ("309810000", "okay"), ("309820000", "okay"), ("309804000", "disabled")):
                node = "/fragment@3/__overlay__/iommu@" + addr
                status = subprocess.check_output(["fdtget", str(boot), node, "status"], text=True).strip()
                self.assertEqual(status, want, addr)
                darts[addr] = subprocess.check_output(["fdtget", "-t", "x", str(boot), node, "phandle"], text=True).split()
            self.assertEqual(
                subprocess.check_output(["fdtget", "-t", "x", str(boot), "/fragment@3/__overlay__/ane@308000000", "iommus"], text=True).split(),
                [darts["309800000"][0], "0", darts["309810000"][0], "0", darts["309820000"][0], "0"],
                "stream 0 of the three enabled instances",
            )
            runtime = Path(tmp) / "t6030-runtime.dtbo"
            runtime.write_bytes(gen.compile_blob(gen.derive("t6030")[0]))
            props = subprocess.check_output(
                ["fdtget", "-p", str(runtime), "/fragment@3/__overlay__/ane@308000000"],
                text=True,
            ).splitlines()
            self.assertNotIn("iommus", props)

    def test_t8122_power_graph_and_dart_client(self):
        if not HAVE_DTC or not shutil.which("fdtget"):
            self.skipTest("no DT tools")
        with tempfile.TemporaryDirectory() as tmp:
            blob = Path(tmp) / "t8122.dtbo"
            blob.write_bytes(gen.compile_blob((HERE / "t8122-ane-dart-experimental.dts").read_text()))

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
            darts = {}
            for addr, want in (("311800000", "okay"), ("311810000", "okay"), ("311820000", "okay"), ("311804000", "disabled")):
                node = "/fragment@3/__overlay__/iommu@" + addr
                status = subprocess.check_output(["fdtget", str(blob), node, "status"], text=True).strip()
                self.assertEqual(status, want, addr)
                darts[addr] = prop(node, "phandle")
            self.assertEqual(
                prop("/fragment@3/__overlay__/ane@310000000", "iommus"),
                [darts["311800000"][0], "0", darts["311810000"][0], "0", darts["311820000"][0], "0"],
                "stream 0 of the three enabled instances, as on the T6030",
            )

    def test_t8122_boot_overlay_keeps_the_darts_disabled(self):
        """Enabled DART nodes at boot hung a T6030: the file a person compiles as a boot overlay must not enable them."""
        if not HAVE_DTC or not shutil.which("fdtget"):
            self.skipTest("no DT tools")
        with tempfile.TemporaryDirectory() as tmp:
            blob = Path(tmp) / "t8122-boot.dtbo"
            blob.write_bytes(gen.compile_blob((HERE / "t8122-ane-experimental.dts").read_text()))
            for addr in ("311800000", "311810000", "311820000", "311804000"):
                status = subprocess.check_output(
                    ["fdtget", str(blob), "/fragment@3/__overlay__/iommu@" + addr, "status"],
                    text=True,
                ).strip()
                self.assertEqual(status, "disabled", addr)
            props = subprocess.check_output(
                ["fdtget", "-p", str(blob), "/fragment@3/__overlay__/ane@310000000"],
                text=True,
            ).splitlines()
            self.assertNotIn("iommus", props)

    def test_only_the_empty_label_fragments_are_removed(self):
        """The runtime derivation is the boot-time source with exactly two edits: the empty label-minting
        fragments removed (pure subtraction) and the three DART instances flipped to disabled (PR 382:
        the live apple-dart driver must not probe enabled ANE DARTs at overlay-apply time)."""
        disabled = '\t\t\t\tstatus = "disabled";'
        for soc, info in gen.SOCS.items():
            with self.subTest(soc=soc):
                source = (HERE / info["src"]).read_text().splitlines()
                derived, syms = gen.derive(soc)
                diff = list(difflib.unified_diff(source, derived.splitlines(), lineterm="", n=0))
                removed = [line for line in diff if line.startswith("-") and not line.startswith("---")]
                added = [line for line in diff if line.startswith("+") and not line.startswith("+++")]
                self.assertEqual(added, ["+" + disabled] * 3, "the only additions are the three DART disables")
                self.assertEqual(removed.count('-\t\t\t\tstatus = "okay";'), 3, "the disables replace the three okay lines")
                body = "\n".join(removed)
                self.assertEqual(body.count("__overlay__"), len(syms))
                self.assertEqual(body.count("target-path"), len(syms))
                for label, path in syms:
                    self.assertIn(f'target-path = "{path}"', body)
                    self.assertIn(f"{label}: __overlay__", body)
                self.assertNotIn("compatible", body)
                self.assertNotIn("status = \"disabled\"", body)
                boot, _ = gen.derive(soc, boot=True)
                self.assertEqual([line for line in difflib.unified_diff(source, boot.splitlines(), lineterm="", n=0)
                                  if line.startswith("+") and not line.startswith("+++")],
                                 [], "the boot derivation is pure subtraction")

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

    def test_t6030_ane_node_claims_the_five_h15_domains_and_the_vote(self):
        """The ane node carries the five h15 ps words plus the PMP report vote
        entry, listed last. The driver finds the vote by its compatible, not by
        position, and drops it in runtime_suspend before the suppliers are put,
        so the position is only the overlay's convention. The
        +0x4e0..+0x4f8 words stay unclaimed - they are USB-C PHY domains."""
        if not HAVE_DTC or not shutil.which("fdtget"):
            self.skipTest("no DT tools")
        for boot in (True, False):
            with self.subTest(boot=boot):
                derived, syms = gen.derive("t6030", boot=boot)
                names = [l for l, _ in syms]
                self.assertEqual(names, ["aic", "ps_ane_sys", "pmp_report_ane"],
                                 "aic, ps_ane_sys and pmp_report_ane label fragments")
                with tempfile.TemporaryDirectory() as tmp:
                    blob = Path(tmp) / "o.dtbo"
                    blob.write_bytes(gen.compile_blob(derived))
                    pd = subprocess.check_output(
                        ["fdtget", "-t", "x", str(blob), "/fragment@3/__overlay__/ane@308000000", "power-domains"],
                        text=True).split()
                self.assertEqual(len(pd), 6, f"five h15 domains plus the vote entry, got {len(pd)}")
                self.assertTrue(all(int(v, 16) != 0 for v in pd), "no zero phandle in the list")
                self.assertEqual(int(pd[-1], 16), 0xffffffff,
                                 "the last entry is the unresolved external vote reference")

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
                fixups_start = dump.index("__fixups__")
                local = dump.find("__local_fixups__")
                # t6031 mints all five ANE power-controller labels too, so with the iommus dropped it has
                # no overlay-internal phandle reference left and dtc emits no __local_fixups__ at all.
                fixups = dump[fixups_start:local] if local != -1 else dump[fixups_start:]
                for label in info["labels"]:
                    self.assertIn(f"{label} =", fixups)
                if soc == "t6030":
                    self.assertNotIn("ps_ane_mpm =", fixups, "t6030 defines its other power controllers itself")


if __name__ == "__main__":
    unittest.main()
