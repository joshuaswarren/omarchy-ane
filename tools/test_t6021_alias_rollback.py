#!/usr/bin/env python3
"""Compile the real alias function with an in-memory IOMMU test double, and
the real reserved-memory coverage check (fw_alias_reserved=1 refuses unless
no-map /reserved-memory nodes cover both iBoot firmware windows) with a fake
device tree. Tests bookkeeping only, never hardware or firmware execution.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'ane/t6021/ane_t6021_fwload.c').read_text()
preload = 'static const struct { u64 phys, len; } ane_t6021_fw_preload[]'
preload += source.split(preload, 1)[1].split('\nbool ane_t6021_fwload_placement_ok', 1)[0]
function = source.split('static int ane_t6021_fw_alias_map(', 1)[1].split('\nstatic const u8 ', 1)[0]
function = preload + 'static int ane_t6021_fw_alias_map(' + function
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
typedef uint64_t u64;
typedef uint64_t phys_addr_t;
#define __iomem
#define ANE_T6021_REG_ENGINE 0
#define ANE_ASC_RVBAR 0
#define ANE_T6021_FW_ALIAS_PAGE 0x4000
#define IOMMU_READ 1
#define IOMMU_WRITE 2
#define IOMMU_CACHE 4
#define GFP_KERNEL 0
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define ENTRY (1ULL << 40)
#define SOURCE 0x100000ULL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define IS_ALIGNED(x, a) (!((x) & ((a) - 1)))
#define PAGE ANE_T6021_FW_ALIAS_PAGE
struct iommu_domain { struct { u64 aperture_end; } geometry; } domain = {{~0ULL}};
struct ane_t6021 { void *dev; void *base[1]; u64 fw_iova, fw_size, fw_alias_iova; u64 fw_alias_ext_iova[3]; size_t fw_alias_ext_len[3]; int fw_alias_extn; };
static unsigned int fw_extra_ram;
static int ane_t6021_pmu_map(struct ane_t6021 *ane, struct iommu_domain *dom) { (void)ane; (void)dom; return 0; }
struct device_node { bool nomap, taken; u64 base, size; };
struct reserved_mem { u64 base, size; };
static struct device_node parent, nodes[4];
static struct reserved_mem rmem[4];
static int nnodes, refs;
static bool have_parent;
static struct device_node *of_find_node_by_path(const char *path)
{
    assert(!strcmp(path, "/reserved-memory"));
    if (!have_parent) return NULL;
    ++refs;
    return &parent;
}
static void of_node_put(struct device_node *np) { if (np) --refs; }
static struct device_node *of_get_next_available_child(struct device_node *p, struct device_node *prev)
{
    assert(p == &parent);
    int i = prev ? (int)(prev - nodes) + 1 : 0;
    of_node_put(prev);
    if (i >= nnodes) return NULL;
    ++refs;
    return &nodes[i];
}
#define for_each_available_child_of_node(p, child) \
    for (child = of_get_next_available_child(p, NULL); child; child = of_get_next_available_child(p, child))
static struct reserved_mem *of_reserved_mem_lookup(struct device_node *np)
{
    struct reserved_mem *r = &rmem[np - nodes];
    r->base = np->base; r->size = np->size;
    return np->taken ? r : NULL;
}
static bool of_property_read_bool(const struct device_node *np, const char *name)
{
    assert(!strcmp(name, "no-map"));
    return np->nomap;
}
static bool pages[3];
static int map_failure, verify_failure, mapped;
static u64 unmapped;
static struct iommu_domain *iommu_get_domain_for_dev(void *dev) { (void)dev; return &domain; }
static u64 readq(void *addr) { (void)addr; return ENTRY | 1; }
static u64 ane_t6021_rvbar_entry_bits(u64 v) { return v & ~1ULL; }
static bool ane_t6021_rvbar_latched(u64 v) { return v & 1; }
static bool ane_t6021_rvbar_entry_ok(u64 v) { return v == ENTRY; }
static bool dev_is_dma_coherent(void *dev) { (void)dev; return true; }
static phys_addr_t iommu_iova_to_phys(struct iommu_domain *dom, u64 addr)
{
    (void)dom;
    if (addr >= SOURCE && addr < SOURCE + 3 * PAGE)
        return 0x800000 + addr - SOURCE;
    assert(addr >= ENTRY && addr < ENTRY + 3 * PAGE);
    int i = (addr - ENTRY) / PAGE;
    if (!pages[i]) return 0;
    if (mapped == 3 && i == verify_failure) return 0;
    return 0x800000 + i * PAGE;
}
static int iommu_map(struct iommu_domain *dom, u64 addr, phys_addr_t pa,
                     u64 size, int prot, int flags)
{
    (void)dom; (void)pa; (void)prot; (void)flags;
    int i = (addr - ENTRY) / PAGE;
    assert(size == PAGE && !pages[i]);
    if (i == map_failure) return -ENOMEM;
    pages[i] = true; ++mapped;
    return 0;
}
static u64 iommu_unmap(struct iommu_domain *dom, u64 addr, u64 size)
{
    (void)dom;
    assert(addr == ENTRY && size <= 3 * PAGE && size % PAGE == 0);
    for (unsigned int i = 0; i < size / PAGE; ++i) pages[i] = false;
    unmapped += size;
    return size;
}
'''
suffix = r'''
int main(void)
{
    for (int mode = 0; mode < 2; ++mode) {
        for (int failure = 0; failure < 3; ++failure) {
            struct ane_t6021 ane = {.fw_iova=SOURCE, .fw_size=3*PAGE};
            for (int i = 0; i < 3; ++i) pages[i] = false;
            mapped = 0; unmapped = 0;
            map_failure = mode == 0 ? failure : -1;
            verify_failure = mode == 1 ? failure : -1;
            assert(ane_t6021_fw_alias_map(&ane, false) == (mode == 0 ? -ENOMEM : -EIO));
            assert(!ane.fw_alias_iova);
            for (int i = 0; i < 3; ++i) assert(!pages[i]);
            assert(unmapped == (u64)(mode == 0 ? failure : 3) * PAGE);
        }
    }
    struct ane_t6021 ane = {.fw_iova=SOURCE, .fw_size=3*PAGE};
    mapped=0; unmapped=0; map_failure=verify_failure=-1;
    assert(ane_t6021_fw_alias_map(&ane, false) == 0);
    assert(ane.fw_alias_iova == ENTRY && unmapped == 0);
    for (int i=0; i<3; ++i) assert(pages[i]);
    puts("PASS actual alias function: map/roundtrip failure at every page and success");

    /* Reserved-memory coverage. SEG0 0x10000848000+0xc4000, SEG1
     * 0x10001400000+0x438000; the lab m1n1 adds one no-map node for each. */
    const struct device_node seg0 = {true, true, 0x10000848000ULL, 0xc4000},
                             seg1 = {true, true, 0x10001400000ULL, 0x438000},
                             other = {true, true, 0x20000000000ULL, 0x100000};
    struct { const char *why; bool parent; int n; struct device_node dt[4]; bool ok; } cases[] = {
        {"no /reserved-memory (packaged m1n1)", false, 0, {{0}}, false},
        {"empty /reserved-memory", true, 0, {{0}}, false},
        {"lab m1n1: both windows", true, 2, {seg0, seg1}, true},
        {"lab m1n1 among other nodes", true, 3, {other, seg1, seg0}, true},
        {"one node over both windows", true, 1, {{true, true, 0x10000848000ULL, 0xff0000}}, true},
        {"SEG0 only", true, 2, {seg0, other}, false},
        {"SEG1 without no-map", true, 2, {seg0, {false, true, seg1.base, seg1.size}}, false},
        {"SEG1 not taken by the kernel", true, 2, {seg0, {true, false, seg1.base, seg1.size}}, false},
        {"SEG0 one page short", true, 2, {{true, true, seg0.base, seg0.size - PAGE}, seg1}, false},
        {"SEG1 one page late", true, 2, {seg0, {true, true, seg1.base + PAGE, seg1.size}}, false},
    };
    for (unsigned int i = 0; i < ARRAY_SIZE(cases); ++i) {
        have_parent = cases[i].parent;
        nnodes = cases[i].n;
        for (int j = 0; j < nnodes; ++j) nodes[j] = cases[i].dt[j];
        refs = 0;
        bool ok = ane_t6021_fw_preload_reserved();
        if (ok != cases[i].ok || refs) {
            printf("FAIL %s: covered=%d refs=%d\n", cases[i].why, ok, refs);
            return 1;
        }
    }
    puts("PASS actual reserved-memory coverage check: both windows, no-map, kernel-taken, no leaked node refs");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(prefix + function + suffix)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-but-set-variable', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
