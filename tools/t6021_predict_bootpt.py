import struct
"""Predict the staged-buffer boot-PT window (fw_buf 0xe0000..0xe8000) for the
cold mode-1 staged vehicle: slide 1 TiB, TEXT mirror + DATA fills. Compare
against the live FW-PT readback; first divergent descriptor = abort page."""
SLIDE = 0x10000000000
TEXT_IOVA = SLIDE
DATA_IOVA = SLIDE + 0xc4000
entries = {}
entries[0] = 0x483 | (((TEXT_IOVA) >> 14) & 0xFFFFFFF) << 14
entries[1] = 0x483 | (((TEXT_IOVA + 0x4000) >> 14) & 0xFFFFFFF) << 14
va = 0x4000
while va < 0x4000 + 0xc07c8:
    k = va >> 14
    out = TEXT_IOVA + 0x4000 + (va - 0x4000)
    entries[k] = 0x483 | (((out >> 14) & 0xFFFFFFF) << 14)
    va += 0x4000
va = 0xc4000
while va < 0xc4000 + 0x4fafd8:
    k = va >> 14
    out = DATA_IOVA + (va - 0xc4000)
    entries[k] = 0x0060000000000403 | (((out >> 14) & 0xFFFFFFF) << 14)
    va += 0x4000
nz = sorted(entries.items())
print('predicted nonzero entries:', len(nz))
print('last entry index:', hex(nz[-1][0]))
print('first 4:', [(hex(k), hex(v)) for k, v in nz[:4]])
print('entry 0xC4 (mirror/DATA boundary):', hex(entries.get(0xC4, 0)))
print('entry 0x140 (last predicted):', hex(entries.get(0x140, 0)))
print('entry 0x141 absent =', 0x141 not in entries)
open('/tmp/pt_predict.bin', 'wb').write(b''.join(struct.pack('<Q', entries.get(i, 0)) for i in range(4096)))
print('wrote /tmp/pt_predict.bin (32KiB)')
