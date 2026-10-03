#!/usr/bin/env python3
"""Per-process ANE program-open battery (Parakeet whole encoder, M1 ABI 1).

Each run starts a fresh `mlx-omarchy-ane-worker --serve` with the CLI's
bundle set, waits for "resident loaded", sends "quit" and waits for
"resident released". That is the whole per-process open the packaged CLI
pays (seal, fork, dlopen, ane_init) and nothing else: no submit, no input
tensor, no GPU work. A completed open is followed by the normal release.

Every run takes /var/tmp/ane-run.lock (wait <= 120 s) after the idle gate
(load1 < 0.5 and PSI cpu some avg10 = 0.00, wait <= 120 s); both values are
recorded per run and an unmet gate stops the battery. The worker prints
MLX_OMARCHY_OPEN_TIMING lines; a libane built with ANE_TRACE_TIMING
support adds per-stage lines; --strace adds per-syscall times for the
ioctl/mmap path. See receipts/2026-10-03-ane-cold-start/README.md.
"""

import argparse
import fcntl
import glob
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

LOCK = "/var/tmp/ane-run.lock"
GATE_S = 120

WORKER_RE = re.compile(
    r"\[omarchy-ane\] (?:timing bundle (?P<bundle>\S+) load"
    r"|(?P<libane>timing libane seal)"
    r"|(?P<factory>child device factory)"
    r"|child load program (?P<prog>\d+) \([^)]*\)"
    r"|(?P<handshake>parent open handshake)) (?P<ms>[0-9.]+) ms")
LIBANE_RE = re.compile(
    r"LIBANE: TIMING stage=(?P<stage>\S+) ms=(?P<ms>[0-9.]+) "
    r"bytes=(?P<bytes>\d+)")
# strace -e raw=ioctl: ioctl(0x5, 0xc0186441, 0xffff...) = 0 <0.081234>
IOCTL_RE = re.compile(r"ioctl\(0x[0-9a-f]+, 0x(?P<req>[0-9a-f]+),.*<(?P<s>[0-9.]+)>$")
MMAP_RE = re.compile(
    r"mmap\(\w+, (?P<len>\d+), [^,]+, (?P<flags>[^,]+), (?P<fd>-?\d+),"
    r".*<(?P<s>[0-9.]+)>$")
DRM_NR = {0x00: "drm_version", 0x41: "bo_init", 0x42: "bo_free",
          0x43: "submit"}


def idle_ok(loadavg_text, psi_text):
    """(ok, load1, avg10) for the idle-benchmark rule."""
    load1 = float(loadavg_text.split()[0])
    some = next(line for line in psi_text.splitlines()
                if line.startswith("some"))
    avg10 = float(re.search(r"avg10=([0-9.]+)", some).group(1))
    return load1 < 0.5 and avg10 == 0.0, load1, avg10


def parse_worker(text):
    """MLX_OMARCHY_OPEN_TIMING lines -> {stage: ms}."""
    out = {}
    for m in WORKER_RE.finditer(text):
        if m["bundle"]:
            key = f"seal:{m['bundle']}"
        elif m["libane"]:
            key = "seal:libane"
        elif m["factory"]:
            key = "child_dlopen"
        elif m["handshake"]:
            key = "fork_to_loaded"
        else:
            key = f"ane_init:{m['prog']}"
        out[key] = float(m["ms"])
    return out


def parse_libane(text):
    """ANE_TRACE_TIMING lines -> one {stage: ms, 'bytes': n} per program.

    The child loads programs one after another; each ends with init_total.
    """
    programs, cur = [], {}
    for m in LIBANE_RE.finditer(text):
        stage, ms = m["stage"], float(m["ms"])
        if stage == "exec":
            continue
        cur[stage] = cur.get(stage, 0.0) + ms
        if stage == "init_total":
            cur["bytes"] = int(m["bytes"])
            programs.append(cur)
            cur = {}
    return programs


def parse_strace(text):
    """strace -T -e raw=ioctl lines -> {class: [count, total_ms, max_ms]}."""
    out = {}

    def add(key, s):
        c = out.setdefault(key, [0, 0.0, 0.0])
        c[0] += 1
        c[1] += s * 1e3
        c[2] = max(c[2], s * 1e3)

    for line in text.splitlines():
        m = IOCTL_RE.search(line)
        if m:
            req = int(m["req"], 16)
            nr = req & 0xFF if (req >> 8) & 0xFF == 0x64 else None
            add(f"ioctl:{DRM_NR.get(nr, hex(req))}", float(m["s"]))
            continue
        m = MMAP_RE.search(line)
        if m and int(m["len"]) >= 1 << 20:
            kind = "shared_fd" if "MAP_SHARED" in m["flags"] and m["fd"] != "-1" else "other"
            add(f"mmap>=1MiB:{kind}", float(m["s"]))
    return out


def read(path):
    try:
        return Path(path).read_text()
    except OSError as error:
        return f"unavailable: {error}"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ane_state():
    """Runtime-PM status and ane_stats of every bound ANE device."""
    return {p: read(p).strip()
            for pat in ("/sys/bus/platform/drivers/ane/*/power/runtime_status",
                        "/sys/bus/platform/drivers/ane/*/ane_stats")
            for p in sorted(glob.glob(pat))}


def wait_idle():
    deadline = time.monotonic() + GATE_S
    while True:
        ok, load1, avg10 = idle_ok(read("/proc/loadavg"),
                                   read("/proc/pressure/cpu"))
        if ok or time.monotonic() > deadline:
            return ok, load1, avg10
        time.sleep(2)


def take_lock():
    fd = os.open(LOCK, os.O_RDWR | os.O_CREAT, 0o666)
    deadline = time.monotonic() + GATE_S
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return fd
        except BlockingIOError:
            if time.monotonic() > deadline:
                os.close(fd)
                return None
            time.sleep(0.5)


def session_args(args):
    """Worker argv tail for the CLI's bundle set, sealed like the CLI."""
    pin = json.loads((args.share / "parakeet-runtime-pin.json").read_text())
    tail = []
    for name, files in sorted(pin["assets"]["bundles"].items()):
        if name == "parakeet-encoder-whole":
            path = args.whole
        elif args.no_islands:
            continue
        else:
            path = args.share / "bundles" / name
        tail += ["--bundle", f"{name}={path}"]
        for file, digest in sorted(files.items()):
            tail += ["--seal-expect", f"{name}:{file}={digest}"]
    libane_sha = sha256(args.libane)
    for file, digest in pin["assets"]["libane"].items():
        if args.libane.name == file and libane_sha == digest:
            tail += ["--seal-expect-libane", f"{file}={digest}"]
    return tail, libane_sha


def environment(args, libane_sha):
    lines = [f"utc {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}",
             f"uname {' '.join(platform.uname())}",
             f"boot_id {read('/proc/sys/kernel/random/boot_id').strip()}",
             f"cmdline {read('/proc/cmdline').strip()}",
             f"page_size {os.sysconf('SC_PAGESIZE')}",
             f"thp_enabled {read('/sys/kernel/mm/transparent_hugepage/enabled').strip()}",
             f"thp_defrag {read('/sys/kernel/mm/transparent_hugepage/defrag').strip()}",
             f"worker {args.worker} sha256 {sha256(args.worker)}",
             f"libane {args.libane} sha256 {libane_sha}",
             f"whole {args.whole} program-0.anec sha256 "
             f"{sha256(args.whole / 'program-0.anec')}"]
    for p in sorted(glob.glob("/sys/module/ane/parameters/*")):
        lines.append(f"param {Path(p).name} {read(p).strip()}")
    for p in ("/sys/module/ane/version", "/sys/module/ane/srcversion"):
        lines.append(f"{Path(p).name} {read(p).strip()}")
    for p in sorted(glob.glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"))[:1]:
        lines.append(f"governor {read(p).strip()}")
    mount = max((line.split() for line in read("/proc/mounts").splitlines()
                 if str(args.whole.resolve()).startswith(line.split()[1])),
                key=lambda f: len(f[1]), default=["?", "?", "?"])
    lines.append(f"whole_fs {mount[1]} {mount[2]}")
    for k, v in ane_state().items():
        lines.append(f"pre {k} {v!r}")
    return "\n".join(lines) + "\n"


def one_run(args, tail, run_dir):
    run_dir.mkdir(parents=True)
    argv = [str(args.worker), "--serve", "--libane", str(args.libane),
            "--deadline-ms", str(args.deadline_ms)] + tail
    if args.strace:
        # --seccomp-bpf: only the traced calls stop, not the ~14,000 seal
        # read/write calls, so the open stays close to the untraced arm.
        argv = ["strace", "-ff", "--seccomp-bpf", "-T", "-ttt",
                "-e", "raw=ioctl",
                "-e", "trace=openat,memfd_create,ioctl,mmap,munmap",
                "-o", str(run_dir / "strace")] + argv
    env = dict(os.environ, MLX_OMARCHY_OPEN_TIMING="1", ANE_TRACE_TIMING="1")
    (run_dir / "argv.json").write_text(json.dumps(argv, indent=1) + "\n")
    result = {"pre": ane_state()}
    stderr = open(run_dir / "stderr.txt", "wb")
    started = time.monotonic_ns()
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=stderr, env=env, text=True)
    # The worker bounds its own handshake by --deadline-ms; this timer
    # bounds the rest of the run (seal, release).
    limit_s = args.deadline_ms / 1000 + 10
    timer = threading.Timer(limit_s, proc.kill)
    timer.start()
    lines = []
    try:
        for line in proc.stdout:
            lines.append(line)
            if line.startswith("resident loaded"):
                result["open_ms"] = (time.monotonic_ns() - started) / 1e6
                break
        if "open_ms" in result:
            released = time.monotonic_ns()
            proc.stdin.write("quit\n")
            proc.stdin.flush()
            lines += proc.stdout.readlines()
            result["release_ms"] = (time.monotonic_ns() - released) / 1e6
        result["rc"] = proc.wait()
    finally:
        timer.cancel()
    if result["rc"] == -9:
        result["rc"] = f"killed by the harness after {limit_s} s"
    stderr.close()
    result["wall_ms"] = (time.monotonic_ns() - started) / 1e6
    result["post"] = ane_state()
    (run_dir / "stdout.txt").write_text("".join(lines))
    err = (run_dir / "stderr.txt").read_text(errors="replace")
    result["worker"] = parse_worker(err)
    result["libane"] = parse_libane(err)
    if args.strace:
        result["strace"] = parse_strace("".join(
            p.read_text(errors="replace") for p in sorted(run_dir.glob("strace.*"))))
    return result


def summary(results):
    """Median per stage over the valid runs: open, worker lines, and the
    largest program's libane stages (the whole encoder)."""
    rows = {}
    for r in results:
        if r.get("rc") != 0 or "open_ms" not in r:
            continue
        vals = {"open_ms": r["open_ms"], "release_ms": r["release_ms"]}
        vals.update(r["worker"])
        if r["libane"]:
            big = max(r["libane"], key=lambda p: p.get("bytes", 0))
            vals.update({f"libane:{k}": v for k, v in big.items() if k != "bytes"})
        for key, (n, total, _) in r.get("strace", {}).items():
            vals[f"strace:{key}:n"] = n
            vals[f"strace:{key}:ms"] = total
        for k, v in vals.items():
            rows.setdefault(k, []).append(v)
    out = ["stage\tn\tmedian\tmin\tmax"]
    for k, v in rows.items():
        out.append(f"{k}\t{len(v)}\t{statistics.median(v):.3f}\t{min(v):.3f}\t{max(v):.3f}")
    return "\n".join(out) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--label", required=True)
    p.add_argument("--worker", type=Path, required=True)
    p.add_argument("--libane", type=Path, required=True)
    p.add_argument("--share", type=Path, required=True,
                   help="CLI share dir holding parakeet-runtime-pin.json and bundles/")
    p.add_argument("--whole", type=Path,
                   help="parakeet-encoder-whole bundle dir "
                        "(default: SHARE/bundles/parakeet-encoder-whole)")
    p.add_argument("--runs", type=int, default=10)
    p.add_argument("--gap-s", type=float, default=0.0,
                   help="sleep before each run (runtime-PM autosuspend arm)")
    p.add_argument("--strace", action="store_true")
    p.add_argument("--no-islands", action="store_true")
    p.add_argument("--deadline-ms", type=int, default=60000)
    args = p.parse_args()
    args.whole = args.whole or args.share / "bundles" / "parakeet-encoder-whole"

    out = args.out / args.label
    out.mkdir(parents=True, exist_ok=False)
    tail, libane_sha = session_args(args)
    (out / "env.txt").write_text(environment(args, libane_sha))
    results = []
    for i in range(args.runs):
        if args.gap_s:
            time.sleep(args.gap_s)
        ok, load1, avg10 = wait_idle()
        lock = take_lock() if ok else None
        if lock is None:
            results.append({"run": i, "load1": load1, "psi_avg10": avg10,
                            "rc": "idle gate not met" if not ok else "lock timeout"})
            break
        try:
            r = one_run(args, tail, out / f"run-{i:02d}")
        finally:
            os.close(lock)
        r.update(run=i, load1=load1, psi_avg10=avg10)
        results.append(r)
        print(f"{args.label} run {i}: rc={r['rc']} open_ms={r.get('open_ms')}",
              file=sys.stderr)
        if r["rc"] != 0:
            break  # never retry a failed open
    with open(out / "results.jsonl", "w") as f:
        for r in results:
            f.write(json.dumps(r) + "\n")
    (out / "summary.tsv").write_text(summary(results))
    sums = [f"{sha256(f)}  {f.relative_to(out)}" for f in sorted(out.rglob("*"))
            if f.is_file() and f.name != "SHA256SUMS"]
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    print((out / "summary.tsv").read_text())
    return 0 if all(r.get("rc") == 0 for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
