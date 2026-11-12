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
can also RAISE numbers, so review its diff. Files listed in CAPTURED_H17_COUNTS are skipped by the baseline and guarded instead by
their frozen recorded match count: they may keep it and may not gain any. The patterns are literal: plain words such as kext, kernelcache, ipsw, im4p,
disassembl, dtrace or 'static read' are not matched, so this is a ratchet, not a clean bill.
"""
import json
import os
import re
import subprocess
import sys

PATTERNS = [
    ("code offset", r"\b(kext|kernelcache|kc)\s*[+@]\s*0x[0-9a-fA-F]+"),
    ("tool or command", r"\b(ipsw\s+(extract|kernel|macho|dl|download)|pyimg4|img4tool|otool\s+-|ghidra|radare2|objdump\s+-d|disarm64|jtool2?)\b"),
    ("restore-image locator", r"kernelcache\.release|\bhdr_off=|__PRELINK_INFO|__TEXT_EXEC|\bBuildManifest\b|updates\.cdn-apple|\.ipsw\b|by HTTP range"),
    ("Apple symbol or bundle", r"\bcom\.apple\.driver\.|\bApple(?=[A-Z0-9])[A-Za-z0-9_]+"),
    ("macOS-sourced value", r"(?i)(macOS|restore[- ]image|\bkext|kernelcache|iBoot|Apple'?s? (firmware|driver|compiler|kernel|ANE firmware)|\bipsw\b).*0x[0-9a-f]{3,}|0x[0-9a-f]{3,}.*(macOS|restore[- ]image|\bkext|kernelcache|iBoot|Apple'?s? (firmware|driver|compiler|kernel|ANE firmware)|\bipsw\b)"),
    ("Apple driver function", r"\b(Enable|Disable)(ANE|CPU)ClocksAndPower\b|\bANE_(deInit|Init)\w*|\benableDevice\w*|\bgetPageSize\b|\bInitializeRTBuddy\w*|\binitializeANESoCConfig\w*|\b[A-Za-z]\w*::~?[A-Za-z_]\w*\s*\(|(?<![\w.])_[a-z]+[A-Z]\w*\("),
    ("kernelcache address", r"\b0xfffffe[0-9a-fA-F]+|\b0x(9[0-9a-fA-F]{6}|8f[0-9a-fA-F]{5})\b"),
    ("driver struct offset or operand", r"\bdev\s*\+\s*0x[0-9a-fA-F]+|\bu(8|16|32|64)\[[^\]]*\b[xw][0-9]+[^\]]*\]|\[x[0-9]+\s*[+\]]"),
    ("decoded provenance", r"(?i)\bdecoded (from|by|statically)\b|\bstatic[- ]decod\w*|\bdecoded (CPU-power|power|step|sequence|driver|kext|kernelcache)|(?-i:DECODED:)|\bDeviceTree\.[\w.]+|\bmapFw\w+|\b(ldr|str)\s+[wx]\d+,\s*\[x\d+,\s*#0x[0-9a-fA-F]+\]|\btbnz\s+w\d+,\s*#\d|macOS 27\.0 ADT|\bADT 27\.0\b"),
    ("restore-image or kernelcache locator", r"(?i)\bkernelcache\b|\bipsw\b|restore[- ]image"),
    ("macOS 27.0 provenance", r"\b27\.0\b|\b26A[0-9]{3}[A-Za-z]?\b|adt-27"),
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
# Grandfathered 2026-11-08 (PR: public-text ratchet cleanup): captured H17 bring-up
# evidence that landed with PR #364 before this check could run on it. Captured output
# and captured-data ledgers are never edited, so they are allowlisted on purpose, file
# by file: disassembly and decode dumps, run logs, rung assessments, the stage-4
# evidence ledger the planner consumes, and the planner test whose assertions embed the
# captured run's addresses. Read-method TOOLING and TEACHING DOCS are not on this list;
# they moved to the private lab store (ane-h17-lab) in the same change.
CAPTURED_H17 = {
    "data/h17/t8140-stage4.json",
    "receipts/2026-10-30-j700-h17-adt/README.md",
    "receipts/2026-10-30-j700-h17-asc-map/README.md",
    "receipts/2026-10-30-j700-h17-hang/README.md",
    "receipts/2026-10-30-j700-h17-qualified-map/README.md",
    "receipts/2026-10-31-j700-h17-power-contract/README.md",
    "receipts/2026-11-01-j700-h17-exclave-vs-sep/README.md",
    "receipts/2026-11-01-j700-h17-linux-support-plan/README.md",
    "receipts/2026-11-02-j700-h17-safe-round/README.md",
    "receipts/2026-11-02-j700-h17-stage5-review/README.md",
    "receipts/2026-11-03-j700-h17-clock-gap/README.md",
    "receipts/2026-11-03-j700-h17-offline-subagents/README.md",
    "receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-exclave-kernelcache-report.md",
    "receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md",
    "receipts/2026-11-03-j700-h17-offline-subagents/t8140-anehal-register-map.md",
    "receipts/2026-11-03-j700-h17-poweron-and-tunable-freezes/README.md",
    "receipts/2026-11-04-j700-h17-adt-parser-fixes/README.md",
    "receipts/2026-11-04-j700-h17-adt-parser-fixes/live-dt.log",
    "receipts/2026-11-04-j700-h17-clkgen-live/README.md",
    "receipts/2026-11-04-j700-h17-clkgen-map71-correction/README.md",
    "receipts/2026-11-04-j700-h17-clock-pmp-command/README.md",
    "receipts/2026-11-04-j700-h17-clock-window-derivation/README.md",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-anecmdsend.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-channel-primitive.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-channel-send-caller.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-chanreg-prereqs.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-chanreg-scans.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-chanreg-trace.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-cmd-sites.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-command-token-correction.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-doorbell-arch.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-doorbell-callsite.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-doorbell-disasm.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-field-consumers.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fw-rtkit-station-arm.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fwcmdsend-id-field.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fwcmdsend-serialization.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fwcmdsend.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-handle-outstanding.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-host-command-constructor-search.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-io-receive-helper.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-irq-line-dispatch-correction.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-irq-sites.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-message-lookup-helper.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-messenger-irq-chain.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-premap-setup-disasm.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-process-command-response.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-process-response-xrefs.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-queue-receive-primitive.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-ring-fns.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-channel-lookup.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-inference.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-premap.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-retry-disasm.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-setup.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sendrequest-full.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-socconfig-all.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-socconfig-cases.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-socconfig.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sweep-0x118.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sweep-0x15c.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sweep-0x160.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-t2h-helper-300.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-t2h-io-command.txt",
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-t2h-token-command-bridge.txt",
    "receipts/2026-11-04-j700-h17-first-firmware-wake/README.md",
    "receipts/2026-11-04-j700-h17-fwboot-clean-negative/README.md",
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/collect.txt",
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/fatal.txt",
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/handle-error.txt",
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/identity-symbols.txt",
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/interrupt.txt",
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/version.txt",
    "receipts/2026-11-04-j700-h17-order-adapter/README.md",
    "receipts/2026-11-04-j700-h17-order-helper/README.md",
    "receipts/2026-11-04-j700-h17-table-discovery-implementation/README.md",
    "receipts/2026-11-04-j700-h17-tunable-decode/selfcheck.txt",
    "receipts/2026-11-04-j700-h17-wake-scratch-snapshot/README.md",
    "receipts/2026-11-05-j700-h17-next-command-rung-assessment.md",
}
# A rung assessment belongs here only when it is a run's own ledger; interpretation prose gets
# neutralized instead, like receipts/2026-11-04-j700-h17-bringup-state/README.md was.
# Captured files are skipped by files(), so their guard is a frozen per-file match count recorded
# at grandfathering time (2026-11-08): a listed file may keep its recorded matches and may not
# gain any. A no-op edit still passes; new read-method text in a captured file fails.
CAPTURED_H17_COUNTS = {
    "data/h17/t8140-stage4.json": 121,
    "receipts/2026-10-30-j700-h17-adt/README.md": 4,
    "receipts/2026-10-30-j700-h17-asc-map/README.md": 2,
    "receipts/2026-10-30-j700-h17-hang/README.md": 1,
    "receipts/2026-10-30-j700-h17-qualified-map/README.md": 2,
    "receipts/2026-10-31-j700-h17-power-contract/README.md": 55,
    "receipts/2026-11-01-j700-h17-exclave-vs-sep/README.md": 8,
    "receipts/2026-11-01-j700-h17-linux-support-plan/README.md": 6,
    "receipts/2026-11-02-j700-h17-safe-round/README.md": 2,
    "receipts/2026-11-02-j700-h17-stage5-review/README.md": 1,
    "receipts/2026-11-03-j700-h17-clock-gap/README.md": 23,
    "receipts/2026-11-03-j700-h17-offline-subagents/README.md": 15,
    "receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-exclave-kernelcache-report.md": 125,
    "receipts/2026-11-03-j700-h17-offline-subagents/j700-h17-first-engine-write.md": 177,
    "receipts/2026-11-03-j700-h17-offline-subagents/t8140-anehal-register-map.md": 150,
    "receipts/2026-11-03-j700-h17-poweron-and-tunable-freezes/README.md": 2,
    "receipts/2026-11-04-j700-h17-adt-parser-fixes/README.md": 3,
    "receipts/2026-11-04-j700-h17-adt-parser-fixes/live-dt.log": 2,
    "receipts/2026-11-04-j700-h17-clkgen-live/README.md": 3,
    "receipts/2026-11-04-j700-h17-clkgen-map71-correction/README.md": 2,
    "receipts/2026-11-04-j700-h17-clock-pmp-command/README.md": 11,
    "receipts/2026-11-04-j700-h17-clock-window-derivation/README.md": 12,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-anecmdsend.txt": 120,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-channel-primitive.txt": 160,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-channel-send-caller.txt": 180,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-chanreg-prereqs.txt": 924,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-chanreg-scans.txt": 27,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-chanreg-trace.txt": 1062,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-cmd-sites.txt": 77,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-command-token-correction.txt": 5,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-doorbell-arch.txt": 77,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-doorbell-callsite.txt": 72,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-doorbell-disasm.txt": 70,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-field-consumers.txt": 369,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fw-rtkit-station-arm.txt": 34,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fwcmdsend-id-field.txt": 48,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fwcmdsend-serialization.txt": 760,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-fwcmdsend.txt": 400,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-handle-outstanding.txt": 180,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-host-command-constructor-search.txt": 3,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-io-receive-helper.txt": 100,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-irq-line-dispatch-correction.txt": 16,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-irq-sites.txt": 58,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-message-lookup-helper.txt": 302,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-messenger-irq-chain.txt": 435,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-premap-setup-disasm.txt": 254,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-process-command-response.txt": 220,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-process-response-xrefs.txt": 16,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-queue-receive-primitive.txt": 104,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-ring-fns.txt": 231,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-channel-lookup.txt": 2,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-inference.txt": 220,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-premap.txt": 180,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-retry-disasm.txt": 180,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-send-setup.txt": 180,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sendrequest-full.txt": 2500,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-socconfig-all.txt": 2783,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-socconfig-cases.txt": 21,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-socconfig.txt": 695,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sweep-0x118.txt": 78,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sweep-0x15c.txt": 9,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-sweep-0x160.txt": 146,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-t2h-helper-300.txt": 100,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-t2h-io-command.txt": 600,
    "receipts/2026-11-04-j700-h17-firmware-load-path/analysis/h17-t2h-token-command-bridge.txt": 12,
    "receipts/2026-11-04-j700-h17-first-firmware-wake/README.md": 1,
    "receipts/2026-11-04-j700-h17-fwboot-clean-negative/README.md": 2,
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/collect.txt": 223,
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/fatal.txt": 411,
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/handle-error.txt": 566,
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/identity-symbols.txt": 200,
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/interrupt.txt": 318,
    "receipts/2026-11-04-j700-h17-offline-dart-decoder/version.txt": 17,
    "receipts/2026-11-04-j700-h17-order-adapter/README.md": 2,
    "receipts/2026-11-04-j700-h17-order-helper/README.md": 6,
    "receipts/2026-11-04-j700-h17-table-discovery-implementation/README.md": 2,
    "receipts/2026-11-04-j700-h17-tunable-decode/selfcheck.txt": 12,
    "receipts/2026-11-04-j700-h17-wake-scratch-snapshot/README.md": 1,
    "receipts/2026-11-05-j700-h17-next-command-rung-assessment.md": 26,
}
CAPTURED_H17 |= CAPTURED_H17_COUNTS.keys()
ALLOWED = ALLOWED | CAPTURED | CAPTURED_H17
COMPILED = [(n, re.compile(p)) for n, p in PATTERNS]
_WRAP = re.compile(r"^\s*(\*|//|#|/\*)?\s*")


def line_hits(lines):
    """(line number, kind) per match; a match that starts on a line and wraps onto the next counts on its first line."""
    out = []
    for i, line in enumerate(lines):
        nxt = _WRAP.sub("", lines[i + 1], count=1) if i + 1 < len(lines) else ""
        joined = line + " " + nxt
        for kind, rx in COMPILED:
            # the macOS-word-plus-hex rule stays per line: joining lines pairs unrelated words and numbers
            if rx.search(line) or (kind != "macOS-sourced value" and any(m.start() < len(line) for m in rx.finditer(joined))):
                out.append((i + 1, kind))
    return out
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
        hits = line_hits(lines)
        if hits:
            result[name] = hits
    return result


def scan_captured(root):
    counts = {}
    for name in CAPTURED_H17_COUNTS:
        try:
            with open(os.path.join(root, name), encoding="utf-8", errors="strict") as f:
                lines = f.read().splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        counts[name] = len({i for i, _ in line_hits(lines)})
    return counts


def main():
    root = os.path.dirname(HERE)
    found = scan(root)
    counts = {k: len({ln for ln, _ in v}) for k, v in found.items()}
    captured = scan_captured(root)
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
    grown = [f"{n}: grew past its frozen captured count ({c} matching lines, frozen {CAPTURED_H17_COUNTS[n]})"
             for n, c in sorted(captured.items()) if c > CAPTURED_H17_COUNTS[n]]
    if grown:
        print("FAIL: captured evidence gained matches. Captured output is never edited; put new content in a new")
        print("capture added to CAPTURED_H17 on purpose, or neutralize prose before it lands.")
        print("\n".join("  " + g for g in grown))
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
