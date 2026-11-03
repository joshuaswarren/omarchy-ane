// SPDX-License-Identifier: GPL-2.0-only OR MIT
#define main existing_sim_main
#include "sim/sim_harness.c"
#undef main

static struct ane_h15 setup_startup(const struct ane_h15_soc *s, const char *compat)
{
	rt_setup(s, compat);
	return (struct ane_h15) {
		.dev = &pdev.dev,
		.soc = s,
		.engine = sim_bus_map(s->engine_pa, s->engine_size),
		.pmgr = sim_bus_map(s->pmgr_pa, s->pmgr_size),
	};
}

static void full_after_first_app(u64 pa, u64 value)
{
	rt_write64(pa, value);
	if (pa == dm_soc->engine_pa + dm_soc->mbox + 0x808 && n_sent == 1)
		eng_set(dm_soc, dm_soc->mbox + 0x110, 0x10000);
}

static void startup_cases(const struct ane_h15_soc *s, const char *compat)
{
	struct ane_h15 ane = setup_startup(s, compat);
	struct ane_h15_rtk k = { .epmap_last = true };
	const u64 on = RT(0xb) | 0x20;

	k.eps[1] = 3;
	ane_h15_rtk_handle(&ane, &k, on, 0);
	chk(k.ap_on && k.app_started && n_sent == 2 &&
	    sent0[0] == (RT(5) | (32ULL << 32) | 2) &&
	    sent0[1] == (RT(5) | (33ULL << 32) | 2),
	    "all advertised app starts sent: startup completes");
	ane_h15_rtk_handle(&ane, &k, on, 0);
	chk(n_sent == 2, "duplicate AP acknowledgement does not resend app starts");

	ane = setup_startup(s, compat);
	k = (struct ane_h15_rtk) { .epmap_last = true, .sent = 15 };
	k.eps[1] = 3;
	ane_h15_rtk_handle(&ane, &k, on, 0);
	chk(k.ap_on && !k.app_started && n_sent == 1 && k.sent == 16,
	    "send cap after one endpoint leaves startup incomplete");

	ane = setup_startup(s, compat);
	k = (struct ane_h15_rtk) { .epmap_last = true };
	k.eps[1] = 3;
	sim_write64_hook = full_after_first_app;
	ane_h15_rtk_handle(&ane, &k, on, 0);
	chk(k.ap_on && !k.app_started && n_sent == 1,
	    "mailbox FULL after one endpoint leaves startup incomplete");
	eng_set(s, s->mbox + 0x110, DM_EMPTY_WORD);
	ane_h15_rtk_handle(&ane, &k, on, 0);
	chk(!k.app_started && n_sent == 1,
	    "later AP acknowledgement does not retry a partial startup");

	ane = setup_startup(s, compat);
	k = (struct ane_h15_rtk) { 0 };
	k.eps[1] = 3;
	ane_h15_rtk_handle(&ane, &k, on, 0);
	chk(k.ap_on && !k.app_started && n_sent == 0,
	    "incomplete endpoint map cannot complete startup");

	ane = setup_startup(s, compat);
	k = (struct ane_h15_rtk) { .hello = true, .epmap_last = true, .ap_on = true };
	ane.chan = true;
	rtkit_wait_ms = 20;
	ane_h15_rtk_session(&ane, &k);
	chk(!k.hs_done && sim_n_dma == 0 &&
	    events_in(s->engine_pa + SCR_OFF, 0x20, 0) == 0,
	    "incomplete startup cannot enter the channel handshake");
	chk(k.shutdown && k.ap_acked && k.iop_acked && n_sent == 2 &&
	    sent0[0] == (RT(0xb) | 0x10) && sent0[1] == (RT(6) | 1),
	    "incomplete startup still performs bounded ordinary shutdown");
}

int main(void)
{
	startup_cases(&ane_t6030_soc, "apple,t6030-ane");
	startup_cases(&ane_t6031_soc, "apple,t6031-ane");
	printf("RTKIT-STARTUP: %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
