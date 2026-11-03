import struct

N = 64
a = [struct.unpack('<e', struct.pack('<e', (c + 1) / 8.0))[0] for c in range(N)]
b = [struct.unpack('<e', struct.pack('<e', 0.25 * (c % 7 + 1)))[0] for c in range(N)]
y = [struct.unpack('<e', struct.pack('<e', x + z))[0] for x, z in zip(a, b)]


def logical(vals):
    return b''.join(struct.pack('<e', v) for v in vals)


def physical(vals):
    buf = bytearray(4096)
    for c, v in enumerate(vals):
        buf[c * 64:c * 64 + 2] = struct.pack('<e', v)
    return bytes(buf)


for name, vals in (('a', a), ('b', b), ('y-oracle', y)):
    open('add-%s.f16' % name, 'wb').write(logical(vals))
    open('add-%s.phys4096' % name, 'wb').write(physical(vals))
print('a[0..3]', a[:4], 'b[0..3]', b[:4], 'y[0..3]', y[:4])
