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

    def close(self):
        self.closed = True


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


def open_decoder():
    """A decoder opened through the real worker setup: no device, no package."""
    d = object.__new__(hd.H13Decoder)
    d.plan, d.prepared, d.state_pairs, d.state_slots = [], [], [], []
    d.chained, d.state_wait_s, d._state_packs = {}, 0.0, {}
    d.resident_states = False
    d._start_state_worker()
    return d


def test_switch_interval_survives_out_of_order_close():
    """The switch interval is process-global: the first worker saves the
    original and the last close restores it, so two decoders closing in either
    order leave the original interval behind."""
    original = sys.getswitchinterval()
    try:
        a, b = open_decoder(), open_decoder()
        assert abs(sys.getswitchinterval() - 0.0001) < 1e-12  # setswitchinterval roundtrips through microseconds
        a.close()
        assert abs(sys.getswitchinterval() - 0.0001) < 1e-12  # b's worker is still open
        b.close()
        assert sys.getswitchinterval() == original

        a, b = open_decoder(), open_decoder()
        b.close()
        assert abs(sys.getswitchinterval() - 0.0001) < 1e-12  # the other close order
        a.close()
        assert sys.getswitchinterval() == original
    finally:
        sys.setswitchinterval(original)


def test_close_is_safe_without_a_started_worker_and_twice():
    """close() without a started worker, and a second close of a started one,
    must leave the counter at zero and the interval at the original."""
    original = sys.getswitchinterval()
    try:
        off = decoder(FakeProgram(), [])
        off.worker_pool = None
        off.close()
        assert off.worker_pool is None
        assert sys.getswitchinterval() == original

        d = open_decoder()
        d.close()
        d.close()
        assert d.worker_pool is None
        assert sys.getswitchinterval() == original
    finally:
        sys.setswitchinterval(original)


def test_close_after_a_started_worker_restores_the_interval_and_closes_prepared():
    """The body of __init__'s reset()-failure branch: close() on a started
    decoder with live prepared programs must unwind the process-global counter
    and release the programs."""
    original = sys.getswitchinterval()
    closed = []
    try:
        d = open_decoder()
        d.prepared = [type("P", (), {"close": lambda self: closed.append(1)})()]
        d.close()
        assert closed == [1]
        assert d.worker_pool is None
        assert sys.getswitchinterval() == original
    finally:
        sys.setswitchinterval(original)


def padded_port(name, shape, align=32):
    """A strided-surface port whose rows are padded past the dense shape and
    whose allocation has a tail gap after the last plane. `align` varies the
    row stride without moving the plane or the allocation."""
    n, c, h, w = shape
    row = (w * 2 + align - 1) // align * align
    plane = (h * row + 127) // 128 * 128
    return {"name": name, "shape": list(shape), "strides": [0, plane, row, 2]}, (n * c * plane + 63) // 64 * 64 + 64


def ref_pack_state(dense, port, alloc):
    """Hand-built strided layout, independent of h13_surface: element (i, j, k, m)
    lands at i*c*plane + j*plane + k*row + m*2 in a zeroed buffer."""
    n, c, h, w = port["shape"]
    plane, row = port["strides"][1], port["strides"][2]
    raw = bytearray(alloc)
    values = np.asarray(dense, dtype=np.float16).reshape(n, c, h, w)
    for i in range(n):
        for j in range(c):
            for k in range(h):
                base = i * c * plane + j * plane + k * row
                raw[base:base + w * 2] = values[i, j, k].tobytes()
    return bytes(raw)


def test_pack_state_writes_padded_ports_exactly_and_leaves_the_padding_zero():
    """The worker's staging buffer spans the port's whole allocation: a padded
    port's row gaps and tail gap must stay zero, and the cached buffer must be
    rewritten in place on the second pack of the same port."""
    port, alloc = padded_port("state_in", (2, 3, 4, 5))
    assert alloc > 2 * 3 * 4 * 5 * 2  # the allocation is really padded
    dense = np.arange(2 * 3 * 4 * 5).reshape(2, 3, 4, 5).astype(F16)
    d = decoder(FakeProgram(), [])

    first = d._pack_state(0, dense, port, alloc)
    assert bytes(first) == ref_pack_state(dense, port, alloc)

    second = (np.arange(2 * 3 * 4 * 5)[::-1] % 512).reshape(2, 3, 4, 5).astype(F16)
    repacked = d._pack_state(0, second, port, alloc)
    assert repacked is first  # the cached staging buffer is rewritten in place, not replaced
    assert bytes(repacked) == ref_pack_state(second, port, alloc)

    # same shape and allocation, tighter row padding: a different cache entry, its own buffer
    loose_port, loose_alloc = padded_port("state_in", (2, 3, 4, 5), align=16)
    assert loose_alloc == alloc
    other = d._pack_state(0, dense, loose_port, loose_alloc)
    assert other is not first
    assert bytes(other) == ref_pack_state(dense, loose_port, loose_alloc)


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
