#!/usr/bin/env python3
"""Host checks for the H13 Qwen decode state worker (tools/h13_qwen_decode.py).

The worker chains each program's state output into its own state input on one
background thread: a call that follows a chained one skips the state surfaces
(state in was staged and sent by the worker, state out comes back deferred), and
the staged bytes equal the packed read of the device's state output. Worker off,
the call is the old whole-set path: everything sent, everything read. The state
pair table must match each state's ports exactly once.
"""

import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import h13_qwen_decode as hd

F16 = hd.f16
STATE_SHAPE = (1, 2, 2, 4)
X_SHAPE = (1, 1, 2, 4)


def port(name, shape):
    """A contiguous strided-surface port and its byte allocation."""
    n, c, h, w = shape
    row = w * 2
    plane = h * row
    return {"name": name, "shape": list(shape), "strides": [0, plane, row, 2]}, n * c * h * w * 2


STATE_IN, STATE_ALLOC = port("state_in", STATE_SHAPE)
X_PORT, X_ALLOC = port("x", X_SHAPE)
HIDDEN, HIDDEN_ALLOC = port("hidden", X_SHAPE)
STATE_OUT, STATE_OUT_ALLOC = port("state_out", STATE_SHAPE)
PLAN = ([STATE_IN, X_PORT], [STATE_ALLOC, X_ALLOC], [HIDDEN, STATE_OUT])


class FakeProgram:
    """Stands in for PreparedProgram: records the one-surface calls and answers
    reads. state_bytes is what the device answers for output surface 1."""

    def __init__(self, state_bytes=None):
        self.state_bytes = state_bytes
        self.sent, self.staged, self.reads, self.submits = [], [], [], 0

    def send_surface(self, index, data):
        assert len(data) == PLAN[1][index], (index, len(data))
        self.sent.append(index)

    def stage_surface(self, index, data):
        assert len(data) == PLAN[1][index], (index, len(data))
        self.staged.append((index, bytes(data)))

    def read_surface(self, index):
        self.reads.append(index)
        if index == 1 and self.state_bytes is not None:
            return self.state_bytes
        return bytes(HIDDEN_ALLOC if index == 0 else STATE_OUT_ALLOC)

    def submit(self):
        self.submits += 1


def decoder(prog, pairs):
    """An H13Decoder skeleton with one program: no device, no package."""
    d = object.__new__(hd.H13Decoder)
    d.plan = [PLAN]
    d.prepared = [prog]
    d.state_pairs = [pairs]
    d.state_slots = [(frozenset(k for k, _, _ in pairs), frozenset(o for _, o, _ in pairs))]
    d.chained = {}
    d.state_wait_s = 0.0
    d._state_packs = {}
    d.worker_pool =  ThreadPoolExecutor(max_workers=1, thread_name_prefix="t")
    return d


def arrays():
    return {"state_in": np.zeros(STATE_SHAPE, dtype=F16), "x": np.ones(X_SHAPE, dtype=F16)}


def test_first_call_sends_every_surface_and_defers_the_state_output():
    state_bytes = (np.arange(16).reshape(STATE_SHAPE).astype(F16)).tobytes()
    prog = FakeProgram(state_bytes=state_bytes)
    d = decoder(prog, [(0, 1, STATE_SHAPE)])

    outs, _ms = d.call(0, arrays())
    assert prog.sent == [0, 1] and prog.submits == 1
    assert prog.reads == [0, 1, 1]  # the host read both outputs, then the worker re-read the state to chain it
    assert isinstance(outs["hidden"], np.ndarray) and outs["hidden"].shape == X_SHAPE
    assert isinstance(outs["state_out"], np.ndarray)

    chained = d.chained.pop(0)
    chained.result()
    assert prog.staged == [(0, state_bytes)]  # the worker staged the packed state for the next call


def test_chained_call_skips_the_state_surfaces_and_awaits_the_worker():
    state_bytes = (np.arange(16).reshape(STATE_SHAPE).astype(F16)).tobytes()
    prog = FakeProgram(state_bytes=state_bytes)
    d = decoder(prog, [(0, 1, STATE_SHAPE)])

    d.call(0, arrays())
    # the step loop stores the deferred state under "state_in"; the call must skip it
    outs, _ms = d.call(0, {"state_in": d.chained, "x": np.ones(X_SHAPE, dtype=F16)})
    assert prog.sent == [0, 1, 1]  # the second call sent x but not state_in
    assert len(prog.staged) == 1  # the worker staged the state before the call returned
    assert isinstance(outs["state_out"], hd.Deferred)
    assert 0 in d.chained  # this step's chain job is pending
    d.drain()
    assert len(prog.staged) == 2


def test_drain_lets_a_fresh_prompt_send_states_again():
    prog = FakeProgram()
    d = decoder(prog, [(0, 1, STATE_SHAPE)])

    d.call(0, arrays())
    d.drain()  # what reset() does before zeroing the host states
    d.call(0, arrays())
    assert prog.sent == [0, 1, 0, 1]  # the fresh prompt sent the state again, not skipped
    assert 0 in d.chained
    d.drain()


def test_worker_off_is_the_whole_set_path():
    prog = FakeProgram()
    d = decoder(prog, [(0, 1, STATE_SHAPE)])
    d.worker_pool = None

    outs, _ms = d.call(0, arrays())
    assert prog.sent == [0, 1]
    assert prog.reads == [0, 1]
    assert isinstance(outs["state_out"], np.ndarray)
    assert d.chained == {}


def test_state_pairs_must_match_exactly_one_surface():
    pr = {"states": [
        {"in_port": "state_in", "out_port": "state_out", "in_shape": list(STATE_SHAPE)},
        {"in_port": "state_in", "out_port": "missing", "in_shape": list(STATE_SHAPE)},
    ]}
    try:
        hd.state_pairs(pr, PLAN[0], PLAN[2], 3)
    except hd.dec.Refuse as error:
        assert "program 3" in str(error) and "missing" in str(error)
    else:
        raise AssertionError("a state pair without exactly one dst surface was accepted")
