import os
os.environ["M1N1DEVICE"] = "/dev/ttyACM0"
from m1n1.setup import *  # noqa: E402,F401,F403

E0 = (913950004, 3481)
E1 = (913956521, 5019)
COMP_SIZE = 34812701
TOTAL = 34816000

print("nvme_init", p.nvme_init(), flush=True)
buf = u.heap.memalign(4096, TOTAL)
print("buf", hex(buf), flush=True)
off = 0
for lba, count in (E0, E1):
    for b in range(count):
        ok = p.nvme_read(1, lba + b, buf + off)
        if not ok:
            print("FAIL lba", lba + b, "off", off, flush=True)
            raise SystemExit(1)
        off += 4096
        if (off // 4096) % 1024 == 0:
            print("blocks", off // 4096, flush=True)
print("read done", off, flush=True)
head = iface.readmem(buf, 16)
print("head", head.hex(), flush=True)
u.heap.free(buf)
p.nvme_shutdown()
print("DONE", flush=True)
