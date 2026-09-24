import os
os.environ["M1N1DEVICE"] = "/dev/ttyACM0"
from m1n1.setup import *  # noqa: E402,F401,F403

print("nvme_init", p.nvme_init(), flush=True)
buf = u.heap.memalign(4096, 4096)
ok = p.nvme_read(1, 913950004, buf)
print("nvme_read", ok, flush=True)
got = iface.readmem(buf, 16)
print("first16", got.hex(), flush=True)
u.heap.free(buf)
p.nvme_shutdown()
print("DONE", flush=True)
