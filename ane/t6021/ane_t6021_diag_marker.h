/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_T6021_DIAG_MARKER_H
#define ANE_T6021_DIAG_MARKER_H

/* Laboratory execution marker, NOT firmware READY or functional ANE.
 * GNU-assembled/byte-verified source: ane-linux-experiments receipt
 * 2026-09-21-m2-reset-marker-offline (2caa6a9), marker.S.
 * At VM 0x204: x0=0x285840064; w1=0x4d325431; str w1,[x0];
 * dsb sy; self-loop. Only the validated coherent copy may be patched.
 */
static inline bool ane_t6021_diag_options_ok(bool diag, bool load,
                                            bool boot, bool transport)
{
    return !diag || (load && !boot && !transport);
}

static inline void ane_t6021_diag_patch(void *image)
{
    static const unsigned char marker[] = {
        0x80,0x0c,0x80,0xd2,0x80,0xb0,0xb0,0xf2,
        0x40,0x00,0xc0,0xf2,0x21,0x86,0x8a,0x52,
        0x41,0xa6,0xa9,0x72,0x01,0x00,0x00,0xb9,
        0x9f,0x3f,0x03,0xd5,0x00,0x00,0x00,0x14
    };
    memcpy((unsigned char *)image + 0x204, marker, sizeof(marker));
}

#endif
