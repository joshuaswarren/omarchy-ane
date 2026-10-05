#!/usr/bin/env python3
"""Contract test for the agx_stats series (aurora-silicon/linux PR #157).

Runs on any host with python3; no kernel build needed. Pins four contracts:
  1. stats.rs StatsSnapshot fields == sysfs.c struct mirror (names, order, widths)
  2. sysfs.c printed keys cover the coreglass producer contract keys
  3. ABI doc keys == printed keys (known gap documented, not silently dropped)
  4. the file is declared read-only (DEVICE_ATTR_RO) as the contract requires

Usage: test_agx_stats_contract.py <kernel-worktree-root>
"""
import re
import sys
from pathlib import Path

CONTRACT_KEYS = {
    "busy_ns", "jobs", "pstate", "power_mw",
    "util1", "util2", "util3", "util4",
}


def read(wt: Path, rel: str) -> str:
    return (wt / rel).read_text()


def snapshot_fields_rust(src: str) -> list:
    body = src.split("pub(crate) struct StatsSnapshot", 1)[1].split("}", 1)[0]
    # All fields, private ones included: the C mirror struct must match layout.
    return re.findall(r"^\s+(?:pub\(crate\) )?(\w+): (AtomicU32|AtomicU64)", body, re.M)


def snapshot_fields_c(src: str) -> list:
    body = src.split("struct asahi_stats_snapshot {", 1)[1].split("};", 1)[0]
    fields = re.findall(r"^\t(u32|u64) (\w+);", body, re.M)
    return [(name, kind) for kind, name in fields]


def printed_keys(src: str) -> set:
    return set(re.findall(r'scnprintf\([^,]+,\s*[^,]+,\s*"(\w+) ', src))


def abi_keys(doc: str) -> set:
    return set(re.findall(r"^\\?\"?(\w+)\\?\"?\s+\|", doc, re.M)) | set(
        re.findall(r"^\\t\\?\"?(\w+)", doc, re.M)
    )


def main() -> int:
    wt = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    rust = read(wt, "drivers/gpu/drm/asahi/stats.rs")
    c = read(wt, "drivers/gpu/drm/asahi/sysfs.c")
    doc = read(wt, "Documentation/ABI/testing/sysfs-driver-asahi-agx-stats")
    queue = read(wt, "drivers/gpu/drm/asahi/queue/mod.rs")
    fail = []

    rf = snapshot_fields_rust(rust)
    cf = snapshot_fields_c(c)
    if [n for n, _ in rf] != [n for n, _ in cf]:
        fail.append(f"struct mirror drift: rust={[n for n, _ in rf]} c={[n for n, _ in cf]}")
    widths = {"AtomicU32": "u32", "AtomicU64": "u64"}
    for (n, rk), (cn, ck) in zip(rf, cf):
        if widths[rk] != ck:
            fail.append(f"width drift on {n}: rust={widths[rk]} c={ck}")

    keys = printed_keys(c)
    missing = CONTRACT_KEYS - keys
    if missing:
        fail.append(f"producer contract keys missing from sysfs output: {sorted(missing)}")

    ak = abi_keys(doc)
    doc_only = ak - keys
    if doc_only:
        # Known, reported mismatch: doc promises keys the code never prints.
        print(f"NOTE (reported PR mismatch, not a gate): doc-only keys {sorted(doc_only)}")

    if "DEVICE_ATTR_RO(agx_stats)" not in c:
        fail.append("sysfs file is not DEVICE_ATTR_RO (read-only)")

    # jobs call-site drift guard: the counter must be bumped at submission
    # completion (JobFence::command_complete), reachable via the fence's
    # snapshot handle.
    if "stats.note_job()" not in queue:
        fail.append("queue/mod.rs no longer bumps stats.note_job() at completion")
    if "note_job" not in rust:
        fail.append("stats.rs lost the note_job counter entry point")

    # Layout contract guard: the snapshot is repr(C) so the C mirror's
    # declared offsets are binding, and sysfs.c asserts them at build time.
    # Without this, rustc reorders the repr(Rust) struct and the readout
    # crosses field boundaries (observed on T6021: busy_ns bouncing in
    # 2^32 steps, jobs reading 0 while incrementing).
    decl_lines = [
        line for line in rust.split("pub(crate) struct StatsSnapshot", 1)[0].splitlines()
        if not line.strip().startswith("//")
    ]
    if "#[repr(C)]" not in "\n".join(decl_lines[-6:]):
        fail.append("StatsSnapshot lost #[repr(C)] — the C mirror offsets are no longer binding")
    for needle in (
        "offsetof(struct asahi_stats_snapshot, busy_ns) != 40",
        "offsetof(struct asahi_stats_snapshot, jobs) != 48",
        "offsetof(struct asahi_stats_snapshot, pstate) != 16",
        "sizeof(struct asahi_stats_snapshot) != 56",
    ):
        if needle not in c:
            fail.append(f"sysfs.c lost the layout assert: {needle}")

    # Off-arm guard: the file must print exactly "unsupported" when the
    # export is disabled (ABI document and producer contract).
    if "!READ_ONCE(asahi_stats_export_enabled)" not in c or '"unsupported\\n"' not in c:
        fail.append("sysfs.c no longer prints 'unsupported' for the disabled export")

    for key in sorted(CONTRACT_KEYS):
        if not re.search(rf'"{key} %\w+\\n"', c):
            fail.append(f"key {key} not printed as a single-space key value line")

    if fail:
        print("FAIL")
        for f in fail:
            print(" -", f)
        return 1
    print("PASS: struct mirror, producer keys, RO attr, format all consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
