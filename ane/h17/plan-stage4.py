#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Offline evidence/plan checker. Never loads modules, opens devices or
changes system state. READY_FOR_REVIEW is not authorization or a hardware
qualification certificate. All evidence assertions remain review inputs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

SCHEMA = "omarchy-ane.h17-stage4-evidence.v1"
PREREQUISITES = (
    "artifact-provenance", "register-window-binding", "power-entry", "power-exit",
    "firmware-ownership", "recovery-ownership", "durable-record-adapter", "adapter-review",
)


def integer(value):
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise ValueError("expected an integer or hexadecimal string")
    return int(value, 0) if isinstance(value, str) else value


def check(evidence, observed_sha256=None):
    if not isinstance(evidence, dict):
        raise ValueError("evidence must be an object")
    if evidence.get("schema") != SCHEMA:
        raise ValueError("unsupported evidence schema")
    if evidence.get("target") != {"soc": "t8140", "board": "j700", "aneType": 512}:
        raise ValueError("evidence target is not J700/T8140 ane-type 0x200")
    digest = evidence["artifact"]["sha256"]
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("invalid artifact SHA-256")
    proposal = evidence["snapshotProposal"]
    if proposal.get("hardwareExecutionAllowed") is not False:
        raise ValueError("offline proposal must not authorize hardware execution")
    if proposal.get("kind") != "single-read-only-observation":
        raise ValueError("only a single read-only proposal is supported")
    ids = proposal["registerIds"]
    if not isinstance(ids, list) or len(ids) != 1 or not isinstance(ids[0], str):
        raise ValueError("proposal must select exactly one register ID")
    resource_bytes = integer(proposal["resourceBytes"])
    if resource_bytes <= 0 or resource_bytes > (1 << 32):
        raise ValueError("invalid resource size")
    sources = evidence["sources"]
    registers = {}
    for record in evidence["registers"]:
        name = record["id"]
        if not isinstance(name, str) or not name or name in registers:
            raise ValueError("empty or duplicate register ID")
        if record["source"] not in sources:
            raise ValueError(f"{name}: missing evidence source")
        off, width = integer(record["offset"]), integer(record["widthBytes"])
        if width not in (4, 8) or off < 0 or off % width or off + width > resource_bytes:
            raise ValueError(f"{name}: invalid width, alignment or resource range")
        if type(record["readSafetyQualified"]) is not bool:
            raise ValueError(f"{name}: read qualification must be boolean")
        registers[name] = record
    if ids[0] not in registers:
        raise ValueError("proposal selects an unknown register")
    blockers = []
    if observed_sha256 is None:
        blockers.append({"id": "artifact-bytes", "reason": "No retained artifact was supplied for hashing."})
    elif observed_sha256 != digest:
        blockers.append({"id": "artifact-bytes", "reason": "Supplied artifact hash does not match the evidence."})
    prerequisites = evidence["prerequisites"]
    # The required set is fixed in code, not chosen by the input document.
    for name in PREREQUISITES:
        item = prerequisites.get(name)
        if not isinstance(item, dict) or item.get("state") != "qualified" or not isinstance(item.get("receipt"), str) or not item["receipt"].strip():
            blockers.append({"id": name, "reason": item.get("reason", "Qualification and a receipt are required.") if isinstance(item, dict) else "Missing prerequisite."})
    record = registers[ids[0]]
    if integer(record["widthBytes"]) != 4:
        raise ValueError("single observation requires a 32-bit register")
    if not record["readSafetyQualified"]:
        blockers.append({"id": "read-safety:" + ids[0], "reason": "Compiled register use is not safe Linux access qualification."})
    return {
        "verdict": "BLOCKED" if blockers else "READY_FOR_REVIEW",
        "hardwareExecutionAllowed": False,
        "artifactSha256": digest,
        "observedArtifactSha256": observed_sha256,
        "operations": [{"direction": "read", "id": ids[0], "offset": record["offset"], "widthBytes": integer(record["widthBytes"]), "source": record["source"]}],
        "blockers": blockers,
    }


def hash_artifact(path):
    # Nonblocking open avoids hanging on a FIFO; fstat checks the opened file,
    # not a potentially stale path lookup. Do not follow artifact symlinks.
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(fd, "rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("artifact must be a regular retained file")
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, default=Path(__file__).resolve().parents[2] / "data/h17/t8140-stage4.json")
    parser.add_argument("--artifact", type=Path, help="optional retained file to hash; no device paths")
    args = parser.parse_args()
    try:
        observed = None
        if args.artifact:
            # Reject known special files before opening; hash_artifact also
            # verifies the opened descriptor to handle path replacement.
            if not args.artifact.is_file() or args.artifact.is_symlink():
                raise ValueError("artifact must be a regular retained file")
            observed = hash_artifact(args.artifact)
        report = check(json.loads(args.evidence.read_text()), observed)
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        print(json.dumps({"verdict": "INVALID", "hardwareExecutionAllowed": False, "error": str(error)}, indent=2))
        return 2
    print(json.dumps(report, indent=2))
    return 2 if report["blockers"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
