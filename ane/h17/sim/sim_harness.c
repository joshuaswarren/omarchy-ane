// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Regression of the real module-init refusal. The historical fake firmware
 * below the shim must never be reached: it does not establish J700 behaviour.
 */
#include "shim.h"

char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
bool sim_j700 = true;
unsigned long sim_registrations, sim_maps, sim_mmio_reads, sim_pmgr_reads;
int sim_genpd_on;
int sim_genpd_disabled;
unsigned long sim_writes_ctl;
unsigned long sim_regmap_writes;
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

static int run_mode(const char *mode)
{
	int ret;
	const char *reason = "REFUSED reason=unqualified-register-map";
	int expected = -EOPNOTSUPP;

	optin = 1;
	sim_fw.cpu_status = 0xa;
	if (!strcmp(mode, "snapshot")) {
		confirm_wake = 0;
	} else if (!strcmp(mode, "wake")) {
		confirm_wake = 1;
	} else if (!strcmp(mode, "legacy-overrides")) {
		/* None of the old escape hatches may reach hardware, even with
		 * parameters that would have caused divide-by-zero or bad MMIO. */
		confirm_wake = 1;
		child_ps = 0;
		poll_ms = 0;
		mbox_off = 0xffffffff;
	} else if (!strcmp(mode, "no-optin")) {
		optin = 0;
		expected = -EPERM;
		reason = "REFUSED reason=board-optin-or-dt";
	} else if (!strcmp(mode, "wrong-board")) {
		sim_j700 = false;
		expected = -EPERM;
		reason = "REFUSED reason=board-optin-or-dt";
	} else {
		fprintf(stderr, "unknown mode %s\n", mode);
		return 2;
	}

	ret = sim_init_fn();
	chk(ret == expected);
	chk(log_contains(reason));
	chk(!log_contains("verdict=PASS"));
	chk(sim_registrations == 0);
	chk(sim_maps == 0);
	chk(sim_pmgr_reads == 0 && sim_regmap_writes == 0);
	chk(sim_mmio_reads == 0 && sim_writes_ctl == 0);
	chk(sim_genpd_on == 0);
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
