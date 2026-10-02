/* SPDX-License-Identifier: GPL-2.0 */
/* CSNE_CMD request layouts for the macOS 13.5 power-on DSID sequence,
 * T6021 (receipts/2026-10-02-ane-dsid).
 *
 * MEASURED, macOS 13.5 (22G74) kext + selene 13.5 firmware:
 *  - H11ANEIn::aneCmdSend requests carry the opcode in the halfword at
 *    +4 (kext135-mcache.txt: `strh w8, [req, #4]`, w8 = 0x25/0x26/0x2e;
 *    ane_rtclient_legacy_exchange writes the same halfword, so the
 *    builders leave +4 alone).
 *  - 0x25 DSID_SET, 0x0c bytes: u32 dsid at +8 (fw135-dsid.txt @0x28a74
 *    `ldr w1, [x21, #8]` feeding CAneEngineExeLoopH14::updateDSID).
 *  - 0x26 MCACHE_SIZE_GET, 0x0c bytes: the fw writes the reply u32 at +8
 *    (@0x27e80 `str w8 #0x300000, [x21, #8]`; the kext reads it back
 *    from request+8, 0xfffffe00094f4b14).
 *  - 0x2e ANE_DEFAULT_SETTING_SET, 0x20 bytes: u64 setting count at +8
 *    (fw @0x28214 `ldr x25, [x21, #8]`), then count 8-byte
 *    {u32 regId, u32 value} pairs at +0x10 (fw @0x28250 `ldp w9, w8`;
 *    kext literal 0xfffffe000739ffe0: {4, 0x33}, {3, 0xe}).
 *
 * Host-testable: tools/test_ane_fwcmd.c compiles this header directly
 * with ANE_FWCMD_HOSTTEST defined. */
#ifndef ANE_T6021_FWCMD_H
#define ANE_T6021_FWCMD_H

#ifdef ANE_FWCMD_HOSTTEST
/* Host shim; the kernel include chain provides these normally. */
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __le32;
typedef uint64_t __le64;
#define cpu_to_le32(x) (x)
#define cpu_to_le64(x) (x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#else
#include <linux/string.h>
#include <linux/types.h>
#endif

#define ANE_FWCMD_DSID_SET		0x25
#define ANE_FWCMD_MCACHE_SIZE_GET	0x26
#define ANE_FWCMD_DEFAULT_SETTING_SET	0x2e

#define ANE_FWCMD_SIZE_GET_LEN		0x0c
#define ANE_FWCMD_DSID_SET_LEN		0x0c
#define ANE_FWCMD_DEFSETTING_LEN	0x20
#define ANE_FWCMD_DSID_OFF		0x08
#define ANE_FWCMD_DEFSETTING_CNT_OFF	0x08
#define ANE_FWCMD_DEFSETTING_ARR_OFF	0x10

/* MCACHE_SIZE_GET (0x26): 12-byte request; the reply lands at +8. */
static inline void ane_fwcmd_mcache_size_get(void *buf)
{
	memset(buf, 0, ANE_FWCMD_SIZE_GET_LEN);
}

/* ANE_DEFAULT_SETTING_SET (0x2e) with the macOS 13.5 power-on settings
 * {regId=4, value=0x33}, {regId=3, value=0xe}. */
static inline void ane_fwcmd_default_setting_set(void *buf)
{
	static const struct { __le32 id; __le32 value; } settings[] = {
		{ cpu_to_le32(4), cpu_to_le32(0x33), },
		{ cpu_to_le32(3), cpu_to_le32(0x0e), },
	};

	memset(buf, 0, ANE_FWCMD_DEFSETTING_LEN);
	*(__le64 *)((u8 *)buf + ANE_FWCMD_DEFSETTING_CNT_OFF) =
		cpu_to_le64(ARRAY_SIZE(settings));
	memcpy((u8 *)buf + ANE_FWCMD_DEFSETTING_ARR_OFF, settings,
	       sizeof(settings));
}

/* DSID_SET (0x25): 12-byte request with the dsid at +8. */
static inline void ane_fwcmd_dsid_set(void *buf, u32 dsid)
{
	memset(buf, 0, ANE_FWCMD_DSID_SET_LEN);
	*(__le32 *)((u8 *)buf + ANE_FWCMD_DSID_OFF) = cpu_to_le32(dsid);
}

#endif
