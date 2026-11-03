// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <assert.h>
#define main existing_sim_main
#include "sim/sim_harness.c"
#undef main

static const struct ane_h15_soc *target;
static const char *compatible;

static void prepare(const char *path)
{
	assert(sim_fw_live == 0);
	reset_all();
	sim_fw_requests = 0;
	build_node(target, 2, compatible);
	set_ps_words(target, -1);
	optin = (char *)target->name;
	stage = "boot";
	confirm_boot = true;
	confirm_read = true;
	fw_path = (char *)path;
	sim_dma_poison = 1;
	sim_bus_map(target->engine_pa, target->engine_size);
	eng_set(target, target->cpu_status, 0x30002a);
	eng_set(target, target->cpu_control, 0);
}

static void refuse(int expected, int requests)
{
	assert(probe_now() == expected);
	assert(sim_fw_requests == requests && sim_fw_live == 0);
	assert(sim_n_dma == 0 && sim_n_iocalls == 0);
	assert(events_in(target->engine_pa, target->engine_size, 1) == 0);
}

static unsigned char *read_expected(const char *path)
{
	unsigned char *bytes = malloc(0x36c000);
	FILE *file = fopen(path, "rb");

	assert(file && bytes);
	assert(fread(bytes, 1, 0x36c000, file) == 0x36c000);
	assert(fgetc(file) == EOF && !fclose(file));
	return bytes;
}

#define IMAGE_BYTES 0x36c000u

static void owned_map_setup(const char *path)
{
	map_setup(target, compatible);
	sim_fw_requests = 0;
	sim_dma_poison = 1;
	fw_seg = NULL;
	fw_path = (char *)path;
	confirm_read = true;
}

static void owned_untouched(int expected, const char *reason)
{
	unsigned int rstage;
	char verdict[32], found[128];

	assert(probe_now() == expected);
	assert(parse_result(&rstage, verdict, sizeof(verdict), found, sizeof(found)));
	assert(rstage == 5 && !strcmp(verdict, "REFUSED") && strstr(found, reason));
	assert(sim_n_events == 0 && sim_n_iocalls == 0 && sim_n_dma == 0 && sim_fw_requests == 0);
}

/* Rebuild what the core can read through the DART from the recorded map calls. */
static unsigned char *mapped_image(u64 *bytes_mapped)
{
	const u64 base = target->rvbar_expect & ~1ull;
	unsigned char *image = malloc(IMAGE_BYTES);
	unsigned char seen[IMAGE_BYTES / 0x4000] = { 0 };
	u64 total = 0;
	int k;

	assert(image && sim_n_dma >= 1);
	memset(image, 0xee, IMAGE_BYTES);
	for (k = 0; k < sim_n_iocalls; k++) {
		const struct sim_iocall *call = &sim_iocalls[k];
		u64 off;

		if (call->code != SIM_IO_MAP)
			continue;
		assert(call->iova >= base && call->size && !(call->size & 0x3fff) && call->iova + call->size <= base + IMAGE_BYTES);
		assert(!((call->iova - base) < 0xcc000 && (call->iova - base) + call->size > 0xcc000));
		assert(call->prot == ((call->iova - base) < 0xcc000 ? IOMMU_READ : (IOMMU_READ | IOMMU_WRITE)));
		for (off = 0; off < call->size; off += 0x4000) {
			u64 page = (call->iova - base + off) / 0x4000;
			u64 phys_page = (call->pa + off - (u64)(uintptr_t)sim_dma[0].va) / 0x4000;

			assert(!seen[page] && phys_page < IMAGE_BYTES / 0x4000);
			seen[page] = 1;
			memcpy(image + page * 0x4000, (unsigned char *)sim_dma[0].va + sim_dma_page(phys_page) * 0x4000, 0x4000);
			total += 0x4000;
		}
	}
	*bytes_mapped = total;
	return image;
}

static void owned_map_success(const char *path, const unsigned char *expected, int scatter)
{
	const u64 base = target->rvbar_expect & ~1ull;
	unsigned int rstage;
	char verdict[32], reason[128], seq[80];
	unsigned char *image;
	u64 mapped, unmapped = 0;
	int k, maps = 0, unmaps = 0, first, n;
	u32 writes[8];
	size_t pos = 2;

	owned_map_setup(path);
	sim_dma_scatter = scatter;
	assert(probe_now() == 0);
	assert(parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	       rstage == 5 && !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"));
	io_seq(seq, sizeof(seq));
	assert(!strncmp(seq, "AT", 2));
	while (seq[pos] == 'M') {
		pos++;
		maps++;
	}
	assert(!strcmp(seq + pos, "UUDF") && maps >= 2 && (!scatter || maps > 8));
	image = mapped_image(&mapped);
	assert(mapped == IMAGE_BYTES && !memcmp(image, expected, IMAGE_BYTES));
	free(image);
	for (k = 0; k < sim_n_iocalls; k++)
		if (sim_iocalls[k].code == SIM_IO_UNMAP) {
			unmaps++;
			unmapped += sim_iocalls[k].size;
		}
	assert(unmaps == 2 && unmapped == IMAGE_BYTES);
	assert(sim_iocalls[2 + maps + 0].iova == base && sim_iocalls[2 + maps + 0].size == 0xcc000);
	assert(sim_iocalls[2 + maps + 1].iova == base + 0xcc000 && sim_iocalls[2 + maps + 1].size == IMAGE_BYTES - 0xcc000);
	n = run_writes(target, writes, 8, &first);
	assert(n == 2 && writes[0] == 0x10 && writes[1] == 0 && sim_iocalls[1 + maps].ev <= first);
	assert(sim_fw_requests == 1 && sim_fw_live == 0 && sim_n_dma == 1 && !sim_dma[0].live);
	assert(sim_dma[0].unmaps_at_free == 2 && sim_n_dom_live == 0 && sim_pm_get == 1 && sim_pm_put == 1);
}

static void owned_map_refusals(const char *path, const char *corrupt)
{
	unsigned int rstage;
	char verdict[32], reason[128], seq[80];
	int ret, k;

	owned_map_setup(path);
	fw_seg = SEGS_OK;
	owned_untouched(-EINVAL, "firmware source");
	owned_map_setup(path);
	confirm_read = false;
	owned_untouched(-EPERM, "confirm_read");
	owned_map_setup(path);
	node.has_iommus = false;
	owned_untouched(-ENODEV, "iommus");

	owned_map_setup(path);
	eng_set(target, target->rvbar, 2);
	ret = probe_now();
	assert(ret == -EBUSY && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	       !strcmp(reason, "preflight-not-as-iboot-left-it"));
	assert(sim_fw_requests == 0 && sim_n_dma == 0 && sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0);

	owned_map_setup(corrupt);
	ret = probe_now();
	assert(ret == -EBADMSG && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	       !strcmp(reason, "owned-firmware-prepare-failed"));
	assert(sim_fw_requests == 1 && sim_fw_live == 0 && sim_n_dma == 0 && sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0);
	eng_set(target, target->rvbar, 1);

	owned_map_setup(path);
	sim_io_fail = 5;
	ret = probe_now();
	assert(ret == -EIO && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	       !strcmp(reason, "fw-map-failed"));
	io_seq(seq, sizeof(seq));
	assert(!strcmp(seq, "ATMUDF") && sim_iocalls[3].iova == (target->rvbar_expect & ~1ull) && sim_iocalls[3].size == 0xcc000);
	assert(events_in(0, ~0ULL, 1) == 0 && sim_n_dom_live == 0 && !sim_dma[0].live && sim_dma[0].unmaps_at_free == 1);
	owned_map_setup(path);
	sim_default_dom_none = 1;
	ret = probe_now();
	assert(ret == -ENODEV && sim_fw_requests == 0 && sim_n_dma == 0 && sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0);

	owned_map_setup(path);
	sim_dma_scatter = 1;
	sim_map_fail_at = 3;
	ret = probe_now();
	assert(ret == -ENOMEM && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	       !strcmp(reason, "fw-map-failed"));
	io_seq(seq, sizeof(seq));
	assert(!strcmp(seq, "ATMMMUDF"));
	assert(sim_iocalls[5].iova == sim_iocalls[2].iova && sim_iocalls[5].size == sim_iocalls[2].size + sim_iocalls[3].size);
	assert(events_in(0, ~0ULL, 1) == 0 && sim_n_dom_live == 0 && !sim_dma[0].live && sim_dma[0].unmaps_at_free == 1);

	owned_map_setup(path);
	dm_stuck = true;
	assert(probe_now() == 0);
	io_seq(seq, sizeof(seq));
	assert(seq[0] == 'A' && seq[1] == 'T' && !strchr(seq, 'U') && !strchr(seq, 'D') && !strchr(seq, 'F'));
	assert(sim_n_dom_live == 1 && sim_dma[0].live && sim_module_gets == 1 && sim_pm_put == 0);
	for (k = 0; k < 3; k++)
		ane_h15_remove(&pdev);
	assert(sim_n_dom_live == 1 && sim_dma[0].live && sim_n_iounmap == 0);
}

static void owned_map_short_unmap(const char *path)
{
	unsigned int rstage;
	char verdict[32], reason[128], seq[80];
	int fail, before, k;

	for (fail = 1; fail <= 4; fail++) {
		owned_map_setup(path);
		sim_unmap_fail_at = fail == 4 ? -1 : fail == 2 ? 2 : 1;
		if (fail == 3) {
			sim_dma_scatter = 1;
			sim_map_fail_at = 3;
		}
		assert(probe_now() == 0);
		assert(parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		       rstage == 5 && !strcmp(verdict, "FAIL"));
		assert(sim_n_dom_live == 1 && sim_dma[0].live && sim_module_gets == 1 &&
		       sim_pm_put == 0 && sim_n_iounmap == 0);
		io_seq(seq, sizeof(seq));
		assert(!strchr(seq, 'D') && !strchr(seq, 'F'));
		before = sim_n_iocalls;
		for (k = 0; k < 3; k++)
			ane_h15_remove(&pdev);
		assert(sim_n_iocalls == before && sim_n_dom_live == 1 && sim_dma[0].live &&
		       sim_pm_put == 0 && sim_n_iounmap == 0);
	}
}

int main(int argc, char **argv)
{
	unsigned char *expected;
	struct ane_h15 *ane;

	assert(argc == 6);
	if (!strcmp(argv[1], "t8122")) {
		target = &ane_t8122_soc;
		compatible = "apple,t8122-ane";
	} else if (!strcmp(argv[1], "t6030")) {
		target = &ane_t6030_soc;
		compatible = "apple,t6030-ane";
	} else if (!strcmp(argv[1], "t6031")) {
		target = &ane_t6031_soc;
		compatible = "apple,t6031-ane";
	} else {
		assert(!strcmp(argv[1], "t6034"));
		target = &ane_t6034_soc;
		compatible = "apple,t6034-ane";
	}
	expected = read_expected(argv[3]);
	prepare(argv[2]);
	assert(probe_now() == -ENOENT);
	assert(strstr(sim_log, "reason=unfilled-facts"));
	assert(sim_n_dma == 1 && sim_dma_mask_bits == 64 && !sim_dma[0].live);
	assert(sim_dma[0].size == 0x36c000);
	assert(!memcmp(sim_dma[0].va, expected, 0x36c000));
	assert(sim_fw_requests == 1 && sim_fw_live == 0);
	assert(sim_n_iocalls == 0);
	assert(events_in(target->engine_pa, target->engine_size, 1) == 0);

	prepare(argv[2]);
	confirm_read = false;
	refuse(-EPERM, 0);
	prepare(argv[2]);
	node.reg_pa[0] += 0x4000;
	refuse(-EINVAL, 0);
	prepare(argv[2]);
	set_ps_words(target, 2);
	refuse(-ETIMEDOUT, 0);
	prepare(argv[2]);
	eng_set(target, target->cpu_status, 0x300028);
	refuse(-EBUSY, 0);
	prepare(argv[2]);
	eng_set(target, target->cpu_status, 0x30002b);
	refuse(-EBUSY, 0);
	prepare(argv[2]);
	eng_set(target, target->cpu_control, BIT(4));
	refuse(-EBUSY, 0);
	prepare(argv[2]);
	sim_default_dom_none = 1;
	refuse(-ENODEV, 0);
	prepare(argv[2]);
	sim_default_dom.type = 0;
	refuse(-ENODEV, 0);
	sim_default_dom.type = __IOMMU_DOMAIN_DMA_API;
	prepare(argv[4]);
	refuse(-EBADMSG, 1);
	prepare(argv[5]);
	refuse(-EINVAL, 1);
	prepare("/nonexistent-h15-input");
	refuse(-ENOENT, 1);
	prepare(argv[2]);
	sim_dma_fail = 1;
	refuse(-ENOMEM, 1);

	prepare(argv[2]);
	ane = calloc(1, sizeof(*ane));
	assert(ane);
	ane->dev = &pdev.dev;
	ane->soc = target;
	ane->engine = of_iomap(&node, 0);
	ane->pmgr = of_iomap(&node, 1);
	platform_set_drvdata(&pdev, ane);
	assert(ane_h15_prepare_owned_firmware(ane) == 0);
	assert(sim_dma[0].live && sim_fw_live == 0);
	assert(!memcmp(ane->owned_fw.va, expected, 0x36c000));
	assert(ane_h15_prepare_owned_firmware(ane) == -EBUSY);
	assert(sim_n_dma == 1 && sim_fw_requests == 1);
	ane->fw_touched = true;
	ane_h15_remove(&pdev);
	assert(sim_dma[0].live && ane->owned_fw.va);
	ane->fw_touched = false;
	ane_h15_remove(&pdev);
	assert(!sim_dma[0].live && !ane->owned_fw.va);
	free(ane);

	/* wrad_patch=0 leaves the payload's own word at the wrapper field: the image equals the expected one everywhere else. */
	wrad_patch = false;
	prepare(argv[2]);
	ane = calloc(1, sizeof(*ane));
	assert(ane);
	ane->dev = &pdev.dev;
	ane->soc = target;
	ane->engine = of_iomap(&node, 0);
	ane->pmgr = of_iomap(&node, 1);
	platform_set_drvdata(&pdev, ane);
	assert(ane_h15_prepare_owned_firmware(ane) == 0);
	{
		u64 wrapper = target->engine_pa + ANE_H15_WRAP_OFF;
		unsigned int off = target->fw->wrad_vm;

		assert(!memcmp(ane->owned_fw.va, expected, off));
		assert(!memcmp((u8 *)ane->owned_fw.va + off + 8, expected + off + 8, 0x36c000 - off - 8));
		assert(memcmp((u8 *)ane->owned_fw.va + off, &wrapper, sizeof(wrapper)));
	}
	ane_h15_remove(&pdev);
	assert(!sim_dma[0].live && !ane->owned_fw.va);
	free(ane);
	wrad_patch = true;
	if (target->rvbar_expect) {
		owned_map_success(argv[2], expected, 0);
		owned_map_success(argv[2], expected, 1);
		owned_map_refusals(argv[2], argv[4]);
		owned_map_short_unmap(argv[2]);
	} else {
		owned_map_setup(argv[2]);
		owned_untouched(-ENODEV, "measured RVBAR");
	}
	free(expected);
	reset_all();
	printf("owned firmware %s: real-image preparation, 12 refusals, busy owner, guarded retention and release, run-stage page mapping PASS\n", argv[1]);
	return 0;
}
