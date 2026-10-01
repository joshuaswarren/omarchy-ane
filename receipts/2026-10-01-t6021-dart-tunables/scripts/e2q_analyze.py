#!/usr/bin/env python3
"""E2' per-group decision: G<mask> arm vs the A-g<mask> arm of the same window; drift = the
next A arm in time (the window after the undo) vs A-g<mask>.

usage: e2q_analyze.py RUNS_DIR
RUNS_DIR holds the copied ab-turn run directories run-A-g<mask>-<UTC>, run-G<mask>-<UTC> and
run-A-gfinal-<UTC>; <mask> is the probe's groups value (1 0x220/0x224, 2 SID words, 4 0x20c).
Arm statistics come from the AfBridgeRun analyze.py (arm(), band()), unchanged."""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "afb", HERE.parent.parent / "2026-10-01-t6021-af-bridge-run/scripts/analyze.py")
afb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(afb)


def arm(path):
    return afb.arm(path) if (path / "DONE").exists() else None


def line(name, a):
    e, q = a["enc"], a["q"]
    return (f"{name}: encoder minmin {e['minmin']:.3f} medmed {e['medmed']:.3f}; prog_020 "
            f"{q['prog_020']['minmin']:.3f} / {q['prog_020']['medmed']:.3f}; prog_006 {q['prog_006']['minmin']:.3f} / "
            f"{q['prog_006']['medmed']:.3f}; bad encoder processes {a['bad'] or 'none'}")


def main():
    runs = sorted(Path(sys.argv[1]).glob("run-*"), key=lambda p: p.name.rsplit("-", 1)[1])
    stamp = {p: p.name.rsplit("-", 1)[1] for p in runs}
    a_runs = [p for p in runs if p.name.startswith("run-A-g")]
    for g in (p for p in runs if p.name.startswith("run-G")):
        mask = g.name.split("-")[1][1:]
        ctl = max((p for p in a_runs if p.name.startswith(f"run-A-g{mask}-") and stamp[p] < stamp[g]),
                  key=stamp.get)
        nxt = next((p for p in a_runs if stamp[p] > stamp[g]), None)
        print(f"== groups={mask} ({g.name})")
        c, t = arm(ctl), arm(g)
        if c is None:
            print(f"control {ctl.name} failed: no verdict")
            continue
        print(line(ctl.name, c))
        if t is None:
            print(f"{g.name}: arm FAILED (stop): no timing verdict")
            continue
        print(line(g.name, t))
        bands = []
        for k in ("minmin", "medmed"):
            drop = 1 - t["enc"][k] / c["enc"][k]
            bands.append(afb.band(drop))
            print(f"encoder {k} drop {drop * 100:+.2f}% -> {bands[-1]}")
        verdict = bands[0] if bands[0] == bands[1] else f"unresolved (minmin {bands[0]}, medmed {bands[1]})"
        n = arm(nxt) if nxt else None
        if n is None:
            verdict += f"; drift not measured ({nxt.name if nxt else 'no next A arm'} missing or failed)"
        else:
            drift = n["enc"]["minmin"] / c["enc"]["minmin"] - 1
            print(f"drift {nxt.name} vs control minmin {drift * 100:+.3f}%")
            if abs(drift) > 0.01:
                verdict = f"unresolved (drift {drift * 100:+.2f}%)"
        for p in ("prog_020", "prog_006"):
            print(f"{p} minmin ratio G/A {t['q'][p]['minmin'] / c['q'][p]['minmin']:.4f}")
        print(f"VERDICT {verdict}")
    for p in a_runs:
        if p.name.startswith("run-A-gfinal-") and arm(p):
            print(line(p.name, arm(p)))


if __name__ == "__main__":
    main()
