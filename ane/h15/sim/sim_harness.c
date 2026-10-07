// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* sim/sim_harness.c — drives the REAL ane_h15 module source through
 * every stage it implements, against the userspace shim, with positive
 * and negative controls (PMP vehicle harness pattern).
 *
 * Modes (argv[1]):
 *   pos                every positive control; exit 0 iff all pass
 *   neg-ps-stuck       ANE_CPU word stuck at 0x30; exit 0 iff the module
 *                      refuses (FAIL verdict, no engine touch, no PASS)
 *   neg-wrong-stage    unknown stage string + wrong optin; exit 0 iff
 *                      both refuse with NO RESULT line and no MMIO
 *   neg-truncated-dt   reg property missing/truncated; exit 0 iff
 *                      of_iomap NULL refusal, no invented defaults
 * The sim-negative-nopsguard make target rebuilds this harness against
 * a copy of the source with the PS guard call deleted and REQUIRES the
 * neg-ps-stuck run to fail (harness discriminates the guardless build).
 *
 * A sim is not silicon.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"

/* the module's real sources, unmodified */
#include "../ane_h15_soc.c"
#include "../ane_h15_main.c"

/* ---- shim storage ---- */
struct sim_region sim_bus[SIM_MAX_REGIONS];
int sim_n_regions;
struct sim_event sim_events[SIM_MAX_EVENTS];
int sim_n_events;
int sim_bus_faults;
int sim_n_iomap;
int sim_n_iounmap;
char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
unsigned long sim_jiffies;
int sim_pm_get;
int sim_pm_put;
int sim_pm_get_fail;

/* ---- helpers ---- */
static int fails;

static void chk(int ok, const char *name)
{
	printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok)
		fails++;
}

static int log_count(const char *needle)
{
	const char *p = sim_log;
	int n = 0;

	while ((p = strstr(p, needle))) {
		n++;
		p++;
	}
	return n;
}

/* Parse the one RESULT line: ane_h15 RESULT stage=%u soc=%s verdict=%s
 * reason=%s (reason runs to end of line). Returns false if absent.
 */
static bool parse_result(unsigned int *stage, char *verdict, size_t vsz,
			 char *reason, size_t rsz)
{
	const char *p = strstr(sim_log, "ane_h15 RESULT stage=");
	const char *v, *r, *e;
	char soc[32];

	if (!p)
		return false;
	if (sscanf(p, "ane_h15 RESULT stage=%u soc=%31s", stage, soc) != 2)
		return false;
	v = strstr(p, "verdict=");
	r = v ? strstr(v, " reason=") : NULL;
	if (!v || !r)
		return false;
	v += 8;				/* past "verdict=" */
	e = v;
	while (*e && *e != ' ' && *e != '\n')
		e++;
	snprintf(verdict, vsz, "%.*s", (int)(e - v), v);
	snprintf(reason, rsz, "%s", r + 8);
	{
		char *nl = strchr(reason, '\n');

		if (nl)
			*nl = '\0';
	}
	return true;
}

/* Collect `ane_h15 hole=Hx` ids in emission order. */
static int collect_holes(char ids[][8], int max)
{
	const char *p = sim_log;
	int n = 0;

	while (n < max && (p = strstr(p, "ane_h15 hole="))) {
		if (sscanf(p, "ane_h15 hole=%7s", ids[n]) != 1)
			break;
		n++;
		p++;
	}
	return n;
}

static int events_in(u64 base, u64 size, int writes_only)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].pa >= base && sim_events[i].pa < base + size &&
		    (!writes_only || sim_events[i].write))
			n++;
	return n;
}

/* Global invariants for every run: the h15 module this build ships is
 * read-only and refuses, so ANY bus write, ANY engine-window access,
 * or ANY fault (unmapped access) is a harness failure.
 */
static void chk_global_invariants(const struct ane_h15_soc *s,
				  const char *tag)
{
	char name[96];

	snprintf(name, sizeof(name), "%s: zero bus writes", tag);
	chk(events_in(0, ~0ULL, 1) == 0, name);
	snprintf(name, sizeof(name), "%s: zero engine-window events", tag);
	chk(s && events_in(s->engine_pa, s->engine_size, 0) == 0, name);
	snprintf(name, sizeof(name), "%s: zero unmapped-access faults", tag);
	chk(sim_bus_faults == 0, name);
}

/* ---- per-test scaffolding ---- */
static struct platform_device pdev;
static struct device_node node;
static const struct ane_h15_soc *cur_soc;

static void reset_all(void)
{
	sim_n_regions = 0;
	sim_n_events = 0;
	sim_bus_faults = 0;
	sim_n_iomap = 0;
	sim_n_iounmap = 0;
	sim_log_reset();
	sim_jiffies = 0;
	sim_pm_get = 0;
	sim_pm_put = 0;
	sim_pm_get_fail = 0;
	memset(&pdev, 0, sizeof(pdev));
	memset(&node, 0, sizeof(node));
	optin = NULL;
	stage = "status";
	ps_wait_ms = 500;
	hello_wait_ms = 0;
	confirm_boot = false;
	fw_path = NULL;
}

/* Build the fake DT node from the REAL soc row and resolve the soc row
 * through the REAL of_match_table, exactly as the OF core would.
 */
static void build_node(const struct ane_h15_soc *s, int nregs,
		       const char *compat)
{
	const struct of_device_id *m;

	for (m = ane_h15_of_match; m->compatible; m++)
		if (!strcmp(m->compatible, compat))
			node.match_data = m->data;
	node.compatible = compat;
	node.has_ane_type = true;
	node.ane_type = s->ane_type;
	node.n_reg = nregs;
	node.reg_pa[0] = s->engine_pa;
	node.reg_size[0] = s->engine_size;
	node.reg_pa[1] = s->pmgr_pa;
	node.reg_size[1] = s->pmgr_size;
	pdev.dev.of_node = &node;
	pdev.dev.name = s->name;
	cur_soc = s;
}

/* Pre-map the pmgr window and set the five ps words (0xffffffff = on);
 * stuck_idx <= 4 pins that word at 0x30 (TARGET 0x0, ACTUAL 0x3).
 */
static void set_ps_words(const struct ane_h15_soc *s, int stuck_idx)
{
	unsigned int i;

	sim_bus_map(s->pmgr_pa, s->pmgr_size);
	for (i = 0; i < 5; i++)
		sim_bus[sim_region_find(s->pmgr_pa) - sim_bus].mem[s->ps_off[i] >> 2] =
			((int)i == stuck_idx) ? 0x30u : 0xffffffffu;
}

static int probe_now(void)
{
	return ane_h15_probe(&pdev);
}

/* T6 test emitter: collect fact ids via the module's own helper. */
static char t6_ids[8][8];
static int t6_n;
static void t6_emit(void *ctx, const struct ane_h15_fact *f)
{
	(void)ctx;
	snprintf(t6_ids[t6_n], sizeof(t6_ids[t6_n]), "%s", f->id);
	t6_n++;
}

/* ---- positive controls ---- */
static void pos_dt_zero_mmio(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	reset_all();
	optin = "t8122";
	stage = "dt";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	chk(ret == 0, "P1 dt: probe returns 0");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 0 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "dt-parse-only"),
	    "P1 dt: RESULT stage=0 PASS dt-parse-only");
	chk(sim_n_events == 0, "P1 dt: ZERO bus events (hardware-silent)");
	chk(sim_n_regions == 0, "P1 dt: nothing even mapped");
	chk_global_invariants(cur_soc, "P1 dt");
}

static void pos_ps_pass(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, -1);
	ret = probe_now();
	chk(ret == 0, "P2 ps: probe returns 0");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 1 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "ps-guard+reads"),
	    "P2 ps: RESULT stage=1 PASS ps-guard+reads");
	chk(log_count("actual=0xf pass=true") == 5,
	    "P2 ps: five word lines with actual=0xf");
	chk(sim_pm_get == 1 && sim_pm_put == 1,
	    "P2 ps: genpd claim released (get==put==1)");
	chk_global_invariants(cur_soc, "P2 ps");

	/* remove() must release the windows the successful probe kept */
	ane_h15_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P2 ps: remove unmaps both windows");
}

static void pos_refusals(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	char ids[8][8];
	int nids, ret;

	/* stage=2 wrapper: REFUSED before any engine read */
	reset_all();
	optin = "t8122";
	stage = "wrapper";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	chk(ret == -EPERM, "P3 wrapper: probe returns -EPERM");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 2 && !strcmp(verdict, "REFUSED") && reason[0],
	    "P3 wrapper: RESULT stage=2 REFUSED <reason>");
	chk(sim_n_events == 0,
	    "P3 wrapper: refusal precedes any MMIO access");
	chk(sim_pm_get == 1 && sim_pm_put == 1, "P3 wrapper: power released");
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P3 wrapper: refused probe unmaps its windows");
	chk_global_invariants(cur_soc, "P3 wrapper");

	/* stage=3 boot: REFUSED, enumerates exactly the unfilled facts */
	reset_all();
	optin = "t8122";
	stage = "boot";
	confirm_boot = true;
	fw_path = "/lib/firmware/synthetic-macho-for-sim";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	chk(ret == -ENOENT, "P4 boot: probe returns -ENOENT");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 3 && !strcmp(verdict, "REFUSED") &&
	    !strcmp(reason, "unfilled-facts"),
	    "P4 boot: RESULT stage=3 REFUSED unfilled-facts");
	nids = collect_holes(ids, 8);
	chk(nids == 5 && !strcmp(ids[0], "H1") && !strcmp(ids[1], "H2") &&
	    !strcmp(ids[2], "H3") && !strcmp(ids[3], "H4") &&
	    !strcmp(ids[4], "H5"),
	    "P4 boot: refusal enumerates exactly H1,H2,H3,H4,H5");
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P4 boot: refused probe unmaps its windows");
	chk_global_invariants(cur_soc, "P4 boot");
}

/* T6 gate logic on synthetic tables: pure data walk, no code path that
 * releases anything exists anywhere in the module.
 */
static void pos_gate_logic(void)
{
	struct ane_h15_fact t[8];
	unsigned int i, n = ane_h15_n_facts;

	chk(n == 5, "T6: real table has exactly 5 fact rows");
	chk(!ane_h15_facts_ready(ane_h15_facts, n),
	    "T6: real table is NOT ready (gate closed)");
	chk(ane_h15_facts_report(ane_h15_facts, n, NULL, NULL) == 5,
	    "T6: real table reports 5 unfilled rows");

	memcpy(t, ane_h15_facts, n * sizeof(t[0]));
	t[2].filled = true;		/* synthetic: H3 measured */
	t6_n = 0;
	chk(ane_h15_facts_report(t, n, t6_emit, NULL) == 4,
	    "T6: synthetic H3-filled table reports 4 unfilled");
	chk(t6_n == 4 && !strcmp(t6_ids[0], "H1") &&
	    !strcmp(t6_ids[1], "H2") && !strcmp(t6_ids[2], "H4") &&
	    !strcmp(t6_ids[3], "H5"),
	    "T6: synthetic H3-filled enumeration is H1,H2,H4,H5");

	for (i = 0; i < n; i++)
		t[i].filled = true;
	chk(ane_h15_facts_ready(t, n),
	    "T6: fully-filled synthetic table WOULD open the gate (logic only)");
	chk(ane_h15_facts_report(t, n, t6_emit, NULL) == 0,
	    "T6: fully-filled table emits no refusal lines");
}

static void pos_other_socs(void)
{
	static const struct {
		const struct ane_h15_soc *soc;
		const char *compat;
	} rows[] = {
		{ &ane_t6030_soc, "apple,t6030-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[64], tag[32];
	size_t r;
	int ret;

	for (r = 0; r < ARRAY_SIZE(rows); r++) {
		reset_all();
		optin = (char *)rows[r].soc->name;
		stage = "status";
		build_node(rows[r].soc, 2, rows[r].compat);
		set_ps_words(rows[r].soc, -1);
		ret = probe_now();
		snprintf(tag, sizeof(tag), "P6 %s", rows[r].soc->name);
		snprintf(name, sizeof(name), "%s: probe returns 0", tag);
		chk(ret == 0, name);
		snprintf(name, sizeof(name), "%s: RESULT PASS ps-guard+reads",
			 tag);
		chk(parse_result(&rstage, verdict, sizeof(verdict),
				 reason, sizeof(reason)) &&
		    rstage == 1 && !strcmp(verdict, "PASS") &&
		    !strcmp(reason, "ps-guard+reads"), name);
		snprintf(name, sizeof(name), "%s: five word actual=0xf lines",
			 tag);
		chk(log_count("actual=0xf pass=true") == 5, name);
		chk_global_invariants(cur_soc, tag);
	}
}

/* ---- negative controls: exit 0 iff the module correctly refuses ---- */
static int neg_ps_stuck(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret, bad = 0;

	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, 2);	/* ANE_CPU stuck at 0x30 */
	ret = probe_now();

	if (ret != -ETIMEDOUT) {
		printf("    NEG-PS-STUCK: expected -ETIMEDOUT, got %d\n", ret);
		bad = 1;
	}
	if (!parse_result(&rstage, verdict, sizeof(verdict),
			  reason, sizeof(reason)) ||
	    rstage != 1 || strcmp(verdict, "FAIL") ||
	    strcmp(reason, "pmgr-actual-stuck")) {
		printf("    NEG-PS-STUCK: expected RESULT stage=1 FAIL pmgr-actual-stuck\n");
		bad = 1;
	}
	if (log_count("pass=true") != 0) {
		printf("    NEG-PS-STUCK: a pass=true line was logged on a stuck word\n");
		bad = 1;
	}
	if (events_in(ane_t8122_soc.engine_pa, ane_t8122_soc.engine_size, 0) ||
	    sim_bus_faults || events_in(0, ~0ULL, 1)) {
		printf("    NEG-PS-STUCK: engine access, fault, or write on stuck word\n");
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    NEG-PS-STUCK: failed probe leaked a window (iomap=%d iounmap=%d)\n",
		       sim_n_iomap, sim_n_iounmap);
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-ps-stuck: harness FAILED the module (BAD)"
			   : "  neg-ps-stuck: module refuses as required");
	return bad;
}

static int neg_wrong_stage(void)
{
	unsigned int rstage;
	int ret, bad = 0;

	/* unknown stage string: -EINVAL, NO RESULT, nothing mapped */
	reset_all();
	optin = "t8122";
	stage = "7";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	if (ret != -EINVAL) {
		printf("    NEG-WRONG-STAGE: expected -EINVAL, got %d\n", ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    NEG-WRONG-STAGE: a RESULT line leaked out\n");
		bad = 1;
	}
	if (sim_n_events || sim_n_regions) {
		printf("    NEG-WRONG-STAGE: MMIO happened before the stage check\n");
		bad = 1;
	}

	/* wrong optin: -EPERM, NO RESULT, nothing mapped */
	reset_all();
	optin = "t9999";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, -1);
	ret = probe_now();
	if (ret != -EPERM) {
		printf("    NEG-WRONG-OPTIN: expected -EPERM, got %d\n", ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    NEG-WRONG-OPTIN: a RESULT line leaked out\n");
		bad = 1;
	}
	if (sim_n_events || sim_bus_faults) {
		printf("    NEG-WRONG-OPTIN: MMIO happened before the optin check\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-wrong-stage: harness FAILED (BAD)"
			   : "  neg-wrong-stage: module refuses as required");
	return bad;
}

static int neg_truncated_dt(void)
{
	int ret, bad = 0;
	static const int nregs_variants[] = { 1, 0 };	/* pmgr missing; empty */
	size_t i;

	for (i = 0; i < ARRAY_SIZE(nregs_variants); i++) {
		reset_all();
		optin = "t8122";
		stage = "status";
		build_node(&ane_t8122_soc, nregs_variants[i],
			   "apple,t8122-ane");
		set_ps_words(&ane_t8122_soc, -1);
		ret = probe_now();
		if (ret != -ENXIO) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): expected -ENXIO, got %d\n",
			       nregs_variants[i], ret);
			bad = 1;
		}
		if (log_count("ane_h15 RESULT") != 0) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): RESULT on a broken tree\n",
			       nregs_variants[i]);
			bad = 1;
		}
		if (log_count("of_iomap failed") != 1) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): no of_iomap refusal line\n",
			       nregs_variants[i]);
			bad = 1;
		}
		if (sim_n_events) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): MMIO on a broken tree\n",
			       nregs_variants[i]);
			bad = 1;
		}
		if (sim_n_iomap != sim_n_iounmap) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): failed probe leaked a window (iomap=%d iounmap=%d)\n",
			       nregs_variants[i], sim_n_iomap, sim_n_iounmap);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-truncated-dt: harness FAILED (BAD)"
			   : "  neg-truncated-dt: module refuses as required");
	return bad;
}

static int neg_rpm_fail(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret, bad = 0;

	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, -1);
	sim_pm_get_fail = -EIO;		/* power-domains bring-up fails */
	ret = probe_now();

	if (ret != -EIO) {
		printf("    NEG-RPM-FAIL: expected -EIO, got %d\n", ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    NEG-RPM-FAIL: RESULT line before the stage ran\n");
		bad = 1;
	}
	if (sim_pm_get != 0 || sim_pm_put != 0) {
		printf("    NEG-RPM-FAIL: power claim taken on a failed get_sync\n");
		bad = 1;
	}
	if (sim_n_iomap != 2 || sim_n_iounmap != 2) {
		printf("    NEG-RPM-FAIL: failed probe leaked a window (iomap=%d iounmap=%d)\n",
		       sim_n_iomap, sim_n_iounmap);
		bad = 1;
	}
	if (events_in(ane_t8122_soc.engine_pa, ane_t8122_soc.engine_size, 0) ||
	    sim_bus_faults || events_in(0, ~0ULL, 1)) {
		printf("    NEG-RPM-FAIL: engine access, fault, or write\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-rpm-fail: harness FAILED (BAD)"
			   : "  neg-rpm-fail: module refuses as required");
	return bad;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "pos";

	if (!strcmp(mode, "pos")) {
		printf("sim-h15 positive controls:\n");
		pos_dt_zero_mmio();
		pos_ps_pass();
		pos_refusals();
		pos_gate_logic();
		pos_other_socs();
		printf("%s\n", fails ? "SIM-TEST: FAILED"
				     : "SIM-TEST: all positive controls PASS");
	} else if (!strcmp(mode, "neg-ps-stuck")) {
		printf("sim-h15 negative control: ps-actual-stuck\n");
		fails = neg_ps_stuck();
	} else if (!strcmp(mode, "neg-wrong-stage")) {
		printf("sim-h15 negative control: wrong-stage / wrong-optin\n");
		fails = neg_wrong_stage();
	} else if (!strcmp(mode, "neg-truncated-dt")) {
		printf("sim-h15 negative control: truncated DT\n");
		fails = neg_truncated_dt();
	} else if (!strcmp(mode, "neg-rpm-fail")) {
		printf("sim-h15 negative control: pm_runtime_get_sync failure\n");
		fails = neg_rpm_fail();
	} else {
		fprintf(stderr,
			"usage: sim_h15 [pos|neg-ps-stuck|neg-wrong-stage|neg-truncated-dt|neg-rpm-fail]\n");
		return 2;
	}
	return fails ? 1 : 0;
}
