// SPDX-License-Identifier: GPL-2.0
/*
 * dart_t8110_test.c - userspace harness for the t8110 page-table
 * builder (ane/common/dart_t8110.h).
 *
 * Positive controls:
 *   P1 byte-exact reproduction of the proven T6021 ANE-DART firmware
 *      map (TEXT 0x10000848000 -> 0x10000000000, 0xc4000; DATA
 *      0x10001400000 -> 0x100000c4000, 0x438000) against an expected
 *      image computed here from the raw format formula, NOT through
 *      the builder.
 *   P2 the measured anchor: leaf PTE 0x000fff1000084801
 *      (receipts/2026-09-24-t6021-coresight-dart) sits at
 *      L1[16]/L2[0]/leaf[0].
 *   P3 walk results equal the measured iommu_iova_to_phys values,
 *      plus the historic fault IOVA 0x100000dca10 from findings
 *      section 3.
 *   P4 2-level mode (window <= 2^36) uses the same measured leaf
 *      format; structure is DERIVED, not hardware-proven.
 * Negative controls (each must fail):
 *   N1 sid outside the allowlist refused (H16 t8132: 0,10,11,15;
 *      second parent mapper-ane-mpm modeled as stream set {11}).
 *   N2 unmapped IOVA walks fault with hardware-shaped codes.
 *   N3 wrong permission bits fault the matching access.
 *   N4 map entries outside the vm window refused.
 *   N5 a corrupted expected vector is caught by the comparison.
 *
 * Addresses and words only: no Apple payload bytes in this repo.
 */
#define _GNU_SOURCE	/* MAP_ANONYMOUS under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include "dart_t8110.h"

#define TEXTPA		0x10000848000ULL
#define TEXTIOVA	0x10000000000ULL
#define TEXTSZ		0xc4000ULL
#define DATAPA		0x10001400000ULL
#define DATAIOVA	0x100000c4000ULL
#define DATASZ		0x438000ULL
#define VMBASE		0x10000000000ULL
#define VMSIZE		0x30000000000ULL

/* Raw format formula, written independently of dart_t8110.h so the
 * byte comparison is not a tautology. GENMASK(37,10) = 0x3ffffffc00.
 */
static u64 ref_table_pte(u64 pa)
{
	return ((pa >> 4) & 0x3ffffffc00ULL) | 1;
}

static u64 ref_leaf_pte(u64 pa, bool nocache, bool nowrite, bool noread)
{
	u64 p = ((pa >> 4) & 0x3ffffffc00ULL) | 0xfff0000000000ULL | 1;

	if (nocache)
		p |= 2;
	if (nowrite)
		p |= 4;
	if (noread)
		p |= 8;
	return p;
}

/* Tables must live inside the DART PA field (pa < 2^42) exactly like
 * kernel-allocated dma memory would be. A libc heap pointer or a PIE
 * static sits far above 2^42, so mmap the arena at 1 GiB (also below
 * the ASan shadow, so the check runs under sanitizers.
 */
#include <sys/mman.h>

static void *arena_base;
static size_t arena_used;

static void *tbl[16];
static size_t n_tbl;

static void *talloc(size_t n)
{
	void *p;

	if (!arena_base) {
		arena_base = mmap((void *)0x40000000ULL, 131072,
				  PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (arena_base == MAP_FAILED)
			return NULL;
	}
	if (arena_used + n > 131072 || n_tbl >= 16)
		return NULL;
	p = (u8 *)arena_base + arena_used;
	arena_used += n;
	memset(p, 0, n);
	tbl[n_tbl++] = p;
	return p;
}

static void tfree(void *p, size_t n)
{
	(void)p;
	(void)n;
}

static size_t diff_words(const u64 *a, const u64 *b, size_t n)
{
	size_t i, d = 0;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			d++;
	return d;
}

static int fails;

static void check(int ok, const char *name, const char *detail)
{
	printf("%-4s %-28s %s\n", ok ? "ok" : "FAIL", name,
	       detail ? detail : "");
	if (!ok)
		fails++;
}

int main(void)
{
	struct dart8_cfg cfg;
	struct dart8_pt pt;
	u64 l2_pa, leaf_pa, root_pa, pa, ttbr, tcr;
	u64 *exp_root, *exp_l2, *exp_leaf;
	size_t d;
	int rc;
	u8 sids[4] = { 0, 10, 11, 15 };
	u8 mpm_sids[1] = { 11 };

	/* P1 + P2 + P3: the proven map at the H16 window parameters. */
	rc = dart8_cfg_init(&cfg, VMBASE, VMSIZE, sids, 4, talloc, tfree);
	check(rc == 0, "cfg-init", "window 0x10000000000+0x30000000000");
	if (rc)
		return 1;
	check(cfg.walk_levels == 3, "levels-derived",
	      "2^40 IOVA window needs 3 walk levels (TCR 0x9 measured)");

	pt.root = talloc(16384);
	if (!pt.root)
		return 1;
	memset(pt.root, 0, 16384);

	rc = dart8_map(&cfg, &pt, TEXTIOVA, TEXTPA, TEXTSZ, 0);
	check(rc == 0, "map-TEXT", rc ? "refused" : "49 pages RW cacheable");
	rc |= dart8_map(&cfg, &pt, DATAIOVA, DATAPA, DATASZ, 0);
	check(rc == 0, "map-DATA", rc ? "refused" : "270 pages RW cacheable");

	/* Expected image from the raw formula. */
	exp_root = calloc(2048, 8);
	exp_l2 = calloc(2048, 8);
	exp_leaf = calloc(2048, 8);
	if (!exp_root || !exp_l2 || !exp_leaf)
		return 1;
	l2_pa = (u64)(unsigned long)tbl[1];
	leaf_pa = (u64)(unsigned long)tbl[2];
	exp_root[16] = ref_table_pte(l2_pa);
	exp_l2[0] = ref_table_pte(leaf_pa);
	for (pa = 0; pa < TEXTSZ; pa += 16384)
		exp_leaf[pa >> 14] = ref_leaf_pte(TEXTPA + pa, 0, 0, 0);
	for (pa = 0; pa < DATASZ; pa += 16384)
		exp_leaf[49 + (pa >> 14)] = ref_leaf_pte(DATAPA + pa, 0, 0, 0);

	check(pt.root[16] == exp_root[16], "P1-root-word",
	      "L1[16] table PTE");
	check(pt.root[0] == 0 && pt.root[17] == 0, "P1-root-untouched",
	      "L1 holes stay zero");
	d = diff_words(exp_l2, (u64 *)(unsigned long)l2_pa, 2048);
	check(d == 0, "P1-l2-image", "L2 table byte-exact");
	d = diff_words(exp_leaf, (u64 *)(unsigned long)leaf_pa, 2048);
	check(d == 0, "P1-leaf-image", "leaf table byte-exact");
	/* P2: the measured cell. */
	check(((u64 *)(unsigned long)leaf_pa)[0] == 0x000fff1000084801ULL,
	      "P2-measured-leaf",
	      "0x000fff1000084801 = receipts/2026-09-24-t6021-coresight-dart");

	/* P3: walks against measured values. */
	rc = dart8_translate(&cfg, &pt, TEXTIOVA, false, &pa);
	check(rc == 0 && pa == TEXTPA, "P3-text-first",
	      "iova_to_phys(0x10000000000)");
	rc = dart8_translate(&cfg, &pt, TEXTIOVA + TEXTSZ - 16384, false, &pa);
	check(rc == 0 && pa == TEXTPA + TEXTSZ - 16384, "P3-text-last",
	      "last TEXT page");
	rc = dart8_translate(&cfg, &pt, 0x100000dca10ULL, false, &pa);
	check(rc == 0 && pa == 0x10001418a10ULL, "P3-historic-fault-iova",
	      "0x100000dca10 -> DATA + 0x18a10");
	rc = dart8_translate(&cfg, &pt, DATAIOVA + DATASZ - 16384, true, &pa);
	check(rc == 0 && pa == DATAPA + DATASZ - 16384, "P3-data-last",
	      "last DATA page, write walk");
	rc = dart8_translate(&cfg, &pt, TEXTIOVA + 0x1234, false, &pa);
	check(rc == 0 && pa == TEXTPA + 0x1234, "P3-offset-carry",
	      "in-page offset preserved");

	/* Stream programming: measured TCR 0x9 on the 3-level shape. */
	root_pa = (u64)(unsigned long)pt.root;
	rc = dart8_stream_program(&cfg, 0, root_pa, &ttbr, &tcr);
	check(rc == 0 && tcr == 0x9, "P3-tcr-word",
	      "TRANSLATE|FOUR_LEVEL = 0x9 (measured T6001 sid0)");
	check(ttbr == (((root_pa >> 14) << 2) | 1), "P3-ttbr-word",
	      "t8110 TTBR form");

	/* P4: 2-level mode, same leaf format, DERIVED structure. */
	{
		struct dart8_cfg c2;
		struct dart8_pt p2;
		u64 xpa;

		rc = dart8_cfg_init(&c2, 0, 1ULL << 36, sids, 4, talloc,
				    tfree);
		check(rc == 0 && c2.walk_levels == 2, "P4-2level-cfg",
		      "64 GiB window -> 2 walk levels");
		p2.root = talloc(16384);
		if (!p2.root)
			return 1;
		memset(p2.root, 0, 16384);
		rc = dart8_map(&c2, &p2, 0x200000000ULL, 0x200000000ULL,
			       16384, 0);
		check(rc == 0, "P4-2level-map", "one page at 16 GiB");
		rc = dart8_translate(&c2, &p2, 0x200000000ULL, false, &xpa);
		check(rc == 0 && xpa == 0x200000000ULL, "P4-2level-walk",
		      "translate round trip");
		check(p2.root[256] == ref_table_pte((u64)(unsigned long)tbl[4]),
		      "P4-2level-l1", "L1[256] table PTE");
		rc = dart8_stream_program(&c2, 0, 0, &ttbr, &tcr);
		check(rc == 0 && tcr == 0x1, "P4-2level-tcr",
		      "TRANSLATE only, no FOUR_LEVEL");
	}

	/* N1: sid allowlist {0,10,11,15}; mapper-ane-mpm {11}. */
	check(dart8_stream_program(&cfg, 1, 0, &ttbr, &tcr) == -EINVAL,
	      "N1-sid1-refused", "1 not in {0,10,11,15}");
	check(dart8_stream_program(&cfg, 5, 0, &ttbr, &tcr) == -EINVAL,
	      "N1-sid5-refused", "5 not in allowlist");
	check(dart8_stream_program(&cfg, 255, 0, &ttbr, &tcr) == -EINVAL,
	      "N1-sid255-refused", "255 not in allowlist");
	check(dart8_stream_program(&cfg, 11, root_pa, &ttbr, &tcr) == 0,
	      "N1-sid11-allowed", "mapper-ane-mpm stream 11");
	{
		struct dart8_cfg cm;

		rc = dart8_cfg_init(&cm, VMBASE, VMSIZE, mpm_sids, 1,
				    talloc, tfree);
		check(rc == 0 &&
		      dart8_stream_program(&cm, 11, 0, &ttbr, &tcr) == 0 &&
		      dart8_stream_program(&cm, 0, 0, &ttbr, &tcr) == -EINVAL,
		      "N1-mpm-set", "second parent modeled as {11}");
	}

	/* N2: unmapped walks fault with hardware-shaped codes. */
	rc = dart8_translate(&cfg, &pt, 0x10002000000ULL, false, &pa);
	check(rc == -(int)(D8_FAULT_NO_PMD | D8_FAULT_READ), "N2-no-pmd",
	      "l2[1] absent under L1[16]");
	rc = dart8_translate(&cfg, &pt, 0x11000000000ULL, true, &pa);
	check(rc == -(int)(D8_FAULT_NO_PGD | D8_FAULT_WRITE), "N2-no-pgd",
	      "L1[17] absent, write walk");

	/* N3: wrong permission bits fault the matching access. The page
	 * sits at leaf index 320, past the mapped ranges (0..318).
	 */
	rc = dart8_map(&cfg, &pt, TEXTIOVA + 320 * 16384, TEXTPA, 16384,
		       D8_PTE_NO_WRITE);
	check(rc == 0, "N3-map-nowrite", "one RO page");
	rc = dart8_translate(&cfg, &pt, TEXTIOVA + 320 * 16384, true, &pa);
	check(rc == -(int)D8_FAULT_WRITE, "N3-write-faults",
	      "write walk on NO_WRITE leaf");
	rc = dart8_translate(&cfg, &pt, TEXTIOVA + 320 * 16384, false, &pa);
	check(rc == 0 && pa == TEXTPA, "N3-read-still-ok", "read walk passes");
	check(((u64 *)(unsigned long)leaf_pa)[320] ==
	      ref_leaf_pte(TEXTPA, 0, 1, 0), "N3-bit-place",
	      "NO_WRITE is bit 2, NO_CACHE clear");
	exp_leaf[320] = ref_leaf_pte(TEXTPA, 0, 1, 0);	/* keep N5 honest */
	rc = dart8_map(&cfg, &pt, TEXTIOVA + 320 * 16384, TEXTPA, 16384,
		       D8_PTE_NO_CACHE);
	check(rc == -EEXIST, "N3-conflict", "remap with other bits refused");

	/* N4: window and alignment refusals. */
	rc = dart8_map(&cfg, &pt, VMBASE - 16384, TEXTPA, 16384, 0);
	check(rc == -ERANGE, "N4-below-window", "iova < vm_base");
	rc = dart8_map(&cfg, &pt, VMBASE + VMSIZE - 16384, TEXTPA, 32768, 0);
	check(rc == -ERANGE, "N4-above-window", "range crosses vm top");
	rc = dart8_map(&cfg, &pt, TEXTIOVA + 8, TEXTPA, 16384, 0);
	check(rc == -EINVAL, "N4-unaligned-iova", "iova not page aligned");
	rc = dart8_map(&cfg, &pt, TEXTIOVA, TEXTPA + 8, 16384, 0);
	check(rc == -EINVAL, "N4-unaligned-pa", "pa not page aligned");

	/* N5: the comparison catches a corrupted vector. */
	exp_leaf[7] ^= 2;	/* flip NO_CACHE on an expected leaf */
	d = diff_words(exp_leaf, (u64 *)(unsigned long)leaf_pa, 2048);
	check(d > 0, "N5-corruption-caught", "one flipped bit detected");

	free(exp_root);
	free(exp_l2);
	free(exp_leaf);

	printf("%s: %d failure(s)\n", fails ? "HARNESS FAILED" : "ALL PASSED",
	       fails);
	return fails ? 1 : 0;
}
