#!/usr/bin/env python3
"""Public-text check: nothing in the tree may show HOW we read macOS, macOS drivers, macOS firmware or macOS restore images.

Rule (2026-10-29): state only observed behaviour. No Apple code addresses, no Apple symbol or bundle names, no extraction or
decode commands, no restore-image locators, no 'decoded from the kext' wording, and (2026-10-30) no hex value on the same line as a macOS,
kext, kernelcache, iBoot or restore-image word: constants stay in code with a neutral comment. History is not rewritten.

Usage:
  check_no_macos_read_methods.py            check the working tree against the baseline; exit 1 on any increase
  check_no_macos_read_methods.py --list     list every match (the scrub backlog)
  check_no_macos_read_methods.py --write-baseline   rewrite the baseline from the current tree (review the diff: it can raise numbers)

The baseline (tools/macos_read_methods_baseline.json) records, per file, how many matching lines the tree had when the rule
started. A file may not gain matches and a new file may not have any. The baseline must equal the tree: when a file gets cleaner, lower its entry
(the check fails until you do, so a cleaned file cannot regain matches). --write-baseline rewrites the file from the tree and
can also RAISE numbers, so review its diff. The patterns are literal: plain words such as kext, kernelcache, ipsw, im4p,
disassembl, dtrace or 'static read' are not matched, so this is a ratchet, not a clean bill.
"""
import json
import os
import re
import subprocess
import sys

PATTERNS = [
    ("kernel address", r"0xfffffe[0-9a-fA-F]{6,}"),
    ("code offset", r"\b(kext|kernelcache|kc)\s*[+@]\s*0x[0-9a-fA-F]+"),
    ("tool or command", r"\b(ipsw\s+(extract|kernel|macho|dl|download)|pyimg4|img4tool|otool\s+-|ghidra|radare2|objdump\s+-d|disarm64|jtool2?)\b"),
    ("restore-image locator", r"kernelcache\.release|\bhdr_off=|__PRELINK_INFO|__TEXT_EXEC|\bBuildManifest\b|updates\.cdn-apple|\.ipsw\b|by HTTP range"),
    ("Apple symbol or bundle", r"\bcom\.apple\.driver\.|\bApple(H1\d|H1\dANE|ASCWrap|A7IOP|T60\d\d|ANE|PMGR|DART)[A-Za-z0-9_]*(::|\b)"),
    ("macOS-sourced value", r"(?i)(macOS|restore[- ]image|\bkext|kernelcache|iBoot|Apple'?s? (firmware|driver|compiler|kernel|ANE firmware)|\bipsw\b).*0x[0-9a-f]{3,}|0x[0-9a-f]{3,}.*(macOS|restore[- ]image|\bkext|kernelcache|iBoot|Apple'?s? (firmware|driver|compiler|kernel|ANE firmware)|\bipsw\b)"),
    ("decode wording", r"(static|offline) decode|decoded (statically )?from the (macOS )?(kext|kernelcache|kernel extension)|reverse[- ]engineer(ed|ing)? (the )?(macOS|kext)"),
]
# Decided 2026-10-29: the tool that installs a Mac's own firmware keeps its two download addresses, in code only.
ALLOWED = {"packaging/omarchy-ane-firmware-fetch"}
# Decided 2026-10-30: captured program output and a run's own source copy are evidence and are never edited. Explicit list, not a glob:
# a new capture that carries the words fails until it is added here on purpose.
CAPTURED = {
    "receipts/2026-09-24-t6021-pwgate/ane_t6021_fwload.c",
    "receipts/2026-09-24-t6021-pwgate/ane_t6021_rtclient_main.c",
    "receipts/2026-10-01-t6021-dart-kernel/logs/hw_reset.disasm.txt",
    "receipts/2026-10-01-t6021-default-on-gate/logs/B/dmesg-ane.txt",
    "receipts/2026-10-01-t6021-default-on-gate/logs/P/dmesg-ane.txt",
    "receipts/2026-10-01-t6021-default-on-gate/logs/final/dmesg-ane.txt",
    "receipts/2026-10-01-t6021-default-on-gate/logs/host/flip-m2-compile.txt",
    "receipts/2026-10-01-t6021-default-on-gate/logs/host/host-tests-c2b7408.txt",
}
ALLOWED = ALLOWED | CAPTURED
COMPILED = [(n, re.compile(p)) for n, p in PATTERNS]
HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, "macos_read_methods_baseline.json")
SKIP_SUFFIX = (".png", ".jpg", ".bin", ".dtb", ".dtbo", ".gz", ".xz", ".zip", ".tar", ".pdf", ".so", ".ko", ".o")


def files():
    out = subprocess.run(["git", "ls-files", "-z"], capture_output=True, check=True, cwd=os.path.dirname(HERE)).stdout.split(b"\0")
    for raw in out:
        name = raw.decode()
        if name and not name.endswith(SKIP_SUFFIX) and name not in ALLOWED and name != "tools/check_no_macos_read_methods.py" and name != "tools/test_check_no_macos_read_methods.py" and not name.endswith("macos_read_methods_baseline.json"):
            yield name


def scan(root):
    result = {}
    for name in files():
        path = os.path.join(root, name)
        try:
            with open(path, encoding="utf-8", errors="strict") as f:
                lines = f.read().splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        hits = [(i + 1, kind) for i, line in enumerate(lines) for kind, rx in COMPILED if rx.search(line)]
        if hits:
            result[name] = hits
    return result


def main():
    root = os.path.dirname(HERE)
    found = scan(root)
    counts = {k: len({ln for ln, _ in v}) for k, v in found.items()}
    if "--list" in sys.argv:
        for name, hits in sorted(found.items()):
            print(f"{name}: {len({ln for ln, _ in hits})} lines: " + ", ".join(sorted({k for _, k in hits})))
        print(f"{len(found)} files, {sum(counts.values())} lines")
        return 0
    if "--write-baseline" in sys.argv:
        with open(BASELINE, "w") as f:
            json.dump(dict(sorted(counts.items())), f, indent=1)
            f.write("\n")
        print(f"baseline written: {len(counts)} files, {sum(counts.values())} lines")
        return 0
    try:
        with open(BASELINE) as f:
            base = json.load(f)
    except OSError:
        base = {}
    bad = []
    for name, n in sorted(counts.items()):
        if n > base.get(name, 0):
            first = [ln for ln, _ in found[name]][:5]
            bad.append(f"{name}: {n} matching lines, baseline {base.get(name, 0)} (lines {first})")
    if bad:
        print("FAIL: new text that shows how we read macOS (rule of 2026-10-29). State only observed behaviour; no Apple addresses,")
        print("symbol names, extraction or decode commands, or restore-image locators.")
        print("\n".join("  " + b for b in bad))
        return 1
    stale = [f"{n}: {counts.get(n, 0)} matching lines, baseline {base[n]}" for n in sorted(base) if counts.get(n, 0) < base[n]]
    if stale:
        print("FAIL: the baseline is higher than the tree. A cleaned file could regain matches up to its old count unnoticed.")
        print("Lower the baseline: run  python3 tools/check_no_macos_read_methods.py --write-baseline  and commit the file.")
        print("\n".join("  " + b for b in stale))
        return 1
    print(f"ok: no file differs from the baseline ({len(counts)} files still in the backlog)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
