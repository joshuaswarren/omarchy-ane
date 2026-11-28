// SPDX-License-Identifier: MIT
//
// kex0-efi.zig — BMN-03 channel probe delivered as an EFI application via
// efibootmgr BootNext (one-shot: consumed, then BootOrder resumes stock).
//
// Runs inside u-boot's EFI environment (MMU on, u-boot's page tables, ES1).
// Safety design: before touching any MMIO we walk the live translation
// tables and only proceed if every target physical address is mapped. If
// anything is unmapped we return an error status — the boot manager then
// falls through to BootOrder and boots Linux normally. Nothing is armed or
// written in that path, so every failure mode returns stock.
//
// Success path: arm the t8103 WDT (178 s), write KEX0 stages into the
// isp-heap marker hole, then WFE. The WDT resets the SoC; next boot takes
// BootOrder (BootNext is consumed) → Linux → marker is read back.
//
// Build: zig build-exe -target aarch64-uefi -O ReleaseSmall \
//          -femit-bin=kex0.efi kex0-efi.zig

const std = @import("std");
const uefi = std.os.uefi;

const WDT_BASE: u64 = 0x23d2b0000;
const WDT_COUNT: u64 = 0x10;
const WDT_ALARM: u64 = 0x14;
const WDT_CTL: u64 = 0x1c;

const PMGR_PS_BASE: u64 = 0x23b700000;
const PMGR_ANE_PAGE: u64 = 0x23b7c000; // ane_sys_cpu + ane_base + set1-5 live here
const ANE_MMIO_BASE: u64 = 0x26b800000; // ANE engine register window
const ANE_SCRATCH0: u64 = 0x26b840048;
const MARKER_BASE: u64 = 0xbd2bc0000;

const PROBE_SECONDS: u32 = 90;

const EFI_GLOBAL_VARIABLE: uefi.Guid = .{
    .time_low = 0x8be4df61,
    .time_mid = 0x93ca,
    .time_high_and_version = 0x11d2,
    .clock_seq_high_and_reserved = 0xaa,
    .clock_seq_low = 0x0d,
    .node = .{ 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c },
};

inline fn rd(addr: u64) u32 {
    const p: *volatile u32 = @ptrFromInt(addr);
    const v = p.*;
    asm volatile ("dsb sy" ::: .{ .memory = true });
    return v;
}

inline fn wr(addr: u64, v: u32) void {
    const p: *volatile u32 = @ptrFromInt(addr);
    p.* = v;
    asm volatile ("dsb sy" ::: .{ .memory = true });
}

inline fn cntpct() u64 {
    return asm volatile ("mrs %[v], cntpct_el0" : [v] "=r" (-> u64));
}

inline fn current_el() u64 {
    return (asm volatile ("mrs %[v], currentel" : [v] "=r" (-> u64)) >> 2) & 3;
}

/// Walk the live translation tables for `va`. Returns true if a table or
/// block descriptor maps it. All reads are of page-table RAM, which u-boot
/// always maps — this function can run before any MMIO is touched.
fn is_mapped(va: u64) bool {
    const el = current_el();
    var ttbr: u64 = undefined;
    var tcr: u64 = undefined;
    if (el == 2) {
        ttbr = asm volatile ("mrs %[v], s3_4_c2_c0_0" : [v] "=r" (-> u64));
        tcr = asm volatile ("mrs %[v], s3_4_c2_c0_2" : [v] "=r" (-> u64));
    } else {
        ttbr = asm volatile ("mrs %[v], s3_0_c2_c0_0" : [v] "=r" (-> u64));
        tcr = asm volatile ("mrs %[v], s3_0_c2_c0_2" : [v] "=r" (-> u64));
    }
    const t0sz = tcr & 0x3f;
    const tg0 = (tcr >> 14) & 0x3; // 0=4K, 2=16K, 1=64K
    const gshift: u6 = switch (tg0) {
        0 => 12,
        1 => 16,
        2 => 14,
        else => return false,
    };
    const idx_bits: u6 = gshift - 3;
    const va_bits: u64 = 64 - t0sz;
    if (va_bits <= gshift) return false;
    const levels_used: u64 = (va_bits - gshift + idx_bits - 1) / idx_bits;
    if (levels_used == 0 or levels_used > 4) return false;
    var level: u64 = 4 - levels_used;
    var table: u64 = ttbr & 0x0000_ffff_ffff_f000;
    const idx_mask: u64 = (@as(u64, 1) << idx_bits) - 1;

    while (level < 4) : (level += 1) {
        const shift: u6 = @intCast(gshift + (3 - level) * idx_bits);
        const idx = (va >> shift) & idx_mask;
        const desc = (@as(*volatile u64, @ptrFromInt(table + idx * 8))).*;
        if (desc & 1 == 0) return false;
        if (level == 3) return true; // page descriptor
        if (desc & 2 == 0) return true; // block descriptor: mapped
        table = desc & 0x0000_ffff_ffff_f000;
    }
    return false;
}

fn wdt_arm(seconds: u32) void {
    wr(WDT_BASE + WDT_CTL, 0);
    wr(WDT_BASE + WDT_COUNT, 0);
    wr(WDT_BASE + WDT_ALARM, seconds *% 24000000);
    wr(WDT_BASE + WDT_CTL, 4);
}

fn marker(off: u64, v: u32) void {
    wr(MARKER_BASE + off, v);
}

/// Status channel that does not depend on MMIO mapping at all: a
/// non-volatile EFI variable persisted into ubootefi.var, readable from
/// Linux after the stock boot. Each call overwrites the previous value, so
/// the final byte says exactly how far the app got.
fn status(v: u8) void {
    const rt = uefi.system_table.runtime_services;
    const data = [_]u8{v};
    rt.setVariable(
        &[_:0]u16{ 'B', 'M', 'N', '0', '3', 'S' },
        &EFI_GLOBAL_VARIABLE,
        .{ .non_volatile = true, .bootservice_access = true, .runtime_access = true },
        &data,
    ) catch {};
}

/// Best-effort delete of BootNext so a boot manager that does not consume
/// it cannot loop us. Harmless if it fails: spec-conformant managers have
/// already consumed it by the time we run.
fn clear_boot_next() void {
    const rt = uefi.system_table.runtime_services;
    rt.setVariable(
        &[_:0]u16{ 'B', 'o', 'o', 't', 'N', 'e', 'x', 't' },
        &EFI_GLOBAL_VARIABLE,
        .{},
        &.{},
    ) catch {};
}

/// Result record, carried in an EFI variable (BMN03R, non-volatile): the
/// isp-heap DRAM hole turned out to be unmapped under u-boot's page tables,
/// so the EFI variable IS the mailbox. 96 bytes:
///   +0x00 magic "BMN3"   +0x04 ps_ok bitmask   +0x08..0x27 eight PS readbacks
///   +0x28..0x37 four scratch reads             +0x38 scratch write readback
///   +0x3c seal 0xdeadbee5
///   +0x40 map bitmap: bit i set when PROBES[i] is mapped in u-boot's tables
///   +0x44 environment: el<<24 | tg0<<8 | t0sz  +0x48..0x5f reserved
var result: [96]u8 = undefined;

fn put32(off: usize, v: u32) void {
    result[off] = @truncate(v);
    result[off + 1] = @truncate(v >> 8);
    result[off + 2] = @truncate(v >> 16);
    result[off + 3] = @truncate(v >> 24);
}

fn save_result() void {
    const rt = uefi.system_table.runtime_services;
    rt.setVariable(
        &[_:0]u16{ 'B', 'M', 'N', '0', '3', 'R' },
        &EFI_GLOBAL_VARIABLE,
        .{ .non_volatile = true, .bootservice_access = true, .runtime_access = true },
        &result,
    ) catch {};
}

const PMGR_PS_TARGET_ACTIVE: u32 = 0xf;
const PMGR_PS_ACTUAL_MASK: u32 = 0xf0;
const PMGR_WRITE_MASK: u32 = (1 << 28) | (1 << 9) | (1 << 8) | 0xf;
const POLL_SPINS: u32 = 0x200000;

fn raise_one(off: u64, i: usize) u32 {
    const a = PMGR_PS_BASE + off;
    var v = rd(a);
    if (v & PMGR_PS_ACTUAL_MASK != PMGR_PS_ACTUAL_MASK) {
        v &= ~PMGR_WRITE_MASK;
        v |= PMGR_PS_TARGET_ACTIVE;
        wr(a, v);
        var spins = POLL_SPINS;
        while (spins != 0) : (spins -= 1) {
            v = rd(a);
            if (v & PMGR_PS_ACTUAL_MASK == PMGR_PS_ACTUAL_MASK) break;
        }
    }
    put32(0x08 + i * 4, v);
    return if (v & PMGR_PS_ACTUAL_MASK == PMGR_PS_ACTUAL_MASK) @as(u32, 1) << @intCast(i) else 0;
}

/// One bit per probed physical base; bits 0..3 keep the BMN-03 layout so the
/// status nibble and gate masks mean what they meant on the first run.
const PROBES = [_]u64{ WDT_BASE, PMGR_PS_BASE, ANE_SCRATCH0, MARKER_BASE, PMGR_ANE_PAGE, ANE_MMIO_BASE };

/// Exception level + translation granule + region size of the tables we
/// walked, so the survey's host side can name the regime it measured.
fn env_word() u32 {
    const el = current_el();
    const tcr = if (el == 2)
        asm volatile ("mrs %[v], s3_4_c2_c0_2" : [v] "=r" (-> u64))
    else
        asm volatile ("mrs %[v], s3_0_c2_c0_2" : [v] "=r" (-> u64));
    return @as(u32, @intCast(el)) << 24 | @as(u32, @intCast((tcr >> 14) & 3)) << 8 |
        @as(u32, @intCast(tcr & 0x3f));
}

pub fn main() uefi.Status {
    status(0x11); // entry reached

    // Map check on every target PA before any MMIO touch. Walk reads only
    // page-table RAM (always mapped), so it cannot fault.
    var map: u32 = 0;
    for (PROBES, 0..) |pa, i| {
        if (is_mapped(pa)) map |= @as(u32, 1) << @intCast(i);
    }
    put32(0x40, map);
    put32(0x44, env_word());
    status(0x30 | @as(u8, @truncate(map & 0xf)));

    clear_boot_next();

    // Without the WDT there is no bound on a hang — bail to stock.
    if (map & 1 == 0) {
        // Survey-only result: the map bitmap and environment word still
        // answer "what does u-boot map" even when no MMIO can be touched.
        put32(0x00, 0x424d4e33);
        save_result();
        status(0x22);
        return .load_error;
    }

    // WDT covers everything from here: a fault at any later step resets the
    // SoC and BootOrder takes the stock path.
    wdt_arm(PROBE_SECONDS);
    asm volatile ("dsb sy" ::: .{ .memory = true });

    if (map & 8 != 0) {
        marker(0x00, 0x4b455830);
        marker(0x04, 3);
        marker(0x18, rd(WDT_BASE + WDT_CTL));
        marker(0x1c, rd(WDT_BASE + WDT_ALARM));
    }

    if (map & 6 == 6) {
        // Full BMN-03 probe: raise the eight t8103 ANE power domains, record
        // each readback, then read the engine scratch window. Results go to
        // the BMN03R EFI variable (and the DRAM hole if it happens to map).
        put32(0x00, 0x424d4e33);
        const ok = raise_one(0x470, 0) | raise_one(0xc000, 1) |
            raise_one(0xc008, 2) | raise_one(0xc010, 3) |
            raise_one(0xc018, 4) | raise_one(0xc020, 5) |
            raise_one(0xc028, 6) | raise_one(0xc030, 7);
        put32(0x04, ok);
        var i: usize = 0;
        while (i < 4) : (i += 1) put32(0x28 + i * 4, rd(ANE_SCRATCH0 + i * 4));
        wr(ANE_SCRATCH0, 0xa5b3c7d1);
        put32(0x38, rd(ANE_SCRATCH0));
        put32(0x3c, 0xdeadbee5);
        status(0x41); // full work done, sealed
    } else {
        status(0x40); // WDT armed, probe-only (PMGR/ANE unmapped)
    }
    save_result();

    while (true) {
        asm volatile ("wfe");
    }
}
