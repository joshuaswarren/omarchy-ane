#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Offline checks for qwen_m2_decode --resident (tools/ane-session.c).

A fake ane-session and a fake ane-run stand in for the device: both apply
one deterministic transform (keystream over the program name, output port
and the packed input surface bytes), so the resident path and the per-call
path run on this host with no ANE and must agree bit for bit. The fixture
is a three-program chain with a resident state (the chaining under test),
built in a tempdir; no gguf is needed because the checks drive
Decoder.step directly (the run loop, head math and records are unchanged
by --resident).

Checks, each with a mutation-proven negative control:
  1. resident output == per-call output over 3 steps incl. state
     chaining, every program input and output bit for bit, final hidden
     and states equal. Mutation: ResidentSession drops the last input
     surface from the CALL payload -> must fail.
  2. the device lock is released between steps: step mode holds it once
     per step, call mode around every call, and the sequence never nests
     a LOCK. Mutation: unlock becomes a no-op -> must fail.
  3. a failing call raises the STOP SystemExit, writes the failed-run log
     and the session exits nonzero. Mutation: the STOP raise is
     suppressed -> must fail.
  4. the real ane-session binary refuses a LOAD before any device open
     on --max-progs and --bo-cap-mb; control: with the guards off the
     same LOAD reaches the device open instead.
"""
import fcntl
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import textwrap
from argparse import Namespace
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import qwen_m2_decode as dec  # noqa: E402
import qwen_prog_run as qpr  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
F16 = np.float16

FAKE_LIB = textwrap.dedent("""\
    import hashlib, json

    def keystream(seed, n):
        h = hashlib.sha256(seed)
        out = bytearray()
        c = 0
        while len(out) < n:
            out.extend(hashlib.sha256(h.digest() + c.to_bytes(4, "little")).digest())
            c += 1
        return bytes(out[:n])

    def outputs_for(prog, table, ins):
        outs = {}
        for p in table["ports"]:
            if p["direction"] != "output":
                continue
            h = hashlib.sha256()
            h.update(prog.encode()); h.update(b"|"); h.update(p["name"].encode())
            for q in table["ports"]:
                if q["direction"] == "input":
                    h.update(q["name"].encode())
                    h.update(ins[q["name"]])
            outs[p["name"]] = keystream(h.digest(), p["tile_bytes"])
        return outs
""")

FAKE_SESSION = textwrap.dedent("""\
    #!/usr/bin/env python3
    import fcntl, os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fake_ane_lib import outputs_for

    import json
    progs = {}
    demand = 0
    failed = 0
    lock_fd = None
    args = sys.argv[1:]
    max_progs = int(args[args.index("--max-progs") + 1]) if "--max-progs" in args else 250
    cap = (int(args[args.index("--bo-cap-mb") + 1]) << 20) if "--bo-cap-mb" in args else 0
    lock_path = args[args.index("--lock") + 1] if "--lock" in args else "/var/tmp/ane-run.lock"
    events = os.environ.get("FAKE_EVENTS")
    guard = 16384
    page = lambda b: (b + 16383) // 16384 * 16384

    def ev(line):
        if events:
            with open(events, "a") as f:
                f.write(line + "\\n")

    def line(text):
        sys.stdout.write(text + "\\n"); sys.stdout.flush()

    for raw in iter(sys.stdin.buffer.readline, b""):
        cmd = raw.decode().strip().split(" ")
        op = cmd[0]
        if op == "QUIT":
            line("OK QUIT"); break
        elif op == "LOCK":
            lock_fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o666)
            fcntl.flock(lock_fd, fcntl.LOCK_EX)
            ev("LOCK")
            line("OK LOCK")
        elif op == "UNLOCK":
            if os.environ.get("FAKE_NO_UNLOCK") != "1" and lock_fd is not None:
                fcntl.flock(lock_fd, fcntl.LOCK_UN)
            ev("UNLOCK")
            line("OK UNLOCK")
        elif op == "LOAD":
            name, table_path = cmd[1], cmd[3]
            if name in progs:
                line(f"ERR LOAD {name} already-loaded"); failed = 1; continue
            if len(progs) >= max_progs:
                line(f"ERR LOAD {name} max-progs {max_progs} reached (driver program "
                     "table bound; refusing before the device)"); failed = 1; continue
            table = json.load(open(table_path))
            d = 6 * page(guard) + sum(page(p["tile_bytes"] + guard)
                                      for p in table["ports"])
            if cap and demand + d > cap:
                line(f"ERR LOAD {name} bo-cap: need {d} bytes, live {demand} plus "
                     f"driver 0 exceeds cap {cap} (refusing before the device)")
                failed = 1; continue
            nin = sum(p["direction"] == "input" for p in table["ports"])
            nout = sum(p["direction"] == "output" for p in table["ports"])
            tin = sum(p["tile_bytes"] for p in table["ports"] if p["direction"] == "input")
            tout = sum(p["tile_bytes"] for p in table["ports"] if p["direction"] == "output")
            progs[name] = (table, tin, tout)
            demand += d
            ev(f"LOAD {name}")
            line(f"OK LOAD {name} {nin} {nout}")
        elif op == "FREE":
            name = cmd[1]
            if name not in progs:
                line(f"ERR FREE {name} not-loaded"); failed = 1; continue
            del progs[name]
            line(f"OK FREE {name}")
        elif op == "CALL":
            name = cmd[1]
            if name not in progs:
                line(f"ERR CALL {name} not-loaded"); failed = 1; continue
            table, tin, tout = progs[name]
            if os.environ.get("FAKE_FAIL_AT") == name:
                for p in table["ports"]:
                    if p["direction"] == "input":
                        sys.stdin.buffer.read(p["tile_bytes"])
                line(f"ERR CALL {name} exec-failed"); failed = 1; continue
            ins = {}
            for p in table["ports"]:
                if p["direction"] == "input":
                    ins[p["name"]] = sys.stdin.buffer.read(p["tile_bytes"])
            if os.environ.get("FAKE_TRACE"):
                with open(os.environ["FAKE_TRACE"], "a") as f:
                    f.write(f"FK CALL {name} read={sum(len(v) for v in ins.values())}\\n")
            ev(f"CALL {name}")
            outs = outputs_for(name, table, ins)
            line(f"OK CALL {name} 1000")
            _w = 0
            for p in table["ports"]:
                if p["direction"] == "output":
                    sys.stdout.buffer.write(outs[p["name"]])
                    _w += len(outs[p["name"]])
            sys.stdout.buffer.flush()
            if os.environ.get("FAKE_TRACE"):
                with open(os.environ["FAKE_TRACE"], "a") as f:
                    f.write(f"FK WROTE {name} wrote={_w}\\n")
    sys.exit(1 if failed else 0)
""")

FAKE_ANE_RUN = textwrap.dedent("""\
    #!/usr/bin/env python3
    import os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fake_ane_lib import outputs_for
    import json

    args = sys.argv[1:]
    anec = args[args.index("--anec") + 1]
    table = json.load(open(args[args.index("--ports") + 1]))
    repeat = int(args[args.index("--repeat") + 1]) if "--repeat" in args else 1
    ins, outs = {}, {}
    for i, a in enumerate(args):
        if a == "--in":
            name, path = args[i + 1].split("=", 1)
            ins[name] = open(path, "rb").read()
        if a == "--out":
            name, path = args[i + 1].split("=", 1)
            outs[name] = path
    if os.environ.get("FAKE_ANE_RUN_FAIL") == "1":
        sys.stderr.write("LIBANE: ERR: simulated exec failure\\n")
        sys.exit(1)
    result = outputs_for(table["program"], table, ins)
    for name, path in outs.items():
        with open(path, "wb") as f:
            f.write(result[name])
    print(f"exec ms over {repeat} calls: min 1.000 p10 1.000 p25 1.000 "
          f"median 1.000 p75 1.000 p90 1.000 p99 1.000 max 1.000")
""")


def write_fakes(root):
    (root / "fake_ane_lib.py").write_text(FAKE_LIB)
    session = root / "fake_ane_session.py"
    session.write_text(FAKE_SESSION)
    run = root / "fake_ane_run.py"
    run.write_text(FAKE_ANE_RUN)
    session.chmod(0o755)
    run.chmod(0o755)
    return session, run


def port_table(prog, ports):
    """A strict-schema table: dense fp16 shapes on 0x4000-byte tiles, one
    BAR slot and buffer_id per port."""
    table = []
    for i, (name, direction, shape) in enumerate(ports):
        tb = 0x4000
        dense = []
        acc = 2
        for d in reversed(shape):
            dense.append(acc)
            acc *= d
        strides = list(reversed(dense))
        table.append({"name": name, "direction": direction,
                      "buffer_id": 4 + i, "channel": 4 + i, "bar_slot": 4 + i,
                      "tile_bytes": tb, "surface_bytes": acc // 2,
                      "shape": list(shape), "strides": strides})
    return {"program": prog, "ports": table,
            "dma_coverage": [{"bar_slot": p["bar_slot"], "port": p["name"],
                              "buffer_id": p["buffer_id"]} for p in table]}


def build_fixtures(root):
    """Three-program chain: 000 (x->h), 001 (x+state->h, state out),
    002 (h->h, group_end)."""
    anec_dir = root / "anec"
    ports_dir = root / "ports"
    for i in range(3):
        prog = f"prog_{i:03d}"
        (anec_dir / prog).mkdir(parents=True, exist_ok=True)
        (anec_dir / prog / "program-0.anec").write_bytes(b"fake")
        (ports_dir / prog).mkdir(parents=True, exist_ok=True)
    x8, st = [1, 1, 1, 8], [2, 1, 1, 4]
    programs = [
        {"group_start": 1, "group_end": 0,
         "srcs": [{"port": "x", "lane": "x", "kind": "lane", "shape": x8}],
         "dsts": [{"port": "h", "lane": "h", "shape": x8}],
         "states": []},
        {"group_start": 0, "group_end": 0,
         "srcs": [{"port": "st_in", "lane": "s", "kind": "state_in", "shape": st},
                  {"port": "x", "lane": "x", "kind": "lane", "shape": x8}],
         "dsts": [{"port": "h", "lane": "h", "shape": x8}],
         "states": [{"in_port": "st_in", "out_port": "st_out",
                     "in_shape": st, "out_shape": st}]},
        {"group_start": 0, "group_end": 1,
         "srcs": [{"port": "in_h", "lane": "h", "kind": "lane", "shape": x8}],
         "dsts": [{"port": "out_h", "lane": "h", "shape": x8}],
         "states": []},
    ]
    specs = [[("x", "input", x8), ("h", "output", x8)],
             [("st_in", "input", st), ("x", "input", x8),
              ("h", "output", x8), ("st_out", "output", st)],
             [("in_h", "input", x8), ("out_h", "output", x8)]]
    for i, specs_ in enumerate(specs):
        prog = f"prog_{i:03d}"
        (ports_dir / prog / "ports.resolved.json").write_text(
            json.dumps(port_table(prog, specs_)))
    (root / "manifest.json").write_text(
        json.dumps({"max_len": 4, "programs": programs}))
    return ports_dir, anec_dir


def make_decoder(root, ports_dir, anec_dir, resident, session_bin, lock_mode="call"):
    args = Namespace(out=str(root / f"out-{int(resident)}-{lock_mode}"),
                     anec_dir=str(anec_dir), ports_dir=str(ports_dir),
                     ane_run=str(root / "fake_ane_run.py"), timeout=5,
                     resident=resident, session_bin=str(session_bin),
                     resident_lock=lock_mode, per_m_dir=str(root / "perM"),
                     session_lock=str(root / "test.lock"),
                     dump=None, band=None)
    return dec.Decoder(args, json.loads((root / "manifest.json").read_text()))


def run_chain(decoder, steps=3):
    """steps over the fixture chain; every program input, output, the
    hidden and the chained states are recorded as sha256."""
    embed = np.zeros((4, 8), F16)
    embed[1] = np.arange(1, 9, dtype=F16)
    cos, sin = dec.rope_tables(4, 4, 4, 10000.0)
    seen = {}

    def hook(i, arrays, outs):
        for name, arr in arrays.items():
            seen[f"in:{i}:{name}"] = hashlib.sha256(
                np.asarray(arr, F16).tobytes()).hexdigest()
        for name, arr in outs.items():
            seen[f"out:{i}:{name}"] = hashlib.sha256(
                np.asarray(arr, F16).tobytes()).hexdigest()

    for pos in range(steps):
        hidden, _ = decoder.step(1, pos, cos, sin, embed, hook)
        seen[f"hidden:{pos}"] = hashlib.sha256(
            np.asarray(hidden, np.float32).tobytes()).hexdigest()
    for i, states in enumerate(decoder.states):
        for name, arr in states.items():
            seen[f"state:{i}:{name}"] = hashlib.sha256(arr.tobytes()).hexdigest()
    decoder.close_session()
    return seen


def check_bit_identical(root, ports_dir, anec_dir, session_bin):
    failures = []
    per_call = run_chain(make_decoder(root, ports_dir, anec_dir, False, session_bin))
    resident = run_chain(make_decoder(root, ports_dir, anec_dir, True, session_bin))
    for key in sorted(set(per_call) | set(resident)):
        if per_call.get(key) != resident.get(key):
            failures.append(f"{key}: per-call {per_call.get(key)} != "
                            f"resident {resident.get(key)}")
    if not any(k.startswith("state:1:") for k in per_call):
        failures.append("no chained state recorded: chaining untested")
    if len([k for k in per_call if k.startswith("hidden:")]) != 3:
        failures.append("expected 3 steps")
    return failures


def read_events(path):
    return Path(path).read_text().splitlines()


def check_lock(root, ports_dir, anec_dir, session_bin, lock_mode):
    """The event sequence never nests a LOCK; step mode takes it once per
    step; call mode around every call; the last event is an UNLOCK; the
    lock file is free afterwards."""
    events = root / f"events-{lock_mode}.log"
    events.write_text("")
    os.environ["FAKE_EVENTS"] = str(events)
    try:
        decoder = make_decoder(root, ports_dir, anec_dir, True, session_bin,
                               lock_mode)
        embed = np.zeros((4, 8), F16)
        cos, sin = dec.rope_tables(4, 4, 4, 10000.0)
        for pos in range(2):
            decoder.step(1, pos, cos, sin, embed)
        decoder.close_session()
    finally:
        os.environ.pop("FAKE_EVENTS", None)
    ev = read_events(events)
    failures = []
    calls = sum(1 for e in ev if e.startswith("CALL"))
    want_locks = 2 if lock_mode == "step" else calls
    if ev.count("LOCK") != want_locks or ev.count("UNLOCK") != want_locks:
        failures.append(f"{lock_mode}: {ev.count('LOCK')} LOCK / "
                        f"{ev.count('UNLOCK')} UNLOCK, want {want_locks}: {ev}")
    held = 0
    for e in ev:
        if e == "LOCK":
            held += 1
            if held > 1:
                failures.append(f"{lock_mode}: nested LOCK: {ev}")
        elif e == "UNLOCK":
            held -= 1
    if held != 0:
        failures.append(f"{lock_mode}: {held} locks still held at end: {ev}")
    if ev and ev[-1] != "UNLOCK":
        failures.append(f"{lock_mode}: last event {ev[-1]!r}")
    probe = root / "lockprobe"
    fd = os.open(probe, os.O_RDWR | os.O_CREAT, 0o666)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        fcntl.flock(fd, fcntl.LOCK_UN)
    except OSError as e:
        failures.append(f"lock probe failed after close: {e}")
    finally:
        os.close(fd)
    return failures


def check_failing_call(root, ports_dir, anec_dir, session_bin):
    failures = []
    os.environ["FAKE_FAIL_AT"] = "prog_001"
    try:
        decoder = make_decoder(root, ports_dir, anec_dir, True, session_bin)
        embed = np.zeros((4, 8), F16)
        cos, sin = dec.rope_tables(4, 4, 4, 10000.0)
        try:
            decoder.step(1, 0, cos, sin, embed)
            failures.append("failing call did not raise")
        except SystemExit as e:
            if "STOP" not in str(e) or "prog_001" not in str(e):
                failures.append(f"STOP text wrong: {e}")
        log = Path(decoder.args.out) / "surf" / "failed-ane-run.log"
        if not log.exists():
            failures.append("failed-ane-run.log missing")
        rc = decoder.session.close()
        if rc == 0:
            failures.append("session exit 0 after a failed call")
    finally:
        os.environ.pop("FAKE_FAIL_AT", None)
    return failures


def check_guards():
    """4: the real binary refuses pre-device; the control reaches the
    device open."""
    work = Path(tempfile.mkdtemp(prefix="qresident-guard-"))
    anec = REPO / "fixtures/h14-anec/add/program-0.anec"
    ports = work / "add.json"
    ports.write_text(json.dumps(port_table(
        "add", [("a", "input", [1, 1, 1, 0x1000]), ("b", "input", [1, 1, 1, 0x1000]),
                ("y", "output", [1, 1, 1, 0x1000])])))
    big = work / "big.json"
    table = port_table("add", [("a", "input", [1, 1, 1, 0x1000]),
                               ("b", "input", [1, 1, 1, 0x1000]),
                               ("y", "output", [1, 1, 1, 0x8000000])])
    table["ports"][2]["tile_bytes"] = 0x10000000  # 256 MiB output tile
    big.write_text(json.dumps(table))
    binary = REPO / "tools/ane-session"
    failures = []

    def scenario(name, args, commands, want, guarded):
        proc = subprocess.run([str(binary), *args], input=commands,
                              capture_output=True, text=True, timeout=60)
        if want not in proc.stdout:
            failures.append(f"{name}: no {want!r} in {proc.stdout!r}")
        device = "failed to open device" in proc.stderr
        if guarded and device:
            failures.append(f"{name}: device opened despite the guard")
        if not guarded and not device:
            failures.append(f"{name}: control did not reach the device open")
        if proc.returncode == 0:
            failures.append(f"{name}: exit 0")

    scenario("max-progs", ["--max-progs", "0", "--lock", str(work / "l0")],
             f"LOAD p0 {anec} {ports}\nQUIT\n",
             "max-progs 0 reached", True)
    scenario("bo-cap", ["--bo-cap-mb", "64", "--lock", str(work / "l1")],
             f"LOAD p0 {anec} {big}\nQUIT\n",
             "exceeds cap 67108864", True)
    scenario("guards-off-control",
             ["--bo-cap-mb", "4096", "--lock", str(work / "l2")],
             f"LOAD p0 {anec} {big}\nQUIT\n", "device-open-failed", False)
    return failures


# ---- mutation-proven negatives -------------------------------------------

def mutate(attr, replacement):
    def patch():
        saved = getattr(qpr.ResidentSession, attr)
        setattr(qpr.ResidentSession, attr, replacement)
        return lambda: setattr(qpr.ResidentSession, attr, saved)
    return patch


def m_drop_input(self, name, ports, arrays):
    """Mutation 1: drop the last input surface from the CALL payload."""
    ins = [(n, p) for n, p in ports.items()
           if p["direction"] == "input"][:-1]
    payload = b"".join(
        qpr.pack_surface(arrays[n].reshape(p["shape"]), p["strides"],
                         p["tile_bytes"]).tobytes() for n, p in ins)
    self._command(f"CALL {name}")
    self.proc.stdin.write(payload)
    self.proc.stdin.flush()
    line = self._line()
    raw = self._read(sum(p["tile_bytes"] for _, p in ports.items()
                         if p["direction"] == "output"))
    return {}, 1.0


def m_no_unlock(self):
    """Mutation 2: unlock does nothing."""


def m_no_stop(self, tag, name, detail):
    """Mutation 3: lose the STOP text (must not swallow the exit itself,
    or the output read would block forever with no reply coming)."""
    raise SystemExit(f"swallowed {tag} {name}")


def main():
    root = Path(tempfile.mkdtemp(prefix="qresident-"))
    session_bin, _ = write_fakes(root)
    ports_dir, anec_dir = build_fixtures(root)
    checks = [
        ("bit-identical",
         lambda: check_bit_identical(root, ports_dir, anec_dir, session_bin),
         mutate("call", m_drop_input)),
        ("lock-step",
         lambda: check_lock(root, ports_dir, anec_dir, session_bin, "step"),
         mutate("unlock", m_no_unlock)),
        ("lock-call",
         lambda: check_lock(root, ports_dir, anec_dir, session_bin, "call"),
         mutate("unlock", m_no_unlock)),
        ("failing-call",
         lambda: check_failing_call(root, ports_dir, anec_dir, session_bin),
         mutate("_fail", m_no_stop)),
        ("guards", check_guards, None),
    ]
    failed = 0
    for name, fn, mutation in checks:
        failures = fn()
        status = "PASS" if not failures else "FAIL"
        print(f"{status} {name}")
        for f in failures:
            print(f"  {f}")
        failed += bool(failures)
        if mutation:
            undo = mutation()
            try:
                mutated_failures = fn()
            except SystemExit as e:
                # A mutated run may STOP (timeout, short read): that is the
                # mutation failing the check, not a broken suite.
                mutated_failures = [f"SystemExit under mutation: {e}"]
            undo()
            if not mutated_failures:
                print(f"FAIL {name}-mutation: the negative control did not "
                      f"fail (mutation is ineffective)")
                failed += 1
            else:
                print(f"PASS {name}-mutation ({len(mutated_failures)} "
                      f"failures under mutation)")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
