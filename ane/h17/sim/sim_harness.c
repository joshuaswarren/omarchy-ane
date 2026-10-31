// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Regression of the real module. With the register map qualified the
 * default build runs the probe against the fake bus; every MMIO access
 * it makes must sit inside the qualified map (scan_unqualified), the
 * containment watch must be armed before the first engine access and
 * disarmed after the last (marker/timer counters), and the hang mode
 * exercises the real panic path through the shim's longjmp. */
#include "shim.h"

char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
bool sim_j700 = true;
unsigned long sim_registrations, sim_maps, sim_mmio_reads;
unsigned long sim_mmio_writes, sim_pmgr_reads;
int sim_genpd_on;
int sim_genpd_disabled;
unsigned long sim_writes_ctl;
unsigned long sim_regmap_writes;
unsigned long sim_setup_writes;
unsigned long sim_marker_opens, sim_marker_writes, sim_marker_closed;
unsigned long sim_timer_armed, sim_timer_disarmed, sim_timer_mods;
int sim_panic_count;
jmp_buf sim_panic_jb;
u32 sim_child_word = 0x00;	/* pmgr pwrstate: TARGET 3:0, ACTUAL 7:4 */
void __iomem *sim_engine_base = (void __iomem *)0x40000000ull;
struct sim_fw sim_fw;

struct device_node sim_ane_node = {
	.name = "ane@400000000",
	.compatible = "apple,t8140-ane",
	.available = 0,
};
struct device_node sim_pmgr_ctl = {
	.name = "power-management@300700000",
	.compatible = "apple,t8140-pmgr",
	.available = 1,
};
struct device_node sim_ps_node = {
	.name = "power-controller@290",
	.compatible = "apple,t8140-pmgr-pwrstate",
	.available = 1,
	.phandle = 160,
	.parent = &sim_pmgr_ctl,
};
struct device_node sim_probe_node = {
	.name = "ane-h17-ps-probe",
	.compatible = "omarchy,t8140-ane-ps-probe",
	.available = 1,
};
struct device_node sim_dart_nodes[3] = {
	{ .name = "iommu@401800000", .compatible = "apple,t8140-dart", .available = 0 },
	{ .name = "iommu@401820000", .compatible = "apple,t8140-dart", .available = 0 },
	{ .name = "iommu@401840000", .compatible = "apple,t8140-dart", .available = 0 },
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

void sim_hang_hook(void)
{
	h17_contain_fn(NULL);
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
	memset(&sim_fw, 0, sizeof(sim_fw));
	sim_fw.cpu_status = 0xa;
	sim_fw.doorbell_state0 = 1;	/* HAL shape: state&3 = 1, ring = 2 */
	optin = 1;
	confirm_wake = 0;
	read_wrapper = 0;
	mbox_peek = 0;
	child_ps = 1;
	poll_ms = 10;
	mbox_off = H17_MBOX;
	scratch0_off = H17_SCRATCH0;
	contain_ms = 1000;
	sim_registrations = sim_maps = sim_mmio_reads = sim_mmio_writes = 0;
	sim_pmgr_reads = 0;
	sim_writes_ctl = sim_regmap_writes = sim_setup_writes = 0;
	sim_marker_opens = sim_marker_writes = sim_marker_closed = 0;
	sim_timer_armed = sim_timer_disarmed = sim_timer_mods = 0;
	sim_genpd_on = sim_genpd_disabled = 0;
	sim_panic_count = 0;
}

static int run_mode(const char *mode)
{
	int volatile ret = 0;
	int expected = 0, refused = 0;
	const char *reason;

	reset_sim();

	if (!strcmp(mode, "snapshot")) {
		reason = "verdict=PASS reason=asc-snapshot";
	} else if (!strcmp(mode, "wake")) {
		confirm_wake = 1;
		reason = "verdict=PASS reason=wake-word";
	} else if (!strcmp(mode, "peek")) {
		confirm_wake = 1;
		read_wrapper = 1;
		mbox_peek = 1;
		reason = "verdict=PASS reason=wake-word";
	} else if (!strcmp(mode, "hang")) {
		sim_fw.hang_off = H17_SCRATCH0;
		reason = "HANG contain_ms=";
	} else if (!strcmp(mode, "legacy-overrides")) {
		/* The legacy parameter shape still produces a contained run:
		 * no child raise, degenerate poll interval, far mailbox. */
		confirm_wake = 1;
		child_ps = 0;
		poll_ms = 0;
		mbox_off = 0xffffffff;
		reason = "verdict=PASS reason=wake-word";
	} else if (!strcmp(mode, "no-optin")) {
		optin = 0;
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

	if (!refused)
		ret = setjmp(sim_panic_jb);
	if (refused || ret == 0)
		ret = sim_init_fn();

	chk(refused ? ret == expected : sim_panic_count || ret == expected);
	chk(log_contains(reason));

	if (refused) {
		chk(!log_contains("verdict=PASS"));
		chk(sim_registrations == 0);
		chk(sim_maps == 0);
		chk(sim_pmgr_reads == 0 && sim_regmap_writes == 0);
		chk(sim_mmio_reads == 0 && sim_writes_ctl == 0);
	} else if (sim_panic_count) {
		/* The hang mode: the panic names the pending access and the
		 * probe never reached its cleanup, so the vote is held. */
		chk(sim_panic_count == 1);
		chk(sim_marker_opens == 1 && sim_marker_closed == 0);
		chk(sim_timer_armed == 1 && sim_timer_disarmed == 0);
		chk(sim_genpd_on == 1);
	} else {
		unsigned long accesses = sim_mmio_reads + sim_mmio_writes;

		chk(sim_registrations == 1);
		chk(sim_maps == 1);
		chk(sim_genpd_on == 0);		/* the vote is released */
		chk(sim_marker_opens == 1 && sim_marker_closed == 1);
		chk(sim_timer_armed == 1 && sim_timer_disarmed == 1);
		/* one pending-marker line per access plus the done line, and
		 * one deadline push per access */
		chk(sim_marker_writes == accesses + 1);
		chk(sim_timer_mods == accesses);
		chk(sim_writes_ctl == 0);	/* the RUN path is retired */
		chk(scan_unqualified(scratch0_off, read_wrapper, mbox_peek,
				     mbox_off) == 0);
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
	chk(sim_genpd_on == (sim_panic_count ? 1 : 0));
	if (failures)
		fprintf(stderr, "---- %s log ----\n%s", mode, sim_log);
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
