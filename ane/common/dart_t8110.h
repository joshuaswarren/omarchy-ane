/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dart_t8110.h - Apple DART t8110 page-table builder (16 KiB pages).
 *
 * Header-only so a kernel module and a userspace test build the same
 * code. Static-inline, no allocations inside the header: the caller
 * supplies table memory through alloc/free callbacks.
 *
 * Format sources:
 *   - Leaf PTE: drivers/iommu/io-pgtable-dart.c, APPLE_DART2 format
 *     (used by both "apple,t8110-dart" and "apple,t6000-dart"):
 *       pte = (pa >> 4) & GENMASK_ULL(37,10) | prot | subpage | VALID
 *       prot: NO_CACHE bit1, NO_WRITE bit2, NO_READ bit3
 *       leaf-only subpage fields: 0xfff << 40, 0 << 52.
 *     MEASURED: T6021 ANE-DART leaf PTE read back 2026-09-24
 *     (receipts/2026-09-24-t6021-coresight-dart): PA 0x10000848000
 *     RW+cacheable mapped at IOVA 0x10000000000 read back
 *     0x000fff1000084801; iommu_iova_to_phys agreed. The same format
 *     was walked by working hardware: the T6021 firmware executes
 *     inference on this map (receipts/2026-09-29-t6021-installed-path).
 *   - Table PTE: same address field, VALID, no prot, no subpage bits
 *     (io-pgtable-dart.c dart_install_table). INFERENCE from the
 *     kernel-built tables the hardware walks end to end.
 *   - TTBR/TCR registers: drivers/iommu/apple-dart.c DART_T8110_*.
 *     TTBR = (root_pa >> 14) << 2 | VALID(bit0). TCR: TRANSLATE bit0,
 *     BYPASS_DART bit1, BYPASS_DAPF bit2, FOUR_LEVEL bit3.
 *     MEASURED: working macOS dart-ane sid0 TCR = 0x9
 *     (TRANSLATE|FOUR_LEVEL, REMAP_EN clear) and TTBR 0x1004102d on
 *     T6001 (docs/t6021-ane-bringup-findings.md section 17); T6021
 *     TCR15 = 0x2 (BYPASS_DART).
 *   - Geometry: 16 KiB granule, 2048 entries per table, index shifts
 *     leaf 14, level2 25, level1 36. A 2-level walk covers 2^36;
 *     windows above that need 3 walk levels (TCR FOUR_LEVEL), indexed
 *     by absolute IOVA bits. MEASURED on the proven map: the ANE
 *     remap IOVAs sit at 2^40, so the proven walk is 3 levels.
 *
 * NOT PROVEN: that an H15/H16 DART accepts any of this. No silicon
 * check exists (a sim is not silicon). The sid allowlist, vm window
 * and register words are parameters fed from the device tree.
 */
#ifndef _DART_T8110_H
#define _DART_T8110_H

#ifdef __KERNEL__
#include <linux/bits.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define BIT_ULL(b) (1ULL << (b))
#endif

/* Leaf PTE, APPLE_DART2 format (io-pgtable-dart.c). */
#define D8_PADDR_SHIFT		4
#define D8_PADDR_MASK		GENMASK_ULL(37, 10)
#define D8_PTE_VALID		BIT_ULL(0)
#define D8_PTE_NO_CACHE		BIT_ULL(1)
#define D8_PTE_NO_WRITE		BIT_ULL(2)
#define D8_PTE_NO_READ		BIT_ULL(3)
#define D8_PTE_SUBPAGE_START	GENMASK_ULL(63, 52)	/* leaves: 0 */
#define D8_PTE_SUBPAGE_END	GENMASK_ULL(51, 40)	/* leaves: 0xfff */

/* Registers (apple-dart.c DART_T8110_*). */
#define D8_TTBR_VALID		BIT_ULL(0)
#define D8_TTBR_ADDR_SHIFT	14
#define D8_TTBR_FIELD_SHIFT	2
#define D8_TCR_TRANSLATE	BIT_ULL(0)
#define D8_TCR_BYPASS_DART	BIT_ULL(1)
#define D8_TCR_BYPASS_DAPF	BIT_ULL(2)
#define D8_TCR_FOUR_LEVEL	BIT_ULL(3)
#define D8_TCR_REMAP_EN		BIT_ULL(7)

/* Hardware error-register shaped walk faults (apple-dart.c
 * DART_T8110_ERROR_*), negated for return as int.
 */
#define D8_FAULT_NO_TTBR	BIT_ULL(0)
#define D8_FAULT_NO_PGD		BIT_ULL(1)
#define D8_FAULT_NO_PMD		BIT_ULL(2)
#define D8_FAULT_NO_PTE		BIT_ULL(3)
#define D8_FAULT_WRITE		BIT_ULL(4)
#define D8_FAULT_READ		BIT_ULL(5)

#define D8_GRANULE		0x4000			/* 16 KiB */
#define D8_ENTRIES		2048			/* per table */
#define D8_BITS_PER_LEVEL	11
#define D8_MAX_WALK_LEVELS	3
#define D8_MAX_SID		256

static inline u64 dart8_leaf_pte(u64 pa, u64 prot)
{
	return ((pa >> D8_PADDR_SHIFT) & D8_PADDR_MASK) |
	       D8_PTE_SUBPAGE_END |	/* field content 0xfff; START stays 0 */
	       (prot & (D8_PTE_NO_CACHE | D8_PTE_NO_WRITE | D8_PTE_NO_READ)) |
	       D8_PTE_VALID;
}

static inline u64 dart8_table_pte(u64 table_pa)
{
	return ((table_pa >> D8_PADDR_SHIFT) & D8_PADDR_MASK) | D8_PTE_VALID;
}

static inline u64 dart8_leaf_pa(u64 pte)
{
	return (pte & D8_PADDR_MASK) << D8_PADDR_SHIFT;
}

static inline u64 dart8_ttbr(u64 root_pa)
{
	return ((root_pa >> D8_TTBR_ADDR_SHIFT) << D8_TTBR_FIELD_SHIFT) |
	       D8_TTBR_VALID;
}

struct dart8_cfg {
	u64 vm_base;
	u64 vm_size;
	u8 walk_levels;		/* 2 or 3, derived from the window */
	const u8 *sids;		/* allowed streams; NULL = any < D8_MAX_SID */
	u8 n_sids;
	void *(*alloc)(size_t size);	/* zeroed, 16 KiB aligned */
	void (*free)(void *p, size_t size);
};

static inline int dart8_sid_ok(const struct dart8_cfg *c, u8 sid)
{
	u8 i;

	if (!c->sids)
		return 1;
	for (i = 0; i < c->n_sids; i++)
		if (c->sids[i] == sid)
			return 1;
	return 0;
}

/* Validate the window and derive the walk level count. Returns 0 or
 * -EINVAL/-ERANGE.
 */
static inline int dart8_cfg_init(struct dart8_cfg *c, u64 vm_base,
				 u64 vm_size, const u8 *sids, u8 n_sids,
				 void *(*alloc)(size_t),
				 void (*freefn)(void *, size_t))
{
	u64 top, va_bits, levels;

	if (!alloc || !freefn || !vm_size || vm_size & (D8_GRANULE - 1) ||
	    vm_base & (D8_GRANULE - 1))
		return -EINVAL;
	top = vm_base + vm_size - 1;
	if (top < vm_base)
		return -ERANGE;
	va_bits = (top >> 14) ? 64 - __builtin_clzll(top) - 14 : 0;
	levels = (va_bits + D8_BITS_PER_LEVEL - 1) / D8_BITS_PER_LEVEL;
	if (levels < 2)
		levels = 2;
	if (levels > D8_MAX_WALK_LEVELS)
		return -ERANGE;
	c->vm_base = vm_base;
	c->vm_size = vm_size;
	c->walk_levels = (u8)levels;
	c->sids = sids;
	c->n_sids = n_sids;
	c->alloc = alloc;
	c->free = freefn;
	return 0;
}

/* TCR word for a translate-enabled stream on this config. */
static inline u64 dart8_tcr(const struct dart8_cfg *c)
{
	return D8_TCR_TRANSLATE |
	       (c->walk_levels == 3 ? D8_TCR_FOUR_LEVEL : 0);
}

/* Register words for stream sid on a root at root_pa. Refuses sids
 * outside the allowlist and unaligned roots.
 */
static inline int dart8_stream_program(const struct dart8_cfg *c, u8 sid,
				       u64 root_pa, u64 *ttbr, u64 *tcr)
{
	if (!dart8_sid_ok(c, sid) || root_pa & (D8_GRANULE - 1))
		return -EINVAL;
	*ttbr = dart8_ttbr(root_pa);
	*tcr = dart8_tcr(c);
	return 0;
}

/* Index into a table at walk level l (1 = level below TTBR). */
static inline u32 dart8_index(const struct dart8_cfg *c, u64 iova, u8 l)
{
	return (u32)((iova >> (14 + D8_BITS_PER_LEVEL *
			       (c->walk_levels - l))) & (D8_ENTRIES - 1));
}

/* Fault code for a walk that dies at level l (1-based), or the leaf. */
static inline int dart8_fault_at(const struct dart8_cfg *c, u8 l, bool write)
{
	u64 code;

	if (l < c->walk_levels)
		code = (l == 1) ? D8_FAULT_NO_PGD : D8_FAULT_NO_PMD;
	else
		code = D8_FAULT_NO_PTE;
	code |= write ? D8_FAULT_WRITE : D8_FAULT_READ;
	return -(int)code;
}

struct dart8_pt {
	u64 *root;		/* first table below the TTBR, 16 KiB */
};

/* Map [iova, iova+size) -> [pa, pa+size), page aligned, inside the vm
 * window. Intermediate tables are allocated zeroed on demand. Returns
 * 0, -EINVAL (alignment), -ERANGE (outside window), -EEXIST (conflict),
 * or -ENOMEM.
 */
static inline int dart8_map(const struct dart8_cfg *c, struct dart8_pt *pt,
			    u64 iova, u64 pa, u64 size, u64 prot)
{
	u64 off, prot_bits, pte, want;
	u64 *table;
	u32 idx;
	u8 l;

	if (!pt->root || (iova | pa | size) & (D8_GRANULE - 1) || !size)
		return -EINVAL;
	if (iova < c->vm_base || iova + size - 1 < iova ||
	    iova + size - 1 >= c->vm_base + c->vm_size)
		return -ERANGE;
	prot_bits = prot & (D8_PTE_NO_CACHE | D8_PTE_NO_WRITE |
			    D8_PTE_NO_READ);
	for (off = 0; off < size; off += D8_GRANULE) {
		table = pt->root;
		for (l = 1; l < c->walk_levels; l++) {
			idx = dart8_index(c, iova + off, l);
			pte = table[idx];
			if (!(pte & D8_PTE_VALID)) {
				void *t = c->alloc(D8_GRANULE);

				if (!t)
					return -ENOMEM;
				memset(t, 0, D8_GRANULE);
				table[idx] = dart8_table_pte((u64)(unsigned long)t);
				pte = table[idx];
			}
			table = (u64 *)(unsigned long)dart8_leaf_pa(pte);
		}
		idx = dart8_index(c, iova + off, c->walk_levels);
		pte = table[idx];
		want = dart8_leaf_pte(pa + off, prot_bits);
		if ((pte & D8_PTE_VALID) && pte != want)
			return -EEXIST;
		table[idx] = want;
	}
	return 0;
}

/* Walk to a physical address. Returns 0 and sets *pa, or a negated
 * DART_T8110_ERROR-shaped fault code. Permission is checked like the
 * NO_READ/NO_WRITE error bits.
 */
static inline int dart8_translate(const struct dart8_cfg *c,
				  const struct dart8_pt *pt, u64 iova,
				  bool write, u64 *pa)
{
	const u64 *table;
	u64 pte = 0;
	u8 l;

	if (!pt->root)
		return -(int)(D8_FAULT_NO_TTBR |
			      (write ? D8_FAULT_WRITE : D8_FAULT_READ));
	table = pt->root;
	for (l = 1; l <= c->walk_levels; l++) {
		u32 idx = dart8_index(c, iova, l);

		pte = table[idx];
		if (!(pte & D8_PTE_VALID))
			return dart8_fault_at(c, l, write);
		if (l < c->walk_levels)
			table = (const u64 *)(unsigned long)dart8_leaf_pa(pte);
	}
	if (write && (pte & D8_PTE_NO_WRITE))
		return -(int)(D8_FAULT_WRITE);
	if (!write && (pte & D8_PTE_NO_READ))
		return -(int)(D8_FAULT_READ);
	*pa = dart8_leaf_pa(pte) + (iova & (D8_GRANULE - 1));
	return 0;
}

#endif /* _DART_T8110_H */
