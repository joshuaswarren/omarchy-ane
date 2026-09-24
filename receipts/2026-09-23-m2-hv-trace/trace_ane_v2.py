# Trace the macOS ANE start sequence under the m1n1 hypervisor, v2.
# Drops the CoreSight window (rung 7: debug domain unclocked, reads hang).
from m1n1.utils import irange

BASE = 0x284000000
ranges = [
    ("asc-wrapper", BASE + 0x1400000, 0x14000),
    ("rvbar", BASE + 0x1050000, 0x100),
    ("pmgr", 0x28E080000, 0x10000),
    ("dart-ane0", 0x285800000, 0x4000),
    ("dart-ane1", 0x285810000, 0x4000),
    ("dart-ane2", 0x285820000, 0x4000),
]
for name, addr, size in ranges:
    hv.trace_range(irange(addr, size), read=False)
    print(f"tracing {name} {addr:#x} size {size:#x}")
print("ANE trace armed (v2, no coresight)")
