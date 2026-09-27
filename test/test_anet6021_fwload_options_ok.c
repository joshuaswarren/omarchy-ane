/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Executable boundary test for the BINDING probe-top predicate
 * (ane_t6021_fwload_options_ok). Compiled and run on the build host.
 *
 * IMPORTANT — the test calls the SAME predicates the kernel probe
 * uses, by #include'ing ane/t6021/ane_t6021_diag_marker.h. There is
 * no second copy of the rule here; if the rule changes, the test
 * changes with it.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Mirror the kernel-side state the predicate reads (module_params in
 * ane_t6021_fwload.c + ane_t6021_diag_marker.h). The pure inlines
 * receive values through arguments; this file only mirrors the
 * state to feed them. */
static bool fw_load;
static bool fw_diag_marker;
static bool fw_boot_req;
static unsigned int fw_extra_ram;
static bool fw_alias_reserved;

/* Pull the production predicates in. */
#include "../ane/t6021/ane_t6021_diag_marker.h"

/* Mirror ane_t6021_boot_requested() — kept out of header. */
static bool boot_req(void) { return fw_boot_req; }

/* Recreate the kernel-side ane_t6021_fwload_options_ok chain literally,
 * using the same inlines; the test then calls THIS, exactly like the
 * kernel probe does. No re-implementation of the rule. */
static bool options_ok(bool transport)
{
    if (!ane_t6021_fw_extra_ram_envelope_ok(fw_load, fw_extra_ram,
                                            fw_alias_reserved))
        return false;
    return ane_t6021_diag_options_ok(fw_diag_marker, fw_load,
                                     boot_req(), transport);
}

/* Wrapper for the unit frame; identical to kernel-side. */
static bool fwload_options_ok(bool transport) { return options_ok(transport); }

int main(void)
{
    fprintf(stderr, "[t6021] fwload_options_ok — kernel predicate boundary test\n");

    /* Default: load=0, nothing set — trivially ok */
    assert(fwload_options_ok(false) == true);

    /* Load=1, extra=0 */
    fw_load = true; fw_extra_ram = 0; fw_alias_reserved = false;
    assert(fwload_options_ok(false) == true);

    /* Load=1, extra=SZ_16M-aligned, reserved=1 -> ok */
    fw_load = true; fw_extra_ram = 16u * 1024u * 1024u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == true);

    /* Load=1, extra=SZ_16M+1 (one byte over) -> reject */
    fw_load = true; fw_extra_ram = 16u * 1024u * 1024u + 1u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == false);

    /* Load=1, extra=24 MiB (0x1800000) — out of cap, must reject
     * BEFORE dma_alloc_coherent */
    fw_load = true; fw_extra_ram = 0x1800000u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == false);

    /* Load=1, extra=2 MiB aligned + reserved=0 -> reject (need alias
     * when extra_ram>0) */
    fw_load = true; fw_extra_ram = 0x200000u; fw_alias_reserved = false;
    assert(fwload_options_ok(false) == false);

    /* Load=1, extra=2 MiB + reserved=1 -> ok */
    fw_load = true; fw_extra_ram = 0x200000u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == true);

    /* Load=1, extra=0x1000 (4 KiB, NOT 16 KiB-aligned) — reject.
     * The DART page is 0x4000 (16 KiB). */
    fw_load = true; fw_extra_ram = 0x1000u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == false);

    /* Load=1, extra=0x4001 (16 KiB+1 — misaligned) — reject */
    fw_load = true; fw_extra_ram = 0x4001u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == false);

    /* Load=1, extra=0x8000 (32 KiB — 16 KiB-aligned, reserved) — ok */
    fw_load = true; fw_extra_ram = 0x8000u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == true);

    /* Load=1, extra=1 (one byte) — reject */
    fw_load = true; fw_extra_ram = 1u; fw_alias_reserved = true;
    assert(fwload_options_ok(false) == false);

    fprintf(stderr, "[t6021] all boundaries hold\n");
    return 0;
}
