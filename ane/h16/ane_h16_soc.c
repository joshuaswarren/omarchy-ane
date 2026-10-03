// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Per-SoC rows for the H16 ANE family. Sources in ane_h16.h. */
#include "ane_h16.h"

static const struct ane_h16_fw ane_fw_leto = {
	.name = "apple/ane/h16_ane_fw_leto_j7x.macho",
	.sha256 = {
		0x4b, 0x33, 0x0c, 0x5f, 0xd3, 0x54, 0x8f, 0xb5,
		0x34, 0xde, 0xd5, 0xe7, 0x03, 0x0c, 0x64, 0xb2,
		0xee, 0x3c, 0x3f, 0xeb, 0x8a, 0xd8, 0x59, 0xc2,
		0x3d, 0x87, 0xdc, 0xb8, 0x29, 0x47, 0xab, 0xd2,
	},
	.size = 0x17c000,
	.text_vmsize = 0xc0000,
	.data_vm = 0xc0000,
	.data_vmsize = 0x2ac000,
	.data_fileoff = 0xc4000,
	.data_filesize = 0xb8000,
	.patchbay_vm = 0xc5a08,
	.tunables_vm = 0xd7c70,
};

static const struct ane_h16_fw ane_fw_aether = {
	.name = "apple/ane/t604x_ane_fw_aether_brvx.macho",
	.sha256 = {
		0x86, 0x59, 0x27, 0x1a, 0x31, 0x73, 0xca, 0x1c,
		0x81, 0x68, 0x41, 0xb2, 0x12, 0xac, 0x40, 0x6a,
		0x65, 0x16, 0xf7, 0x4f, 0x51, 0xd0, 0x8f, 0x19,
		0x63, 0x27, 0x56, 0x0c, 0xb1, 0xf6, 0xb9, 0x25,
	},
	.size = 0x17c000,
	.text_vmsize = 0xc0000,
	.data_vm = 0xc0000,
	.data_vmsize = 0x2ac000,
	.data_fileoff = 0xc4000,
	.data_filesize = 0xb8000,
	.patchbay_vm = 0xc5a18,
	.tunables_vm = 0xd7c80,
};

/* T8132 (M4). ADT j604ap; kext ane-type 0x100 row; iBoot j604 coproc
 * id 4. ps words inside pmgr 0x380700000+0x18000. */
const struct ane_h16_soc ane_t8132_soc = {
	.name = "t8132",
	.ane_type = 0x100,
	.cpu_control = 0x1600044,
	.cpu_status = 0x1600048,
	.rvbar = 0x1050000,
	.scratch0 = 0x1880020,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.mbox = 0x1608000,
	.wrapper_pa = 0x501600000ull,
	.cpu_pa = 0x501000000ull,
	.ps_off = { 0x570, 0xc000, 0xc008, 0xc010, 0xc018 },
	.fw = &ane_fw_leto,
};

/* T6040 (M4 Pro). Same die as T6041 (identical ADT ane0/dart-ane0 on
 * j614s/j616s/j773s vs j614c/j616c except phandles; the kext has one
 * row for both, ane-type 0x110, and one HAL, AppleT6041ANEHAL). ps
 * words inside pmgr 0x502280000+0x18000. */
const struct ane_h16_soc ane_t6040_soc = {
	.name = "t6040",
	.ane_type = 0x110,
	.cpu_control = 0x1600044,
	.cpu_status = 0x1600048,
	.rvbar = 0x1050000,
	.scratch0 = 0x1880020,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.mbox = 0x1608000,
	.wrapper_pa = 0x485600000ull,
	.cpu_pa = 0x485000000ull,
	.ps_off = { 0x3c0, 0xc000, 0xc008, 0xc010, 0xc018 },
	.fw = &ane_fw_aether,
};

/* T6041 (M4 Max). Same row as T6040; iBoot j614c coproc id 4 has the
 * identical wrapper/cpu PA pair. */
const struct ane_h16_soc ane_t6041_soc = {
	.name = "t6041",
	.ane_type = 0x110,
	.cpu_control = 0x1600044,
	.cpu_status = 0x1600048,
	.rvbar = 0x1050000,
	.scratch0 = 0x1880020,
	.irq_status = 0x188c000,
	.irq_ack = 0x1890000,
	.mbox = 0x1608000,
	.wrapper_pa = 0x485600000ull,
	.cpu_pa = 0x485000000ull,
	.ps_off = { 0x3c0, 0xc000, 0xc008, 0xc010, 0xc018 },
	.fw = &ane_fw_aether,
};
