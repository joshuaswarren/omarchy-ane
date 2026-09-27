/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Executable predicate test for ane_t6021_fwload_options_ok.
 *
 * Mirrors the same conditions the kernel module enforces (binder binds at
 * probe top BEFORE devm_kzalloc/power; late alloc check is defense in
 * depth). Compiled and run on the build host (here: x86_64). The kernel
 * version of the predicate reads module_param globals, so we duplicate
 * those declarations here and assert the boundary matrix.
 *
 * Not linked against the kernel module — module_param state is kernel-side
 * state. The source-level predicate receives the same boolean inputs the
 * kernel probe would, and the output is what ane_rtclient_probe /
 * ane_t6021_probe use to gate at probe top.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Mirror kernel definitions */
#define SZ_16M              (16u * 1024u * 1024u)
#define ANE_T6021_FW_ALIAS_PAGE 0x4000u

/* Mirror module parameters (kernel-side state, see ane_t6021_fwload.c) */
static bool fw_load;
static bool fw_diag_marker;
static bool fw_boot_req;
static unsigned int fw_extra_ram;
static bool fw_alias_reserved;

bool ane_t6021_boot_requested(void) { return fw_boot_req; }

static inline bool ane_t6021_diag_options_ok(bool diag, bool load,
                                            bool boot, bool transport)
{
    return !diag || (load && !boot && !transport);
}

static bool ane_t6021_fwload_options_ok(bool transport)
{
    if (!fw_load)
        return ane_t6021_diag_options_ok(fw_diag_marker, fw_load,
                                       fw_boot_req, transport);
    if (fw_extra_ram) {
        if (!((fw_extra_ram & (ANE_T6021_FW_ALIAS_PAGE - 1)) == 0))
            return false;
        if (fw_extra_ram > SZ_16M)
            return false;
        if (!fw_alias_reserved)
            return false;
    }
    return ane_t6021_diag_options_ok(fw_diag_marker, fw_load,
                                   fw_boot_req, transport);
}

/* helpers */
#define R(r) do { memset(&C, 0, sizeof(C)); C.r = r; if (!ane_t6021_fwload_options_ok(false)) goto bad; goto ok; bad: rc = -1; ok: ; } while (0)

int main(void)
{
    int rc = 0;
    struct {
        bool fw_load, fw_diag_marker, fw_boot_req, fw_alias_reserved;
        unsigned int fw_extra_ram;
    } C = {0};
#define PROBE (C.fw_load || C.fw_diag_marker || C.fw_extra_ram || C.fw_alias_reserved)
    fprintf(stderr, "[t6021] fwload_options_ok boundary test\n");

    /* fw_load=0: trivial */
    fw_load = false; fw_extra_ram = 0;
    assert(ane_t6021_fwload_options_ok(false) == true);

    /* fw_load=1, no extra ram: pass */
    fw_load = true; fw_extra_ram = 0;
    assert(ane_t6021_fwload_options_ok(false) == true);

    /* fw_load=1, extra_ram = SZ_16M, reserved=1: pass */
    fw_load = true; fw_extra_ram = SZ_16M; fw_alias_reserved = true;
    assert(ane_t6021_fwload_options_ok(false) == true);

    /* fw_load=1, extra_ram = SZ_16M+1: fail (16 MiB guard) */
    fw_load = true; fw_extra_ram = SZ_16M + 1; fw_alias_reserved = true;
    assert(ane_t6021_fwload_options_ok(false) == false);

    /* fw_load=1, extra_ram = 0x1800000 (24 MiB) attempt: fail before alloc */
    fw_load = true; fw_extra_ram = 0x1800000u; fw_alias_reserved = true;
    assert(ane_t6021_fwload_options_ok(false) == false);

    /* fw_load=1, extra_ram = 4 KiB-aligned 2 MiB without reserved: fail */
    fw_load = true; fw_extra_ram = 0x200000u; fw_alias_reserved = false;
    assert(ane_t6021_fwload_options_ok(false) == false);

    /* fw_load=1, extra_ram = 2 MiB with reserved: pass */
    fw_load = true; fw_extra_ram = 0x200000u; fw_alias_reserved = true;
    assert(ane_t6021_fwload_options_ok(false) == true);

    /* fw_load=1, extra_ram = 1 (not aligned): fail */
    fw_load = true; fw_extra_ram = 1u; fw_alias_reserved = true;
    assert(ane_t6021_fwload_options_ok(false) == false);

    /* fw_load=1, extra_ram = 0x4000: pass with reserved */
    fw_load = true; fw_extra_ram = 0x4000u; fw_alias_reserved = true;
    assert(ane_t6021_fwload_options_ok(false) == true);

    fprintf(stderr, "[t6021] all boundaries hold\n");
    return rc;
}
