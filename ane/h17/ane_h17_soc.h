/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h17_soc.h: T8140/J700 (H17) rows for the H16/H17 ANE family table.
 *
 * The struct definitions live in ane/h16/ane_h16.h (owned by the H16
 * driver); this header is the H17 row extension point the H16 header
 * names. Enablement is source-only: the H16 driver is opt-in and not
 * autoloaded, and the T8140 boot path is not device-qualified.
 */
#ifndef __ANE_H17_SOC_H__
#define __ANE_H17_SOC_H__

#include "../h16/ane_h16.h"

extern const struct ane_h16_soc ane_t8140_soc;

#endif /* __ANE_H17_SOC_H__ */
