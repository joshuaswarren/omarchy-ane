#include <assert.h>
#define main existing_sim_main
#include "sim/sim_harness.c"
#undef main

int main(void)
{
	struct iommu_domain d = { .type = __IOMMU_DOMAIN_DMA_API };
	const u64 base = ane_t6031_soc.rvbar_expect & ~1ULL;
	unsigned int i;

	d.geometry.aperture_start = 10;
	d.geometry.aperture_end = 20;
	d.geometry.force_aperture = true;
	assert(ane_h15_classify_window(&d, 10, 11) == ANE_WINDOW_INSIDE);
	assert(ane_h15_classify_window(&d, 20, 1) == ANE_WINDOW_INSIDE);
	assert(ane_h15_classify_window(&d, 9, 1) == ANE_WINDOW_OUTSIDE);
	assert(ane_h15_classify_window(&d, 20, 2) == ANE_WINDOW_OUTSIDE);
	assert(ane_h15_classify_window(&d, 10, 0) == ANE_WINDOW_OUTSIDE);
	d.geometry.aperture_start = 21;
	assert(ane_h15_classify_window(&d, 21, 1) == ANE_WINDOW_OUTSIDE);
	d.geometry.aperture_start = 0;
	d.geometry.aperture_end = ~0ULL;
	assert(ane_h15_classify_window(&d, ~0ULL, 1) == ANE_WINDOW_INSIDE);
	assert(ane_h15_classify_window(&d, ~0ULL, 2) == ANE_WINDOW_OUTSIDE);
	d.geometry.force_aperture = false;
	assert(ane_h15_classify_window(&d, 10, 1) == ANE_WINDOW_UNBOUNDED);
	assert(ane_h15_classify_window(NULL, 10, 1) == ANE_WINDOW_MISSING);

	for (i = 0; i < 4; i++) {
		int ret;

		translation_setup(&ane_t6031_soc, "apple,t6031-ane");
		stage = "domain-window";
		sim_window_queries = 0;
		sim_default_dom_none = i == 1;
		sim_default_dom.geometry.aperture_start = base;
		sim_default_dom.geometry.aperture_end = i == 3 ? base + 0xb0000 - 1 : base + 0x4ec000 - 1;
		sim_default_dom.geometry.force_aperture = i != 2;
		ret = probe_now();
		assert((ret == 0) == (i == 0));
		assert(strstr(sim_log, i == 0 ? "reason=domain-window-inside" :
			i == 1 ? "reason=domain-window-missing" :
			i == 2 ? "reason=domain-window-unbounded" : "reason=domain-window-outside"));
		assert(sim_window_queries == 0);
		assert(sim_n_iocalls == 0);
		assert(events_in(0, ~0ULL, 1) == 0);
	}
	translation_setup(&ane_t6031_soc, "apple,t6031-ane");
	stage = "domain-window";
	fw_seg = NULL;
	assert(probe_now() != 0);
	assert(strstr(sim_log, "fw_seg missing or unsafe"));
	puts("domain-window: 10 boundary assertions and 5 real-probe controls PASS");
	return 0;
}
