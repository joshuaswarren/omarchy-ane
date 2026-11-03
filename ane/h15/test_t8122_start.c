// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include <assert.h>
#define main existing_sim_main
#include "sim/sim_harness.c"
#undef main

#define T8122_COMPAT "apple,t8122-ane"

static void result(unsigned int want_stage, const char *want_verdict, const char *want_reason)
{
	unsigned int stage_number;
	char verdict[32], reason[128];

	assert(parse_result(&stage_number, verdict, sizeof(verdict), reason, sizeof(reason)));
	assert(stage_number == want_stage && !strcmp(verdict, want_verdict) && !strcmp(reason, want_reason));
}

static void stage5_runs(void)
{
	const struct ane_h15_soc *s = &ane_t8122_soc;
	u32 writes[8];
	int first;

	run_setup(s, T8122_COMPAT, 0);
	assert(probe_now() == 0);
	result(5, "PASS", "core-started-and-stopped");
	assert(run_writes(s, writes, 8, &first) == 2 && writes[0] == 0x10 && writes[1] == 0);
	assert(events_in(0, ~0ULL, 1) == 2 && sim_bus_faults == 0);
}

static void stage6_session(void)
{
	const struct ane_h15_soc *s = &ane_t8122_soc;
	const u64 want[] = {
		RT(2) | (12ULL << 16) | 12,
		RT(8) | 1,
		RT(8) | (1ULL << 32) | (1ULL << 51),
		RT(5) | (1ULL << 32) | 2,
		RT(0xb) | 0x20,
		RT(5) | (0x20ULL << 32) | 2,
		RT(0xb) | 0x10,
		RT(6) | 0x01,
	};
	int i;

	rt_setup(s, T8122_COMPAT);
	rtkit_stop = 1;
	assert(probe_now() == 0);
	result(6, "PASS", "rtkit-session-and-shutdown");
	assert(n_sent == 8);
	for (i = 0; i < 8; i++)
		assert(sent0[i] == want[i] && sent1[i] == 0);
	assert(eng_get(s, s->cpu_control) == 0 && (eng_get(s, s->cpu_status) & DM_STOPPED));
	assert(sim_bus_faults == 0 && sim_module_gets == 0 && sim_pm_get == sim_pm_put);
}

static void refuses_before_writing(void)
{
	const struct ane_h15_soc *s = &ane_t8122_soc;

	run_setup(s, T8122_COMPAT, 0);
	eng_set(s, s->rvbar, 2);
	assert(probe_now() == -EBUSY);
	result(5, "FAIL", "preflight-not-as-iboot-left-it");
	assert(events_in(0, ~0ULL, 1) == 0);

	run_setup(s, T8122_COMPAT, 0);
	dart_set(s, false);
	assert(probe_now() == -ENODEV);
	result(5, "FAIL", "no-dart-stream-enabled");
	assert(events_in(0, ~0ULL, 1) == 0);

	run_setup(s, T8122_COMPAT, 0);
	eng_set(s, s->cpu_status, 0x300028);
	assert(probe_now() == -EBUSY);
	result(5, "FAIL", "preflight-not-as-iboot-left-it");
	assert(events_in(0, ~0ULL, 1) == 0);
}

static void stuck_core_is_pinned(void)
{
	run_setup(&ane_t8122_soc, T8122_COMPAT, 0);
	dm_stuck = true;
	assert(probe_now() == 0);
	result(5, "FAIL", "core-not-stopped");
	assert(sim_module_gets == 1 && sim_pm_put == 0);
}

int main(void)
{
	stage5_runs();
	stage6_session();
	refuses_before_writing();
	stuck_core_is_pinned();
	reset_all();
	puts("t8122 start: stage 5 and 6 run on the measured entry; preflight refusals write nothing; a stuck core stays pinned");
	return 0;
}
