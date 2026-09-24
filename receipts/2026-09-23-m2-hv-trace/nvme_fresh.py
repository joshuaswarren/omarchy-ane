import os, time
os.environ["M1N1DEVICE"] = "/dev/ttyACM0"
from m1n1.setup import *  # noqa: E402,F401,F403

E0LBA = 913950004
print("nvme_init", p.nvme_init(), flush=True)
buf = u.heap.memalign(4096, 4096)
ok = p.nvme_read(1, E0LBA, buf)
print("first-block", ok, flush=True)
print("sleeping 120s (hv is initialized by setup, ANS may lapse)...", flush=True)
for i in range(12):
    time.sleep(10)
    ok = p.nvme_read(1, E0LBA, buf)
    head = iface.readmem(buf, 4).hex()
    print(f"t+{(i + 1) * 10}s read", ok, head, flush=True)
p.nvme_shutdown()
print("DONE", flush=True)
