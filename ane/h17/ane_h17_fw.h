/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* T8140/J700 ANE payload from Apple's macOS 26.6.2 (25G83) IPSW.
 * The IM4P member and its SHA-256 are recorded in
 * receipts/2026-10-30-j700-h17-adt/README.md. These are file facts,
 * not a claim that the live iBoot preload is byte-identical. */
#ifndef __ANE_H17_FW_H__
#define __ANE_H17_FW_H__

#define ANE_H17_FW_NAME "apple/ane/h17_ane_fw_theia_d9x.macho"
#define ANE_H17_FW_SIZE 0x180000
#define ANE_H17_FW_SHA256 "32ce2ba5af65742d68e703db66430ca15e0132af7ca5e59ec9b743d7e9ca3f0c"
#define ANE_H17_TEXT_FILE_OFFSET 0x4000
#define ANE_H17_TEXT_SIZE 0xc0000
#define ANE_H17_DATA_VM_SIZE 0x2ac000
#define ANE_H17_DATA_FILE_OFFSET 0xc4000
#define ANE_H17_DATA_FILE_SIZE 0xbc000
/* Mach-O section VM addresses in the pinned payload (25G83). */
#define ANE_H17_CE_ENV_VM 0xb8280
#define ANE_H17_CE_ENV_SIZE 0x4000
#define ANE_H17_PATCHBAY_VM 0xcbdc8
#define ANE_H17_PATCHBAY_SIZE 0x261
#define ANE_H17_TUNABLES_VM 0xde030
#define ANE_H17_TUNABLES_SIZE 0x6a0

#endif /* __ANE_H17_FW_H__ */
