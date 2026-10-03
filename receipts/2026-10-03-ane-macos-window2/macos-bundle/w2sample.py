#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Window-2 register sampler around an ANE runner (macOS side), plus the shared series parser.

  w2sample.py ranges MASTER --blocks t1,set --mode gated|raw [--poll-us N]   -> ranges file on stdout
  w2sample.py series --out DIR [--cli CLI --ranges R] [--count N] [--period-us P] [--until-up]
                     [--pre S] [--post S] [--json F] [-- RUNNER ARGS...]
  w2sample.py show SERIES.bin                                              -> TSV on stdout
  w2sample.py check DIR --block B      exit 0 iff the last islands-up dump under DIR read every
                                       range of block B with status ok
  w2sample.py dtrace-gen FBT_LISTING                                       -> D script on stdout

series: without a runner, one `aneregdump DIR R COUNT PERIOD_US` loop. With a runner, the loop
starts first, the runner starts after --pre s, the loop stops --post s after the runner exits.
--until-up (first exposure of a block): the runner starts first, then single dumps (the ranges
file's own poll budget) until one dump reports islands_up, at most --count tries.
Events (spawn/exit, CLOCK_REALTIME ns, the clock the CLI stamps) go to DIR/events.tsv. Run as root
for the kext; the runner then drops to $SUDO_USER (window 1 ran the ANE runner as the user).
W2_SINGLE_CLI=<window-1 aneregdump> replaces the CLI loop with single dumps of that proven CLI
(same record format, slower cadence): the fallback when the series mode fails its on-box self-test.
"""
import glob
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

BLOCKS = {"t1": ("fabric-ps", "dcs-ps"), "set": ("set-",), "clk": ("clk6",),
          "perf1": ("pll-", "dev-", "ev-"), "dvfm": ("ane0-",),
          "opp": ("opp-",), "ctx": ("ctx-",)}  # window 4: the op-point token word and its two neighbours
TIER1 = BLOCKS["t1"]
STATUS = {0: "ok", 1: "gated", 2: "nomap", 3: "absent"}
REC = struct.Struct("<QQII")                  # aneregdump.c series record head
HDR = struct.Struct("<6I8IQQ8IIIQ")           # struct ane_dump_hdr up to range[]
RANGE = struct.Struct("<24sQQIIIIII")         # struct ane_range_rec


def block_of(name):
    for b, prefixes in BLOCKS.items():
        if name.startswith(prefixes):
            return b
    raise ValueError(f"range {name} is in no block")


def ranges(master, blocks, mode, poll_us=None):
    """Subset of the master file: header lines kept, range lines filtered by block, tier 2
    gated (mode gated) or not (mode raw), poll_us replaced when given."""
    out = []
    for line in open(master):
        f = line.split()
        if not f or line.startswith("#"):
            continue
        if f[0] == "range":
            if block_of(f[1]) not in blocks:
                continue
            gated = mode == "gated" and not f[1].startswith(TIER1)
            line = " ".join(f[:4] + (["gated"] if gated else [])) + "\n"
        elif f[0] == "poll_us" and poll_us is not None:
            line = f"poll_us {poll_us:#x}\n"
        out.append(line)
    return "".join(out)


def read_series(path):
    """Yield one dict per record; a truncated tail record (a killed loop) is dropped."""
    data = open(path, "rb").read()
    pos = 0
    while pos + REC.size <= len(data):
        t0, t1, kr, n = REC.unpack_from(data, pos)
        pos += REC.size
        if pos + n > len(data):
            break
        s = {"t0": t0, "t1": t1, "kr": kr, "islands_up": None, "ps": [], "ranges": {}}
        if n:
            h = HDR.unpack_from(data, pos)
            hdr_bytes, nranges = h[2], h[3]
            s["islands_up"], s["ps"] = h[5], list(h[6:14])
            s["poll_iters"], s["poll_us"] = h[24], h[25]
            for i in range(nranges):
                name, pa, _src, ln, off, st, _b, _fl, _p = RANGE.unpack_from(
                    data, pos + HDR.size + i * RANGE.size)
                raw = data[pos + hdr_bytes + off: pos + hdr_bytes + off + ln]
                s["ranges"][name.rstrip(b"\0").decode()] = (
                    pa, STATUS.get(st, str(st)), list(struct.unpack(f"<{ln // 4}I", raw)))
        pos += n
        yield s


def show(path):
    print("t0_ns\tcall_us\tkr\tislands_up\tps\trange\tpa\tstatus\twords")
    for s in read_series(path):
        ps = " ".join(f"{w:#x}" for w in s["ps"])
        head = f"{s['t0']}\t{(s['t1'] - s['t0']) // 1000}\t{s['kr']}\t{s['islands_up']}\t{ps}"
        for name, (pa, st, words) in s["ranges"].items():
            print(f"{head}\t{name}\t{pa:#x}\t{st}\t{' '.join(f'{w:#010x}' for w in words)}")
        if not s["ranges"]:
            print(f"{head}\t-\t-\t-\t-")


def check(d, block):
    """True when the last islands-up record under d (every series.bin, name order) read every
    range of `block` with status ok."""
    up = [s for p in sorted(glob.glob(os.path.join(d, "**", "series.bin"), recursive=True))
          for s in read_series(p) if s["islands_up"]]
    if not up:
        return False
    mine = [st for n, (_pa, st, _w) in up[-1]["ranges"].items() if block_of(n) == block]
    return bool(mine) and all(st == "ok" for st in mine)


class SingleLoop:
    """The CLI series loop emulated with one process per dump of a single-dump aneregdump; its
    dump.bin is the same header + data a series record carries. Popen-shaped for series()."""

    def __init__(self, cli, d, ranges, count, period_us):
        self.halt = threading.Event()
        self.t = threading.Thread(target=self.run, args=(cli, d, ranges, count, period_us))
        self.t.start()

    def run(self, cli, d, ranges, count, period_us):
        one = os.path.join(d, "one")
        with open(os.path.join(d, "series.bin"), "ab") as f:
            for _ in range(count):
                if self.halt.is_set():
                    break
                os.makedirs(one, exist_ok=True)
                t0 = time.time_ns()
                rc = subprocess.run([cli, one, ranges], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL).returncode
                t1 = time.time_ns()
                dump = os.path.join(one, "dump.bin")
                body = open(dump, "rb").read() if rc in (0, 3) and os.path.exists(dump) else b""
                f.write(REC.pack(t0, t1, 0 if body else rc, len(body)) + body)
                f.flush()
                shutil.rmtree(one)
                if period_us:
                    time.sleep(period_us / 1e6)

    def send_signal(self, _sig):
        self.halt.set()

    def wait(self):
        self.t.join()
        return 0


def series(a, runner):
    os.makedirs(a.out, exist_ok=True)
    ev = open(os.path.join(a.out, "events.tsv"), "a")

    def event(what, detail=""):
        ev.write(f"{time.time_ns()}\t{what}\t{detail}\n")
        ev.flush()

    def loop(sub, count, period):
        d = os.path.join(a.out, sub)
        os.makedirs(d, exist_ok=True)
        single = os.environ.get("W2_SINGLE_CLI")
        event("cli-start", f"{sub} single={single}" if single else sub)
        if single:
            return SingleLoop(single, d, a.ranges, count, period)
        return subprocess.Popen([a.cli, d, a.ranges, str(count), str(period)],
                                stdout=open(os.path.join(d, "cli.out"), "w"), stderr=subprocess.STDOUT)

    def spawn():
        cmd = runner
        if os.geteuid() == 0 and os.environ.get("SUDO_USER"):
            cmd = ["sudo", "-n", "-u", os.environ["SUDO_USER"]] + cmd
        out = open(a.json or os.path.join(a.out, "runner.json"), "w")
        p = subprocess.Popen(cmd, stdout=out, stderr=open(os.path.join(a.out, "runner.err"), "w"))
        event("runner-spawn", " ".join(runner))
        return p

    def finish(p, what):
        rc = p.wait()
        event(what, f"rc={rc}")
        return rc

    if a.until_up:
        p = spawn()
        rc = 3
        for i in range(1, a.count + 1):
            finish(loop(f"a{i}", 1, 0), f"cli-exit a{i}")
            recs = list(read_series(os.path.join(a.out, f"a{i}", "series.bin")))
            if recs and recs[-1]["islands_up"]:
                event("up", f"a{i}")
                rc = 0
                break
        finish(p, "runner-exit")
        return rc
    if not runner:
        return finish(loop("s", a.count, a.period_us), "cli-exit")
    c = loop("s", a.count, a.period_us) if a.cli else None
    time.sleep(a.pre)
    rrc = finish(spawn(), "runner-exit")
    time.sleep(a.post)
    rc = 0
    if c:
        c.send_signal(signal.SIGTERM)
        rc = finish(c, "cli-exit")
    return rc or rrc


def dtrace_gen(listing, max_clauses=300):
    """fbt entry clauses for the ANE perf path, generated from the box's own `dtrace -l -P fbt`
    (a probe name that does not exist stops dtrace; window 9's lesson)."""
    rules = [  # tag, module pattern, function pattern, args printed
        ("P", r"PMGR", r"writeReg32|[Pp]erfState|DVFS|Dvfs", 4),
        ("C", r"CLPC|PerformanceController|PerfControl", r"[Aa]ne|ANE", 4),
        ("H", r"H1\dANE|AppleANE", r"[Pp]erf|DVFS|[Cc]lock|[Ff]req|PowerO(n|ff)", 3),
    ]
    picked = {tag: set() for tag, *_ in rules}
    for line in open(listing):
        f = line.split()
        if len(f) < 5 or f[1] != "fbt" or f[-1] != "entry":  # a "[demangled]" column may sit before the kind
            continue
        mod, fn = f[2], f[3]
        if not re.fullmatch(r"[A-Za-z0-9_]+", fn) or "MetaClass" in fn:
            continue
        for tag, mpat, fpat, n in rules:
            if re.search(mpat, mod) and re.search(fpat, fn):
                picked[tag].add((mod, fn, n))
                break
    out = ["#pragma D option quiet", "#pragma D option bufsize=64m", "#pragma D option switchrate=20hz",
           'dtrace:::BEGIN { printf("TRACE-START w=%llu\\n", (unsigned long long)walltimestamp); }']
    clauses = [(t, m, f, n) for t, *_ in rules for (m, f, n) in sorted(picked[t])][:max_clauses]
    for tag, mod, fn, n in clauses:
        fmt = " ".join(f"a{i}=0x%llx" for i in range(1, n + 1))
        args = ", ".join(f"(unsigned long long)arg{i}" for i in range(1, n + 1))
        out.append(f'fbt:{mod}:{fn}:entry {{ printf("{tag} w=%llu %s {fmt}\\n", '
                   f"(unsigned long long)walltimestamp, probefunc, {args}); }}")
    out.append("tick-120s { exit(0); }")
    return "\n".join(out) + "\n", len(clauses)


def main(argv):
    import argparse
    runner = []
    if "--" in argv:
        i = argv.index("--")
        argv, runner = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("ranges")
    r.add_argument("master")
    r.add_argument("--blocks", required=True)
    r.add_argument("--mode", choices=("gated", "raw"), required=True)
    r.add_argument("--poll-us", type=lambda v: int(v, 0))
    s = sub.add_parser("series")
    s.add_argument("--out", required=True)
    s.add_argument("--cli")
    s.add_argument("--ranges")
    s.add_argument("--count", type=int, default=100000)
    s.add_argument("--period-us", type=int, default=2000)
    s.add_argument("--until-up", action="store_true")
    s.add_argument("--pre", type=float, default=0.0)
    s.add_argument("--post", type=float, default=0.0)
    s.add_argument("--json")
    sub.add_parser("show").add_argument("series")
    c = sub.add_parser("check")
    c.add_argument("dir")
    c.add_argument("--block", required=True, choices=sorted(BLOCKS))
    sub.add_parser("dtrace-gen").add_argument("listing")
    a = ap.parse_args(argv)
    if a.cmd == "ranges":
        blocks = set(a.blocks.split(","))
        if blocks - set(BLOCKS):
            raise SystemExit(f"unknown blocks {sorted(blocks - set(BLOCKS))}")
        sys.stdout.write(ranges(a.master, blocks, a.mode, a.poll_us))
        return 0
    if a.cmd == "show":
        show(a.series)
        return 0
    if a.cmd == "check":
        return 0 if check(a.dir, a.block) else 1
    if a.cmd == "dtrace-gen":
        text, n = dtrace_gen(a.listing)
        sys.stdout.write(text)
        print(f"clauses={n}", file=sys.stderr)
        return 0 if n else 1
    if bool(a.cli) != bool(a.ranges):
        raise SystemExit("--cli and --ranges go together")
    if a.until_up and not (runner and a.cli):
        raise SystemExit("--until-up needs --cli, --ranges and a runner")
    if not (runner or a.cli):
        raise SystemExit("series: nothing to run")
    return series(a, runner)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
