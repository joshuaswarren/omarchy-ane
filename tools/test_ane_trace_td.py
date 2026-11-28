#!/usr/bin/env python3
"""Self-test for tools/ane_trace_td.py: trace parse, per-call rows, take curve.

Feeds it a synthetic trace_td blob (ATD1 header + CALL/ACK/TD/EVENT/DONE
records) and a small ANEC stream, and pins: the record decode, per-call
grouping with nid filtering and stale samples, the even-spread take-time
model over skipped indices, and the census rows (idx, ne gate, MACs,
kDMA bytes, src/dst bytes). report() runs end to end on the fixture.
"""
import importlib.util
import io
import struct
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ane_trace_td", REPO / "tools/ane_trace_td.py")
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)

CALL, ACK, TD, EVENT, GATE, DONE = range(1, 7)
NID = 5


def trace(records, cap=1024, dropped=0, calls=1):
    head = struct.pack("<6I", 0x31445441, 16, cap, len(records), dropped, calls) + bytes(8)
    return head + b"".join(struct.pack("<QIHH", t, w, k, c) for t, w, k, c in records)


def td_rec(t, index, nid=NID, call=0):
    return (t, (nid << 16) | index, TD, call)


def call_records(call, ack_t, tds, fin_t=None, nid=NID):
    recs = [(ack_t - 10, 0, CALL, call), (ack_t, 0, ACK, call)]
    recs += [(t, (nid << 16) | i, TD, call) for t, i in tds]
    if fin_t:
        recs.append((fin_t, 1, EVENT, call))
        recs.append((fin_t + 5, 4, DONE, call))
    return recs


def dense(reg, *values):
    # a non-BAR dense record: bit31 clear, count = len(values), written regs
    # are consecutive from reg/4
    header = ((len(values) - 1) << 15) | (reg // 4)
    return [header, *values]


def scattered(regs_values):
    # a scattered record: bit31 set, mask bits pick the regs after the base
    regs = sorted(regs_values)
    base = regs[0]
    mask = 0
    for r in regs[1:]:
        mask |= 1 << (r - base) // 4 - 1
    header = 0x80000000 | (mask << 15) | (base // 4)
    return [header] + [regs_values[r] for r in regs]


def task(idx, *records):
    body = []
    for rec in records:
        body += list(rec)
    words = [idx | ((8 + len(body)) << 16)] + [0] * 7 + body
    return struct.pack(f"<{len(words)}I", *words)


def anec(*tasks):
    # each task occupies (words*4 + 15) & ~15 bytes: pad to the boundary
    stream = b"".join(t.ljust((len(t) + 15) & ~15, b"\0") for t in tasks)
    stream = stream.ljust((len(stream) + 63) // 64 * 64, b"\0")
    head = bytearray(0x1000)
    struct.pack_into("<Q", head, 0x10, len(stream))
    return bytes(head) + stream


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as tmp:
        blob = trace([(95, (9 << 16) | 3, TD, 0)]        # a stale TD from a previous nid
                     + call_records(0, 110, [(120, 0), (130, 2), (140, 4)], fin_t=150)
                     + call_records(1, 210, [(220, 1), (230, 4)], fin_t=250)
                     + [(300, 0, CALL, 9)],              # a call never ACKed is skipped
                     calls=2)
        path = Path(tmp) / "trace_td"
        path.write_bytes(blob)

        hdr, recs = td.read_trace(path)
        check("header", (hdr["n"], hdr["dropped"], hdr["calls"], hdr["cap"]), (15, 0, 2, 1024))

        cls = td.per_call(recs)
        check("calls grouped", [c["call"] for c in cls], [0, 1])
        check("own td", [i for t, i in cls[0]["own"]], [0, 2, 4])
        check("stale counted", len(cls[0]["stale"]), 1)
        check("fin times", (cls[0]["fin"], cls[0]["ack"]), (150, 110))
        check("done rec", cls[0]["done"], (155, 4))
        check("call without ack skipped", all(c["call"] != 9 for c in cls), True)

        # take times spread evenly over skipped indices
        times = td.take_times(cls[0], 6)
        want = np.array([10, 15, 20, 25, 30, np.nan]) / 1e6
        check("take times", np.isclose(times, want, equal_nan=True).all(), True)

        # census rows from a small ANEC
        # task 0: registers 0 (inW|inH), 0xC (inC), 0x10 (outC), 0x14 (outW|outH),
        #         0x3C (ne flag), 0x1100 (src), 0x1500 (dst)
        dims = {0: (2 << 16) | 4, 0xC: 8, 0x10: 16, 0x14: (6 << 16) | 3,
                0x3C: 0x100000}
        t0 = task(0, scattered(dims), dense(0x1100, 1), dense(0x1500, 1))
        t1 = task(1, scattered({0: (1 << 16) | 1, 0xC: 1, 0x10: 1, 0x14: (1 << 16) | 1}))
        anec_path = Path(tmp) / "p.anec"
        anec_path.write_bytes(anec(t0, t1))
        rows = td.census(anec_path)
        check("census rows", len(rows), 2)
        check("idx", [r["idx"] for r in rows], [0, 1])
        check("ne state persists", [r["ne"] for r in rows], [True, True])
        check("macs", rows[0]["macs"], 8 * 16 * 3 * 6)
        check("sbytes", rows[0]["sbytes"], 8 * 4 * 2 * 2)
        check("dbytes", rows[0]["dbytes"], 16 * 3 * 6 * 2)

        # report runs the whole path without raising (no anec: the
        # fixture's 5 traced tasks do not match the 2-row census)
        out = io.StringIO()
        with redirect_stdout(out):
            td.report(str(path))
        text = out.getvalue()
        check("report tasks", "tasks 5" in text, True)
        check("report calls", "traced calls 2" in text, True)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_ane_trace_td: {len(fails)} failed")
        return 1
    print("test_ane_trace_td: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
