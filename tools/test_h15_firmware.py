#!/usr/bin/env python3
"""Stage private Apple H15 images into host-owned memory; no device or network."""

import ctypes as ct
import errno
import hashlib
import os
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMAGES = {
    0x8122: ("h15_ane_fw_themis_j51y.im4p.payload", "19b6a49997ecf3eb4ec15960f42ab6a79fecd34c8e1448d2e5c1034e2ec47808"),
    0x6030: (
        "t603x_ane0_fw_erebus_ls5x.im4p.payload",
        "43da9d566a880bab1bd8ef9cd3dd3ea4a55ae6c016d2ec7f4f97f4fda99867a0",
    ),
    0x6031: (
        "t603x_ane0_fw_erebus_pc5x.im4p.payload",
        "ddac37c70bdcaedc6239d5cc85d848e606cd66df454e8a432c24fd7730a1f977",
    ),
}


class Firmware(ct.Structure):
    _fields_ = [("storage", ct.c_void_p), ("size", ct.c_uint64), ("entry", ct.c_uint64)]


def macho_commands(blob):
    magic, cpu, subtype, kind, count, command_bytes, _flags, _reserved = struct.unpack_from("<8I", blob)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x100000C, 0x80000002, 5):
        raise ValueError("unexpected actual firmware header")
    offset = 32
    for _ in range(count):
        command, size = struct.unpack_from("<II", blob, offset)
        if size < 8 or offset + size > 32 + command_bytes:
            raise ValueError("load command outside image header")
        yield command, offset
        offset += size
    if offset != 32 + command_bytes:
        raise ValueError("incomplete image commands")


def macho_segment(blob, offset):
    fields = struct.unpack_from("<II16sQQQQiiII", blob, offset)
    name, vm, extent, file_offset, file_bytes = fields[2:7]
    if file_offset + file_bytes > len(blob) or file_bytes > extent:
        raise ValueError("segment outside image")
    return name.rstrip(b"\0"), vm, extent, file_offset, file_bytes


def macho_entry(blob, offset):
    flavor, words = struct.unpack_from("<II", blob, offset + 8)
    if flavor != 6 or words != 68:
        raise ValueError("unexpected ARM64 thread state")
    return struct.unpack_from("<Q", blob, offset + 16 + 32 * 8)[0]


def macho_image(blob):
    segments, entry = [], None
    for command, offset in macho_commands(blob):
        if command == 0x19:
            segments.append(macho_segment(blob, offset))
        elif command == 5:
            entry = macho_entry(blob, offset)
    if entry is None:
        raise ValueError("missing entry")
    size = max(vm + extent for _, vm, extent, _, _ in segments)
    result = bytearray(size)
    for _, vm, _, file_offset, file_bytes in segments:
        result[vm : vm + file_bytes] = blob[file_offset : file_offset + file_bytes]
    return bytes(result), entry


class H15FirmwareTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        location = os.environ.get("H15_FW_INPUT_DIR")
        if not location:
            raise unittest.SkipTest(
                "set H15_FW_INPUT_DIR to the private 26A428 acquisition; firmware is not redistributed"
            )
        cls.images = {}
        for soc, (name, digest) in IMAGES.items():
            blob = (Path(location) / name).read_bytes()
            if len(blob) != 1605632 or hashlib.sha256(blob).hexdigest() != digest:
                raise ValueError(f"private input does not match actual measured profile: {name}")
            cls.images[soc] = blob
        ane1_location = os.environ.get("H15_FW_ANE1_INPUT_DIR")
        if not ane1_location:
            raise unittest.SkipTest("set H15_FW_ANE1_INPUT_DIR to the separate private ANE1 acquisition")
        cls.ane1 = (Path(ane1_location) / "t603x_ane1_fw_erebus_pc5x.im4p.payload").read_bytes()
        if (
            len(cls.ane1) != 1605632
            or hashlib.sha256(cls.ane1).hexdigest()
            != "0e0a01ec3b4ef08050cae2df75d19c531254f7e1ec109ff6bb05ff575c7c13d4"
        ):
            raise ValueError("private input does not match the measured ANE1 profile")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        library = Path(cls.temp.name) / "libane.so"
        wrapper = Path(cls.temp.name) / "allocation.c"
        wrapper.write_text(
            "#include <stddef.h>\n#include <string.h>\nint fail_calloc;\n"
            "void *__real_calloc(size_t, size_t);\nvoid *__real_malloc(size_t);\n"
            "void *__wrap_calloc(size_t n, size_t size) {\n"
            "if (fail_calloc) { fail_calloc = 0; return NULL; }\n"
            "return __real_calloc(n, size);\n}\n"
            "void *__wrap_malloc(size_t n) { void *p = __real_malloc(n);\n"
            "if (p) { memset(p, 0xa5, n); }\nreturn p; }\n"
        )
        command = [
            os.environ.get("CC", "cc"),
            "-shared",
            "-fPIC",
            "-std=gnu99",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(ROOT / "libane"),
            "-I" + str(ROOT / "ane/src/uapi/drm"),
            "-I/usr/include/libdrm",
            *map(str, sorted((ROOT / "libane").glob("ane*.c"))),
            str(wrapper),
            "-Wl,--wrap=calloc,--wrap=malloc",
            "-lm",
            "-o",
            str(library),
        ]
        subprocess.run(command, check=True)
        cls.library = ct.CDLL(str(library))

    def consumer(self):
        stage = self.library.ane_h15_firmware_stage
        stage.argtypes = [ct.c_uint32, ct.c_uint32, ct.c_void_p, ct.c_uint64, ct.POINTER(Firmware)]
        stage.restype = ct.c_int
        release = self.library.ane_h15_firmware_free
        release.argtypes = [ct.POINTER(Firmware)]
        release.restype = None
        return stage, release

    def test_actual_images_match_independent_macho_layout_and_own_the_bytes(self):
        stage, release = self.consumer()
        for soc in (*IMAGES, 0x6034):
            with self.subTest(soc=hex(soc)):
                blob = self.images[0x6031 if soc == 0x6034 else soc]
                expected, entry = macho_image(blob)
                source = ct.create_string_buffer(blob)
                image = Firmware()
                self.assertEqual(stage(soc, 0, source, len(blob), ct.byref(image)), 0)
                try:
                    ct.memset(source, 0xA5, len(blob))
                    self.assertEqual(image.size, len(expected))
                    self.assertEqual(image.entry, entry)
                    self.assertEqual(ct.string_at(image.storage, image.size), expected)
                finally:
                    release(ct.byref(image))
                self.assertEqual(bytes(image), bytes(ct.sizeof(image)))
                release(ct.byref(image))
                self.assertEqual(bytes(image), bytes(ct.sizeof(image)))

    def test_corruption_truncation_and_foreign_profiles_preserve_output(self):
        stage, _ = self.consumer()
        blob = self.images[0x8122]
        for offset in (0, 32, 584 + 40, 2536 + 16 + 32 * 8, len(blob) - 1):
            damaged = bytearray(blob)
            damaged[offset] ^= 1
            image = Firmware(None, 0x1234, 0x5678)
            before = bytes(image)
            self.assertEqual(
                stage(0x8122, 0, ct.create_string_buffer(bytes(damaged)), len(blob), ct.byref(image)), -errno.EBADMSG
            )
            self.assertEqual(bytes(image), before)
        source = ct.create_string_buffer(blob)
        for size in (0, len(blob) - 1, len(blob) + 1, 2**64 - 1):
            image = Firmware(None, 0x1234, 0x5678)
            before = bytes(image)
            self.assertEqual(stage(0x8122, 0, source, size, ct.byref(image)), -errno.EINVAL)
            self.assertEqual(bytes(image), before)
        for soc in (0x6030, 0x6031, 0x6034):
            image = Firmware()
            self.assertEqual(stage(soc, 0, source, len(blob), ct.byref(image)), -errno.EBADMSG)
            self.assertEqual(bytes(image), bytes(ct.sizeof(image)))
        image = Firmware()
        self.assertEqual(stage(0x9999, 0, source, len(blob), ct.byref(image)), -errno.ENODEV)
        self.assertEqual(stage(0x8122, 0, None, len(blob), ct.byref(image)), -errno.EINVAL)
        self.assertEqual(stage(0x8122, 0, source, len(blob), None), -errno.EINVAL)

    def test_live_owner_is_not_replaced(self):
        stage, release = self.consumer()
        blob = self.images[0x6030]
        source = ct.create_string_buffer(blob)
        image = Firmware()
        self.assertEqual(stage(0x6030, 0, source, len(blob), ct.byref(image)), 0)
        try:
            before = bytes(image)
            self.assertEqual(stage(0x6030, 0, source, len(blob), ct.byref(image)), -errno.EBUSY)
            self.assertEqual(bytes(image), before)
        finally:
            release(ct.byref(image))

    def test_ultra_engines_use_distinct_images_and_independent_host_owners(self):
        payloads = (self.images[0x6031], self.ane1)
        self.assertNotEqual(payloads[0], payloads[1])
        stage, release = self.consumer()
        images = (Firmware(), Firmware())
        try:
            for engine, payload in enumerate(payloads):
                source = ct.create_string_buffer(payload)
                self.assertEqual(stage(0x6032, engine, source, len(payload), ct.byref(images[engine])), 0)
                ct.memset(source, 0xA5, len(payload))
                expected, entry = macho_image(payload)
                self.assertEqual(images[engine].entry, entry)
                self.assertEqual(ct.string_at(images[engine].storage, images[engine].size), expected)
            first, second = sorted((image.storage, image.storage + image.size) for image in images)
            self.assertLessEqual(first[1], second[0])
            release(ct.byref(images[0]))
            self.assertEqual(bytes(images[0]), bytes(ct.sizeof(images[0])))
            expected, _ = macho_image(payloads[1])
            self.assertEqual(ct.string_at(images[1].storage, images[1].size), expected)
        finally:
            for image in images:
                release(ct.byref(image))

    def test_engine_identity_refuses_aliases_and_missing_engines(self):
        stage, _ = self.consumer()
        source = ct.create_string_buffer(self.images[0x6031])
        for soc, engine in ((0x8122, 1), (0x6030, 1), (0x6031, 1), (0x6034, 1), (0x6032, 2), (0x6032, 0xFFFFFFFF)):
            image = Firmware(None, 0x1234, 0x5678)
            before = bytes(image)
            self.assertEqual(stage(soc, engine, source, len(self.images[0x6031]), ct.byref(image)), -errno.ENODEV)
            self.assertEqual(bytes(image), before)
        for engine, wrong in ((0, self.ane1), (1, self.images[0x6031])):
            image = Firmware(None, 0x1234, 0x5678)
            before = bytes(image)
            self.assertEqual(
                stage(0x6032, engine, ct.create_string_buffer(wrong), len(wrong), ct.byref(image)), -errno.EBADMSG
            )
            self.assertEqual(bytes(image), before)

    def test_validation_precedes_allocation_and_failure_is_transactional(self):
        stage, _ = self.consumer()
        blob = self.images[0x6031]
        flag = ct.c_int.in_dll(self.library, "fail_calloc")
        image = Firmware(None, 0x1234, 0x5678)
        before = bytes(image)
        flag.value = 1
        try:
            damaged = ct.create_string_buffer(bytes(len(blob)))
            self.assertEqual(stage(0x6031, 0, damaged, len(blob), ct.byref(image)), -errno.EBADMSG)
            self.assertEqual(flag.value, 1)
            self.assertEqual(stage(0x6031, 0, ct.create_string_buffer(blob), len(blob), ct.byref(image)), -errno.ENOMEM)
            self.assertEqual(bytes(image), before)
        finally:
            flag.value = 0


if __name__ == "__main__":
    unittest.main()
