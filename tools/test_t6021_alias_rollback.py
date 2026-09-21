#!/usr/bin/env python3
"""Compile the real alias function with an in-memory IOMMU test double.
Tests cleanup bookkeeping only, never hardware or firmware execution.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'ane/t6021/ane_t6021_fwload.c').read_text()
function = source.split('static int ane_t6021_fw_alias_map(', 1)[1].split('\nstatic const u8 ', 1)[0]
function = 'static int ane_t6021_fw_alias_map(' + function
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
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
#define PAGE ANE_T6021_FW_ALIAS_PAGE
struct iommu_domain { struct { u64 aperture_end; } geometry; } domain = {{~0ULL}};
struct ane_t6021 { void *dev; void *base[1]; u64 fw_iova, fw_size, fw_alias_iova; };
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
            assert(ane_t6021_fw_alias_map(&ane) == (mode == 0 ? -ENOMEM : -EIO));
            assert(!ane.fw_alias_iova);
            for (int i = 0; i < 3; ++i) assert(!pages[i]);
            assert(unmapped == (u64)(mode == 0 ? failure : 3) * PAGE);
        }
    }
    struct ane_t6021 ane = {.fw_iova=SOURCE, .fw_size=3*PAGE};
    mapped=0; unmapped=0; map_failure=verify_failure=-1;
    assert(ane_t6021_fw_alias_map(&ane) == 0);
    assert(ane.fw_alias_iova == ENTRY && unmapped == 0);
    for (int i=0; i<3; ++i) assert(pages[i]);
    puts("PASS actual alias function: map/roundtrip failure at every page and success");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    c.write_text(prefix + function + suffix)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-Wno-unused-but-set-variable', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
