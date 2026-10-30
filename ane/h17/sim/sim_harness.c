// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sim/sim_harness.c: runs the real ane_h17_wake.c against the fake bus,
 * genpd and firmware model of sim/shim.h. One mode per run:
 *
 *   pos-snapshot    read-only run (confirm_wake=0): zero engine writes,
 *                   domain released, PASS reason=asc-snapshot
 *   pos-wake        confirm_wake=1 against a responsive model: exactly two
 *                   CPU_CONTROL writes (RUN, clear), wake word seen, one
 *                   mailbox message read, clean stop, domain released
 *   neg-not-stopped the core is already RUNNING: the RUN write must be
 *                   refused (asc-not-stopped), no writes, domain released
 *   neg-core-stuck  the model ignores the RUN clear: FAIL core-not-stopped,
 *                   the power vote stays held, module stays
 *   neg-bad-optin   no optin=1: REFUSED before any read or write
 *
 * A green run proves the harness logic only, never T8140 behaviour.
 */
#include "shim.h"

char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
int sim_genpd_on;
int sim_genpd_disabled;
unsigned long sim_writes_ctl;
unsigned long sim_regmap_writes;
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

static void scenario_reset(int stuck, u32 status0)
{
	sim_log_reset();
	memset(&sim_fw, 0, sizeof(sim_fw));
	sim_fw.cpu_status = status0;
	sim_fw.i2a = 0x1; /* ENABLE, the shape the T6021 mailbox showed */
	sim_fw.stuck = stuck;
	sim_genpd_on = 0;
	sim_genpd_disabled = 1;
	sim_writes_ctl = 0;
	sim_regmap_writes = 0;
	optin = 1;
	confirm_wake = 0;
	poll_ms = 10;
	wake_wait_ms = 500;
	mbox_wait_ms = 500;
	stop_wait_ms = 500;
}

static int run_mode(const char *mode)
{
	int ret;

	failures = 0;

	if (strcmp(mode, "pos-snapshot") == 0) {
		scenario_reset(0, 0xa);
		ret = sim_init_fn();
		chk(ret == 0);
		chk(log_contains("verdict=PASS reason=asc-snapshot"));
		chk(sim_writes_ctl == 0);
		chk(sim_regmap_writes == 0);
		chk(sim_genpd_on == 0);
	} else if (strcmp(mode, "pos-wake") == 0) {
		scenario_reset(0, 0xa);
		confirm_wake = 1;
		ret = sim_init_fn();
		chk(ret == 0);
		chk(log_contains("verdict=PASS reason=wake-word-hello msgs=1"));
		chk(sim_writes_ctl == 2);
		chk(sim_genpd_on == 0);
		chk(log_contains("I2A_RECV0"));
	} else if (strcmp(mode, "neg-not-stopped") == 0) {
		scenario_reset(0, 0x8);
		confirm_wake = 1;
		ret = sim_init_fn();
		chk(ret == -EIO);
		chk(log_contains("reason=asc-not-stopped"));
		chk(sim_writes_ctl == 0);
		chk(sim_genpd_on == 0);
	} else if (strcmp(mode, "neg-core-stuck") == 0) {
		scenario_reset(1, 0xa);
		confirm_wake = 1;
		ret = sim_init_fn();
		chk(ret == 0);
		chk(log_contains("reason=core-not-stopped"));
		chk(sim_genpd_on == 1);
		chk(sim_writes_ctl == 2);
	} else if (strcmp(mode, "neg-bad-optin") == 0) {
		scenario_reset(0, 0xa);
		optin = 0;
		ret = sim_init_fn();
		chk(ret != 0);
		chk(log_contains("REFUSED reason=board-optin-or-dt"));
		chk(sim_writes_ctl == 0);
		chk(!log_contains("read off="));
	} else {
		fprintf(stderr, "unknown mode %s\n", mode);
		return 2;
	}

	printf("%s: %s (%d check failures)\n", mode,
	       failures ? "FAIL" : "ok", failures);
	if (failures && getenv("SIM_DEBUG"))
		fputs(sim_log, stderr);
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
