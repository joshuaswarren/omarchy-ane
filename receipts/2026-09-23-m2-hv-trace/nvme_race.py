import os
os.environ["M1N1DEVICE"] = "/dev/ttyACM0"
from m1n1.setup import *  # noqa: E402,F401,F403

print("nvme_init", p.nvme_init(), flush=True)
ok1 = p.nvme_read(1, 913950004, u.malloc(4096))
print("read-while-engine-on-1", ok1, flush=True)
print("cpu status", hex(p.read32(0x284140048)), flush=True)
print("pmgr 2e0?", flush=True)
print("DONE", flush=True)
