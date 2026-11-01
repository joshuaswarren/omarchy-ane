// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h17_soc.c: T8140/J700 (H17) ANE row.
 *
 * Values pinned from two offline sources (see
 * docs/j700-h17-linux-support-plan.md and
 * receipts/2026-11-01-j700-h17-linux-support-plan/):
 *   - the retained macOS kernelcache legacy direct-boot block
 *     (ANE_Init 0xfffffe0008f3e138..0xfffffe0008f3e198), and
 *   - the recorded ADT (data/ane-soc/t8140.json, source A1).
 * Fields marked "candidate" are the T8132/H16 template and are NOT
 * device-qualified for T8140. No of_match entry is added here: the H16
 * driver would then claim a boot path that has never run, so wiring it
 * in is a separate, hardware-gated decision.
 */
#include "ane_h17_soc.h"

/* h17_ane_fw_theia_d9x.im4p, fetched from macOS 26.6.2 and installed as
 * apple/ane/h17_ane_fw_theia_d9x.macho. Unencrypted raw arm64e
 * MH_PRELOAD; the SHA-256 and segment layout below are the on-disk
 * payload and match packaging/omarchy-ane-firmware-fetch's THEIA_D9X
 * pin (verified with --check). */
static const struct ane_h16_fw ane_fw_theia_d9x = {
	.name = "apple/ane/h17_ane_fw_theia_d9x.macho",
	.sha256 = {
		0x32, 0xce, 0x2b, 0xa5, 0xaf, 0x65, 0x74, 0x2d,
		0x68, 0xe7, 0x03, 0xdb, 0x66, 0x43, 0x0c, 0xa1,
		0x5e, 0x01, 0x32, 0xaf, 0x7c, 0xa5, 0xe5, 0x9e,
		0xc9, 0xb7, 0x43, 0xd7, 0xe9, 0xca, 0x3f, 0x0c,
	},
	.size = 0x180000,
	.text_vmsize = 0xc0000,
	.data_vm = 0xc0000,
	.data_vmsize = 0x2ac000,
	.data_fileoff = 0xc4000,
	.data_filesize = 0xbc000,
	.patchbay_vm = 0xcbdc8,
	.tunables_vm = 0xde030,
};

/* T8140 / J700 (A18 Pro-class, H17). ADT ane-type 0x200; engine
 * 0x400000000+0x2000000.
 *
 * Pinned from the kernelcache legacy direct-boot block:
 *   cpu_control 0x1600044 (write 0 then 0x10 RUN)
 *   cpu_status  0x1600048
 *   rvbar       0x1050000 (write64)
 *   scratch0    0x1880020 (SCRATCH7 at 0x188003c, DONE 0x08042006)
 *
 * Pinned from the ADT:
 *   ane_type 0x200; ANE_SYS ps word at pmgr +0x290.
 *
 * Candidates, not qualified for T8140: irq_status/irq_ack/mbox keep the
 * T8132 row's values (the CPU block shares the +0x200000 shift), and the
 * ps words after ANE_SYS follow the T8132 0xc000/0xc008/0xc010/0xc018
 * chain. Only 0xc000 has T8140 evidence (the Apple driver's PS write);
 * the ADT marks ANE_CPU no_ps, so the row may need fewer words.
 */
const struct ane_h16_soc ane_t8140_soc = {
	.name = "t8140",
	.ane_type = 0x200,
	.cpu_control = 0x1600044,
	.cpu_status = 0x1600048,
	.rvbar = 0x1050000,
	.scratch0 = 0x1880020,
	.irq_status = 0x188c000,	/* candidate: T8132 template */
	.irq_ack = 0x1890000,		/* candidate: T8132 template */
	.mbox = 0x1608000,		/* candidate: T8132 template */
	.wrapper_pa = 0x401600000ull,
	.cpu_pa = 0x401000000ull,
	.ps_off = { 0x290, 0xc000, 0xc008, 0xc010, 0xc018 },
	.fw = &ane_fw_theia_d9x,
};
