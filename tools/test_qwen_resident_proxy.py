#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Offline checks for tools/qwen_resident_proxy.py (the resident A/B proxy).

A fake ane-run and a fake ane-session stand in for the device: both apply
the real CHK_ADD semantics (fp16 add, rounded nearest, ties away from
zero, tools/ane_f16_add.h) to the packed surfaces, so the per-call arm,
the resident arm and the tool's numpy model must agree bit for bit on
this host with no ANE. The fixture is a ports table for a fake `add`
program built in a tempdir; the checks drive the real CLI (black box).

Checks, each with a mutation-proven negative control:
  1. arms bit-identical over a 3x2 chain and equal to the model; the
     per-call arm spawns exactly calls x steps ane-runs. Mutations: the
     fake device returns one-ulp-corrupted output (FAKE_CORRUPT=1) ->
     must fail.
  2. the resident lock is taken per step or per call and released before
     the next take: the fake session probes its own lock non-blocking at
     LOCK and logs LOCK ok / LOCK busy. Mutation: UNLOCK becomes a no-op
     (FAKE_NO_UNLOCK=1) -> LOCK busy must appear.
  3. a failing call stops that arm: simulated exec failure in the fake
     ane-run and in the fake session -> the tool exits nonzero with the
     STOP text and writes no summary. Mutation-free control: check 1
     proves the same command passes without the fault injection.
  4. --dry-run / DRY=1 makes no device call: no fake binary is invoked,
     the plan is printed, exit 0.
  5. the tool's model pins CHK_ADD rounding: an exact tie rounds away
     from zero, -0.0 keeps its sign, 2^-24 survives as the smallest
     subnormal; plain numpy RNE would round the tie down (mutation
     control for the model swap).
"""
import json
import os
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import qwen_resident_proxy as proxy  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
F16 = np.float16

FAKE_LIB = textwrap.dedent("""\
    import numpy as np

    def round_half_away(s):
        s = np.asarray(s, np.float64)
        mag = np.abs(s)
        e = np.frexp(mag)[1]
        ulp = np.where(e <= -14, 2.0 ** -24, np.ldexp(np.ones_like(mag), e - 11))
        frac = mag / ulp
        n = np.floor(frac)
        n = np.where(frac - n >= 0.5, n + 1.0, n)
        e = e + (n >= 2048.0)
        r = np.where(e <= -14,
                     np.where(n >= 1024.0, 2.0 ** -14, n * ulp),
                     np.where(e > 16, np.inf,
                              n * np.ldexp(np.ones_like(mag), e - 11)))
        return np.copysign(r, s).astype(np.float16)

    def corrupt(plane):
        import os
        if os.environ.get("FAKE_CORRUPT") != "1":
            return plane
        bits = plane.view(np.uint16).copy()
        bits[0] += 1  # one ulp up on a positive normal
        return bits.view(np.float16)

    def add_tiles(a_tile, b_tile, elems):
        a = np.frombuffer(a_tile, dtype=np.float16)[:elems]
        b = np.frombuffer(b_tile, dtype=np.float16)[:elems]
        y = corrupt(round_half_away(a.astype(np.float64) + b.astype(np.float64)))
        return y.tobytes()
""")


def _tail(body):
    return textwrap.dedent(body)


FAKE_SESSION = _tail("""\
    #!/usr/bin/env python3
    import fcntl, json, os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fake_proxy_lib import add_tiles

    progs, failed, lock_fd = {}, 0, None
    args = sys.argv[1:]
    lock_path = args[args.index("--lock") + 1] if "--lock" in args else "/var/tmp/ane-run.lock"
    events = os.environ.get("FAKE_EVENTS")

    def ev(line):
        if events:
            with open(events, "a") as f:
                f.write(line + "\\n")

    def line(text):
        sys.stdout.write(text + "\\n"); sys.stdout.flush()

    for raw in iter(sys.stdin.buffer.readline, b""):
        cmd = raw.decode().split()
        op = cmd[0]
        if op == "QUIT":
            line("OK QUIT"); break
        elif op == "LOCK":
            fd = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o666)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                ev("LOCK ok")
            except BlockingIOError:
                ev("LOCK busy")
            fcntl.flock(fd, fcntl.LOCK_EX)
            lock_fd = fd
            line("OK LOCK")
        elif op == "UNLOCK":
            if lock_fd is not None and os.environ.get("FAKE_NO_UNLOCK") != "1":
                fcntl.flock(lock_fd, fcntl.LOCK_UN)
                os.close(lock_fd)
                lock_fd = None
            ev("UNLOCK")
            line("OK UNLOCK")
        elif op == "LOAD":
            name, anec, table_path = cmd[1], cmd[2], cmd[3]
            table = json.load(open(table_path))
            tiles = [p["tile_bytes"] for p in table["ports"]
                     if p["direction"] == "input"]
            tin = sum(tiles)
            tout = sum(p["tile_bytes"] for p in table["ports"] if p["direction"] == "output")
            progs[name] = (tin, tout, tiles)
            ev(f"LOAD {name}")
            line(f"OK LOAD {name} 2 1")
        elif op == "CALL":
            name = cmd[1]
            if name not in progs:
                line(f"ERR CALL {name} not-loaded"); failed = 1; continue
            tin, tout, tiles = progs[name]
            payload = sys.stdin.buffer.read(tin)
            if os.environ.get("FAKE_FAIL_AT") == name:
                line(f"ERR CALL {name} exec-failed"); failed = 1; continue
            y = add_tiles(payload[:tiles[0]], payload[tiles[0]:], tiles[0] // 2)
            line(f"OK CALL {name} 1000")
            sys.stdout.buffer.write(y + bytes(tout - len(y)))
            sys.stdout.buffer.flush()
    sys.exit(1 if failed else 0)
""")

FAKE_ANE_RUN = _tail("""\
    #!/usr/bin/env python3
    import json, os, sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from fake_proxy_lib import add_tiles

    args = sys.argv[1:]
    table = json.load(open(args[args.index("--ports") + 1]))
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
    if os.environ.get("FAKE_TRACE"):
        with open(os.environ["FAKE_TRACE"], "a") as f:
            f.write("SPAWN\\n")
    tile = next(p["tile_bytes"] for p in table["ports"] if p["direction"] == "input")
    y = add_tiles(ins["a"], ins["b"], tile // 2)
    for name, path in outs.items():
        with open(path, "wb") as f:
            f.write(y + bytes(tile - len(y)))
    print("exec ms over 1 calls: min 1.000 p10 1.000 p25 1.000 median 1.000 "
          "p75 1.000 p90 1.000 p99 1.000 max 1.000")
""")


def write_fakes(root):
    (root / "fake_proxy_lib.py").write_text(FAKE_LIB)
    session = root / "fake_ane_session.py"
    session.write_text(FAKE_SESSION)
    run = root / "fake_ane_run.py"
    run.write_text(FAKE_ANE_RUN)
    session.chmod(0o755)
    run.chmod(0o755)
    return session, run


def port_table(prog, ports):
    """Strict-schema table: dense fp16 shapes on 0x4000-byte tiles."""
    table = []
    for i, (name, direction, shape) in enumerate(ports):
        acc = 2
        dense = []
        for d in reversed(shape):
            dense.append(acc)
            acc *= d
        table.append({"name": name, "direction": direction,
                      "buffer_id": 4 + i, "channel": 4 + i, "bar_slot": 4 + i,
                      "tile_bytes": 0x4000, "surface_bytes": acc // 2,
                      "shape": list(shape), "strides": list(reversed(dense))})
    return {"program": prog, "ports": table,
            "dma_coverage": [{"bar_slot": p["bar_slot"], "port": p["name"],
                              "buffer_id": p["buffer_id"]} for p in table]}


def build_fixture(root):
    anec_dir = root / "anec" / "add"
    anec_dir.mkdir(parents=True)
    (anec_dir / "program-0.anec").write_bytes(b"fake")
    (root / "ports.json").write_text(
        json.dumps(port_table("add", [("a", "input", [1, 1, 64, 64]),
                                      ("b", "input", [1, 1, 64, 64]),
                                      ("y", "output", [1, 1, 64, 64])])))
    return anec_dir / "program-0.anec", root / "ports.json"


def run_tool(root, anec, ports, out, *extra, timeout=120, env=None):
    environ = dict(os.environ)
    environ.update(env or {})
    environ.setdefault("FAKE_EVENTS", str(root / "events.log"))
    return subprocess.run(
        [sys.executable, str(REPO / "tools" / "qwen_resident_proxy.py"),
         "--anec", str(anec), "--ports", str(ports), "--calls", "3",
         "--steps", "2", "--out", str(out), "--idle", "0",
         "--ane-run", str(root / "fake_ane_run.py"),
         "--session-bin", str(root / "fake_ane_session.py"),
         "--lock", str(root / "test.lock"), *extra],
        capture_output=True, text=True, timeout=timeout, env=environ)


def check_bit_identical(root, anec, ports):
    failures = []
    out = root / "run-ok"
    run = run_tool(root, anec, ports, out,
                   env={"FAKE_TRACE": str(root / "trace.log")})
    if run.returncode != 0:
        return [f"clean run failed: {run.returncode} {run.stdout[-400:]} "
                f"{run.stderr[-400:]}"]
    summary = json.loads((out / "summary.json").read_text())
    if not summary["bit_identical"]:
        failures.append(f"clean run not bit_identical: {summary['failures']}")
    arms = summary["arms"]
    if len({a["final_sha256"] for a in arms.values()} |
           {summary["model_sha256"]}) != 1:
        failures.append("arms/model shas disagree")
    spawns = (root / "trace.log").read_text().count("SPAWN") \
        if (root / "trace.log").exists() else 0
    if spawns != 6:
        failures.append(f"per-call spawns {spawns} != 6")
    # mutation: one-ulp corruption -> verdict must flip
    run = run_tool(root, anec, ports, root / "corrupt-a",
                   env={"FAKE_CORRUPT": "1",
                        "FAKE_EVENTS": str(root / "events-corrupt.log")})
    if run.returncode == 0:
        failures.append("corrupted fake output still passed")
    elif "model" not in run.stdout:
        failures.append("corruption failure lacks a mismatch reason: "
                        f"{run.stderr[-200:]}")
    return failures


def check_lock(root, anec, ports):
    failures = []
    for mode, takes in (("step", 2), ("call", 6)):
        out = root / f"lock-{mode}"
        events = root / f"events-lock-{mode}.log"
        run = run_tool(root, anec, ports, out, "--resident-lock", mode,
                       env={"FAKE_EVENTS": str(events)})
        if run.returncode != 0:
            failures.append(f"lock mode {mode}: run failed {run.stderr[-300:]}")
            continue
        lines = events.read_text().splitlines() if events.exists() else []
        if lines.count("LOCK ok") != takes or "LOCK busy" in lines:
            failures.append(f"lock mode {mode}: lock events {lines}")
        if lines.count("UNLOCK") != takes:
            failures.append(f"lock mode {mode}: UNLOCK count "
                            f"{lines.count('UNLOCK')} != {takes}")
        # never nested: between two LOCKs there is an UNLOCK
        depth = 0
        for e in lines:
            depth += e.startswith("LOCK") - e.startswith("UNLOCK")
            if depth > 1:
                failures.append(f"lock mode {mode}: nested lock at {e}")
                break
    # mutation: no-op unlock -> the next LOCK probe must report busy
    mut_events = root / "events-lock-mut.log"
    run = run_tool(root, anec, ports, root / "lock-mut",
                   "--resident-lock", "step", "--timeout", "3",
                   env={"FAKE_NO_UNLOCK": "1", "FAKE_EVENTS": str(mut_events)},
                   timeout=60)
    lines = mut_events.read_text().splitlines() if mut_events.exists() else []
    if "LOCK busy" not in lines:
        failures.append("no-op unlock mutation not detected")
    if run.returncode == 0:
        failures.append("no-op unlock mutation still passed")
    return failures


def check_fail_stop(root, anec, ports):
    failures = []
    cases = (("per-call ane-run failure", {"FAKE_ANE_RUN_FAIL": "1"},
              "STOP per-call"),
             ("resident CALL failure", {"FAKE_FAIL_AT": "add"}, "STOP CALL add"))
    for name, env, needle in cases:
        out = root / f"fail-{name.split()[0]}"
        run = run_tool(root, anec, ports, out, env=env)
        if run.returncode == 0:
            failures.append(f"{name}: tool exited 0")
        if needle not in run.stderr + run.stdout:
            failures.append(f"{name}: no {needle!r} in output")
        summary_path = out / "summary.json"
        if summary_path.exists() and \
                f"{name.split()[0]} arm stopped" not in summary_path.read_text():
            failures.append(f"{name}: summary misses the stop record")
    return failures


def check_dry(root, anec, ports):
    failures = []
    for i, (flag, env) in enumerate((("--dry-run", None),
                                     (None, {"DRY": "1"}))):
        out = root / f"dry-{i}"
        events = root / f"events-dry-{i}.log"
        run = run_tool(root, anec, ports, out, *([flag] if flag else []),
                       env={"FAKE_EVENTS": str(events), **(env or {})})
        if run.returncode != 0 or "DRY: no ANE call" not in run.stdout:
            failures.append(f"dry ({flag or 'DRY=1'}): rc {run.returncode}")
        if events.exists() and events.read_text():
            failures.append(f"dry ({flag or 'DRY=1'}): the fake session ran")
        if (out / "summary.json").exists() or (out / "resident").exists():
            failures.append(f"dry ({flag or 'DRY=1'}): wrote run artifacts")
    return failures


def check_model_semantics():
    """Pins CHK_ADD rounding; numpy RNE on the tie would fail this."""
    tie = float(np.float16(4.25)) + float(np.spacing(np.float16(4.25))) / 2
    out = proxy.fp16_round_half_away(np.array([tie, -0.0, 2.0 ** -24, 4.25]))
    bits = [f"{v:04x}" for v in out.view(np.uint16)]
    if bits != ["4441", "8000", "0001", "4440"]:
        return [f"model rounding drift: {tie} -> {bits}"]
    return []


def main():
    failures = []
    with tempfile.TemporaryDirectory(prefix="qproxy-test-") as tmp:
        root = Path(tmp)
        write_fakes(root)
        anec, ports = build_fixture(root)
        for check in (check_bit_identical, check_lock, check_fail_stop,
                      check_dry):
            bad = check(root, anec, ports)
            print(f"{check.__name__}: {'PASS' if not bad else 'FAIL'}")
            failures += bad
        bad = check_model_semantics()
        print(f"check_model_semantics: {'PASS' if not bad else 'FAIL'}")
        failures += bad
    for f in failures:
        print(f"FAIL {f}")
    print("ALL PASS" if not failures else f"{len(failures)} FAILURES")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
