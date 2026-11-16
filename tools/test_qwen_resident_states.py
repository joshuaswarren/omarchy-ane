#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host test of the session-resident states of qwen_m2_decode (the default with --resident; --host-states opts out; no ANE, no device).

The REAL ane-session (tools/ane-session.c) runs over a fake DRM node whose device writes every output tile byte,
padding included, as a function of every input tile byte (tools/test_ane_session_state.c in serve mode). The decoder
is driven through three programs of the prog000 fixture (conv state [1,1,6144,3], row stride 64 B, so 90.6 percent
of the tile is padding the fake device fills with nonzero bytes), two prompts of three steps each (configure per
prompt, as the run loop does).

Checks:
  1. host-chained resident decode and resident-states decode agree bit for bit: every program's lane inputs, lane
     outputs and the hidden of every step of both prompts (the padding semantic and RESET per prompt);
  2. resident states write far fewer bytes to the session than the host-chained arm (the state tiles are gone);
  3. LOAD-time refusals: a pair whose output strides differ is refused before anything is sent; the pairing request
     takes the port indices from the manifest pair, not from shapes (K and V of identical shape, listed in the
     opposite order of the ports);
  4. the command line turns resident states on with --resident, off with --host-states or --dump, and the retired
     --resident-states flag is refused; the Decoder refuses resident states with a dump.
Run test_qwen_resident_states_mutants.py for the mutation proof."""
import copy
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from argparse import Namespace
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import qwen_m2_decode as dec  # noqa: E402
import qwen_prog_run as qpr  # noqa: E402

REPO = Path(os.environ.get("ANE_REPO", HERE.parent))
F16 = np.float16
ANEC = REPO / "fixtures/h14-anec/add/program-0.anec"
PORTS = REPO / "tests/fixtures/prog000-ports.json"


def build_serve(work):
    exe = Path(os.environ.get("ANE_SESSION_STATE_SERVE", work / "serve"))
    if exe.exists():
        return exe
    libane, uapi = REPO / "libane", REPO / "ane/src/uapi/drm"
    subprocess.run(["gcc", "-O1", "-std=gnu99", "-w", f"-I{libane}", f"-I{uapi}", "-I/usr/include/libdrm",
                    "-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap", "-o", str(exe), str(REPO / "tools/test_ane_session_state.c"),
                    str(libane / "ane.c"), str(libane / "ane_m2.c"), str(libane / "ane_sha256.c"), "-lm"],
                   check=True, cwd=REPO / "tools")
    return exe


def fixture(root):
    anec_dir, ports_dir = root / "anec", root / "ports"
    table0 = json.loads(PORTS.read_text())
    n = 3
    for i in range(n):
        prog = f"prog_{i:03d}"
        (anec_dir / prog).mkdir(parents=True)
        (anec_dir / prog / "program-0.anec").write_bytes(ANEC.read_bytes())
        (ports_dir / prog).mkdir(parents=True)
        t = dict(table0, program=prog)
        (ports_dir / prog / "ports.resolved.json").write_text(json.dumps(t))
    lane = [1, 1, 16, 128]
    outs = [("t16", "h", lane), ("t20", "o20", lane), ("t22", "o22", lane), ("t26", "o26", [1, 16, 1, 1]),
            ("t35", "o35", [1, 16, 1, 1]), ("t37", "o37", lane)]
    programs = []
    for i in range(n):
        programs.append({
            "group_start": 1 if i == 0 else 0, "group_end": 1 if i == n - 1 else 0,
            "srcs": [{"port": "t1", "lane": "x" if i == 0 else "h", "kind": "lane", "shape": [1, 1, 1, 2048]},
                     {"port": "t5", "lane": "s", "kind": "state_in", "shape": [1, 1, 6144, 3]}],
            "dsts": [{"port": p, "lane": ln, "shape": sh} for p, ln, sh in outs],
            "states": [{"in_port": "t5", "out_port": "t38", "in_shape": [1, 1, 6144, 3], "out_shape": [1, 1, 6144, 3]}]})
    manifest = {"max_len": 4, "programs": programs}
    return manifest, ports_dir, anec_dir


def make_decoder(root, manifest, ports_dir, anec_dir, serve, resident_states, tag):
    args = Namespace(out=str(root / f"out-{tag}"), anec_dir=str(anec_dir), ports_dir=str(ports_dir),
                     ane_run="unused", timeout=20, resident=True, session_bin=str(serve), resident_lock="call",
                     per_m_dir=str(root / "perM"), session_lock=str(root / f"{tag}.lock"), dump=None, band=None,
                     resident_states=resident_states)
    return dec.Decoder(args, manifest)


class Wire:
    """Counts the bytes a decoder writes to the session."""

    def __init__(self, decoder):
        self.n = 0
        sess = decoder.session
        orig = sess._write_all

        def counted(data):
            self.n += len(data)
            return orig(data)
        sess._write_all = counted


def run_decode(decoder, prompts=((1, 2, 3), (2, 3, 1)), between="configure"):
    embed = np.zeros((4, 2048), F16)
    for t in range(4):
        embed[t] = (np.arange(2048) % 251 + t * 7).astype(F16)
    cos, sin = dec.rope_tables(4, 4, 4, 10000.0)
    wire = Wire(decoder)
    seen = {}
    state_ports = {"t5", "t38"}

    def hook(i, arrays, outs):
        for name, arr in arrays.items():
            if name not in state_ports:
                seen[f"in:{cur[0]}:{i}:{name}"] = hashlib.sha256(np.asarray(arr, F16).tobytes()).hexdigest()
        for name, arr in outs.items():
            if name not in state_ports:
                seen[f"out:{cur[0]}:{i}:{name}"] = hashlib.sha256(np.asarray(arr, F16).tobytes()).hexdigest()
    cur = [""]
    for pi, tokens in enumerate(prompts):
        # "configure": the run loop configures once per prompt (FREE, LOAD, STATE, RESET); "reset": RESET alone.
        if between == "configure" or pi == 0:
            decoder.configure(4)
        else:
            decoder.reset()
        for pos, tok in enumerate(tokens):
            cur[0] = f"p{pi}s{pos}"
            hidden, _ = decoder.step(tok, pos, cos, sin, embed, hook)
            seen[f"hidden:{cur[0]}"] = hashlib.sha256(np.asarray(hidden, np.float32).tobytes()).hexdigest()
    decoder.close_session()
    return seen, wire.n


class MockSession(qpr.ResidentSession):
    """Records the STATE commands; answers like the session."""

    def __init__(self):
        self.names, self.resident, self.sent = [], {}, []

    def _command(self, text):
        self.sent.append(text)

    def _write_all(self, data):
        self.sent.append(("payload", np.frombuffer(data, "<u8").tolist()[:3]))

    def _line(self):
        return "OK STATE " + [s for s in self.sent if isinstance(s, str)][-1].split()[1]


def port(name, direction, shape, strides, tile):
    return {"name": name, "direction": direction, "shape": shape, "strides": strides, "surface_bytes": 2 * int(np.prod(shape)),
            "tile_bytes": tile, "dtype": "fp16"}


def check_pairing_and_refusals(failures):
    shape, dense = [1, 2, 3, 4], [48, 24, 8, 2]
    ports = {"k_out": port("k_out", "output", shape, dense, 16384), "v_out": port("v_out", "output", shape, dense, 16384),
             "v_in": port("v_in", "input", shape, dense, 16384), "k_in": port("k_in", "input", shape, dense, 16384),
             "x": port("x", "input", [1, 1, 1, 8], [16, 16, 16, 2], 16384)}
    # inputs in table order: v_in (0), k_in (1), x (2). outputs: k_out (0), v_out (1).
    s = MockSession()
    s.states("p", ports, [("k_in", "k_out"), ("v_in", "v_out")])
    cmds = [c for c in s.sent if isinstance(c, str)]
    if not (cmds[0].startswith("STATE p 1 0 ") and cmds[1].startswith("STATE p 0 1 ")):
        failures.append(f"pairing must follow the manifest pairs, not the shapes: {cmds}")
    if s.resident["p"] != {"in": {"k_in", "v_in"}, "out": {"k_out", "v_out"}}:
        failures.append(f"resident sets wrong: {s.resident}")
    for label, mutate in (("strides", lambda p: p["k_out"].update(strides=[48, 24, 16, 2])),
                          ("shape", lambda p: p["k_out"].update(shape=[1, 2, 4, 3])),
                          ("surface_bytes", lambda p: p["k_out"].update(surface_bytes=2)),
                          ("tile_bytes", lambda p: p["k_out"].update(tile_bytes=32768))):
        bad = copy.deepcopy(ports)
        mutate(bad)
        s2 = MockSession()
        try:
            s2.states("p", bad, [("k_in", "k_out")])
            failures.append(f"a pair differing in {label} was not refused")
        except qpr.Refuse:
            if s2.sent:
                failures.append(f"a pair differing in {label} was refused only after sending {s2.sent}")
    for label, mutate in (("dtype differs", lambda p: p["k_out"].update(dtype="fp32")),
                          ("dtype missing on the output", lambda p: p["k_out"].pop("dtype")),
                          ("dtype missing on the input", lambda p: p["k_in"].pop("dtype")),
                          ("dtype not fp16 on both", lambda p: (p["k_out"].update(dtype="int8"), p["k_in"].update(dtype="int8"))),
                          ("element stride 4", lambda p: (p["k_out"].update(strides=[48, 24, 8, 4]), p["k_in"].update(strides=[48, 24, 8, 4])))):
        bad = copy.deepcopy(ports)
        mutate(bad)
        s4 = MockSession()
        try:
            s4.states("p", bad, [("k_in", "k_out")])
            failures.append(f"a pair with {label} was not refused")
        except qpr.Refuse:
            if s4.sent:
                failures.append(f"a pair with {label} was refused only after sending {s4.sent}")
    s3 = MockSession()
    try:
        s3.states("p", ports, [("x", "k_out")])
        failures.append("an input that is not paired in shape was accepted")
    except qpr.Refuse:
        pass
    try:
        s3.states("p", ports, [("k_in", "k_in")])
        failures.append("an output name that is an input was accepted")
    except qpr.Refuse:
        pass


def check_args(failures):
    wanted = {(): False, ("--resident",): True, ("--resident", "--host-states"): False,
              ("--resident", "--dump", "x"): False, ("--host-states",): False}
    for argv, states in wanted.items():
        try:
            got = dec.parse_args(list(argv)).resident_states
        except SystemExit:
            failures.append(f"parse_args({list(argv)}) refused")
            continue
        if got != states:
            failures.append(f"parse_args({list(argv)}).resident_states is {got}, wanted {states}")
    try:
        dec.parse_args(["--resident", "--resident-states"])
        failures.append("the retired --resident-states flag was accepted")
    except SystemExit as e:
        if e.code == 0:
            failures.append("--resident-states exited 0")
    args = Namespace(out="/tmp/unused", resident=True, resident_states=True, dump="d")
    try:
        dec.Decoder(args, {"max_len": 4, "programs": []})
        failures.append("Decoder accepted --resident-states with a dump")
    except dec.Refuse as e:
        if "--dump" not in str(e):
            failures.append(f"the dump refusal does not name --dump: {e}")
    except Exception as e:
        failures.append(f"Decoder raised {e!r} instead of refusing resident states with a dump")


def check_states_refusal_hint(failures, decoder):
    """A session refusal of a state pair names --host-states: there is no fallback to host states."""
    def refuse(name, ports, pairs):
        raise qpr.Refuse(f"{name}: session refused STATE a->b: ERR BAD")
    decoder.session.states = refuse
    try:
        decoder.configure(4)
        failures.append("configure did not refuse when the session refused the state pair")
    except dec.Refuse as e:
        if "session refused STATE a->b: ERR BAD" not in str(e) or "--host-states" not in str(e):
            failures.append(f"the refusal lost its cause or does not name --host-states: {e}")
    decoder.close_session()


def main():
    failures = []
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        serve = build_serve(root)
        manifest, ports_dir, anec_dir = fixture(root)
        host_chained, bytes_a = run_decode(make_decoder(root, manifest, ports_dir, anec_dir, serve, False, "a"))
        resident, bytes_b = run_decode(make_decoder(root, manifest, ports_dir, anec_dir, serve, True, "b"))
        if set(host_chained) != set(resident):
            failures.append("the two arms record different keys")
        for key in sorted(set(host_chained) & set(resident)):
            if host_chained[key] != resident[key]:
                failures.append(f"{key}: host-chained {host_chained[key][:12]} != resident-states {resident[key][:12]}")
        if not host_chained:
            failures.append("nothing was recorded")
        if not bytes_b < bytes_a / 5:
            failures.append(f"payload did not shrink: host-chained {bytes_a} B, resident-states {bytes_b} B")
        print(f"decode arms compared over {len(host_chained)} values; bytes to the session {bytes_a} -> {bytes_b}")
        check_states_refusal_hint(failures, make_decoder(root, manifest, ports_dir, anec_dir, serve, True, "h"))
        reset_a, _ = run_decode(make_decoder(root, manifest, ports_dir, anec_dir, serve, False, "c"), between="reset")
        reset_b, _ = run_decode(make_decoder(root, manifest, ports_dir, anec_dir, serve, True, "d"), between="reset")
        for key in sorted(set(reset_a) | set(reset_b)):
            if reset_a.get(key) != reset_b.get(key):
                failures.append(f"{key} (RESET between prompts, no reload): host-chained {str(reset_a.get(key))[:12]} != resident-states {str(reset_b.get(key))[:12]}")
        if reset_a != host_chained:
            failures.append("host-chained arm differs between reload and RESET-only prompt starts")
    check_pairing_and_refusals(failures)
    check_args(failures)
    for f in failures[:20]:
        print("FAIL", f)
    print("RESIDENT-STATES FAIL" if failures else "RESIDENT-STATES PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
