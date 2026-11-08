#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only identity check. Does not arm, load, unload, or submit anything."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import uuid


RELEASE = "7.2.11-14-1-h17pub-ARCH"
SOURCE = "4fdd0fc5c931ef9f14474942b37c3d451e97fc04"
PATCH = "a7e9e91dcfbd0cea8dec6cc15cf4ccde93e282b5e1f5ddc328e657eea3cbc911"
FAILED_BOOT = "0855c23b-dce5-47b0-83c5-a19baef2beef"
FIRMWARE = "usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho"
FIELDS = {"schema", "source_commit", "patch_sha256", "kernel_release",
          "kernel_build_id", "image_sha256", "module_sha256", "firmware_sha256"}


def build_id(notes):
    """Parse little-endian ELF notes as exposed by arm64 /sys/kernel/notes."""
    offset, ids = 0, []
    while offset < len(notes):
        if len(notes) - offset < 12:
            raise ValueError("truncated kernel note header")
        namesz, descsz, kind = struct.unpack_from("<III", notes, offset)
        offset += 12
        name_end = offset + namesz
        desc_start = (name_end + 3) & ~3
        desc_end = desc_start + descsz
        end = (desc_end + 3) & ~3
        if end > len(notes):
            raise ValueError("truncated kernel note")
        if notes[offset:name_end] == b"GNU\0" and kind == 3:
            ids.append(notes[desc_start:desc_end].hex())
        offset = end
    if len(ids) != 1 or not re.fullmatch(r"(?:[0-9a-f]{2}){8,64}", ids[0]):
        raise ValueError("expected exactly one valid GNU kernel build ID")
    return ids[0]


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check(expected, module, vermagic, root=Path("/")):
    """root is a test fixture hook; the CLI always checks the live filesystem."""
    result = {"identity": "REFUSED", "arms_rung": False}
    try:
        if not isinstance(expected, dict) or set(expected) != FIELDS:
            raise ValueError("unexpected manifest fields")
        if (expected["schema"] != "h17-publication-identity.v1" or
                expected["source_commit"] != SOURCE or expected["patch_sha256"] != PATCH or
                expected["kernel_release"] != RELEASE):
            raise ValueError("manifest is not for the reviewed diagnostic")
        for field in ("image_sha256", "module_sha256", "firmware_sha256"):
            if not isinstance(expected[field], str) or not re.fullmatch(r"[0-9a-f]{64}", expected[field]):
                raise ValueError("invalid manifest digest")
        if (not isinstance(expected["kernel_build_id"], str) or
                not re.fullmatch(r"(?:[0-9a-f]{2}){8,64}", expected["kernel_build_id"])):
            raise ValueError("invalid manifest build ID")
        boot = (root / "proc/sys/kernel/random/boot_id").read_text().strip()
        if str(uuid.UUID(boot)) != boot or boot == FAILED_BOOT:
            raise ValueError("invalid or failed/pinned boot")
        if (root / "proc/sys/kernel/osrelease").read_text().strip() != RELEASE:
            raise ValueError("running kernel release differs")
        if build_id((root / "sys/kernel/notes").read_bytes()) != expected["kernel_build_id"]:
            raise ValueError("running kernel build ID differs")
        if (root / "sys/module/apple_dart/parameters/h17_pub_diag").read_text().strip() != "Y":
            raise ValueError("publication diagnostic is not enabled")
        compatibles = (root / "proc/device-tree/compatible").read_bytes().split(b"\0")
        if b"apple,j700" not in compatibles or b"apple,t8140" not in compatibles:
            raise ValueError("unexpected board/SoC")
        for line in (root / "proc/modules").read_text().splitlines():
            name = line.split()[0] if line.split() else ""
            if name == "ane" or name.startswith("ane_"):
                raise ValueError("ANE module already loaded; fresh boot required")
        if not vermagic.split() or vermagic.split()[0] != RELEASE:
            raise ValueError("ANE module vermagic differs")
        for path, field in ((root / f"lib/modules/{RELEASE}/vmlinuz", "image_sha256"),
                            (module, "module_sha256"), (root / FIRMWARE, "firmware_sha256")):
            if sha(path) != expected[field]:
                raise ValueError(field + " differs")
        result.update(identity="MATCHED", reason="identity-only-not-rung-authorization",
                      boot_id=boot, kernel_build_id=expected["kernel_build_id"])
    except (OSError, ValueError, TypeError) as exc:
        result["reason"] = str(exc)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="reviewed root-owned immutable-to-users manifest")
    parser.add_argument("module", type=Path)
    args = parser.parse_args()
    try:
        fd = os.open(args.manifest, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd) as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
                raise ValueError("manifest must be root-owned regular file, not group/world writable")
            expected = json.load(stream)
        vermagic = subprocess.check_output(["modinfo", "-F", "vermagic", str(args.module.resolve())],
                                          text=True, stderr=subprocess.STDOUT)
        result = check(expected, args.module, vermagic)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        result = {"identity": "REFUSED", "arms_rung": False, "reason": str(exc)}
    print(json.dumps(result, sort_keys=True))
    return 0 if result["identity"] == "MATCHED" else 2


if __name__ == "__main__":
    raise SystemExit(main())
