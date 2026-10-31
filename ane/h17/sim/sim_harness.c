// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Regression of the real module init. The gate is 0, so the engine
 * probe must never run here: snapshot/no-optin/wrong-board assert the
 * refusal and zero hardware interaction. The rehearsal mode exercises
 * the containment-lifecycle failure branch (watchdog verified, armed,
 * timer fired-or-not, marker records, disarm verified) against a fake
 * WD1 register block; the gate-removal mutant is rejected because it
 * would run the probe instead of refusing.
 */
#include "shim.h"

char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
bool sim_j700 = true;
unsigned long sim_registrations, sim_maps, sim_mmio_reads, sim_mmio_writes;
unsigned long sim_pmgr_reads, sim_regmap_writes;
unsigned long sim_marker_opens, sim_marker_writes, sim_marker_closed;
unsigned long sim_timer_armed, sim_timer_disarmed, sim_timer_mods;
int sim_genpd_on, sim_genpd_disabled;
int sim_panic_count;
int sim_processor_id;
jmp_buf sim_panic_jb;
int (*sim_kthread_fn)(void *);
void *sim_kthread_data;
u32 sim_child_word;
void __iomem *sim_engine_base = (void __iomem *)0x40000000ull;
u32 sim_wdt_regs[12];
struct device_node sim_ane_node = {
	.name = "ane@400000000", .compatible = "apple,t8140-ane", .available = 0,
};
struct device_node sim_pmgr_ctl = {
	.name = "power-management@300700000", .compatible = "apple,t8140-pmgr",
	.available = 1,
};
struct device_node sim_ps_node = {
	.name = "power-controller@290",
	.compatible = "apple,t8140-pmgr-pwrstate", .available = 1,
	.parent = &sim_pmgr_ctl,
};
struct device_node sim_probe_node = {
	.name = "ane-h17-ps-probe",
	.compatible = "omarchy,t8140-ane-ps-probe", .available = 1,
};
struct device_node sim_dart_nodes[3] = {
	{ .name = "iommu@401800000", .compatible = "apple,t8140-dart", .available = 0 },
	{ .name = "iommu@401820000", .compatible = "apple,t8140-dart", .available = 0 },
	{ .name = "iommu@401840000", .compatible = "apple,t8140-dart", .available = 0 },
};
struct device_node sim_wdt_node = {
	.name = "watchdog@3082b0000", .compatible = "apple,t8140-wdt", .available = 1,
};

#include "../ane_h17_wake.c"

static int failures;

static void chk(int cond)
{
	if (!cond) {
		failures++;
		rec_log("CHK FAILED\n");
	}
}

static void reset_sim(void)
{
	memset(&sim_ane_node, 0, sizeof(sim_ane_node));
	sim_ane_node.name = "ane@400000000";
	sim_ane_node.compatible = "apple,t8140-ane";
	sim_ane_node.available = 0;
	memset(sim_wdt_regs, 0, sizeof(sim_wdt_regs));
	sim_wdt_regs[5] = 0x03000000;	/* BITE_TIME as apple_wdt left it */
	optin = true;
	confirm_wake = false;
	read_wrapper = false;
	mbox_peek = false;
	child_ps = true;
	poll_ms = 10;
	contain_ms = 1000;
	contain_rehearse = false;
	sim_registrations = sim_maps = sim_mmio_reads = sim_mmio_writes = 0;
	sim_pmgr_reads = sim_regmap_writes = 0;
	sim_marker_opens = sim_marker_writes = sim_marker_closed = 0;
	sim_timer_armed = sim_timer_disarmed = sim_timer_mods = 0;
	sim_genpd_on = sim_genpd_disabled = 0;
	sim_panic_count = 0;
	sim_child_word = 0;
	sim_processor_id = 0;
	sim_kthread_fn = NULL;
	sim_kthread_data = NULL;
}

static int run_mode(const char *mode)
{
	int ret, expected, refused = 0;
	const char *reason;

	reset_sim();
	if (!strcmp(mode, "snapshot")) {
		refused = 1;
		expected = -EOPNOTSUPP;
		reason = "REFUSED reason=unqualified-register-map";
	} else if (!strcmp(mode, "no-optin")) {
		optin = false;
		refused = 1;
		expected = -EPERM;
		reason = "REFUSED reason=board-optin-or-dt";
	} else if (!strcmp(mode, "wrong-board")) {
		sim_j700 = false;
		refused = 1;
		expected = -EPERM;
		reason = "REFUSED reason=board-optin-or-dt";
	} else if (!strcmp(mode, "rehearse")) {
		contain_rehearse = 2;	/* lifecycle: no panic */
		expected = 0;
		reason = "verdict=PASS reason=rehearsal-lifecycle";
	} else if (!strcmp(mode, "rehearse-fire")) {
		contain_rehearse = 1;	/* fire: the timer never fires in the
					 * sim, so the no-fire branch runs */
		expected = -EIO;
		reason = "verdict=FAIL reason=rehearsal-no-fire";
	} else {
		fprintf(stderr, "unknown mode %s\n", mode);
		return 2;
	}

	ret = sim_init_fn();
	chk(ret == expected);
	chk(strstr(sim_log, reason) != NULL);
	if (refused) {
		chk(sim_registrations == 0 && sim_maps == 0);
		chk(sim_pmgr_reads == 0 && sim_regmap_writes == 0);
		chk(sim_mmio_reads == 0 && sim_mmio_writes == 0);
		chk(sim_marker_opens == 0);
		chk(sim_timer_armed == 0 && sim_timer_disarmed == 0);
		chk(sim_genpd_on == 0);
	} else {
		/* rehearsal: the watch verified, armed once, disarmed once,
		 * two marker lines, no engine MMIO, genpd untouched, WD1
		 * left disarmed. */
		chk(sim_registrations == 0 && sim_maps == 0);
		chk(sim_mmio_reads == 0 && sim_mmio_writes == 0);
		chk(!(sim_wdt_regs[7] & BIT(2)));	/* WD1 CTRL: RESET_EN clear */
		chk(sim_wdt_regs[5] != 0);		/* BITE_TIME kept */
		chk(sim_marker_opens == 1 && sim_marker_closed == 1);
		chk(sim_marker_writes == 2);	/* armed line + final line */
		chk(sim_timer_armed == 1 && sim_timer_disarmed == 1);
		chk(sim_genpd_on == 0 && sim_genpd_disabled == 0);
	}
	if (failures)
		fprintf(stderr, "---- %s log ----\ncounters: reg=%lu maps=%lu mmio_r=%lu mmio_w=%lu pmgr=%lu rmap_w=%lu m_open=%lu m_w=%lu m_c=%lu t_a=%lu t_d=%lu t_m=%lu genpd=%d disabled=%d panic=%d\n%s",
			mode, sim_registrations, sim_maps, sim_mmio_reads,
			sim_mmio_writes, sim_pmgr_reads, sim_regmap_writes,
			sim_marker_opens, sim_marker_writes, sim_marker_closed,
			sim_timer_armed, sim_timer_disarmed, sim_timer_mods,
			sim_genpd_on, sim_genpd_disabled, sim_panic_count,
			sim_log);
	printf("%s: %s (%d check failures)\n", mode,
	       failures ? "FAIL" : "ok", failures);
	return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: sim_h17_wake <mode>\n");
		return 2;
	}
	return run_mode(argv[1]);
}
