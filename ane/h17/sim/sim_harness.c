// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Regression of the real module. The gate is 1: the engine probe runs
 * against a fake bus and every MMIO access must sit inside the
 * qualified map, with the watchdog verified armed during the phase and
 * disarmed after. The rehearsal mode runs the watchdog lifecycle
 * (verify/arm/pet/disarm) against a fake WD1 block with no ANE access.
 * The refusal modes prove the gate and the board/opt-in checks.
 */
#include "shim.h"

char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
bool sim_j700 = true;
unsigned long sim_registrations, sim_maps, sim_mmio_reads, sim_mmio_writes;
unsigned long sim_pmgr_reads, sim_regmap_writes;
unsigned long sim_marker_opens, sim_marker_writes, sim_marker_closed;
unsigned long sim_wdt_armed_latch;
unsigned long sim_setup_writes;
u32 sim_child_word;
int sim_genpd_on, sim_genpd_disabled;
void __iomem *sim_engine_base = (void __iomem *)0x40000000ull;
u32 sim_wdt_regs[12];
struct device_node sim_pmgr_ctl = {
	.name = "power-management@300700000", .compatible = "apple,t8140-pmgr",
	.available = 1,
};
struct device_node sim_ane_node = {
	.name = "ane@400000000", .compatible = "apple,t8140-ane", .available = 0,
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

static int log_contains(const char *s)
{
	return strstr(sim_log, s) != NULL;
}

/* The qualified map: the T8140-sourced rows plus, only when their opt-in
 * parameter is set, the unsourced wrapper trio / mailbox peek. */
static int off_qualified(u32 off, u32 scratch0, int wrapper, int mbox, u32 mbox_base)
{
	if (off >= scratch0 && off < scratch0 + H17_SCRATCH_N * 4)
		return 1;
	if (off == H17_DOORBELL || off == H17_DOORBELL_ACK ||
	    off == H17_DOORBELL_STATE0 || off == H17_DOORBELL_STATE1 ||
	    off == H17_IPI_PENDING || off == H17_MBOX_SETUP0 ||
	    off == H17_MBOX_SETUP1 || off == H17_MBOX_SETUP2 ||
	    off == H17_MBOX_SETUP3)
		return 1;
	if (wrapper && (off == H17_CPU_CONTROL || off == H17_CPU_STATUS ||
			off == H17_RVBAR || off == H17_RVBAR + 4))
		return 1;
	if (mbox && (off == mbox_base + H17_MBOX_A2I_CTRL ||
		     off == mbox_base + H17_MBOX_I2A_CTRL ||
		     off == mbox_base + H17_MBOX_I2A_RECV0 ||
		     off == mbox_base + H17_MBOX_I2A_RECV1))
		return 1;
	return 0;
}

/* Every MMIO access the probe made must sit in the qualified map. */
static int scan_unqualified(u32 scratch0, int wrapper, int mbox, u32 mbox_base)
{
	static const char *const tags[2] = {
		"ane_h17_wake read off=", "ane_h17_wake write off="
	};
	unsigned int bad = 0, t;

	for (t = 0; t < 2; t++) {
		const char *p = sim_log;
		size_t tag = strlen(tags[t]);

		while ((p = strstr(p, tags[t])) != NULL) {
			u32 off = (u32)strtoul(p + tag, NULL, 16);

			if (!off_qualified(off, scratch0, wrapper, mbox, mbox_base)) {
				rec_log("sim: UNQUALIFIED access off=%#x\n", off);
				bad++;
			}
			p += tag;
		}
	}
	return bad;
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
	mbox_off = H17_MBOX;
	scratch0_off = H17_SCRATCH0;
	contain_rehearse = false;
	sim_registrations = sim_maps = sim_mmio_reads = sim_mmio_writes = 0;
	sim_pmgr_reads = sim_regmap_writes = sim_setup_writes = 0;
	sim_marker_opens = sim_marker_writes = sim_marker_closed = 0;
	sim_wdt_armed_latch = 0;
	sim_genpd_on = sim_genpd_disabled = 0;
	sim_child_word = 0;
}

static int run_mode(const char *mode)
{
	int ret, refused = 0, expected = 0, direct_helper = 0;
	const char *reason;

	reset_sim();
	if (!strcmp(mode, "snapshot")) {
		refused = 1;
		expected = -EOPNOTSUPP;
		reason = "REFUSED reason=unqualified-register-map";
	} else if (!strcmp(mode, "wake")) {
		confirm_wake = 1;
		reason = "verdict=PASS reason=wake-word";
	} else if (!strcmp(mode, "peek")) {
		confirm_wake = 1;
		read_wrapper = 1;
		mbox_peek = 1;
		reason = "verdict=PASS reason=wake-word";
	} else if (!strcmp(mode, "rehearse")) {
		contain_rehearse = true;
		direct_helper = 1;
		reason = "verdict=PASS reason=rehearsal-lifecycle";
	} else if (!strcmp(mode, "rehearse-init")) {
		contain_rehearse = true;
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
	} else {
		fprintf(stderr, "unknown mode %s\n", mode);
		return 2;
	}

	/* Direct helper exercise against fake registers only. Never a live
	 * init bypass: the real init refuses even with contain_rehearse. */
	ret = direct_helper ? h17_rehearse_run() : sim_init_fn();
	chk(ret == expected);
	chk(strstr(sim_log, reason) != NULL);
	if (refused) {
		chk(sim_registrations == 0 && sim_maps == 0);
		chk(sim_pmgr_reads == 0 && sim_regmap_writes == 0);
		chk(sim_mmio_reads == 0 && sim_mmio_writes == 0);
		chk(sim_marker_opens == 0);
		chk(sim_wdt_armed_latch == 0);
		chk(sim_genpd_on == 0);
	} else if (contain_rehearse) {
		/* rehearsal: the watchdog lifecycle with no ANE access. */
		chk(sim_registrations == 0 && sim_maps == 0);
		chk(sim_mmio_reads == 0 && sim_mmio_writes == 0);
		chk(sim_wdt_armed_latch == 1);
		chk(!(sim_wdt_regs[7] & BIT(2)));	/* left disarmed */
		chk(sim_wdt_regs[5] != 0);		/* BITE_TIME kept */
		chk(sim_marker_opens == 1 && sim_marker_closed == 1);
		chk(sim_marker_writes == 2);	/* armed line + ok line */
		chk(sim_genpd_on == 0 && sim_genpd_disabled == 0);
	} else {
		unsigned long accesses = sim_mmio_reads + sim_mmio_writes;

		chk(sim_registrations == 1);
		chk(sim_maps == 1);
		chk(sim_genpd_on == 0);		/* the vote is released */
		chk(sim_marker_opens == 1 && sim_marker_closed == 1);
		chk(sim_wdt_armed_latch == 1);	/* the watch was armed */
		chk(!(sim_wdt_regs[7] & BIT(2)));	/* and left disarmed */
		/* one pending-marker line per access plus the done line */
		chk(sim_marker_writes == accesses + 1);
		chk(scan_unqualified(scratch0_off, read_wrapper,
				     mbox_peek, mbox_off) == 0);
		if (confirm_wake) {
			chk(sim_setup_writes == 5);	/* SETUP1 pulses 0 then 1 */
			chk(log_contains("wake_seen=1"));
		} else {
			chk(!log_contains("ane_h17_wake write off="));
			chk(sim_setup_writes == 0);
		}
		if (read_wrapper)
			chk(log_contains("read off=0x1600048 CPU_STATUS"));
		if (mbox_peek)
			chk(log_contains("mailbox peek done mbox_off=0x1608000 msgs=1"));
	}
	if (failures)
		fprintf(stderr, "---- %s log ----\ncounters: reg=%lu maps=%lu mmio_r=%lu mmio_w=%lu m_open=%lu m_w=%lu m_c=%lu latch=%lu setup=%lu genpd=%d disabled=%d\n%s",
			mode, sim_registrations, sim_maps, sim_mmio_reads,
			sim_mmio_writes, sim_marker_opens, sim_marker_writes,
			sim_marker_closed, sim_wdt_armed_latch,
			sim_setup_writes, sim_genpd_on, sim_genpd_disabled,
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
