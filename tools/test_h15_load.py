#!/usr/bin/env python3
"""Host-only H15 load construction; golden bytes are independent fixture observations."""
import ctypes as ct
import os
import struct
import subprocess
import tempfile
import unittest
from itertools import pairwise
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / 'fixtures/h15-hwx/add'
NAMES = ('generic', 'constants', 'text', 'operation', 'procedure', 'property')
SIZES = (664, 16384, 284, 1040, 56, 40)
ADDRESSES = [0x10002000000 + n * 0x10000 for n in range(6)]


class Payload(ct.Structure):
    _fields_ = [('data', ct.c_void_p), ('size', ct.c_uint64)]


class Relocation(ct.Structure):
    _fields_ = [('iova', ct.c_uint64), ('capacity', ct.c_uint64)]


class Load(ct.Structure):
    _fields_ = [('storage', ct.c_void_p), ('section', ct.c_void_p * 6),
                ('command', ct.c_ubyte * 448)]


class H15LoadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / 'libane.so'
        wrapper = Path(cls.temp.name) / 'allocation.c'
        wrapper.write_text('#include <stddef.h>\nint fail_calloc;\n'
                           'void *__real_calloc(size_t, size_t);\n'
                           'void *__wrap_calloc(size_t n, size_t size) {\n'
                           'if (fail_calloc) { fail_calloc = 0; return NULL; }\n'
                           'return __real_calloc(n, size);\n}\n')
        command = [os.environ.get('CC', 'cc'), '-shared', '-fPIC', '-std=gnu99',
                   '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'libane'),
                   '-I' + str(ROOT / 'ane/src/uapi/drm'), '-I/usr/include/libdrm',
                   *map(str, sorted((ROOT / 'libane').glob('ane*.c'))),
                   str(wrapper), '-Wl,--wrap=calloc', '-lm', '-o', str(library)]
        subprocess.run(command, check=True)
        cls.library = ct.CDLL(str(library))

    def builder(self):
        build = self.library.ane_h15_load_build
        build.argtypes = [ct.POINTER(Payload), ct.POINTER(Relocation), ct.POINTER(Load)]
        build.restype = ct.c_int
        release = self.library.ane_h15_load_free
        release.argtypes = [ct.POINTER(Load)]
        release.restype = None
        return build, release

    def inputs(self, label='base'):
        hwx = (FIXTURE / 'model.hwx').read_bytes()
        payloads = [(FIXTURE / 'load' / label / 'generic.bin').read_bytes(),
                    hwx[0x8140:0xc140], hwx[0x8000:0x811c],
                    *( (FIXTURE / 'load' / label / (name + '.bin')).read_bytes()
                       for name in NAMES[3:] )]
        self.assertEqual(tuple(map(len, payloads)), SIZES)
        buffers = [ct.create_string_buffer(value) for value in payloads]
        args = (Payload * 6)(*[Payload(ct.cast(value, ct.c_void_p), size)
                                 for value, size in zip(buffers, SIZES)])
        relocations = (Relocation * 6)(*[Relocation(address, 0x4000) for address in ADDRESSES])
        return payloads, buffers, args, relocations

    def test_oracle_family_and_owned_storage(self):
        build, release = self.builder()
        for label in ('base', 'extent', 'ne', 'property', 'slots'):
            with self.subTest(label=label):
                payloads, buffers, args, relocations = self.inputs(label)
                load = Load()
                self.assertEqual(build(args, relocations, ct.byref(load)), 0)
                try:
                    expected = (FIXTURE / 'load' / label / 'command.bin').read_bytes()
                    self.assertEqual(bytes(load.command), expected)
                    self.assertTrue(load.storage)
                    ends = []
                    for pointer, payload, buffer in zip(load.section, payloads, buffers):
                        ct.memset(buffer, 0x5a, len(payload))
                        self.assertEqual(ct.string_at(pointer, len(payload)), payload)
                        self.assertGreaterEqual(pointer, load.storage)
                        ends.append((pointer, pointer + len(payload)))
                    for left, right in pairwise(ends):
                        self.assertLessEqual(left[1], right[0])
                finally:
                    release(ct.byref(load))
                self.assertEqual(bytes(load), bytes(ct.sizeof(load)))
                release(ct.byref(load))
                self.assertEqual(bytes(load), bytes(ct.sizeof(load)))

    def test_invalid_input_is_transactional(self):
        build, _ = self.builder()
        for index in range(6):
            cases = [('payload', 'data', None), ('payload', 'size', SIZES[index] - 1),
                     ('reloc', 'iova', 0), ('reloc', 'iova', ADDRESSES[index] + 1),
                     ('reloc', 'capacity', 0), ('reloc', 'capacity', 0x4001),
                     ('reloc', 'iova', 0xffffffffffffc000),
                     ('reloc', 'iova', ADDRESSES[(index + 1) % 6])]
            for target, field, value in cases:
                with self.subTest(index=index, target=target, field=field, value=value):
                    _, _buffers, args, relocations = self.inputs()
                    load = Load()
                    load.command[0] = 0xa5
                    before = bytes(load)
                    record = args[index] if target == 'payload' else relocations[index]
                    setattr(record, field, value)
                    self.assertEqual(build(args, relocations, ct.byref(load)), -22)
                    self.assertEqual(bytes(load), before)
        self.assertEqual(build(None, None, None), -22)

    def test_existing_owner_is_not_replaced(self):
        build, release = self.builder()
        _, _buffers, args, relocations = self.inputs()
        load = Load()
        self.assertEqual(build(args, relocations, ct.byref(load)), 0)
        try:
            before = bytes(load)
            self.assertEqual(build(args, relocations, ct.byref(load)), -16)
            self.assertEqual(bytes(load), before)
        finally:
            release(ct.byref(load))

    def test_explicit_relocations_change_only_address_fields(self):
        build, release = self.builder()
        _, _buffers, args, relocations = self.inputs()
        for value in relocations:
            value.iova += 0x1000000
        load = Load()
        self.assertEqual(build(args, relocations, ct.byref(load)), 0)
        try:
            expected = bytearray((FIXTURE / 'load/base/command.bin').read_bytes())
            for offset, value in zip((0x20, 0x50, 0x80, 0xb0, 0xe0, 0x140), relocations):
                struct.pack_into('<Q', expected, offset, value.iova)
            self.assertEqual(bytes(load.command), expected)
        finally:
            release(ct.byref(load))

    def test_allocation_failure_preserves_output(self):
        build, _ = self.builder()
        _, _buffers, args, relocations = self.inputs()
        load = Load()
        load.command[0] = 0xa5
        before = bytes(load)
        ct.c_int.in_dll(self.library, 'fail_calloc').value = 1
        self.assertEqual(build(args, relocations, ct.byref(load)), -12)
        self.assertEqual(bytes(load), before)



if __name__ == '__main__':
    unittest.main()
