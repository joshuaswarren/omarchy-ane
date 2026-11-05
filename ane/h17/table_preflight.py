#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail-closed, read-only preflight for the H17 pre-registration table rung."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import uuid

from diagnostic_preflight import build_id


FIELDS = {
    "schema", "source_commit", "failed_boot", "kernel_release",
    "kernel_build_id", "module_sha256", "firmware_sha256",
    "uki_sha256", "m1n1_sha256",
}
RELEASE = "7.2.11-14-1-ARCH"
FAILED_BOOT = "0855c23b-dce5-47b0-83c5-a19baef2beef"
FW = Path("usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho")
UKI = Path("boot/efi/EFI/Linux/omarchy_linux-aurora.efi")
M1N1 = Path("boot/efi/m1n1/boot.bin")
ANE = Path("proc/device-tree/soc/ane@400000000")
RESERVED = Path("proc/device-tree/reserved-memory")
DARTS = (
    ("iommu@401800000", 0x401800000, 0xC000),
    ("iommu@401820000", 0x401820000, 0x20000),
    ("iommu@401840000", 0x401840000, 0x20000),
)
DM = 0x10000380000
ALIAS_BYTES = 0xF50000
ATTEMPTS = Path("var/lib/omarchy-ane/h17-table-attempts")
PENDING = Path("var/lib/omarchy-ane/h17-table.pending")


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def load_manifest(path):
    """Open the run manifest without following a symlink and enforce ownership."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError("manifest must be root-owned regular file, not group/world writable")
        raw = stream.read()
    return json.loads(raw), hashlib.sha256(raw).hexdigest()


def words(path):
    data = path.read_bytes()
    if len(data) % 4:
        raise ValueError(f"malformed cell property: {path}")
    return [int.from_bytes(data[i:i + 4], "big") for i in range(0, len(data), 4)]


def strings(path):
    return [s.decode("ascii") for s in path.read_bytes().split(b"\0") if s]


def phandle(node):
    for key in ("phandle", "linux,phandle"):
        path = node / key
        if path.exists():
            value = words(path)
            if len(value) == 1:
                return value[0]
    return None


def _rooted(root, path):
    return root / path


def check(expected, *, root=Path("/"), module, vermagic, repo,
          boot_id=None, module_loaded=None, attempt_exists=None,
          evidence_exists=None, pending_ok=None, manifest_sha256=None):
    """Validate a supplied test root or the live root. Never arms the rung."""
    out = {"identity": "REFUSED", "arms_rung": False, "checks": []}

    def require(condition, name, detail):
        out["checks"].append({"name": name, "pass": bool(condition), "observed": detail})
        if not condition:
            raise ValueError(name)

    try:
        if not isinstance(expected, dict) or set(expected) != FIELDS:
            raise ValueError("manifest fields differ from schema")
        if expected["schema"] != "h17-table-discovery.v1" or expected["kernel_release"] != RELEASE:
            raise ValueError("manifest does not describe the table-discovery target")
        if expected["failed_boot"] != FAILED_BOOT:
            raise ValueError("manifest failed-boot identity differs from the recorded failure")
        for field in ("module_sha256", "firmware_sha256", "uki_sha256", "m1n1_sha256"):
            if not isinstance(expected[field], str) or not re.fullmatch(r"[0-9a-f]{64}", expected[field]):
                raise ValueError(f"invalid {field}")
        if not re.fullmatch(r"[0-9a-f]{40}", expected["source_commit"]):
            raise ValueError("invalid source commit")

        boot_id = boot_id or _rooted(root, Path("proc/sys/kernel/random/boot_id")).read_text().strip()
        if str(uuid.UUID(boot_id)) != boot_id or boot_id == expected["failed_boot"] or boot_id == FAILED_BOOT:
            raise ValueError("invalid or previously failed boot ID")
        require(_rooted(root, Path("proc/sys/kernel/osrelease")).read_text().strip() == RELEASE,
                "kernel-release", RELEASE)
        notes = _rooted(root, Path("sys/kernel/notes")).read_bytes()
        observed_build_id = build_id(notes)
        require(expected["kernel_build_id"] == observed_build_id, "kernel-build-id", observed_build_id)

        compatible = strings(_rooted(root, Path("proc/device-tree/compatible")))
        require("apple,j700" in compatible and "apple,t8140" in compatible,
                "board", compatible)
        ane = _rooted(root, ANE)
        require(strings(ane / "compatible") == ["apple,t8140-ane"],
                "ane-compatible", strings(ane / "compatible"))
        require(strings(ane / "status") == ["okay"], "ane-enabled", strings(ane / "status"))

        ane_phandle = phandle(ane)
        refs = words(ane / "memory-region")
        require(ane_phandle is not None and len(refs) == 2, "memory-region-count", refs)
        reserved_nodes = {}
        for child in _rooted(root, RESERVED).iterdir():
            value = phandle(child)
            if value is not None:
                reserved_nodes[value] = child
        expected_regions = (
            (0x10000000000, 0x380000),
            (DM, ALIAS_BYTES),
        )
        for index, (reference, (start, size)) in enumerate(zip(refs, expected_regions)):
            node = reserved_nodes.get(reference)
            if node is None:
                raise ValueError(f"memory-region {index} phandle is unresolved")
            require(not (node / "reg").exists(), f"reservation-{index}-no-physical-reg", str(node))
            cells = words(node / "iommu-addresses")
            wanted = [ane_phandle, start >> 32, start & 0xffffffff,
                      size >> 32, size & 0xffffffff]
            require(cells == wanted, f"reservation-{index}-iommu-addresses", cells)

        dart_refs = []
        for node_name, base, size in DARTS:
            dart = _rooted(root, Path("proc/device-tree/soc") / node_name)
            dart_refs.append(phandle(dart))
            observed = {"compatible": strings(dart / "compatible"),
                        "status": strings(dart / "status"), "reg": words(dart / "reg")}
            wanted = {"compatible": ["apple,t8140-dart", "apple,t8110-dart"],
                      "status": ["okay"],
                      "reg": [base >> 32, base & 0xffffffff, 0, size]}
            require(observed == wanted, f"dart-{base:x}", observed)
        expected_iommus = [value for ph in dart_refs for value in (ph, 0)]
        require(None not in dart_refs and words(ane / "iommus") == expected_iommus,
                "ane-dart-links", words(ane / "iommus"))

        release_module = _rooted(root, Path("proc/modules")).read_text().splitlines()
        loaded = module_loaded if module_loaded is not None else any(
            line.split() and (line.split()[0] == "ane" or line.split()[0].startswith("ane_"))
            for line in release_module)
        require(not loaded, "ane-modules-absent", loaded)
        require(vermagic.split()[:1] == [RELEASE], "module-vermagic", vermagic.strip())
        actual = {
            "module_sha256": digest(module),
            "firmware_sha256": digest(_rooted(root, FW)),
            "uki_sha256": digest(_rooted(root, UKI)),
            "m1n1_sha256": digest(_rooted(root, M1N1)),
        }
        for field, value in actual.items():
            require(expected[field] == value, field, value)

        head = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
        dirty = subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"], text=True)
        require(head == expected["source_commit"] and not dirty,
                "source-checkout", {"head": head, "dirty": bool(dirty)})

        attempt = attempt_exists if attempt_exists is not None else \
            os.path.lexists(_rooted(root, ATTEMPTS / boot_id))
        evidence = evidence_exists if evidence_exists is not None else \
            os.path.lexists(Path(f"/var/tmp/opencode/h17-table-{boot_id}"))
        require(not attempt and not evidence, "one-shot-unused", {"attempt": attempt, "evidence": evidence})
        if pending_ok is None:
            pending_path = _rooted(root, PENDING)
            fd = os.open(pending_path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            with os.fdopen(fd) as marker_file:
                info = os.fstat(marker_file.fileno())
                pending = (stat.S_ISREG(info.st_mode) and info.st_uid == 0 and
                           not (info.st_mode & 0o022))
                marker = marker_file.read()
            pending = pending and marker == (
                "schema=h17-table-attempt.v1\n"
                f"manifest_sha256={manifest_sha256}\n")
        else:
            pending = pending_ok
        require(pending, "pending-marker-bound-to-manifest", str(PENDING))
        out.update(identity="MATCHED", reason="identity-and-device-tree-only-not-rung-authorization",
                   boot_id=boot_id)
    except (OSError, ValueError, TypeError, subprocess.CalledProcessError) as exc:
        out["reason"] = str(exc)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("module", type=Path)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        expected, manifest_sha256 = load_manifest(args.manifest)
        vermagic = subprocess.check_output(["modinfo", "-F", "vermagic", str(args.module.resolve())],
                                           text=True, stderr=subprocess.STDOUT)
        result = check(expected, module=args.module, vermagic=vermagic, repo=args.repo,
                       manifest_sha256=manifest_sha256)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        result = {"identity": "REFUSED", "arms_rung": False, "reason": str(exc)}
    print(json.dumps(result, sort_keys=True))
    return 0 if result["identity"] == "MATCHED" else 2


if __name__ == "__main__":
    raise SystemExit(main())
