#!/usr/bin/env python3
"""Validate per-SoC ANE JSON files using only the Python standard library."""
from __future__ import annotations
import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

HEX64 = re.compile(r"^[0-9a-f]{64}$")
RESERVED = {"blob", "raw", "payload", "bytes"}
MAX_FILE_BYTES = 256 * 1024
MAX_LEAF_STR = 4096
MAX_ARRAY_ITEMS = 256
KINDS = {"ipsw-adt", "kernelcache-kext", "buildmanifest", "aurora-dts", "community-row", "ipsw-firmware", "compiler-oracle"}


class Violation(Exception):
    """A schema violation with a readable location."""


def walk(node: Any, path: str, refs: set[str]) -> None:
    """Validate every non-metadata branch and collect its source references."""
    if isinstance(node, dict):
        keys = set(node)
        if keys == {"v", "src"}:
            value, src = node["v"], node["src"]
            if not isinstance(src, str) or not src:
                raise Violation(f"{path}.src: expected a non-empty source id")
            if isinstance(value, str) and len(value) > MAX_LEAF_STR:
                raise Violation(f"{path}.v: string value exceeds {MAX_LEAF_STR} characters")
            if isinstance(value, (dict, list)) and len(json.dumps(value)) > MAX_LEAF_STR:
                raise Violation(f"{path}.v: structured value exceeds {MAX_LEAF_STR} bytes")
            refs.add(src)
            return
        if keys == {"v", "reason"} and node["v"] is None:
            if not isinstance(node["reason"], str) or not node["reason"].strip():
                raise Violation(f"{path}.reason: expected a non-empty reason")
            if len(node["reason"]) > MAX_LEAF_STR:
                raise Violation(f"{path}.reason: exceeds {MAX_LEAF_STR} characters")
            return
        if "v" in keys or "src" in keys or "reason" in keys:
            raise Violation(f"{path}: malformed leaf; expected {{v, src}} or {{v: null, reason}}")
        for key, value in node.items():
            if not isinstance(key, str) or not key:
                raise Violation(f"{path}: object keys must be non-empty strings")
            if key.lower() in RESERVED:
                raise Violation(f"{path}.{key}: raw Apple payload content is forbidden")
            walk(value, f"{path}.{key}", refs)
        return
    if isinstance(node, list):
        if len(node) > MAX_ARRAY_ITEMS:
            raise Violation(f"{path}: array exceeds {MAX_ARRAY_ITEMS} items")
        for i, value in enumerate(node):
            walk(value, f"{path}[{i}]", refs)
        return
    if isinstance(node, (str, int, float, bool)) or node is None:
        raise Violation(f"{path}: bare value {node!r}; every data leaf needs a source envelope")
    raise Violation(f"{path}: unsupported value type {type(node).__name__}")


def validate(path: Path) -> dict:
    raw = path.read_bytes()
    if len(raw) > MAX_FILE_BYTES:
        raise Violation(f"{path}: file exceeds {MAX_FILE_BYTES} bytes; store hashes, not payloads")
    try:
        doc = json.loads(raw)
    except (json.JSONDecodeError, UnicodeDecodeError) as e:
        raise Violation(f"{path}: invalid JSON: {e}") from None
    if not isinstance(doc, dict):
        raise Violation(f"{path}: top-level value must be an object")
    for key in ("soc", "generation", "boards", "sources", "state"):
        if key not in doc:
            raise Violation(f"{path}: required top-level key {key!r} is missing")
    if not isinstance(doc["soc"], str) or not re.fullmatch(r"t[0-9a-z]+", doc["soc"]):
        raise Violation(f"{path}: soc must be a lowercase T-number such as t8122")
    if path.stem != doc["soc"]:
        raise Violation(f"{path}: filename {path.name!r} does not match soc {doc['soc']!r}")
    if "synthetic" in doc and (doc["synthetic"] is not True or "tests/fixtures/" not in path.as_posix()):
        raise Violation(f"{path}: synthetic entries are allowed only under tests/fixtures/")
    if "tests/fixtures/" in path.as_posix() and doc.get("synthetic") is not True:
        raise Violation(f"{path}: test fixture must be explicitly marked synthetic")
    if doc["state"] != "data-only":
        raise Violation(f"{path}: state must be 'data-only', got {doc['state']!r}")
    if not isinstance(doc["sources"], list) or not doc["sources"]:
        raise Violation(f"{path}: sources must be a non-empty list")
    if not isinstance(doc["boards"], list) or not doc["boards"]:
        raise Violation(f"{path}: boards must be a non-empty list")

    source_ids = set()
    for i, src in enumerate(doc["sources"]):
        where = f"{path}#sources[{i}]"
        if not isinstance(src, dict):
            raise Violation(f"{where}: source must be an object")
        sid, kind, ref, digest = src.get("id"), src.get("kind"), src.get("ref"), src.get("sha256")
        if not isinstance(sid, str) or not sid:
            raise Violation(f"{where}: id must be a non-empty string")
        if sid in source_ids:
            raise Violation(f"{where}: duplicate id {sid!r}")
        if kind not in KINDS:
            raise Violation(f"{where}.{sid}: kind must be one of {sorted(KINDS)}")
        if not isinstance(ref, str) or not ref:
            raise Violation(f"{where}.{sid}: ref must be a non-empty string")
        if not isinstance(digest, str) or not HEX64.fullmatch(digest):
            raise Violation(f"{where}.{sid}: sha256 must be 64 lowercase hex characters")
        source_ids.add(sid)

    # soc, state, and source descriptors are metadata. All other values,
    # including generation and boards, obey the leaf-envelope rule.
    refs: set[str] = set()
    for key, value in doc.items():
        if key in {"soc", "state", "sources", "synthetic"}:
            continue
        walk(value, f"{path}#{key}", refs)
    unknown = refs - source_ids
    if unknown:
        raise Violation(f"{path}: leaf source ids not present in sources: {sorted(unknown)}")
    return {"file": str(path), "soc": doc["soc"], "state": doc["state"],
            "sources": len(source_ids), "referenced": len(refs)}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", type=Path)
    args = ap.parse_args(argv)
    ok = failed = 0
    for path in args.paths:
        # An unmatched shell glob (data/ane-soc/*.json before any data lands).
        if not path.exists() and any(c in str(path) for c in "*?["):
            continue
        try:
            result = validate(path)
        except (OSError, Violation) as e:
            print(f"FAIL  {e}")
            failed += 1
            continue
        print(f"ok    {result['soc']} state={result['state']} sources={result['sources']} "
              f"leaves={result['referenced']} ({path})")
        ok += 1
    if failed:
        print(f"validate_ane_soc: {failed} failed, {ok} ok", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
