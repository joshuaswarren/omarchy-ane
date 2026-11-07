#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only J700 device-tree gate before an H17 power-stage experiment.

This reads only /proc/device-tree and the optional genpd summary. It does
not enable any node, map registers, load a module, or request power.
"""

import json
import sys
from pathlib import Path


DT = Path("/proc/device-tree")
GENPD = Path("/sys/kernel/debug/pm_genpd/pm_genpd_summary")
ANE = "/soc/ane@400000000"
PMGR = "/soc/power-management@300700000/power-controller@290"
DARTS = (
    ("/soc/iommu@401800000", 0x401800000, 0xC000),
    ("/soc/iommu@401820000", 0x401820000, 0x20000),
    ("/soc/iommu@401840000", 0x401840000, 0x20000),
)


def prop(node, key):
    try:
        return (DT / node.lstrip("/") / key).read_bytes()
    except OSError:
        return None


def words(node, key):
    data = prop(node, key)
    if data is None or len(data) % 4:
        return None
    return [int.from_bytes(data[i:i + 4], "big") for i in range(0, len(data), 4)]


def strings(node, key):
    data = prop(node, key)
    return None if data is None else [s.decode("ascii", "replace") for s in data.split(b"\0") if s]


def check(ok, name, observed, out):
    out["checks"].append({"name": name, "pass": bool(ok), "observed": observed})


def main():
    out = {"board": strings("/", "compatible"), "checks": []}
    check(out["board"] is not None and "apple,j700" in out["board"],
          "board-j700", out["board"], out)
    check(strings(ANE, "compatible") == ["apple,t8140-ane"],
          "ane-compatible", strings(ANE, "compatible"), out)
    check(strings(ANE, "status") == ["disabled"],
          "ane-disabled", strings(ANE, "status"), out)
    check(words(ANE, "reg") == [4, 0, 0, 0x2000000],
          "ane-window", words(ANE, "reg"), out)
    check(words(ANE, "interrupts") == [0, 655, 4, 0, 668, 4],
          "ane-irqs", words(ANE, "interrupts"), out)
    check(strings(PMGR, "status") == ["okay"] and words(PMGR, "reg") == [0x290, 4],
          "ane-sys-pmgr", {"status": strings(PMGR, "status"), "reg": words(PMGR, "reg")}, out)
    domain = words(PMGR, "phandle")
    check(domain is not None and len(domain) == 1 and words(ANE, "power-domains") == domain,
          "ane-power-domain", {"pmgr": domain, "ane": words(ANE, "power-domains")}, out)
    dart_phandles = []
    for node, base, size in DARTS:
        phandle = words(node, "phandle")
        dart_phandles.append(phandle[0] if phandle and len(phandle) == 1 else None)
        observed = {"compatible": strings(node, "compatible"),
                    "status": strings(node, "status"), "reg": words(node, "reg"),
                    "power-domains": words(node, "power-domains")}
        check(observed == {"compatible": ["apple,t8140-dart", "apple,t8110-dart"],
                           "status": ["disabled"],
                           "reg": [base >> 32, base & 0xFFFFFFFF, 0, size],
                           "power-domains": domain},
              f"dart-{base:x}", observed, out)
    expected_iommus = [v for ph in dart_phandles for v in (ph, 0)]
    check(None not in dart_phandles and words(ANE, "iommus") == expected_iommus,
          "ane-dart-links", {"dart_phandles": dart_phandles,
                             "iommus": words(ANE, "iommus")}, out)
    try:
        lines = [s.split() for s in GENPD.read_text().splitlines() if s.strip().startswith("ane_sys")]
        state = lines[0][1] if len(lines) == 1 and len(lines[0]) > 1 else None
    except OSError:
        state = None
    out["genpd_ane_sys"] = state  # Observed only; debugfs may be unavailable.
    out["verdict"] = "PASS" if all(c["pass"] for c in out["checks"]) else "REFUSED"
    print(json.dumps(out, sort_keys=True))
    return 0 if out["verdict"] == "PASS" else 2


if __name__ == "__main__":
    sys.exit(main())
