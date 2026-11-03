"""Offline tests for trace_ane_h15.py with stub m1n1 modules (no hypervisor, no hardware)."""
import contextlib
import importlib.util
import io
import os
import sys
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENGINE = 0x308000000
ENGINE_SIZE = 0x2000000
PS_ANE_SYS = 0x28E080438
PS_ANE_CPU = 0x28E080440
PS_GATED = 0x28E080500
DART_BASE = 0x309800000
DART_OFFSETS = [0, 0x10000, 0x20000, 0x4000]


class TraceMode:
    OFF = 0
    ASYNC = 1
    SYNC = 2


class StubTracer:
    def __init__(self, hv, verbose=False, ident=None):
        self.hv = hv
        self.ident = ident

    def trace(self, start, size, mode, **kwargs):
        self.hv.zones.append((start, size, mode))

    def log(self, msg, show_cpu=True):
        self.hv.lines.append(msg)

    def start(self):
        pass


def irange(start, size):
    return range(start, start + size)



class Forbidden:
    """Any use of the proxy handles fails the test: the script must stay passive."""

    def __getattr__(self, name):
        raise AssertionError(f"the script touched hv.u/hv.p ({name})")


def load_script():
    # No m1n1.trace.dart module exists in the stubs: any import of m1n1's DARTTracer would fail the load.
    for name, attrs in (("m1n1", {}), ("m1n1.hv", {"TraceMode": TraceMode}), ("m1n1.trace", {"Tracer": StubTracer}),
                        ("m1n1.utils", {"irange": irange})):
        module = types.ModuleType(name)
        module.__dict__.update(attrs)
        sys.modules[name] = module
    sys.modules["m1n1"].__path__ = []
    spec = importlib.util.spec_from_file_location("trace_ane_h15_under_test", HERE / "trace_ane_h15.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


script = load_script()


class Flags:
    def __init__(self, write, width):
        self.WRITE = write
        self.WIDTH = width


class Evt:
    def __init__(self, addr, data, write, width=2):
        self.addr, self.data, self.flags = addr, data, Flags(write, width)


class Obj:
    def __init__(self, **kw):
        self.__dict__.update(kw)


class StubADT(dict):
    def pmgr_dev_get_id(self, dev):
        return dev.id

    def pmgr_dev_get_addr(self, dev):
        return dev.addr


def make_hv(chip=0x6030, dart=True):
    hv = Obj(zones=[], releases=[], lines=[], u=Forbidden(), p=Forbidden(), tracer_caches={})
    hv.add_tracer = lambda zone, ident, mode, read=None, write=None, **kw: hv.releases.append((zone, ident, mode))
    ane = Obj(get_reg=lambda i: (ENGINE, ENGINE_SIZE), power_gates=[7], ane_type=0xC0)
    devices = [
        Obj(name="ANE_SYS", flags=Obj(no_ps=False), id=1, addr=PS_ANE_SYS),
        Obj(name="ANE_CPU", flags=Obj(no_ps=False), id=2, addr=PS_ANE_CPU),
        Obj(name="GATED_BY_NODE", flags=Obj(no_ps=False), id=7, addr=PS_GATED),
        Obj(name="ANE_NO_PS", flags=Obj(no_ps=True), id=3, addr=0x28E080600),
        Obj(name="GPU", flags=Obj(no_ps=False), id=9, addr=0x28E080700),
    ]
    adt = StubADT({"/arm-io/ane": ane, "/arm-io/pmgr": Obj(devices=devices), "/chosen": Obj(chip_id=chip)})
    if dart:
        adt["/arm-io/dart-ane"] = Obj(reg=[0, 1, 2, 3], get_reg=lambda i: (DART_BASE + DART_OFFSETS[i], 0x4000))
    hv.adt = adt
    return hv


def lines_of(tracer):
    tracer.flush()
    return tracer.hv.lines


def new_tracer(cap=1000):
    hv = make_hv()
    labeler = script.Labeler(ENGINE, ENGINE_SIZE, {PS_ANE_SYS: "ANE_SYS"})
    return script.ANEH15Tracer(hv, labeler, cap, False)


class TraceTests(unittest.TestCase):
    def test_start_sequence_labels_and_notes(self):
        t = new_tracer()
        for evt in (Evt(ENGINE + 0x1050000, 0x0000010000000001, True, 3),
                    Evt(ENGINE + 0x1400044, 0x10, True),
                    Evt(ENGINE + 0x184006C, 0x08042006, False),
                    Evt(ENGINE + 0x184006C, 0xF7FBDFF9, True),
                    Evt(PS_ANE_SYS, 0xF, True)):
            t.evt_rw(evt)
        out = lines_of(t)
        self.assertEqual(len(out), 5)
        self.assertIn("W8 RVBAR +0x1050000 = 0x10000000001 <iova 0x10000000000 valid 1>", out[0])
        self.assertIn("W4 CPU_CONTROL +0x1400044 = 0x10 <RUN>", out[1])
        self.assertIn("R4 SCRATCH7 +0x184006c = 0x8042006 <firmware ready word>", out[2])
        self.assertIn("W4 SCRATCH7 +0x184006c = 0xf7fbdff9 <wake word>", out[3])
        self.assertIn(f"W4 PS:ANE_SYS {PS_ANE_SYS:#x} = 0xf", out[4])

    def test_cpu_control_zero_and_unknown_offsets(self):
        t = new_tracer()
        t.evt_rw(Evt(ENGINE + 0x1400044, 0, True))
        t.evt_rw(Evt(ENGINE + 0x1900008, 5, False))
        out = lines_of(t)
        self.assertIn("CPU_CONTROL +0x1400044 = 0x0 <release: 0>", out[0])
        self.assertIn("R4 ENG +0x1900008 = 0x5", out[1])
        self.assertNotIn("<", out[1])

    def test_unrelated_addresses_are_ignored(self):
        t = new_tracer()
        t.evt_rw(Evt(0x28E080900, 1, True))
        t.evt_rw(Evt(ENGINE + ENGINE_SIZE, 1, True))
        t.evt_rw(Evt(ENGINE - 4, 1, True))
        self.assertEqual(lines_of(t), [])

    def test_polling_collapses_and_a_change_breaks_the_run(self):
        t = new_tracer()
        for _ in range(5):
            t.evt_rw(Evt(ENGINE + 0x1400048, 0x2A, False))
        t.evt_rw(Evt(ENGINE + 0x1400048, 0x28, False))
        out = lines_of(t)
        self.assertEqual(len(out), 2)
        self.assertTrue(out[0].endswith("= 0x2a x5"), out[0])
        self.assertIn("= 0x28", out[1])
        self.assertNotIn(" x", out[1])

    def test_a_write_does_not_collapse_with_an_identical_read(self):
        t = new_tracer()
        t.evt_rw(Evt(ENGINE + 0x1400044, 0x10, False))
        t.evt_rw(Evt(ENGINE + 0x1400044, 0x10, True))
        self.assertEqual(len(lines_of(t)), 2)

    def test_ready_and_wake_notes_need_the_right_direction(self):
        t = new_tracer()
        t.evt_rw(Evt(ENGINE + 0x184006C, 0x08042006, True))
        t.evt_rw(Evt(ENGINE + 0x184006C, 0xF7FBDFF9, False))
        out = lines_of(t)
        self.assertEqual(len(out), 2)
        self.assertNotIn("<", out[0])
        self.assertNotIn("<", out[1])

    def test_event_cap_logs_once_and_stops(self):
        t = new_tracer(cap=2)
        for value in range(6):
            t.evt_rw(Evt(ENGINE + 0x1840050, value, True))
        out = lines_of(t)
        self.assertEqual(len([line for line in out if "event cap 2 reached" in line]), 1)
        self.assertEqual(len([line for line in out if "SCRATCH0" in line]), 2)

    def test_the_cap_releases_every_hook_to_the_hardware_mapping_once(self):
        t = new_tracer(cap=2)
        plan = [(ENGINE + 0x1400000, 0x10000, "a"), (ENGINE + 0x1840000, 0x8000, "b"), (0x28E080000, 0x4000, "c")]
        t.watch(plan)
        t.evt_rw(Evt(ENGINE + 0x1840050, 1, True))
        t.evt_rw(Evt(ENGINE + 0x1840050, 2, True))
        self.assertEqual(t.hv.releases, [], "nothing is released before the cap")
        for value in range(3, 8):
            t.evt_rw(Evt(ENGINE + 0x1840050, value, True))
        released = t.hv.releases
        self.assertEqual([(list(z)[0], len(z), ident, mode) for z, ident, mode in released],
                         [(start, size, "ANEH15", TraceMode.OFF) for start, size, _ in plan])
        self.assertTrue(any("hooks released" in line for line in t.hv.lines))

    def test_no_release_when_the_cap_is_never_reached(self):
        t = new_tracer(cap=100)
        t.watch([(ENGINE + 0x1840000, 0x8000, "b")])
        for value in range(5):
            t.evt_rw(Evt(ENGINE + 0x1840050, value, True))
        self.assertEqual(t.hv.releases, [])


class InstallTests(unittest.TestCase):
    def run_install(self, **env):
        hv = make_hv(**env.pop("hv_args", {}))
        old = {k: os.environ.get(k) for k in ("ANE_TRACE_FULL", "ANE_TRACE_MAX", "ANE_TRACE_PLAN_ONLY")}
        os.environ.update(env)
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                tracer = script.install(hv)
        finally:
            for key, value in old.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
        return hv, tracer, buf.getvalue()

    def test_default_install_traces_named_windows_dart_and_ps_pages_only(self):
        hv, tracer, out = self.run_install()
        starts = sorted(z[0] for z in hv.zones)
        expected = ([ENGINE + off for off, _, _ in script.WINDOWS] + [DART_BASE + off for off in DART_OFFSETS]
                    + sorted({PS_ANE_SYS & ~0x3FFF, PS_ANE_CPU & ~0x3FFF, PS_GATED & ~0x3FFF}))
        self.assertEqual(starts, sorted(expected))
        self.assertTrue(all(z[2] == TraceMode.ASYNC for z in hv.zones))
        self.assertFalse(hasattr(script, "DARTTracer"))
        self.assertIs(hv.ane_h15_tracer, tracer)
        self.assertIn("chip T6030", out)
        self.assertIn("ps register ANE_SYS", out)
        self.assertIn("ps register GATED_BY_NODE", out)
        self.assertNotIn("GPU", out)
        self.assertNotIn("ANE_NO_PS", out)

    def test_dart_accesses_are_labelled_by_window(self):
        _, tracer, _ = self.run_install()
        tracer.evt_rw(Evt(DART_BASE + 0x10000 + 0x20, 0x9, True))
        tracer.evt_rw(Evt(DART_BASE + 0x4000 + 0x8, 0x1, False))
        out = lines_of(tracer)
        self.assertIn("W4 DART1 +0x20 = 0x9", out[0])
        self.assertIn("R4 DART3 +0x8 = 0x1", out[1])

    def test_full_mode_traces_the_whole_aperture(self):
        hv, _, _ = self.run_install(ANE_TRACE_FULL="1")
        self.assertIn((ENGINE, ENGINE_SIZE, TraceMode.ASYNC), hv.zones)
        self.assertEqual(len(hv.zones), 1 + 1)

    def test_full_mode_still_traces_a_dart_outside_the_aperture(self):
        plan = script.make_plan(ENGINE, 0x1000000, [(DART_BASE, 0x4000, "DART0")], [], True)
        self.assertEqual([(s, n) for s, n, _ in plan], [(ENGINE, 0x1000000), (DART_BASE, 0x4000)])

    def test_narrow_plan_lists_windows_darts_and_ps_pages(self):
        plan = script.make_plan(ENGINE, ENGINE_SIZE, [(DART_BASE, 0x4000, "DART0")], [0x28E080000], False)
        self.assertEqual([(s, n) for s, n, _ in plan],
                         [(ENGINE + off, size) for off, size, _ in script.WINDOWS] + [(DART_BASE, 0x4000), (0x28E080000, 0x4000)])

    def test_cap_from_environment(self):
        _, tracer, _ = self.run_install(ANE_TRACE_MAX="7")
        self.assertEqual(tracer.cap, 7)

    def test_other_chip_is_flagged_not_refused(self):
        _, _, out = self.run_install(hv_args={"chip": 0x8132})
        self.assertIn("not an M3 chip", out)

    def test_missing_dart_node_is_reported(self):
        hv, _, out = self.run_install(hv_args={"dart": False})
        self.assertIn("no ANE DART node found", out)
        self.assertFalse([z for z in hv.zones if DART_BASE <= z[0] < DART_BASE + 0x40000])

    def test_every_hooked_range_is_printed_before_the_hooks_are_installed(self):
        hv, tracer, out = self.run_install()
        printed = []
        for line in out.splitlines():
            if line.startswith("ANE trace: hook "):
                head = line.split(" (")[0].split()
                printed.append((int(head[3], 16), int(head[4].lstrip("+"), 16)))
        self.assertEqual(sorted(printed), sorted((z[0], z[1]) for z in hv.zones))
        self.assertEqual(sorted(printed), sorted(tracer.zones))

    def test_plan_only_prints_the_plan_and_installs_nothing(self):
        hv, tracer, out = self.run_install(ANE_TRACE_PLAN_ONLY="1")
        self.assertIsNone(tracer)
        self.assertEqual(hv.zones, [])
        self.assertEqual(hv.releases, [])
        self.assertFalse(hasattr(hv, "ane_h15_tracer"))
        self.assertIn("plan only; no hook is installed", out)
        self.assertIn(f"hook {ENGINE + 0x1400000:#x} +0x10000 (ASC wrapper)", out)

    def test_missing_ane_node_is_an_error(self):
        hv = make_hv()
        del hv.adt["/arm-io/ane"]
        with self.assertRaises(RuntimeError):
            script.install(hv)


if __name__ == "__main__":
    unittest.main()
