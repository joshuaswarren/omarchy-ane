/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_fw_validate.h — pinned ANE firmware images, their validator, and
 * iBoot's runtime patches.
 *
 * Single source of truth shared by the kernel loader
 * (ane_t6021_fwload.c) and the offline regression
 * (tools/h14_boot_regression.c). Strict EXACT-image assertions (no
 * generic Mach-O parsing).
 *
 * The images are the macOS 13.5 (22G74) payloads that iBoot preloads on
 * the stub every Omarchy install boots: selene for T6020/T6021/T6022
 * (byte-verified live at SEG0 on T6021, notebook 20260926T230146Z) and
 * bia for T8112 (receipts/2026-10-01-t8112-ane). Both have 5 load
 * commands (2 segments + LC_SYMTAB + LC_UUID + LC_UNIXTHREAD), entry 0.
 *
 * Userspace consumers typedef u8/u32/u64/size_t/bool and provide
 * get_unaligned_le32/64, memcmp and memcpy (see that tool's source);
 * kernel consumers get these from linux/types.h + linux/string.h.
 * This header intentionally includes nothing.
 */
#ifndef __ANE_FW_VALIDATE_H__
#define __ANE_FW_VALIDATE_H__

/* FWIM surface size = config+0x138 byte-count = 0x500000 (Main
 * 2026-09-20, audit 751caa4, T6021). Covers the selene vmsize 0x4fc000
 * and the bia vmsize 0x4ec000; NOT derivable from the blob length. */
#define ANE_FW_BUF_SIZE		0x500000
#define ANE_FW_ENTRY_PC		0x0

#define MH_MAGIC_64	0xfeedfacfU
#define MH_PRELOAD	5
#define CPU_TYPE_ARM64	0x0100000cU
#define LC_SEGMENT_64	0x19
#define LC_UNIXTHREAD	0x5
#define ARM_THREAD_STATE64 6

#define ANE_FW_NCMDS		5
#define ANE_FW_SIZEOF_CMDS	0x960
#define ANE_FW_FLAGS		0x1
#define ANE_FW_NSEGS		2
#define ANE_FW_RTK_PATCHES	5

struct ane_fw_seg {
	u64 vmaddr;
	u64 vmsize;
	u64 fileoff;
	u64 filesize;
	char name[17];		/* 16 + NUL */
};

/* One pinned image. The patch fields are the payload's LC_SYMTAB
 * addresses: the __rtk_patch records _rtk_stack_guard, RTK_soc,
 * RTK_soc_revision, RTK_cpu_physical_address and
 * RTK_cpu_wrapper_physical_address, then the type-1
 * __rtk_platform_asc_tunables_block. */
struct ane_fw_image {
	const char *name;		/* request_firmware() path */
	u8 sha256[32];
	u32 size;
	struct ane_fw_seg segs[ANE_FW_NSEGS];
	u32 rtk_vm[ANE_FW_RTK_PATCHES];
	u32 tunables_vm;
};

static const struct ane_fw_image ane_fw_selene = {
	.name = "apple/ane/t602x_ane0_fw_selene_rc4x.macho",
	.sha256 = {
		0xa9, 0xc4, 0xb7, 0x71, 0x29, 0x4a, 0x6b, 0x11,
		0x56, 0x24, 0xd9, 0x48, 0x0a, 0x62, 0x48, 0xd0,
		0x89, 0x9a, 0x16, 0x81, 0xa5, 0x75, 0xe8, 0x65,
		0x07, 0x0b, 0x87, 0xa3, 0x24, 0x84, 0x27, 0xbc,
	},
	.size = 0x4c5b28,
	.segs = {
		{ 0x000000, 0x0c4000, 0x004000, 0x0c4000, "__TEXT" },
		{ 0x0c4000, 0x438000, 0x0c8000, 0x3e8000, "__DATA" },
	},
	.rtk_vm = { 0xca848, 0xca9b3, 0xca9bf, 0xca9cb, 0xca9db },
	.tunables_vm = 0xdcd78,
};

static const struct ane_fw_image ane_fw_bia = {
	.name = "apple/ane/h14_ane_fw_bia_j4xx.macho",
	.sha256 = {
		0xaf, 0x58, 0x7d, 0xfa, 0x96, 0xb1, 0xe0, 0x1d,
		0x2b, 0x2e, 0x0f, 0x9f, 0x77, 0x6e, 0x5d, 0xbe,
		0xfe, 0xbb, 0x44, 0xc7, 0x96, 0x8b, 0x86, 0xff,
		0xf7, 0xd2, 0xb1, 0x9c, 0x4c, 0xbe, 0xf0, 0xdd,
	},
	.size = 0x4b5b38,
	.segs = {
		{ 0x000000, 0x0b4000, 0x004000, 0x0b4000, "__TEXT" },
		{ 0x0b4000, 0x438000, 0x0b8000, 0x3e8000, "__DATA" },
	},
	.rtk_vm = { 0xba688, 0xba7f3, 0xba7ff, 0xba80b, 0xba81b },
	.tunables_vm = 0xccbb8,
};

/* Validate a candidate payload against image FW.  Returns 0 on pass,
 * else -1 with *reason set.  actual_sha = caller-computed SHA-256 of
 * blob. */
static inline int
ane_fw_validate_blob(const u8 *blob, size_t size,
		     const struct ane_fw_image *fw, const u8 actual_sha[32],
		     struct ane_fw_seg *segs_out, u64 *entry_out,
		     const char **reason)
{
	u32 magic, cputype, filetype, ncmds, sizeofcmds, flags;
	u32 off, i, nsegs = 0;
	u64 entry = (u64)~0ULL;

	if (size != fw->size) {
		*reason = "size != pinned image";
		return -1;
	}
	if (memcmp(fw->sha256, actual_sha, 32) != 0) {
		*reason = "sha256 != pinned payload hash";
		return -1;
	}

	magic = get_unaligned_le32(blob + 0);
	cputype = get_unaligned_le32(blob + 4);
	filetype = get_unaligned_le32(blob + 12);
	ncmds = get_unaligned_le32(blob + 16);
	sizeofcmds = get_unaligned_le32(blob + 20);
	flags = get_unaligned_le32(blob + 24);

	if (magic != MH_MAGIC_64) {
		*reason = "magic";
		return -1;
	}
	if (cputype != CPU_TYPE_ARM64) {
		*reason = "cputype";
		return -1;
	}
	if (filetype != MH_PRELOAD) {
		*reason = "filetype != MH_PRELOAD";
		return -1;
	}
	if (ncmds != ANE_FW_NCMDS || sizeofcmds != ANE_FW_SIZEOF_CMDS ||
	    flags != ANE_FW_FLAGS) {
		*reason = "header fields != pinned image";
		return -1;
	}

	off = 32;
	for (i = 0; i < ncmds; i++) {
		u32 cmd, cmdsize;

		if (off + 8 > 32 + sizeofcmds || off + 8 > size) {
			*reason = "load command header out of bounds";
			return -1;
		}
		cmd = get_unaligned_le32(blob + off);
		cmdsize = get_unaligned_le32(blob + off + 4);
		if (cmdsize < 8 || off + cmdsize > 32 + sizeofcmds ||
		    off + cmdsize > size) {
			*reason = "cmdsize out of bounds";
			return -1;
		}

		if (cmd == LC_SEGMENT_64) {
			u64 vmaddr = get_unaligned_le64(blob + off + 24);
			u64 vmsize = get_unaligned_le64(blob + off + 32);
			u64 fileoff = get_unaligned_le64(blob + off + 40);
			u64 filesize = get_unaligned_le64(blob + off + 48);
			const char *sname = (const char *)blob + off + 8;
			unsigned int k;

			if (fileoff > size || filesize > size - fileoff) {
				*reason = "segment file range out of bounds";
				return -1;
			}
			if (filesize > vmsize) {
				*reason = "filesize > vmsize";
				return -1;
			}
			for (k = 0; k < ANE_FW_NSEGS; k++) {
				if (vmaddr == fw->segs[k].vmaddr &&
				    vmsize == fw->segs[k].vmsize &&
				    fileoff == fw->segs[k].fileoff &&
				    filesize == fw->segs[k].filesize &&
				    !memcmp(sname, fw->segs[k].name, 16)) {
					if (segs_out)
						segs_out[nsegs] = fw->segs[k];
					break;
				}
			}
			if (k == ANE_FW_NSEGS) {
				*reason = "segment != pinned layout";
				return -1;
			}
			nsegs++;
		} else if (cmd == LC_UNIXTHREAD) {
			u32 p = off + 8;

			/* thread commands: flavor u32, count u32
			 * (count = number of 32-bit words in state) */
			while (p + 8 <= off + cmdsize) {
				u32 flavor = get_unaligned_le32(blob + p);
				u32 count = get_unaligned_le32(blob + p + 4);

				if (count > (cmdsize - (p - off) - 8) / 4) {
					*reason = "thread state count out of bounds";
					return -1;
				}
				if (flavor == ARM_THREAD_STATE64) {
					/* 33 u64 registers; PC at index 32:
					 * requires count*4 >= 33*8 bytes */
					if (count < 66 ||
					    p + 8 + count * 4 > off + cmdsize) {
						*reason = "thread state too small for pc";
						return -1;
					}
					entry = get_unaligned_le64(blob + p + 8 +
								   32 * 8);
				}
				p += 8 + count * 4;
			}
		}
		off += cmdsize;
	}

	if (nsegs != ANE_FW_NSEGS) {
		*reason = "segment count != 2";
		return -1;
	}
	if (entry != ANE_FW_ENTRY_PC) {
		*reason = "entry pc != 0";
		return -1;
	}

	if (segs_out) {
		for (i = 0; i < ANE_FW_NSEGS; i++)
			segs_out[i] = fw->segs[i];
	}
	if (entry_out)
		*entry_out = entry;
	return 0;
}

/*
 * iBoot runtime patches. iBoot places the image at its Mach-O vm layout
 * (TEXT vm 0, DATA vm segs[1].vmaddr) and then writes the fields below.
 * The 17 pre-Linux T6021 captures (receipts/2026-10-01-t602x-independent)
 * differ from the archive in these bytes only; only the stack guard
 * changes between boots. A driver that runs its own copy writes them
 * itself.
 *
 * Each __rtk_patch_* record is {u32 tag, u32 value length, value}.
 */
#define ANE_FW_DATA_BASE_VM	0x423c	/* TEXT u64: IOVA of DATA (both images) */
#define ANE_FW_TUNABLES_MAX	0x24	/* type-1 block capacity (header byte 2) */

/*
 * iBoot's ASC tunables for the ANE (coprocessor id 4): one record list
 * and the chip-revision keys of the entries that carry it, highest
 * first. iBoot fills the firmware's type-1 block from the first entry
 * whose key <= the chip revision: header {type 1, flags 3 (20-byte
 * records), capacity 0x24, count, u32 key}, then {u32 offset, u64 mask,
 * u64 value} per record (iBoot table offsets carry flag bit 30; the
 * block does not). Source: iBootStage2 macOS 26.6.2 (25G83), LZFSE, not
 * encrypted (receipts/2026-10-01-t8112-optin). The 13.5 iBoot2 that
 * preloads the 13.5 image is encrypted; on T6021 its block (the
 * captures) equals the 25G83 records exactly.
 */
struct ane_asc_tunable {
	u32 off;
	u64 mask;
	u64 val;
};

struct ane_asc_tunables {
	u8 keys[4];
	u8 nkeys;
	u8 n;
	struct ane_asc_tunable r[ANE_FW_TUNABLES_MAX];
};

/* T6021, T6022: j414c/j416c/j180d/j475d 25G83, type-1 key 0x10 (the
 * 0x150010 record) then type-3 key 0x10 (the rest), which is the T6021
 * capture block byte for byte (header key 0x10). 25G83 adds key 0x11;
 * the capture (RTK_soc_revision 0x11, block key 0x10) shows 13.5 has
 * none, so it is left out. */
static const struct ane_asc_tunables ane_t602x_asc_tunables = {
	.keys = { 0x10, 0x01, 0x00 },
	.nkeys = 3,
	.n = 24,
	.r = {
		{ 0x150010, 0x000000000000003f, 0x0000000000000010 },
		{ 0x051c08, 0x0000000010000000, 0x0000000010000000 },
		{ 0x051c18, 0x0010000000000000, 0x0010000000000000 },
		{ 0x140020, 0x0000000000f00000, 0x0000000000700000 },
		{ 0x140120, 0x0000000000002eff, 0x00000000000002b1 },
		{ 0x140130, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140138, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x140140, 0x0000000000003fff, 0x000000000000002f },
		{ 0x140148, 0x0000000000007fff, 0x00000000000000c8 },
		{ 0x140150, 0x0000000000003fff, 0x000000000000004f },
		{ 0x140158, 0x0000000000007fff, 0x000000000000014e },
		{ 0x140160, 0x0000000000003fff, 0x000000000000004f },
		{ 0x140168, 0x0000000000007fff, 0x000000000000014e },
		{ 0x140170, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140178, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x140180, 0x0000000000003fff, 0x0000000000000028 },
		{ 0x140188, 0x0000000000007fff, 0x00000000000000a7 },
		{ 0x140190, 0x0000000000003fff, 0x0000000000000028 },
		{ 0x140198, 0x0000000000007fff, 0x00000000000000a7 },
		{ 0x1401a0, 0x0000000000003fff, 0x000000000000004f },
		{ 0x1401a8, 0x0000000000007fff, 0x000000000000014e },
		{ 0x145010, 0x000000000008000c, 0x000000000000000c },
		{ 0x14a008, 0x000000000ff0ffff, 0x0000000000002520 },
		{ 0x14a010, 0x000000000ff0ffff, 0x0000000002002520 },
	},
};

/* T6020: j414s/j416s 25G83, one type-1 entry, key 0x00. The offsets of
 * T6021; 13 values differ. */
static const struct ane_asc_tunables ane_t6020_asc_tunables = {
	.keys = { 0x00 },
	.nkeys = 1,
	.n = 24,
	.r = {
		{ 0x150010, 0x000000000000003f, 0x0000000000000010 },
		{ 0x051c08, 0x0000000010000000, 0x0000000010000000 },
		{ 0x051c18, 0x0010000000000000, 0x0010000000000000 },
		{ 0x140020, 0x0000000000f00000, 0x0000000000700000 },
		{ 0x140120, 0x0000000000002eff, 0x00000000000002b1 },
		{ 0x140130, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140138, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x140140, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140148, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x140150, 0x0000000000003fff, 0x0000000000000020 },
		{ 0x140158, 0x0000000000007fff, 0x0000000000000086 },
		{ 0x140160, 0x0000000000003fff, 0x000000000000004f },
		{ 0x140168, 0x0000000000007fff, 0x000000000000014e },
		{ 0x140170, 0x0000000000003fff, 0x0000000000000008 },
		{ 0x140178, 0x0000000000007fff, 0x0000000000000022 },
		{ 0x140180, 0x0000000000003fff, 0x0000000000000008 },
		{ 0x140188, 0x0000000000007fff, 0x0000000000000022 },
		{ 0x140190, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140198, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x1401a0, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x1401a8, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x145010, 0x000000000008000c, 0x000000000000000c },
		{ 0x14a008, 0x000000000ff0ffff, 0x0000000000002520 },
		{ 0x14a010, 0x000000000ff0ffff, 0x0000000001002520 },
	},
};

/* T8112: j413/j415/j473/j493 25G83, one type-1 table under keys 0x10
 * and 0x00, no type-3 entry. The T6021 offsets without 0x150010. */
static const struct ane_asc_tunables ane_t8112_asc_tunables = {
	.keys = { 0x10, 0x00 },
	.nkeys = 2,
	.n = 23,
	.r = {
		{ 0x051c08, 0x0000000010000000, 0x0000000010000000 },
		{ 0x051c18, 0x0010000000000000, 0x0010000000000000 },
		{ 0x140020, 0x4000000000f80000, 0x4000000000780000 },
		{ 0x140120, 0x0000000000002eff, 0x00000000000002b1 },
		{ 0x140130, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140138, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x140140, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140148, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x140150, 0x0000000000003fff, 0x0000000000000020 },
		{ 0x140158, 0x0000000000007fff, 0x0000000000000086 },
		{ 0x140160, 0x0000000000003fff, 0x0000000000000028 },
		{ 0x140168, 0x0000000000007fff, 0x00000000000000a7 },
		{ 0x140170, 0x0000000000003fff, 0x0000000000000010 },
		{ 0x140178, 0x0000000000007fff, 0x0000000000000043 },
		{ 0x140180, 0x0000000000003fff, 0x0000000000000010 },
		{ 0x140188, 0x0000000000007fff, 0x0000000000000043 },
		{ 0x140190, 0x0000000000003fff, 0x0000000000000018 },
		{ 0x140198, 0x0000000000007fff, 0x0000000000000064 },
		{ 0x1401a0, 0x0000000000003fff, 0x0000000000000028 },
		{ 0x1401a8, 0x0000000000007fff, 0x00000000000000a7 },
		{ 0x145010, 0x000000000008000c, 0x000000000008000c },
		{ 0x14a008, 0x000000000ff0ffff, 0x0000000000002520 },
		{ 0x14a010, 0x000000000ff0ffff, 0x0000000003002520 },
	},
};

/* T8112 RTK_soc_revision from its two eFuse words at 0x23d2c8060, as
 * iBootStage2 25G83 j413 0x40270 (and j415/j473/j493) computes it:
 * bits 2:0 = w0[29:27], bits 6:4 = {w1[0], w0[31:30]}. */
static inline u32 ane_t8112_fuse_revision(u32 w0, u32 w1)
{
	return ((w0 >> 27) & 7) | ((((w0 >> 30) | (w1 << 2)) & 7) << 4);
}

struct ane_fw_boot_patch {
	u64 exec_base;		/* IOVA the ASC runs vm 0 at */
	u64 stack_guard;
	u32 soc;
	u32 soc_revision;
	u64 cpu_pa;		/* ASC core block, engine + 0x1000000 */
	u64 wrapper_pa;		/* ASC wrapper, engine + 0x1400000 */
};

static inline void ane_fw_put_le(u8 *p, u64 v, unsigned int n)
{
	while (n--) {
		*p++ = (u8)v;
		v >>= 8;
	}
}

/* Patch the staged image FW in place (vm-indexed, at least
 * ANE_FW_BUF_SIZE bytes). Every precondition is checked before the first
 * write, so a refusal leaves the image untouched. Returns 0, or -1 with
 * *reason set. */
static inline int
ane_fw_apply_boot_patches(u8 *img, const struct ane_fw_image *fw,
			  const struct ane_asc_tunables *t,
			  const struct ane_fw_boot_patch *p, const char **reason)
{
	static const u8 tunables_unset[8] = {
		0x01, 0x03, ANE_FW_TUNABLES_MAX, 0x00, 0xff, 0xff, 0xff, 0xff };
	static const struct { u32 tag, len; } rec[ANE_FW_RTK_PATCHES] = {
		{ 0x53544b47, 8 },	/* _rtk_stack_guard */
		{ 0x534f435f, 4 },	/* RTK_soc */
		{ 0x534f4352, 4 },	/* RTK_soc_revision */
		{ 0x43704164, 8 },	/* RTK_cpu_physical_address */
		{ 0x57724164, 8 },	/* ..._wrapper_physical_address */
	};
	const u64 val[ANE_FW_RTK_PATCHES] = {
		p->stack_guard, p->soc, p->soc_revision, p->cpu_pa,
		p->wrapper_pa,
	};
	u8 *blk = img + fw->tunables_vm;
	unsigned int i, k;

	for (k = 0; k < t->nkeys && t->keys[k] > p->soc_revision; k++)
		;
	if (k == t->nkeys || t->n > ANE_FW_TUNABLES_MAX) {
		*reason = "no ASC tunables for this chip revision";
		return -1;
	}
	if (get_unaligned_le64(img + ANE_FW_DATA_BASE_VM)) {
		*reason = "DATA base field is not the archive zero";
		return -1;
	}
	for (i = 0; i < ANE_FW_RTK_PATCHES; i++) {
		if (get_unaligned_le32(img + fw->rtk_vm[i]) != rec[i].tag ||
		    get_unaligned_le32(img + fw->rtk_vm[i] + 4) != rec[i].len) {
			*reason = "__rtk_patch record != pinned image";
			return -1;
		}
	}
	if (memcmp(blk, tunables_unset, 8)) {
		*reason = "tunables block header != pinned image";
		return -1;
	}

	ane_fw_put_le(img + ANE_FW_DATA_BASE_VM,
		      p->exec_base + fw->segs[1].vmaddr, 8);
	for (i = 0; i < ANE_FW_RTK_PATCHES; i++)
		ane_fw_put_le(img + fw->rtk_vm[i] + 8, val[i], rec[i].len);
	blk[3] = t->n;
	ane_fw_put_le(blk + 4, t->keys[k], 4);
	for (i = 0; i < t->n; i++) {
		ane_fw_put_le(blk + 8 + 20 * i, t->r[i].off, 4);
		ane_fw_put_le(blk + 12 + 20 * i, t->r[i].mask, 8);
		ane_fw_put_le(blk + 20 + 20 * i, t->r[i].val, 8);
	}
	return 0;
}
#endif /* __ANE_FW_VALIDATE_H__ */
