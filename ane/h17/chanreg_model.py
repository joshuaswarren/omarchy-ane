#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline H17 initialization-descriptor model. No device or filesystem I/O.

Source: retained-cache hash and instruction VAs in firmware-load-path receipt
sections 15/16. This models stores, not firmware acceptance. Region ownership
here is a caller assertion; it does not establish a Linux DMA mapping.
"""
from dataclasses import dataclass
import struct


PAGE = 0x4000
FW_BYTES = 0x380000
DM_BYTES = 0x40000
DESCRIPTOR_BYTES = 0x174
DESCRIPTOR_ALLOCATION = 0x180
VM_BASE = 0x10000000000
VM_END = VM_BASE + 0x30000000000
U32_MAX = (1 << 32) - 1
DM_IOVA = VM_BASE + FW_BYTES
IPC_IOVA = DM_IOVA + DM_BYTES
IPC_MAPPED = 0x10000
HEAP_IOVA = IPC_IOVA + IPC_MAPPED


def uint(value, bits, name):
    if type(value) is not int or not 0 <= value < (1 << bits):
        raise ValueError(f"{name}: not u{bits}")
    return value


def round_page(size):
    uint(size, 64, "size")
    rounded = (size + PAGE - 1) & -PAGE
    return uint(rounded, 64, "rounded size")


def validate_channel_layout(dm, ipc):
    """c4c43d3: channel W-load + unchanged SCRATCH1 constrain both aliases."""
    for region in (dm, ipc):
        region.validate()
        alias = region.iova & 0xFFFFFFFFF
        if alias >= 1 << 32 or region.mapped_bytes > (1 << 32) - alias:
            raise ValueError("channel mapping exceeds firmware low-32 CPU alias")
    if dm.iova >> 32 != ipc.iova >> 32:
        raise ValueError("DM/IPC prefixes break SCRATCH1 reply reconstruction")


@dataclass(frozen=True)
class Region:
    name: str
    iova: int
    size: int                 # logical extent, not mapping padding
    mapped_bytes: int
    owned: bool = False       # explicit model assertion ONLY
    retained: bool = False    # must remain true after release/timeout

    def validate(self):
        uint(self.iova, 64, self.name + ".iova")
        uint(self.size, 64, self.name + ".size")
        uint(self.mapped_bytes, 64, self.name + ".mapped_bytes")
        if not self.size or not self.owned or not self.retained:
            raise ValueError(self.name + ": empty, unowned, or not retained")
        if self.iova % PAGE or self.mapped_bytes % PAGE:
            raise ValueError(self.name + ": mapping is not 16-KiB aligned")
        if self.size > self.mapped_bytes:
            raise ValueError(self.name + ": logical extent exceeds mapping")
        if self.iova < VM_BASE or self.iova + self.mapped_bytes > VM_END:
            raise ValueError(self.name + ": outside source-pinned DART aperture")

    def offset(self, address, length):
        uint(address, 64, "address")
        uint(length, 64, "length")
        delta = address - self.iova
        if not length or delta < 0 or delta > self.size or length > self.size - delta:
            raise ValueError(self.name + ": complete range is not owned")
        return delta


@dataclass(frozen=True)
class BootInputs:
    channel_count: int        # initial SCRATCH0
    ipc_last_offset: int      # initial SCRATCH1, kext adds one in w8
    heap_bytes: int           # initial SCRATCH3, optional (zero => absent)
    firmware_argument: int = 0  # [dev+0x4a8], initialized zero for preload
    option_flags: int = 4     # template +0xc0; conditional OR 8 / 0x10

    def validate(self):
        for name in ("channel_count", "ipc_last_offset", "heap_bytes",
                     "firmware_argument", "option_flags"):
            uint(getattr(self, name), 32, name)
        # Kext permits zero at its count bound but its required-name lookups
        # cannot succeed then. This model deliberately refuses zero early.
        if not 1 <= self.channel_count <= 32:
            raise ValueError("channel_count: first-run policy requires 1..32")
        if self.ipc_last_offset == U32_MAX:
            raise ValueError("SCRATCH1 + 1 would wrap to zero")
        if self.option_flags & ~0x1C or not self.option_flags & 4:
            raise ValueError("option_flags: outside traced default/conditional bits")

    @property
    def ipc_requested(self):
        self.validate()
        return max(PAGE, self.ipc_last_offset + 1)

    @property
    def heap_requested(self):
        self.validate()
        return max(PAGE, self.heap_bytes) if self.heap_bytes else 0


def build_descriptor(inputs, firmware, ipc, dm, heap=None, *, offset=0):
    """Return the initialized 0x180-byte allocation and its published IOVA.

    Preloaded, direct path only. Optional modes/unknown template bits are
    refused rather than guessed. Must be copied into a zeroed DM arena and
    synchronized for the device by a future kernel implementation.
    """
    inputs.validate()
    regions = [firmware, ipc, dm] + ([heap] if heap is not None else [])
    for region in regions:
        region.validate()
    validate_channel_layout(dm, ipc)
    if len({r.name for r in regions}) != len(regions):
        raise ValueError("duplicate region names")
    ordered = sorted(regions, key=lambda r: r.iova)
    for left, right in zip(ordered, ordered[1:]):
        if left.iova + left.mapped_bytes > right.iova:
            raise ValueError("overlapping DMA mappings")
    if firmware.size != FW_BYTES or dm.size != DM_BYTES:
        raise ValueError("unexpected firmware/DM arena size")
    if ipc.size != inputs.ipc_requested:
        raise ValueError("IPC logical extent differs from traced allocation request")
    if bool(heap is not None) != bool(inputs.heap_requested):
        raise ValueError("optional heap presence does not match SCRATCH3")
    if heap is not None and heap.size != inputs.heap_requested:
        raise ValueError("heap size differs from traced allocation request")
    uint(offset, 32, "descriptor offset")
    if offset % 64:
        raise ValueError("descriptor offset is not allocator-aligned")
    address = dm.iova + offset
    dm.offset(address, DESCRIPTOR_ALLOCATION)

    blob = bytearray(DESCRIPTOR_ALLOCATION)  # arena is bzero'd in start()
    for at, value in (
        (0x00, firmware.iova), (0x08, ipc.iova),
        (0x10, FW_BYTES), (0x18, 0x10000000 - FW_BYTES),
        (0x20, heap.iova if heap else 0),
        (0x28, inputs.heap_requested),
        (0x50, inputs.ipc_last_offset + 1),
        (0x58, dm.iova), (0x60, DM_BYTES),
    ):
        struct.pack_into("<Q", blob, at, value)
    struct.pack_into("<I", blob, 0x30, inputs.firmware_argument)
    struct.pack_into("<I", blob, 0x68, 0x40)
    # kalloc_data(0x100, Z_ZERO), then template[0xc0] |= 4 / 8 / 0x10.
    struct.pack_into("<I", blob, 0x6C + 0xC0, inputs.option_flags)
    return bytes(blob), address


def table_offset(inputs, ipc, table_address):
    """Stricter-than-kext first-run policy: table must fit in owned IPC.

    No arbitrary physical-memory fallback or inclusive-end acceptance. The
    kext checks only the starting pointer; this validates the whole table.
    """
    inputs.validate()
    ipc.validate()
    if ipc.size != inputs.ipc_requested:
        raise ValueError("unexpected IPC extent")
    return ipc.offset(table_address, inputs.channel_count * 0x100)


@dataclass(frozen=True)
class ChannelRecord:
    """Decoded 0x100-byte firmware table entry; values are observations."""
    index: int
    name: str
    channel_type: int
    source: int
    slots: int
    buffer_iova: int


def registration_init_ranges(records, ipc):
    """Validate the host's source-traced type-0 control-block initialization.

    The pinned J700 trace shows the channel constructor initializing `slots`
    0x40-byte blocks only when the entry's type is zero. This deliberately
    does not claim that firmware-side use of other channel buffers is safe.
    """
    ipc.validate()
    required = {"SHAREDMALLOC", "TERMINAL", "BUF_T2H"}
    if len(records) != 8:
        raise ValueError("first-run registration model requires eight records")
    names = set()
    ranges = []
    for expected, record in enumerate(records):
        if not isinstance(record, ChannelRecord) or record.index != expected:
            raise ValueError("channel records must be unique and ordered 0..7")
        if (not isinstance(record.name, str) or not record.name or
                len(record.name.encode("ascii", errors="strict")) > 31):
            raise ValueError("invalid channel name")
        for value, label in ((record.channel_type, "type"),
                             (record.source, "source"), (record.slots, "slots")):
            uint(value, 32, f"channel[{expected}].{label}")
        uint(record.buffer_iova, 64, f"channel[{expected}].buffer_iova")
        if record.name in names:
            raise ValueError("duplicate channel name")
        names.add(record.name)
        if record.channel_type != 0:
            continue
        if record.slots == 0:
            continue
        length = record.slots * 0x40
        if length >= 1 << 64:
            raise ValueError("channel control-block extent overflows u64")
        if record.buffer_iova % 0x40:
            raise ValueError("channel control-block base is not 0x40-aligned")
        offset = ipc.offset(record.buffer_iova, length)
        ranges.append((record.index, offset, length))
    if not required <= names:
        raise ValueError("required channel names are missing")
    ordered = sorted((offset, offset + length, index)
                     for index, offset, length in ranges)
    if any(left[1] > right[0] for left, right in zip(ordered, ordered[1:])):
        raise ValueError("host-initialized channel control-block ranges overlap")
    return tuple(ranges)
