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
#ifdef ANE15_SIM_FILL_FACTS
/* P7 build: compile the real soc file with its fact table renamed
 * aside, then substitute a synthetic fully-filled table, so the REAL
 * probe + stage_boot run their every-fact-filled fall-through. The
 * renamed real table stays in the binary for the substitution check.
 */
#define ane_h15_facts ane_h15_facts_real_unfilled
#define ane_h15_n_facts ane_h15_n_facts_real_unfilled
#include "../ane_h15_soc.c"
#undef ane_h15_facts
#undef ane_h15_n_facts
const struct ane_h15_fact ane_h15_facts[] = {
	{ "H1", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H2", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H3", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H4", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H5", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
};

const unsigned int ane_h15_n_facts = ARRAY_SIZE(ane_h15_facts);
#else
#include "../ane_h15_soc.c"
#endif
/* The run-time overlay glue is tested by sim/sim_overlay.c with its own fake tree; here the module's init and exit are never run. */
int ane_h15_register(struct platform_driver *drv, const char *opt, bool apply)
{
	(void)drv;
	(void)opt;
	(void)apply;
	return 0;
}

void ane_h15_unregister(struct platform_driver *drv)
{
	(void)drv;
}

#include "../ane_h15_main.c"

/* steplog shim state */
char sim_steplog_buf[8192];
int sim_steplog_len;
unsigned long sim_steplog_fsyncs;
int sim_steplog_open_fail;
unsigned long long sim_ktime_ns;

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
int sim_pm_get_err;
int sim_pm_noidle;
struct device sim_pd_dev[SIM_MAX_PD];
struct device_link sim_pd_link[SIM_MAX_PD];
int sim_pd_state[SIM_MAX_PD];
int sim_link_state[SIM_MAX_PD];
int sim_pd_attach;
int sim_pd_detach;
int sim_link_add;
int sim_link_del;
int sim_pd_bad;
int sim_pd_attach_fail_at = -1;
int sim_link_fail_at = -1;
int sim_pd_attach_calls;
int sim_module_gets;
struct sim_dma sim_dma[8];
int sim_n_dma, sim_dma_fail, sim_dma_mask_bits, sim_dma_scatter, sim_map_fail_at;
int sim_unmap_fail_at;
struct iommu_domain sim_default_dom = { .type = __IOMMU_DOMAIN_DMA_API };
int sim_default_dom_none;
int sim_region_ram = REGION_DISJOINT;
struct sim_iocall sim_iocalls[64];
int sim_n_iocalls, sim_io_fail, sim_n_dom_live;
void (*sim_attach_hook)(void);
int sim_link_calls;

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
	sim_steplog_len = 0; sim_steplog_buf[0] = 0; sim_steplog_fsyncs = 0; sim_steplog_open_fail = 0;
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
	sim_pm_get_err = 0;
	sim_pm_noidle = 0;
	memset(sim_pd_state, 0, sizeof(sim_pd_state));
	memset(sim_link_state, 0, sizeof(sim_link_state));
	sim_pd_attach = sim_pd_detach = sim_link_add = sim_link_del = 0;
	sim_pd_bad = 0;
	sim_pd_attach_fail_at = -1;
	sim_link_fail_at = -1;
	sim_pd_attach_calls = 0;
	sim_link_calls = 0;
	memset(&pdev, 0, sizeof(pdev));
	memset(&node, 0, sizeof(node));
	optin = NULL;
	stage = "status";
	ps_wait_ms = 500;
	confirm_boot = false;
	confirm_read = false;
	confirm_run = false;
	run_scratch = false;
	run_pmgr = false;
	scratch_boot = false;
	sb_wait_ms = 1000;
	pwgate_t6031 = 0;
	pwgate_read = 0;
	stop_after_pwgate = false;
	steplog = NULL;
	run_unmapped = false;
	sim_module_gets = 0;
	map_fw = false;
	fw_seg = NULL;
	sim_region_ram = REGION_DISJOINT;
	sim_n_iocalls = sim_io_fail = sim_n_dom_live = sim_map_fail_at = sim_unmap_fail_at = 0;
	sim_attach_hook = NULL;
	run_wait_ms = 300;
	sim_write_hook = NULL;
	sim_read_hook = NULL;
	sim_write64_hook = NULL;
	confirm_chan = false;
	chan_cmds = false;
	hs_wait_ms = 1000;
	{ int k; for (k = 0; k < sim_n_dma; k++) free(sim_dma[k].va); }
	sim_dma_scatter = 0;
	sim_n_dma = sim_dma_fail = sim_dma_mask_bits = sim_default_dom_none = 0;
	confirm_rtkit = false;
	rtkit_wait_ms = 2000;
	rtkit_stop = 1;
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
	int ret;

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

	/* Stage 3 needs separate read consent before firmware preparation. */
	reset_all();
	optin = "t8122";
	stage = "boot";
	confirm_boot = true;
	fw_path = "/lib/firmware/synthetic-macho-for-sim";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	chk(ret == -EPERM, "P4 boot: missing read consent returns -EPERM");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 3 && !strcmp(verdict, "REFUSED") &&
	    !strcmp(reason, "owned-firmware-prepare-failed"),
	    "P4 boot: preparation refuses without read consent");
	chk(sim_fw_requests == 0 && sim_n_dma == 0 && sim_n_events == 0,
	    "P4 boot: no file, DMA allocation or MMIO before consent");
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


/* ---- stage 2 preflight (confirm_read=1): read-only ---- */
static void eng_set(const struct ane_h15_soc *s, u32 off, u32 v)
{
	sim_bus[sim_region_find(s->engine_pa + off) - sim_bus].mem[off >> 2] = v;
}

static void stage2_setup(const struct ane_h15_soc *s, const char *compat,
			 int stuck_ps)
{
	reset_all();
	optin = (char *)s->name;
	stage = "wrapper";
	confirm_read = true;
	build_node(s, 2, compat);
	set_ps_words(s, stuck_ps);
	sim_bus_map(s->engine_pa, s->engine_size);
	eng_set(s, s->cpu_status, 0x2a);
	eng_set(s, s->rvbar, 0x1);
	eng_set(s, s->rvbar + 4, 0x0);
	eng_set(s, s->mbox + 0x110, 0x20001);
	eng_set(s, s->mbox + 0x114, 0x20001);
}

/* every engine event, as a pa offset list; returns the count of reads
 * that hit `off` and of all engine events
 */
static int eng_events_at(const struct ane_h15_soc *s, u32 off)
{
	return events_in(s->engine_pa + off, 4, 0);
}

static void pos_stage2(void)
{
	static const struct {
		const struct ane_h15_soc *soc;
		const char *compat;
	} rows[] = {
		{ &ane_t8122_soc, "apple,t8122-ane" },
		{ &ane_t6030_soc, "apple,t6030-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
		{ &ane_t6034_soc, "apple,t6034-ane" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[96], tag[32];
	size_t r;
	int ret;

	for (r = 0; r < ARRAY_SIZE(rows); r++) {
		const struct ane_h15_soc *s = rows[r].soc;

		stage2_setup(s, rows[r].compat, -1);
		ret = probe_now();
		snprintf(tag, sizeof(tag), "P9 %s", s->name);
		snprintf(name, sizeof(name), "%s: probe returns 0", tag);
		chk(ret == 0, name);
		snprintf(name, sizeof(name), "%s: RESULT stage=2 PASS preflight-read-only", tag);
		chk(parse_result(&rstage, verdict, sizeof(verdict),
				 reason, sizeof(reason)) &&
		    rstage == 2 && !strcmp(verdict, "PASS") &&
		    !strcmp(reason, "preflight-read-only"), name);
		snprintf(name, sizeof(name), "%s: exactly five engine reads at the five words", tag);
		chk(events_in(s->engine_pa, s->engine_size, 0) == 5 &&
		    eng_events_at(s, s->cpu_status) == 1 &&
		    eng_events_at(s, s->rvbar) == 1 &&
		    eng_events_at(s, s->rvbar + 4) == 1 &&
		    eng_events_at(s, s->mbox + 0x110) == 1 &&
		    eng_events_at(s, s->mbox + 0x114) == 1, name);
		snprintf(name, sizeof(name), "%s: CPU_CONTROL never touched", tag);
		chk(eng_events_at(s, s->cpu_control) == 0, name);
		snprintf(name, sizeof(name), "%s: zero bus writes, zero faults", tag);
		chk(events_in(0, ~0ULL, 1) == 0 && sim_bus_faults == 0, name);
		snprintf(name, sizeof(name), "%s: each address logged before its read (5 lines)", tag);
		chk(log_count("ane_h15 read CPU_STATUS") == 1 &&
		    log_count("ane_h15 read RVBAR_LO") == 1 &&
		    log_count("ane_h15 read RVBAR_HI") == 1 &&
		    log_count("ane_h15 read MBOX_A2I_CTRL") == 1 &&
		    log_count("ane_h15 read MBOX_I2A_CTRL") == 1, name);
		snprintf(name, sizeof(name), "%s: values logged", tag);
		chk(log_count("cpu_status=0x2a rvbar=0x1 a2i_ctrl=0x20001 i2a_ctrl=0x20001 t6021_stopped_status=equal") == 1, name);
		snprintf(name, sizeof(name), "%s: pmgr words read before the first engine read", tag);
		{
			int first_eng = -1, last_pmgr = -1, i;

			for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
				if (sim_events[i].pa >= s->engine_pa &&
				    sim_events[i].pa < s->engine_pa + s->engine_size) {
					if (first_eng < 0)
						first_eng = i;
				} else {
					last_pmgr = i;
				}
			}
			chk(first_eng > last_pmgr && last_pmgr >= 4, name);
		}
		snprintf(name, sizeof(name), "%s: power claim released", tag);
		chk(sim_pm_get == 1 && sim_pm_put == 1, name);
		ane_h15_remove(&pdev);
		snprintf(name, sizeof(name), "%s: remove unmaps both windows", tag);
		chk(sim_n_iomap == 2 && sim_n_iounmap == 2, name);
	}
	/* a status word that differs from the T6021 stopped capture */
	stage2_setup(&ane_t6030_soc, "apple,t6030-ane", -1);
	eng_set(&ane_t6030_soc, ane_t6030_soc.cpu_status, 0x1);
	ret = probe_now();
	chk(ret == 0 && log_count("cpu_status=0x1 ") == 1 &&
	    log_count("t6021_stopped_status=differs") == 1,
	    "P9 differing CPU_STATUS is logged, not interpreted");
	ane_h15_remove(&pdev);
}

/* reg[0] or reg[1] that is not the window the SoC row describes (shifted
 * base, shorter, or another block): stage 2 refuses before any bus access
 */
static int neg_read_window(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int i, bad = 0;

	for (i = 0; i < 6; i++) {
		int ret;

		stage2_setup(&ane_t6030_soc, "apple,t6030-ane", -1);
		if (i == 0)
			node.reg_pa[0] += 0x1000000;	/* base moved */
		else if (i == 1)
			node.reg_size[0] -= 0x100000;	/* window shorter */
		else if (i == 2)
			node.reg_pa[0] = ane_t6030_soc.pmgr_pa + 0x100000;	/* some other block */
		else if (i == 3)
			node.reg_pa[1] += 0x10000;	/* pmgr base moved */
		else if (i == 4)
			node.reg_size[1] -= 0x4000;	/* pmgr window shorter */
		else
			node.reg_pa[1] = ane_t6030_soc.engine_pa + 0x100000;	/* pmgr is some other block */
		ret = probe_now();
		if (ret != -EINVAL) {
			printf("    NEG-READ-WINDOW case %d: expected -EINVAL, got %d\n", i, ret);
			bad = 1;
		}
		if (!parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) ||
		    rstage != 2 || strcmp(verdict, "FAIL") || strcmp(reason, "window-mismatch")) {
			printf("    NEG-READ-WINDOW case %d: expected RESULT stage=2 FAIL window-mismatch\n", i);
			bad = 1;
		}
		if (sim_n_events != 0 || sim_bus_faults) {
			printf("    NEG-READ-WINDOW case %d: bus access before the window check\n", i);
			bad = 1;
		}
		if (sim_n_iomap != sim_n_iounmap || sim_pm_get != sim_pm_put) {
			printf("    NEG-READ-WINDOW case %d: a refused probe leaked a window or the power claim\n", i);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-read-window: harness FAILED the module (BAD)"
			   : "  neg-read-window: module refuses as required");
	return bad;
}

/* stuck pmgr word: no engine read at all */
static int neg_read_ps_stuck(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret, bad = 0;

	stage2_setup(&ane_t6030_soc, "apple,t6030-ane", 3);
	ret = probe_now();
	if (ret != -ETIMEDOUT) {
		printf("    NEG-READ-PS-STUCK: expected -ETIMEDOUT, got %d\n", ret);
		bad = 1;
	}
	if (!parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) ||
	    rstage != 2 || strcmp(verdict, "FAIL") || strcmp(reason, "pmgr-actual-stuck")) {
		printf("    NEG-READ-PS-STUCK: expected RESULT stage=2 FAIL pmgr-actual-stuck\n");
		bad = 1;
	}
	if (events_in(ane_t6030_soc.engine_pa, ane_t6030_soc.engine_size, 0) ||
	    events_in(0, ~0ULL, 1) || sim_bus_faults) {
		printf("    NEG-READ-PS-STUCK: engine access, write or fault on a stuck pmgr word\n");
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    NEG-READ-PS-STUCK: leaked a window\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-read-ps-stuck: harness FAILED the module (BAD)"
			   : "  neg-read-ps-stuck: module refuses as required");
	return bad;
}

/* a soc row whose word lies outside the two ADT windows, on CPU_CONTROL
 * or beyond the wrapper: the read is refused and never reaches the bus
 */
static int neg_read_forbidden(void)
{
	static struct ane_h15_soc bad_soc;
	struct {
		const char *what;
		u32 cpu_status, rvbar, mbox;
		u32 forbidden;
	} cases[] = {
		/* RVBAR in the T6021 fatal range */
		{ "rvbar-in-fatal-range", 0x1400048, 0x1854000, 0x1408000, 0x1854000 },
		/* a mailbox word that lands on CPU_CONTROL */
		{ "mbox-on-cpu-control", 0x1400048, 0x1050000, 0x1400044 - 0x110, 0x1400044 },
		/* CPU_STATUS past the end of the wrapper window */
		{ "status-past-wrapper", 0x146c000, 0x1050000, 0x1408000, 0x146c000 },
		/* RVBAR on the CoreSight line */
		{ "rvbar-on-coresight", 0x1400048, 0x1010000, 0x1408000, 0x1010000 },
	};
	unsigned int rstage;
	char verdict[32], reason[128];
	size_t i;
	int bad = 0;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		int ret;

		bad_soc = ane_t6030_soc;
		bad_soc.cpu_status = cases[i].cpu_status;
		bad_soc.rvbar = cases[i].rvbar;
		bad_soc.mbox = cases[i].mbox;
		stage2_setup(&ane_t6030_soc, "apple,t6030-ane", -1);
		node.match_data = &bad_soc;
		cur_soc = &bad_soc;
		ret = probe_now();
		if (ret != -EPERM) {
			printf("    NEG-READ-FORBIDDEN %s: expected -EPERM, got %d\n",
			       cases[i].what, ret);
			bad = 1;
		}
		if (!parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) ||
		    rstage != 2 || strcmp(verdict, "FAIL") || strcmp(reason, "read-refused")) {
			printf("    NEG-READ-FORBIDDEN %s: expected RESULT stage=2 FAIL read-refused\n",
			       cases[i].what);
			bad = 1;
		}
		if (events_in(bad_soc.engine_pa + cases[i].forbidden, 4, 0) ||
		    events_in(0, ~0ULL, 1) || sim_bus_faults) {
			printf("    NEG-READ-FORBIDDEN %s: the forbidden word reached the bus\n",
			       cases[i].what);
			bad = 1;
		}
		if (sim_n_iomap != sim_n_iounmap || sim_pm_get != sim_pm_put) {
			printf("    NEG-READ-FORBIDDEN %s: a refused probe leaked a window or the power claim\n",
			       cases[i].what);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-read-forbidden: harness FAILED the module (BAD)"
			   : "  neg-read-forbidden: module refuses as required");
	return bad;
}

/* ---- P7 filled-table probe control (filled build only) ---- */
#ifdef ANE15_SIM_FILL_FACTS
/* P7: the gate logic driven through the REAL probe path with a fully
 * filled table. The filled-table run must refuse (-ENOSYS) with zero
 * bus writes: this build has no boot path, no CPU release.
 */
static void pos_filled_probe(void)
{
	unsigned int rstage;
	char verdict[32], reason[128], path[1024];
	const char *directory = getenv("H15_FW_INPUT_DIR");
	int ret, length;

	chk(directory != NULL, "P7 filled: private real-image input directory required");
	if (!directory)
		return;
	length = snprintf(path, sizeof(path), "%s/t603x_ane0_fw_erebus_pc5x.im4p.payload", directory);
	chk(length > 0 && length < (int)sizeof(path), "P7 filled: input path fits");
	if (length <= 0 || length >= (int)sizeof(path))
		return;
	stage2_setup(&ane_t6031_soc, "apple,t6031-ane", -1);
	stage = "boot";
	confirm_boot = true;
	fw_path = path;

	chk(ane_h15_facts_ready(ane_h15_facts, ane_h15_n_facts),
	    "P7 filled: substituted table is ready (gate open)");
	chk(ane_h15_n_facts_real_unfilled == 5 &&
	    !ane_h15_facts_ready(ane_h15_facts_real_unfilled,
				 ane_h15_n_facts_real_unfilled),
	    "P7 filled: real table renamed aside, still 5 unfilled");

	ret = probe_now();
	chk(ret == -ENOSYS, "P7 filled: probe returns -ENOSYS");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 3 && !strcmp(verdict, "REFUSED") &&
	    !strcmp(reason, "facts-filled-boot-path-not-built"),
	    "P7 filled: RESULT stage=3 REFUSED facts-filled-boot-path-not-built");
	chk(log_count("ane_h15 hole=") == 0,
	    "P7 filled: no hole lines from a filled table");
	chk(sim_pm_get == 1 && sim_pm_put == 1, "P7 filled: power released");
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P7 filled: refused probe unmaps its windows");
	chk(sim_n_dma == 1 && !sim_dma[0].live && sim_fw_live == 0,
	    "P7 filled: owned preparation cleaned up on boot refusal");
	chk(events_in(cur_soc->engine_pa, cur_soc->engine_size, 1) == 0 &&
	    sim_bus_faults == 0, "P7 filled: no device write or bus fault");
}
#endif

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
	stage = "8";
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
			printf("    NEG-TRUNCATED-DT(%d): window leak (iomap=%d iounmap=%d)\n",
			       nregs_variants[i], sim_n_iomap,
			       sim_n_iounmap);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-truncated-dt: harness FAILED (BAD)"
			   : "  neg-truncated-dt: module refuses as required");
	return bad;
}

static int neg_rpm_fail(void)
{
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

/* ---- multiple power-domains (attach by id + runtime-PM device links) ---- */
static int pd_live(void)
{
	int i, n = 0;

	for (i = 0; i < SIM_MAX_PD; i++)
		n += sim_pd_state[i] == 1;
	return n;
}

static int link_live(void)
{
	int i, n = 0;

	for (i = 0; i < SIM_MAX_PD; i++)
		n += sim_link_state[i] == 1;
	return n;
}

/* A node with n_pd power-domains at stage 1; the caller sets the fault
 * knobs (sim_pd_attach_fail_at, sim_link_fail_at, sim_pm_get_fail)
 * after this returns and before probe_now().
 */
static void pd_setup(int n_pd)
{
	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	node.n_pd = n_pd;
	set_ps_words(&ane_t8122_soc, -1);
}

static void pos_multi_pd(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret, variant;

	/* P8: five domains; variant 0 = get_sync succeeds, 1 = get_sync
	 * returns -EACCES (what T6030 stage 1 reported): both must pass
	 */
	for (variant = 0; variant < 2; variant++) {
		pd_setup(5);
		if (variant)
			sim_pm_get_fail = -EACCES;
		ret = probe_now();
		chk(ret == 0, variant ? "P8b pd/-EACCES: probe returns 0"
				      : "P8a pd: probe returns 0");
		chk(parse_result(&rstage, verdict, sizeof(verdict),
				 reason, sizeof(reason)) &&
		    rstage == 1 && !strcmp(verdict, "PASS") &&
		    !strcmp(reason, "ps-guard+reads"),
		    "P8 pd: RESULT stage=1 PASS ps-guard+reads");
		chk(sim_pd_attach == 5 && sim_link_add == 5,
		    "P8 pd: 5 domains attached and 5 links added");
		chk(pd_live() == 5 && link_live() == 5,
		    "P8 pd: all 5 attached and linked after probe");
		if (variant) {
			chk(sim_pm_get == 0 && sim_pm_get_err == 1 &&
			    sim_pm_put == 1,
			    "P8b pd/-EACCES: failing get_sync usage count dropped");
			chk(log_count("main get_sync -13 ignored") == 1,
			    "P8b pd/-EACCES: ignore line logged once");
		} else {
			chk(sim_pm_get == 1 && sim_pm_put == 1,
			    "P8a pd: get==put==1");
		}
		chk_global_invariants(cur_soc, "P8 pd");
		ane_h15_remove(&pdev);
		chk(sim_pd_detach == 5 && sim_link_del == 5,
		    "P8 pd: remove detaches 5 domains and deletes 5 links");
		chk(pd_live() == 0 && link_live() == 0 && sim_pd_bad == 0,
		    "P8 pd: nothing left attached, none released twice");
		chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
		    "P8 pd: remove unmaps both windows");
	}

	/* P9: zero or one domain is the core's job: no attach, no link */
	for (variant = 0; variant < 2; variant++) {
		pd_setup(variant);
		ret = probe_now();
		chk(ret == 0 && sim_pd_attach_calls == 0 && sim_link_calls == 0,
		    variant ? "P9 one domain: module attaches nothing"
			    : "P9 no domain: module attaches nothing");
	}
}

/* Common refusal checks for a failed multi-domain probe. */
static int pd_refused(const char *tag, int ret, int want_ret)
{
	int bad = 0;

	if (ret != want_ret) {
		printf("    %s: expected %d, got %d\n", tag, want_ret, ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    %s: RESULT line before the stage ran\n", tag);
		bad = 1;
	}
	if (pd_live() || link_live() || sim_pd_bad) {
		printf("    %s: leak or double release (domains live=%d links live=%d bad=%d)\n",
		       tag, pd_live(), link_live(), sim_pd_bad);
		bad = 1;
	}
	if (sim_pd_attach != sim_pd_detach || sim_link_add != sim_link_del) {
		printf("    %s: attach/detach %d/%d, link add/del %d/%d\n", tag,
		       sim_pd_attach, sim_pd_detach, sim_link_add, sim_link_del);
		bad = 1;
	}
	if (sim_pm_get_err != sim_pm_noidle) {
		printf("    %s: failed get_sync usage count not dropped (get_err=%d noidle=%d)\n", tag,
		       sim_pm_get_err, sim_pm_noidle);
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    %s: window leak (iomap=%d iounmap=%d)\n", tag,
		       sim_n_iomap, sim_n_iounmap);
		bad = 1;
	}
	if (events_in(ane_t8122_soc.engine_pa, ane_t8122_soc.engine_size, 0) ||
	    sim_bus_faults || events_in(0, ~0ULL, 1)) {
		printf("    %s: engine access, fault, or write\n", tag);
		bad = 1;
	}
	return bad;
}

static int neg_pd_attach_fail(void)
{
	char tag[64];
	int i, bad = 0;

	for (i = 0; i < 5; i++) {
		pd_setup(5);
		sim_pd_attach_fail_at = i;
		snprintf(tag, sizeof(tag), "NEG-PD-ATTACH-FAIL(i=%d)", i);
		bad |= pd_refused(tag, probe_now(), -ENODEV);
		if (sim_pd_attach != i) {
			printf("    %s: %d domains attached before the failure, expected %d\n",
			       tag, sim_pd_attach, i);
			bad = 1;
		}
		if (sim_pm_get || sim_pm_put || sim_pm_get_err) {
			printf("    %s: runtime PM touched after a failed attach\n", tag);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-pd-attach-fail: harness FAILED (BAD)"
			   : "  neg-pd-attach-fail: module refuses and releases as required");
	return bad;
}

static int neg_pd_link_fail(void)
{
	char tag[64];
	int i, bad = 0;

	for (i = 0; i < 5; i++) {
		pd_setup(5);
		sim_link_fail_at = i;
		snprintf(tag, sizeof(tag), "NEG-PD-LINK-FAIL(i=%d)", i);
		bad |= pd_refused(tag, probe_now(), -EINVAL);
		/* the domain whose link failed is attached and must be
		 * detached too: i+1 attaches, i links
		 */
		if (sim_pd_attach != i + 1 || sim_link_add != i) {
			printf("    %s: attach=%d link=%d, expected %d and %d\n",
			       tag, sim_pd_attach, sim_link_add, i + 1, i);
			bad = 1;
		}
		if (sim_pm_get || sim_pm_put || sim_pm_get_err) {
			printf("    %s: runtime PM touched after a failed link\n", tag);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-pd-link-fail: harness FAILED (BAD)"
			   : "  neg-pd-link-fail: module refuses and releases as required");
	return bad;
}

/* get_sync fails with something other than -EACCES while five domains
 * are attached: the probe must fail and release everything
 */
static int neg_pd_rpm_err(void)
{
	int bad;

	pd_setup(5);
	sim_pm_get_fail = -EIO;
	bad = pd_refused("NEG-PD-RPM-ERR", probe_now(), -EIO);
	if (sim_pd_attach != 5 || sim_link_add != 5) {
		printf("    NEG-PD-RPM-ERR: attach=%d link=%d, expected 5 and 5\n",
		       sim_pd_attach, sim_link_add);
		bad = 1;
	}
	if (sim_pm_get != 0 || sim_pm_put != 0) {
		printf("    NEG-PD-RPM-ERR: power claim taken on a failed get_sync\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-pd-rpm-err: harness FAILED (BAD)"
			   : "  neg-pd-rpm-err: module refuses as required");
	return bad;
}

/* -EACCES is ignored only with domains attached by the module: with
 * zero or one domain it must still fail the probe
 */
static int neg_pd_eacces_alone(void)
{
	int n, bad = 0;

	for (n = 0; n < 2; n++) {
		pd_setup(n);
		sim_pm_get_fail = -EACCES;
		bad |= pd_refused(n ? "NEG-PD-EACCES(one domain)"
				    : "NEG-PD-EACCES(no domain)",
				  probe_now(), -EACCES);
		if (sim_pd_attach_calls) {
			printf("    NEG-PD-EACCES(n=%d): module attached a domain\n", n);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-pd-eacces-alone: harness FAILED (BAD)"
			   : "  neg-pd-eacces-alone: module refuses as required");
	return bad;
}

/* ---- stage 4 (confirm_run=1): the only stage with writes ----
 *
 * A tiny device model behind the shim's bus hooks: a CPU_CONTROL write with RUN clears STOPPED in
 * CPU_STATUS (the M6 observation, 0xa -> 0x8), a write without it sets STOPPED again, unless the core
 * is "stuck". Optionally the core posts mailbox messages once started: I2A_CTRL loses EMPTY until the
 * module pops RECV1.
 */
void (*sim_write_hook)(u64 pa, u32 v);
u32 (*sim_read_hook)(u64 pa, u32 v);
void (*sim_write64_hook)(u64 pa, u64 v);

#define DM_RUN 0x10u
#define DM_STOPPED 0x2u
#define DM_EMPTY_WORD 0x20001u
/* the t6031 row's PWGATE window (ADT ane0 reg entry 5) and the word the
 * macOS clears */
#define PW_PA		0x292290000ull
#define PW_SZ		0x4000u
#define PW_WORD_OFF	0x199cu
#define PW_WORD_PA	(PW_PA + PW_WORD_OFF)
static const struct ane_h15_soc *dm_soc;
static bool dm_stuck, dm_nostart, dm_ambiguous_stop;
static int dm_msgs_left;
static bool dm_s7_ready;	/* scratch_boot model: the firmware publishes the ready word in SCRATCH7 once started */
static bool dm_s7_stuck;	/* scratch_boot model: the SCRATCH7 register ignores the clear and always reads the stale word */
static u32 dm_pw_word;		/* pwgate model: what the word at PWGATE + 0x199c reads */
static int dm_pw_reads;		/* pwgate model: how many times the word was read */
static bool dm_pw_stuck;	/* pwgate model: the register ignores the 0 write and the poll never opens */

static u32 eng_get(const struct ane_h15_soc *s, u32 off)
{
	return sim_bus[sim_region_find(s->engine_pa + off) - sim_bus].mem[off >> 2];
}

static void dm_write(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (pa != s->engine_pa + s->cpu_control) {
		/* pwgate model: a 0 to the gate word opens it within the
		 * first 10 us poll, unless the register ignores the write */
		if (s->pwgate_pa && pa == s->pwgate_pa + s->pwgate_word && !dm_pw_stuck)
			dm_pw_word &= ~3u;
		return;
	}
	if (v & DM_RUN) {
		if (!dm_nostart)
			eng_set(s, s->cpu_status, eng_get(s, s->cpu_status) & ~DM_STOPPED);
		if (dm_s7_ready)
			eng_set(s, 0x1840050u + 4 * 7, 0x08042006u);
		if (dm_msgs_left && !dm_nostart)
			eng_set(s, s->mbox + 0x114, 0x1);
	} else if (!dm_stuck) {
		eng_set(s, s->cpu_status, eng_get(s, s->cpu_status) | DM_STOPPED |
			(dm_ambiguous_stop ? 1u : 0u));
	}
}

static u32 dm_read(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (s->pwgate_pa && pa == s->pwgate_pa + s->pwgate_word) {
		dm_pw_reads++;
		return dm_pw_word;
	}
	if (dm_s7_stuck && pa == s->engine_pa + 0x1840050u + 4 * 7)
		return 0x08042006u;	/* the register ignored the clear */
	if (pa == s->engine_pa + s->mbox + 0x838) {	/* popping a message */
		if (dm_msgs_left > 0)
			dm_msgs_left--;
		if (!dm_msgs_left)
			eng_set(s, s->mbox + 0x114, DM_EMPTY_WORD);
	}
	return v;
}

/* ANE DART model: window 0 reads as a t8110 DART (PARAMS3 0x1c200301, 16 streams); stream 3 has TCR translate,
 * a valid TTBR and its ENABLE bit. Windows 1..3 read 0, so their version differs and they are not scanned. */
#define DART_P3 0x1c200301u
static void dart_set(const struct ane_h15_soc *s, bool live)
{
	u32 b = s->dart_off;

	eng_set(s, b + 0x08, DART_P3);
	eng_set(s, b + 0x0c, 0x00080010);
	eng_set(s, b + 0xc00, live ? 1u << 3 : 0);
	eng_set(s, b + 0x1000 + 4 * 3, live ? 0x1 : 0);
	eng_set(s, b + 0x1400 + 4 * 3, live ? 0x00abc001 : 0);
}

static void run_setup(const struct ane_h15_soc *s, const char *compat, u32 ctrl)
{
	stage2_setup(s, compat, -1);
	stage = "run";
	confirm_read = false;
	confirm_run = true;
	run_wait_ms = 50;
	eng_set(s, s->cpu_status, 0x30002a);
	eng_set(s, s->rvbar + 4, 0x100);
	eng_set(s, s->cpu_control, ctrl);
	eng_set(s, s->mbox + 0x830, 0xabc0);
	eng_set(s, s->mbox + 0x838, 0x1234);
	eng_set(s, s->mbox + 0x834, 0x11);	/* upper halves: a 32-bit read would lose them */
	eng_set(s, s->mbox + 0x83c, 0x22);
	dart_set(s, true);
	dm_soc = s;
	dm_stuck = dm_nostart = dm_ambiguous_stop = false;
	dm_s7_ready = false;
	dm_s7_stuck = false;
	dm_pw_word = 0;
	dm_pw_reads = 0;
	dm_pw_stuck = false;
	dm_msgs_left = 0;
	sim_write_hook = dm_write;
	sim_read_hook = dm_read;
}

/* engine writes in order; returns how many */
static int run_writes(const struct ane_h15_soc *s, u32 *vals, int max, int *first_idx)
{
	int i, n = 0;

	*first_idx = -1;
	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
		if (!sim_events[i].write || sim_events[i].pa < s->engine_pa ||
		    sim_events[i].pa >= s->engine_pa + s->engine_size)
			continue;
		if (*first_idx < 0)
			*first_idx = i;
		if (n < max)
			vals[n] = sim_events[i].val;
		n++;
	}
	return n;
}

/* no engine write at all, no write anywhere, no fault, and the failed probe released everything */
static int run_refused_clean(const struct ane_h15_soc *s, int ret, const char *want_verdict,
			     const char *want_reason_part, const char *tag)
{
	unsigned int rstage;
	char verdict[32], reason[128], name[128];
	int before = fails;

	snprintf(name, sizeof(name), "%s: probe fails, RESULT stage=5 %s", tag, want_verdict);
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, want_verdict) && strstr(reason, want_reason_part), name);
	snprintf(name, sizeof(name), "%s: zero bus writes, zero faults, no RECV read", tag);
	chk(events_in(0, ~0ULL, 1) == 0 && sim_bus_faults == 0 &&
	    eng_events_at(s, s->mbox + 0x830) == 0 && eng_events_at(s, s->mbox + 0x838) == 0, name);
	snprintf(name, sizeof(name), "%s: windows unmapped, power claim balanced", tag);
	chk(sim_n_iomap == sim_n_iounmap && sim_pm_get == sim_pm_put, name);
	return fails - before;
}

static void pos_run_case(const char *tag, u32 ctrl, int msgs_left, bool nostart, int want_msgs,
			 const u32 *want_w, int n_want_w)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[128];
	u32 w[8];
	int first, n, i, ok, ret;

	run_setup(s, "apple,t6030-ane", ctrl);
	dm_msgs_left = msgs_left;
	dm_nostart = nostart;
	ret = probe_now();
	snprintf(name, sizeof(name), "%s: probe returns 0, RESULT stage=5 PASS %s", tag,
		 nostart ? "core-never-left-stopped" : "core-started-and-stopped");
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, nostart ? "core-never-left-stopped" : "core-started-and-stopped"), name);
	n = run_writes(s, w, 8, &first);
	ok = n == n_want_w;
	for (i = 0; ok && i < n; i++)
		ok = w[i] == want_w[i];
	snprintf(name, sizeof(name), "%s: engine writes are exactly the %d CPU_CONTROL values", tag, n_want_w);
	chk(ok, name);
	snprintf(name, sizeof(name), "%s: every write is at CPU_CONTROL, nothing written elsewhere", tag);
	chk(events_in(s->engine_pa + s->cpu_control, 4, 1) == n && events_in(0, ~0ULL, 1) == n &&
	    sim_bus_faults == 0, name);
	snprintf(name, sizeof(name), "%s: preflight reads (6 words) all precede the first write", tag);
	{
		u32 want[6] = { s->cpu_status, s->cpu_control, s->rvbar, s->rvbar + 4,
				s->mbox + 0x110, s->mbox + 0x114 };
		int k, seen_all = 1;

		for (k = 0; k < 6; k++) {
			int found = 0;

			for (i = 0; i < first; i++)
				if (!sim_events[i].write && sim_events[i].pa == s->engine_pa + want[k])
					found = 1;
			seen_all &= found;
		}
		chk(first > 0 && seen_all, name);
	}
	snprintf(name, sizeof(name), "%s: pmgr words read before the first engine event", tag);
	{
		int first_eng = -1, last_pmgr = -1;

		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
			if (sim_events[i].pa >= s->engine_pa && sim_events[i].pa < s->engine_pa + s->engine_size) {
				if (first_eng < 0)
					first_eng = i;
			} else {
				last_pmgr = i;
			}
		}
		chk(first_eng > last_pmgr && last_pmgr >= 4, name);
	}
	snprintf(name, sizeof(name), "%s: each write and the CPU_CONTROL read logged (%d write lines)", tag, n_want_w);
	chk(log_count("ane_h15 write CPU_CONTROL=") == n_want_w && log_count("ane_h15 read CPU_CONTROL") >= 1, name);
	snprintf(name, sizeof(name), "%s: %d message(s) popped, raw words logged, never a mailbox write", tag, want_msgs);
	chk(log_count("ane_h15 msg") == want_msgs &&
	    (want_msgs == 0 || log_count("raw0=0x110000abc0 raw1=0x2200001234") == want_msgs) &&
	    eng_events_at(s, s->mbox + 0x834) == 0 && eng_events_at(s, s->mbox + 0x83c) == 0 &&
	    eng_events_at(s, s->mbox + 0x830) == want_msgs && eng_events_at(s, s->mbox + 0x838) == want_msgs &&
	    events_in(s->engine_pa + s->mbox, 0x1000, 1) == 0, name);
	snprintf(name, sizeof(name), "%s: power claim released, no 'do NOT unload'", tag);
	chk(sim_pm_get == 1 && sim_pm_put == 1 && log_count("do NOT unload") == 0, name);
	ane_h15_remove(&pdev);
	snprintf(name, sizeof(name), "%s: remove unmaps both windows (core stopped again)", tag);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2 && sim_module_gets == 0, name);
}

/* stage 5, M3 next unit: after the wait, before RUN is cleared, log the final CPU_STATUS and both mailbox CTRL words on
 * one line. Read only, inside the ADT-published wrapper window. The interrupt-pending word (engine +0x184c000) is OUTSIDE
 * both published windows, so stage 5 must still NOT read it; stage 7 reads it only behind its table check. */
static void pos_run_postwait(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	const u32 pend_off = 0x184c000u, ack_off = 0x1850000u;
	int i, a2i_reads = 0, a2i_last = -1, clear_idx = -1, writes = 0, ret;

	run_setup(s, "apple,t6030-ane", 0x0);
	eng_set(s, pend_off, 0x4);
	ret = probe_now();
	chk(ret == 0, "RP1 stage 5 with a pending word set: probe returns 0");
	chk(events_in(s->engine_pa + pend_off, 4, 0) == 0 && events_in(s->engine_pa + ack_off, 4, 0) == 0,
	    "RP2 the pending and acknowledge words stay outside the stage-5 read allowlist (never touched)");
	chk(log_count("ane_h15 post-wait cpu_status=") == 1 && log_count("post-wait cpu_status=") == 1 &&
	    log_count("a2i_ctrl=") >= 1 && log_count("i2a_ctrl=") >= 1,
	    "RP3 one post-wait line carries CPU_STATUS and both mailbox CTRL words");
	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
		if (!sim_events[i].write && sim_events[i].pa == s->engine_pa + s->mbox + 0x110) {
			a2i_reads++;
			a2i_last = i;
		}
		if (sim_events[i].write && sim_events[i].pa == s->engine_pa + s->cpu_control) {
			writes++;
			if (writes == 2)
				clear_idx = i;
		}
	}
	chk(a2i_reads == 2 && a2i_last >= 0 && clear_idx >= 0 && a2i_last < clear_idx,
	    "RP4 the A2I CTRL word is read once in the preflight and once after the wait, before RUN is cleared");
	chk(events_in(0, ~0ULL, 1) == writes && writes == 2 && sim_bus_faults == 0,
	    "RP5 the only writes are still the two CPU_CONTROL values");
	ane_h15_remove(&pdev);
}

static void pos_run(void)
{
	static const u32 w0[] = { 0x10, 0x0 };
	static const u32 w20[] = { 0x30, 0x20, 0x0 };

	pos_run_case("R1 T6030 ctrl=0", 0x0, 0, false, 0, w0, 2);
	pos_run_case("R2 T6030 ctrl=0x20 (bit 5 cleared too)", 0x20, 0, false, 0, w20, 3);
	pos_run_case("R3 T6030 two messages", 0x0, 2, false, 2, w0, 2);
	pos_run_case("R4 T6030 never-ending sender, capped at 4", 0x0, 99, false, 4, w0, 2);
	pos_run_case("R5 T6030 core does not leave STOPPED", 0x0, 0, true, 0, w0, 2);
}

/* stage 5 with run_scratch=1: eight SCRATCH words read before RUN and again after the wait, never written;
 * without the parameter the SCRATCH window is not touched at all */
static void pos_run_scratch(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	const u32 off = 0x1840050u;	/* ANE_H15_SCRATCH_OFF, the T6030 case */
	unsigned int rstage;
	char verdict[32], reason[128];
	int i, first, k, n_touch, before_first = 0, ret;
	u32 w[8];

	run_setup(s, "apple,t6030-ane", 0x0);
	ret = probe_now();
	for (i = 0, n_touch = 0; i < 8; i++)
		n_touch += eng_events_at(s, off + 4 * i);
	chk(ret == 0 && n_touch == 0 && log_count("ane_h15 scratch") == 0,
	    "RS1 run_scratch off: no SCRATCH word is read, none is logged");
	ane_h15_remove(&pdev);

	run_setup(s, "apple,t6030-ane", 0x0);
	run_scratch = true;
	for (i = 0; i < 8; i++)
		eng_set(s, off + 4 * i, 0x100 + i);
	eng_set(s, off + 4 * 7, 0x08042006);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) && rstage == 5 &&
	    !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"),
	    "RS2 run_scratch on: the verdict of stage 5 is unchanged");
	for (i = 0, n_touch = 0; i < 8; i++)
		n_touch += eng_events_at(s, off + 4 * i) == 2;
	chk(n_touch == 8 && events_in(s->engine_pa + off, 0x20, 1) == 0,
	    "RS3 each of the eight words is read exactly twice and never written");
	chk(log_count("ane_h15 scratch before RUN: S0=0x100 S1=0x101") == 1 &&
	    log_count("ane_h15 scratch after the wait: S0=0x100") == 1 &&
	    log_count("S7=0x8042006") == 2,
	    "RS4 one 'before RUN' line and one 'after the wait' line with the eight values");
	run_writes(s, w, 8, &first);
	for (k = 0; k < first && k < sim_n_events && k < SIM_MAX_EVENTS; k++)
		if (!sim_events[k].write && sim_events[k].pa == s->engine_pa + off)
			before_first++;
	chk(first > 0 && before_first == 1,
	    "RS5 SCRATCH0 is read once before the first CPU_CONTROL write (the 'before RUN' dump)");
	chk(events_in(0, ~0ULL, 1) == events_in(s->engine_pa + s->cpu_control, 4, 1) && sim_bus_faults == 0,
	    "RS6 the only writes are the CPU_CONTROL values, no fault");
	ane_h15_remove(&pdev);
}

/* stage 5 with run_pmgr=1: the five pmgr ANE words read before RUN, just after the RUN write and after the wait, never written;
 * without the parameter no pmgr dump line is logged */
static void pos_run_pmgr(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128];
	const char *before, *run_write, *after_run, *after_wait;
	int ret;

	run_setup(s, "apple,t6030-ane", 0x0);
	ret = probe_now();
	chk(ret == 0 && log_count("ane_h15 pmgr before RUN") == 0 && log_count("ane_h15 pmgr after") == 0,
	    "PM1 run_pmgr off: no pmgr dump line is logged");
	ane_h15_remove(&pdev);

	run_setup(s, "apple,t6030-ane", 0x0);
	run_pmgr = true;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) && rstage == 5 &&
	    !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"),
	    "PM2 run_pmgr on: the verdict of stage 5 is unchanged");
	chk(log_count("ane_h15 pmgr before RUN: ANE_SYS=0xffffffff ANE_MPM=0xffffffff ANE_CPU=0xffffffff ANE_TD=0xffffffff ANE_BASE=0xffffffff") == 1 &&
	    log_count("ane_h15 pmgr after RUN: ANE_SYS=0xffffffff ANE_MPM=0xffffffff ANE_CPU=0xffffffff ANE_TD=0xffffffff ANE_BASE=0xffffffff") == 1 &&
	    log_count("ane_h15 pmgr after the wait: ANE_SYS=0xffffffff ANE_MPM=0xffffffff ANE_CPU=0xffffffff ANE_TD=0xffffffff ANE_BASE=0xffffffff") == 1,
	    "PM3 one line each before RUN, after RUN and after the wait, with the five words");
	before = strstr(sim_log, "ane_h15 pmgr before RUN");
	run_write = strstr(sim_log, "(RUN)");
	after_run = strstr(sim_log, "ane_h15 pmgr after RUN");
	after_wait = strstr(sim_log, "ane_h15 pmgr after the wait");
	chk(before && run_write && after_run && after_wait && before < run_write && run_write < after_run &&
	    after_run < after_wait,
	    "PM4 the dumps come before the RUN write, right after it, and after the wait, in that order");
	chk(events_in(0, ~0ULL, 1) == events_in(s->engine_pa + s->cpu_control, 4, 1) && sim_bus_faults == 0,
	    "PM5 the only writes are the CPU_CONTROL values, nothing is written to pmgr, no fault");
	ane_h15_remove(&pdev);
}

/* stage 5 with scratch_boot=1: macOS's cold-boot order, never run on an M3. Zero SCRATCH0..7, SCRATCH6=1, SCRATCH7=0 with a read back, CPU_CONTROL 0
 * then 0x10 (RUN), then poll SCRATCH7 for 0x08042006. The RESULT line carries
 * scratch_boot=1 s7_ready=<yes|no>; the verdict rules do not change. */
static void pos_run_scratch_boot(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	const u32 off = 0x1840050u;	/* ANE_H15_SCRATCH_OFF, the T6030 case */
	unsigned int rstage;
	char verdict[32], reason[128], name[128];
	int i, n, first, ret, before = fails;
	u32 w[16];

	/* KB1: scratch_boot off (default) keeps the old write sequence byte for byte. */
	run_setup(s, "apple,t6030-ane", 0x0);
	ret = probe_now();
	n = run_writes(s, w, 16, &first);
	chk(ret == 0 && n == 2 && w[0] == 0x10 && w[1] == 0x0 &&
	    events_in(s->engine_pa + off, 0x20, 1) == 0,
	    "KB1 scratch_boot off: the engine writes stay [CPU_CONTROL=0x10, CPU_CONTROL=0], nothing at SCRATCH");
	ane_h15_remove(&pdev);

	/* KB2: scratch_boot=1, the firmware publishes the ready word. SCRATCH7 starts
	 * holding the stale ready word (the T6030 observation the hypothesis names). */
	run_setup(s, "apple,t6030-ane", 0x0);
	scratch_boot = true;
	sb_wait_ms = 50;
	dm_s7_ready = true;
	eng_set(s, off + 4 * 7, 0x08042006);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) && rstage == 5 &&
	    !strcmp(verdict, "PASS") && !strcmp(reason, "scratch_boot=1 s7_ready=yes core-started-and-stopped"),
	    "KB2 scratch_boot=1 ready seen: PASS, reason scratch_boot=1 s7_ready=yes core-started-and-stopped");
	{
		const struct { u32 off; u32 val; } want[] = {
			{ off + 4 * 0, 0 }, { off + 4 * 1, 0 }, { off + 4 * 2, 0 }, { off + 4 * 3, 0 },
			{ off + 4 * 4, 0 }, { off + 4 * 5, 0 }, { off + 4 * 6, 0 }, { off + 4 * 7, 0 },
			{ off + 4 * 6, 1 }, { off + 4 * 7, 0 },
			{ s->cpu_control, 0 }, { s->cpu_control, 0x10 },
			{ s->cpu_control, 0 },	/* the stop path's clear RUN; bit 5 already clear */
		};
		u32 offs[16], vals[16];
		int nw = 0, ok;

		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
			if (!sim_events[i].write || sim_events[i].pa < s->engine_pa ||
			    sim_events[i].pa >= s->engine_pa + s->engine_size || nw >= 16)
				continue;
			offs[nw] = sim_events[i].pa - s->engine_pa;
			vals[nw] = sim_events[i].val;
			nw++;
		}
		ok = nw == 13;
		for (i = 0; ok && i < 13; i++)
			ok = offs[i] == want[i].off && vals[i] == want[i].val;
		chk(ok, "KB2 the engine writes are exactly S0..S7=0, S6=1, S7=0, CPU_CONTROL=0, CPU_CONTROL=0x10, then the stop clear");
	}
	chk(events_in(s->engine_pa + off, 0x20, 1) == 10 && events_in(0, ~0ULL, 1) == 13,
	    "KB2 ten SCRATCH writes (eight zeros, S6=1, S7=0) and no write outside the two words' ranges");
	chk(eng_events_at(s, s->rvbar) == 1 && eng_events_at(s, s->rvbar + 4) == 1,
	    "KB2 RVBAR is read once (preflight) and never written");
	chk(log_count("ane_h15 write SCRATCH") == 10 && log_count("ane_h15 write CPU_CONTROL=") == 3,
	    "KB2 every write logged before it happens (10 SCRATCH lines, 3 CPU_CONTROL lines)");
	chk(log_count("SCRATCH7 reads 0 after the cold-boot write") == 1 &&
	    log_count("ready word 0x8042006 after 1ms") == 1,
	    "KB2 SCRATCH7 read back after the cold-boot write, ready word logged at its first poll");
	chk(log_count("ane_h15 scratch before RUN") == 1 && log_count("ane_h15 scratch after the wait") == 1,
	    "KB2 the SCRATCH dump runs before RUN and after the wait without run_scratch");
	ane_h15_remove(&pdev);

	/* KB3: scratch_boot=1, the ready word never comes (the firmware idles, the hypothesis). */
	run_setup(s, "apple,t6030-ane", 0x0);
	scratch_boot = true;
	sb_wait_ms = 20;
	eng_set(s, off + 4 * 7, 0x08042006);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) && rstage == 5 &&
	    !strcmp(verdict, "PASS") && !strcmp(reason, "scratch_boot=1 s7_ready=no core-started-and-stopped"),
	    "KB3 scratch_boot=1 ready not seen: the reason carries scratch_boot=1 s7_ready=no");
	chk(log_count("not seen within 20ms") == 1, "KB3 the timeout is logged once with the sb_wait_ms value");
	n = run_writes(s, w, 16, &first);
	chk(n == 13 && eng_events_at(s, off + 4 * 7) == 2 + 1 + 20 + 2,
	    "KB3 SCRATCH7 is written twice, read back, polled 20 times and dumped twice");
	ane_h15_remove(&pdev);

	/* KB4: scratch_boot=1 is stage 5 only; stage 2 and stage 4 refuse before any MMIO.
	 * No remove(): a probe refused this early never ran devm_kzalloc, so drvdata is NULL. */
	run_setup(s, "apple,t6030-ane", 0x0);
	stage = "wrapper";
	scratch_boot = true;
	ret = probe_now();
	chk(ret != 0 && log_count("scratch_boot=1 is stage=run (5) only") == 1 &&
	    events_in(0, ~0ULL, 0) == 0 && sim_n_iomap == 0,
	    "KB4 stage=2 with scratch_boot=1 refuses before any bus access");
	run_setup(s, "apple,t6030-ane", 0x0);
	stage = "dart";
	scratch_boot = true;
	ret = probe_now();
	chk(ret != 0 && log_count("scratch_boot=1 is stage=run (5) only") == 1 &&
	    events_in(0, ~0ULL, 0) == 0 && sim_n_iomap == 0,
	    "KB4 stage=4 with scratch_boot=1 refuses before any bus access");

	/* KB5: sb_wait_ms outside 10 to 5000 refuses stage 5 before any write. */
	run_setup(s, "apple,t6030-ane", 0x0);
	scratch_boot = true;
	sb_wait_ms = 9;
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) && rstage == 5 &&
	    !strcmp(verdict, "REFUSED") && strstr(reason, "sb_wait_ms") &&
	    run_writes(s, w, 16, &first) == 0 && sim_bus_faults == 0,
	    "KB5 sb_wait_ms=9 refuses with no write");
	ane_h15_remove(&pdev);

	/* KB6: the SCRATCH7 clear does not take (the register ignores the write and keeps
	 * reading the stale ready word). The run must stop before the first CPU_CONTROL
	 * write: the core never started, nothing is pinned and the result names the cause.
	 * The firmware never starts and never publishes READY (dm_nostart, dm_s7_ready off),
	 * so a s7_ready=yes here could only come from the stale word. */
	run_setup(s, "apple,t6030-ane", 0x0);
	scratch_boot = true;
	sb_wait_ms = 50;
	dm_nostart = true;
	dm_s7_stuck = true;
	eng_set(s, off + 4 * 7, 0x08042006);
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) && rstage == 5 &&
	    !strcmp(verdict, "FAIL") && !strcmp(reason, "scratch_boot=1 s7_ready=no clear-failed"),
	    "KB6 stale S7 survives the clear: FAIL, reason scratch_boot=1 s7_ready=no clear-failed");
	n = run_writes(s, w, 16, &first);
	chk(n == 10 && events_in(s->engine_pa + s->cpu_control, 4, 1) == 0,
	    "KB6 the ten SCRATCH writes happen and no CPU_CONTROL write at all");
	chk(log_count("SCRATCH7 reads 0x8042006 after the cold-boot write") == 1,
	    "KB6 the non-zero readback value is logged");
	chk(sim_pm_get == sim_pm_put && sim_n_iomap == sim_n_iounmap && sim_module_gets == 0 &&
	    log_count("do NOT unload") == 0 && log_count("clear RUN") == 0,
	    "KB6 power claim released, windows unmapped, module not pinned, stop path not entered");
	ane_h15_remove(&pdev);

	/* KB7: control with the same setup, the clear takes. The run goes on, the poll
	 * times out and the result stays PASS with s7_ready=no and the core never left
	 * STOPPED (the model never starts it). */
	run_setup(s, "apple,t6030-ane", 0x0);
	scratch_boot = true;
	sb_wait_ms = 20;
	dm_nostart = true;
	eng_set(s, off + 4 * 7, 0x08042006);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) && rstage == 5 &&
	    !strcmp(verdict, "PASS") && !strcmp(reason, "scratch_boot=1 s7_ready=no core-never-left-stopped"),
	    "KB7 same setup, clear takes: PASS, reason scratch_boot=1 s7_ready=no core-never-left-stopped");
	n = run_writes(s, w, 16, &first);
	chk(n == 13 && eng_events_at(s, off + 4 * 7) == 2 + 1 + 20 + 2,
	    "KB7 the boot sequence and the poll run as before the clear check");
	ane_h15_remove(&pdev);

	printf("  %-58s %s\n", "pos_run_scratch_boot summary", fails == before ? "all KB checks PASS" : "FAILURES above");
}

/* stage 5 with pwgate_t6031: macOS's PWGATE step on a T6031,
 * never run on an M3. The device model
 * holds a configurable word at PWGATE + 0x199c (the window at 0x292290000,
 * 0x4000, the fifth reg entry of the ADT ane0 node): mode 1 writes 0 once
 * when bits 1:0 are set and the gate then opens at the first poll; a stuck
 * register ignores the write (the negative controls); mode 2 writes 0 even
 * when the word already reads 0 (the unconditional macOS order). The step
 * must sit between the preflight (and the macOS SCRATCH sequence, when
 * scratch_boot=1) and the first CPU_CONTROL write. */
static void pos_run_pwgate(void)
{
	const struct ane_h15_soc *s = &ane_t6031_soc;
	unsigned int rstage;
	char verdict[32], reason[192];
	int i, first, n, ret, before = fails;
	u32 w[8];

	/* PW1 (a): pwgate_t6031 off (default). The run is byte-identical to
	 * before: the two CPU_CONTROL writes, no PWGATE access of any kind,
	 * no third window mapped, no suffix on the RESULT line. */
	run_setup(s, "apple,t6031-ane", 0x0);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"),
	    "PW1 default off: PASS core-started-and-stopped, no pwgate suffix");
	chk(events_in(PW_PA, PW_SZ, 0) == 0 && log_count("PWGATE") == 0,
	    "PW1 default off: no PWGATE access of any kind, none logged");
	n = run_writes(s, w, 8, &first);
	chk(n == 2 && w[0] == 0x10 && w[1] == 0x0,
	    "PW1 default off: the engine writes stay [CPU_CONTROL=0x10, CPU_CONTROL=0]");
	chk(sim_n_iomap == 2 && sim_n_iounmap == 0,
	    "PW1 default off: only the two ADT windows mapped, none released by the run");
	ane_h15_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "PW1 default off: remove unmaps exactly the two windows");

	/* PW2 (b): pwgate_t6031=1 with bits 1:0 set. Exactly one write of 0,
	 * then the poll opens the gate, then the normal RUN sequence; the
	 * RESULT line carries the opened suffix with the final word. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 1;
	dm_pw_word = 0x1237u;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "core-started-and-stopped pwgate=opened word=0x1234"),
	    "PW2 word 0x1237: PASS, reason core-started-and-stopped pwgate=opened word=0x1234");
	{
		int wi = -1, ri = -1, cc = -1;

		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
			if (sim_events[i].pa != PW_WORD_PA)
				continue;
			if (sim_events[i].write) {
				if (wi < 0)
					wi = i;
			} else if (ri < 0) {
				ri = i;
			}
		}
		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++)
			if (sim_events[i].write && sim_events[i].pa == s->engine_pa + s->cpu_control) {
				cc = i;
				break;
			}
		chk(wi >= 0 && ri >= 0 && ri < wi && sim_events[wi].val == 0,
		    "PW2 exactly one write of 0, and the first read of the word precedes it");
		chk(cc > wi, "PW2 the PWGATE write happens before the first CPU_CONTROL write");
	}
	chk(events_in(PW_WORD_PA, 4, 1) == 1,
	    "PW2 exactly one write inside the whole PWGATE window");
	chk(log_count("ane_h15 read PWGATE pa=0x29229199c") == 2 &&
	    log_count("ane_h15 write PWGATE=0x0 pa=0x29229199c") == 1,
	    "PW2 the word is read before the write and once in the poll; the write is logged before it happens");
	chk(log_count("ane_h15 pwgate open after 1 poll") == 1,
	    "PW2 the poll opens at its first pass and the final word is logged");
	n = run_writes(s, w, 8, &first);
	chk(n == 2 && w[0] == 0x10 && w[1] == 0x0,
	    "PW2 the engine writes stay the normal RUN sequence");
	chk(sim_pm_get == 1 && sim_pm_put == 1, "PW2 power claim released");
	ane_h15_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 3,
	    "PW2 the PWGATE window was mapped and unmapped again (remove unmaps engine and pmgr)");

	/* PW3 (c): pwgate_t6031=1, the word already 0. A read, no write, no
	 * poll, the already-open suffix. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 1;
	dm_pw_word = 0;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "core-started-and-stopped pwgate=already-open"),
	    "PW3 word already 0: PASS, reason core-started-and-stopped pwgate=already-open");
	chk(events_in(PW_WORD_PA, 4, 1) == 0 && log_count("ane_h15 write PWGATE") == 0,
	    "PW3 nothing is written to the PWGATE window");
	chk(log_count("ane_h15 read PWGATE pa=0x29229199c") == 1 &&
	    log_count("pwgate already open") == 1,
	    "PW3 one logged read and the already-open line once");
	n = run_writes(s, w, 8, &first);
	chk(n == 2 && w[0] == 0x10 && w[1] == 0x0,
	    "PW3 the engine writes stay the normal RUN sequence");
	ane_h15_remove(&pdev);

	/* PW4 (d): pwgate_t6031=2 with the word 0. The unconditional macOS
	 * order writes 0 anyway, exactly once, and the suffix names the
	 * forced write. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 2;
	dm_pw_word = 0;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "core-started-and-stopped pwgate=forced word=0x0"),
	    "PW4 mode 2 word 0: PASS, reason core-started-and-stopped pwgate=forced word=0x0");
	chk(events_in(PW_WORD_PA, 4, 1) == 1 &&
	    log_count("ane_h15 write PWGATE=0x0 pa=0x29229199c") == 1,
	    "PW4 exactly one forced write with its log line");
	n = run_writes(s, w, 8, &first);
	chk(n == 2 && w[0] == 0x10 && w[1] == 0x0,
	    "PW4 the engine writes stay the normal RUN sequence");
	ane_h15_remove(&pdev);

	/* PW5 (h): scratch_boot=1 with pwgate_t6031=1. The macOS SCRATCH sequence
	 * is unchanged; the PWGATE step sits after the last SCRATCH write and
	 * before the first CPU_CONTROL write, the last action before RUN. */
	run_setup(s, "apple,t6031-ane", 0x0);
	scratch_boot = true;
	sb_wait_ms = 50;
	pwgate_t6031 = 1;
	dm_pw_word = 0x1237u;
	dm_nostart = true;	/* the model never starts the core: s7_ready=no */
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "scratch_boot=1 s7_ready=no core-never-left-stopped pwgate=opened word=0x1234"),
	    "PW5 scratch_boot=1: PASS with the pwgate suffix after the s7_ready reason");
	{
		const u32 soff = 0x1840050u;
		int last_scr = -1, pw_ri = -1, pw_wi = -1, cc = -1, nw = 0, ok;

		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
			u64 pa = sim_events[i].pa;

			if (!sim_events[i].write) {
				if (pa == PW_WORD_PA && pw_ri < 0)
					pw_ri = i;
				continue;
			}
			if (pa == PW_WORD_PA) {
				if (pw_wi < 0)
					pw_wi = i;
				continue;
			}
			if (pa < s->engine_pa || pa >= s->engine_pa + s->engine_size)
				continue;
			nw++;
			if (pa >= s->engine_pa + soff && pa < s->engine_pa + soff + 0x20)
				last_scr = i;
			else if (pa == s->engine_pa + s->cpu_control && cc < 0)
				cc = i;
		}
		ok = nw == 13 && last_scr >= 0 && pw_ri >= 0 && pw_wi >= 0 && cc >= 0 &&
		     last_scr < pw_ri && pw_ri < pw_wi && pw_wi < cc;
		chk(ok, "PW5 the order is the 13 SCRATCH-sequence writes, SCRATCH last, then PWGATE read, write, first CPU_CONTROL write");
	}
	chk(log_count("ane_h15 write SCRATCH") == 10 && log_count("ane_h15 write CPU_CONTROL=") == 3,
	    "PW5 the macOS SCRATCH and CPU_CONTROL write counts are unchanged");
	ane_h15_remove(&pdev);

	printf("  %-58s %s\n", "pos_run_pwgate summary", fails == before ? "all PW checks PASS" : "FAILURES above");
}

static int neg_run(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	static const struct { const struct ane_h15_soc *soc; const char *compat; } others[] = {
		{ &ane_t6034_soc, "apple,t6034-ane" },
	};
	static const struct { const char *tag; u32 off; u32 val; } bad[] = {
		{ "N4 core already running", 0, 0x1 },
		{ "N5 core not stopped", 0, 0x8 },
		{ "N6 RVBAR_LO differs", 1, 0x2 },
		{ "N7 RVBAR_HI differs", 2, 0x101 },
		{ "N8 A2I mailbox not empty", 3, 0x1 },
		{ "N9 I2A mailbox not empty", 4, 0x1 },
		{ "N10 RUN already set", 5, 0x10 },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[128];
	u32 w[8];
	int first, i, ret, before;

	/* N1 no confirm_run: nothing touched at all */
	run_setup(s, "apple,t6030-ane", 0);
	confirm_run = false;
	ret = probe_now();
	run_refused_clean(s, ret, "REFUSED", "confirm_run=1", "N1 no confirm_run");
	chk(sim_n_events == 0, "N1 no confirm_run: zero bus events of any kind");
	/* N2 run_wait_ms out of range */
	for (i = 0; i < 2; i++) {
		run_setup(s, "apple,t6030-ane", 0);
		run_wait_ms = i ? 5000 : 5;
		ret = probe_now();
		run_refused_clean(s, ret, "REFUSED", "run_wait_ms", i ? "N2 run_wait_ms=5000" : "N2 run_wait_ms=5");
		chk(sim_n_events == 0, "N2 run_wait_ms: zero bus events of any kind");
	}
	/* N3 SoC rows with no measured RVBAR */
	for (i = 0; i < (int)ARRAY_SIZE(others); i++) {
		run_setup(others[i].soc, others[i].compat, 0);
		ret = probe_now();
		snprintf(name, sizeof(name), "N3 %s no measured RVBAR", others[i].soc->name);
		run_refused_clean(others[i].soc, ret, "REFUSED", "no measured RVBAR", name);
		chk(sim_n_events == 0, "N3 no measured RVBAR: zero bus events of any kind");
	}
	/* N3b t6031 has a measured RVBAR (issue 176, stage 2 on a J516C), so the gate passes and the stage runs */
	run_setup(&ane_t6031_soc, "apple,t6031-ane", 0);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"),
	    "N3b t6031: measured RVBAR, stage 5 runs to core-started-and-stopped");
	chk(log_count("ane_h15 write CPU_CONTROL=") == 2 && sim_bus_faults == 0,
	    "N3b t6031: exactly the RUN and clear-RUN writes, no fault");
	ane_h15_remove(&pdev);
	chk(sim_pm_get == 1 && sim_pm_put == 1 && sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "N3b t6031: power claim released, both windows unmapped");
	/* N4..N10 preflight mismatches: read-only, FAIL, nothing written */
	for (i = 0; i < (int)ARRAY_SIZE(bad); i++) {
		run_setup(s, "apple,t6030-ane", 0);
		switch (bad[i].off) {
		case 0: eng_set(s, s->cpu_status, bad[i].val); break;
		case 1: eng_set(s, s->rvbar, bad[i].val); break;
		case 2: eng_set(s, s->rvbar + 4, bad[i].val); break;
		case 3: eng_set(s, s->mbox + 0x110, bad[i].val); break;
		case 4: eng_set(s, s->mbox + 0x114, bad[i].val); break;
		default: eng_set(s, s->cpu_control, bad[i].val); break;
		}
		ret = probe_now();
		run_refused_clean(s, ret, "FAIL", "preflight-not-as-iboot-left-it", bad[i].tag);
	}
	/* N11 stuck pmgr word: refuses before any engine access */
	run_setup(s, "apple,t6030-ane", 0);
	set_ps_words(s, 2);
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "pmgr-actual-stuck", "N11 pmgr word stuck");
	chk(events_in(s->engine_pa, s->engine_size, 0) == 0, "N11 pmgr word stuck: zero engine events");
	/* N12 live reg[0] is not the SoC row's window */
	run_setup(s, "apple,t6030-ane", 0);
	node.reg_pa[0] += 0x1000;
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "window-mismatch", "N12 reg[0] shifted");
	chk(sim_n_events == 0, "N12 reg[0] shifted: zero bus events of any kind");
	/* N13 the core does not stop: probe succeeds on purpose, power and windows kept, remove refuses */
	run_setup(s, "apple,t6030-ane", 0);
	dm_stuck = true;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "FAIL") && !strcmp(reason, "core-not-stopped"),
	    "N13 stuck core: RESULT stage=5 FAIL core-not-stopped, probe returns 0");
	chk(run_writes(s, w, 8, &first) == 2 && w[0] == 0x10 && w[1] == 0x0,
	    "N13 stuck core: exactly one RUN and one clear-RUN write, no retry storm");
	chk(log_count("reboot required") >= 1 && log_count("do NOT unload") >= 1,
	    "N13 stuck core: reboot-required and do-NOT-unload logged");
	chk(sim_pm_get == 1 && sim_pm_put == 0 && sim_n_iounmap == 0,
	    "N13 stuck core: power claim and windows kept");
	chk(sim_module_gets == 1, "N13 stuck core: the module is pinned (rmmod cannot free it under the core)");
	before = sim_n_iounmap;
	ane_h15_remove(&pdev);
	chk(sim_n_iounmap == before && sim_pm_put == 0 && log_count("remove with the firmware released") == 1,
	    "N13 stuck core: remove() refuses");

	/* PW6 (e): pwgate_t6031=1 and the register ignores the 0 write: the
	 * poll runs its 5000 passes, the run stops BEFORE any CPU_CONTROL
	 * write, nothing is pinned, every window is unmapped. */
	{
		const struct ane_h15_soc *t = &ane_t6031_soc;

		run_setup(t, "apple,t6031-ane", 0x0);
		pwgate_t6031 = 1;
		dm_pw_word = 0x1237u;
		dm_pw_stuck = true;
		dm_nostart = true;	/* the firmware never runs and never publishes READY */
		ret = probe_now();
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 5 && !strcmp(verdict, "FAIL") && !strcmp(reason, "pwgate-not-open"),
		    "PW6 stuck gate: FAIL, reason pwgate-not-open");
		chk(run_writes(t, w, 16, &first) == 0 && log_count("ane_h15 write CPU_CONTROL") == 0,
		    "PW6 stuck gate: ZERO CPU_CONTROL writes, the core never started");
		chk(sim_module_gets == 0 && sim_pm_get == sim_pm_put &&
		    sim_n_iomap == 2 && sim_n_iounmap == 3 &&
		    log_count("do NOT unload") == 0 && log_count("clear RUN") == 0,
		    "PW6 stuck gate: nothing pinned, claims released, all three windows unmapped");
		chk(log_count("bits 1:0 still set after 5000 polls") == 1,
		    "PW6 stuck gate: the timeout is logged once with the poll count and the final word");
		ane_h15_remove(&pdev);

		/* PW7 (f): pwgate_t6031=3 refuses in probe before any bus
		 * access and before any mapping. */
		run_setup(t, "apple,t6031-ane", 0x0);
		pwgate_t6031 = 3;
		ret = probe_now();
		chk(ret != 0 && log_count("REFUSED reason=pwgate_t6031 0 to 2") == 1 &&
		    events_in(0, ~0ULL, 0) == 0 && sim_n_iomap == 0 && sim_bus_faults == 0,
		    "PW7 pwgate_t6031=3: refused before any bus access, no window mapped");

		/* PW8 (g): a row without a PWGATE window (t6034) refuses the parameter in probe
		 * before any MMIO and any mapping. */
		run_setup(&ane_t6034_soc, "apple,t6034-ane", 0);
		pwgate_t6031 = 1;
		ret = probe_now();
		chk(ret != 0 && log_count("pwgate_t6031: the t6034 row has no PWGATE window") == 1 &&
		    events_in(0, ~0ULL, 0) == 0 && sim_n_iomap == 0,
		    "PW8 t6034 pwgate_t6031=1: refused before any MMIO and any mapping");
	}
	return fails;
}

/* ---- stage 5 with pwgate_t6031 on the T8122 and T6030 rows ----
 * Every row with a PWGATE window runs the same step as the T6031 row. The addresses below are literals, so a
 * wrong window or word offset in the SoC table fails here. The step is the last action before the first
 * CPU_CONTROL write and writes the one PWGATE word, nothing else in the window. */
static void pw_order(const struct ane_h15_soc *s, u64 wpa, int *ri, int *wi, int *cc)
{
	int i;

	*ri = *wi = *cc = -1;
	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
		if (sim_events[i].pa == wpa) {
			if (sim_events[i].write) {
				if (*wi < 0)
					*wi = i;
			} else if (*ri < 0) {
				*ri = i;
			}
		} else if (sim_events[i].write && sim_events[i].pa == s->engine_pa + s->cpu_control && *cc < 0) {
			*cc = i;
		}
	}
}

/* steplog and stop_after_pwgate: the crash-forensics pair of the write step. */
static void pos_steplog_stop(void)
{
	const struct ane_h15_soc *s = &ane_t6031_soc;
	unsigned int rstage;
	char verdict[32], reason[128];
	int i, ret, before = fails;
	u32 w[8];
	int n, first;

	/* SL1: steplog=1 with the gate write. Every step line lands before its
	 * access; the fsync count matches the line count. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 1;
	steplog = "/tmp/sim-step.log";
	dm_pw_word = 0x1237u;
	ret = probe_now();
	chk(ret == 0, "SL1 the steplog run passes");
	{
		char *b = sim_steplog_buf;
		int lines = 0;

		for (i = 0; b[i]; i++)
			if (b[i] == '\n')
				lines++;
		chk(strstr(b, "read PWGATE pa=0x29229199c") && strstr(b, "write PWGATE=0x0 pa=0x29229199c") &&
		    strstr(b, "read CPU_CONTROL pa=") && strstr(b, "write CPU_CONTROL=0x10"),
		    "SL1 the log carries the gate read, the gate write, the CPU_CONTROL read and the RUN write");
		chk(strstr(b, "write PWGATE=0x0") < strstr(b, "write CPU_CONTROL=0x10"),
		    "SL1 the gate write precedes the RUN write in the log");
		chk((unsigned long)lines <= sim_steplog_fsyncs && lines > 0,
		    "SL1 every line is fsynced before the next access");
	}
	ane_h15_remove(&pdev);

	/* SL2: the steplog open fails: the run refuses before any MMIO. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 1;
	steplog = "/tmp/sim-step.log";
	sim_steplog_open_fail = 1;
	ret = probe_now();
	chk(ret != 0 && log_count("ane_h15 steplog: open") == 1 && events_in(0, ~0ULL, 0) == 0,
	    "SL2 a failed steplog open refuses the run before any bus access");
	ane_h15_remove(&pdev);

	/* SA1: stop_after_pwgate with the gate closed. The gate step is the whole
	 * run: no CPU_CONTROL write at all, the core stays untouched. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 1;
	stop_after_pwgate = true;
	dm_pw_word = 0x1237u;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "pwgate=opened word=0x1234 stop-after-pwgate"),
	    "SA1 the split run passes with the stop-after-pwgate reason");
	n = run_writes(s, w, 8, &first);
	chk(n == 0 && events_in(0, ~0ULL, 1) == 1,
	    "SA1 exactly one write in the whole run: the gate word, no engine write");
	chk(eng_events_at(s, s->cpu_control) >= 1,
	    "SA1 CPU_CONTROL is read (preflight) and never written");
	chk(sim_pm_get == 1 && sim_pm_put == 1, "SA1 the power claim is released");
	ane_h15_remove(&pdev);

	/* SA2: stop_after_pwgate without pwgate_t6031: refuse before any MMIO. */
	run_setup(s, "apple,t6031-ane", 0x0);
	stop_after_pwgate = true;
	ret = probe_now();
	chk(ret != 0 && log_count("stop_after_pwgate: needs pwgate_t6031") == 1 &&
	    events_in(0, ~0ULL, 0) == 0 && sim_n_iomap == 0,
	    "SA2 stop_after_pwgate without pwgate_t6031 refuses before any bus access");

	/* SA3: stop_after_pwgate with scratch_boot: refuse before any MMIO. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 1;
	scratch_boot = true;
	stop_after_pwgate = true;
	ret = probe_now();
	chk(ret != 0 && log_count("stop_after_pwgate: needs pwgate_t6031") == 1 &&
	    events_in(0, ~0ULL, 0) == 0,
	    "SA3 stop_after_pwgate with scratch_boot refuses before any bus access");

	/* SA4: stop_after_pwgate on stage 2: refuse before any MMIO. */
	run_setup(s, "apple,t6031-ane", 0x0);
	pwgate_t6031 = 0;
	stop_after_pwgate = true;
	stage = "wrapper";
	ret = probe_now();
	chk(ret != 0 && log_count("stop_after_pwgate: needs pwgate_t6031") == 1,
	    "SA4 stop_after_pwgate on another stage refuses before any bus access");

	printf("  %-58s %s\n", "pos_steplog_stop summary", fails == before ? "all SL/SA checks PASS" : "FAILURES above");
}

static void pos_run_pwgate_rows(void)
{
	static const struct {
		const struct ane_h15_soc *soc;
		const char *compat;
		u64 window, wpa;
		const char *read_log, *write_log;
	} rows[] = {
		{ &ane_t8122_soc, "apple,t8122-ane", 0x2d0724000ull, 0x2d072672cull,
		  "ane_h15 read PWGATE pa=0x2d072672c", "ane_h15 write PWGATE=0x0 pa=0x2d072672c" },
		{ &ane_t6030_soc, "apple,t6030-ane", 0x350724000ull, 0x350726b6cull,
		  "ane_h15 read PWGATE pa=0x350726b6c", "ane_h15 write PWGATE=0x0 pa=0x350726b6c" },
	};
	unsigned int rstage;
	char verdict[32], reason[192], name[160];
	int r, ri, wi, cc, n, first, ret, before = fails;
	u32 w[8];

	for (r = 0; r < (int)ARRAY_SIZE(rows); r++) {
		const struct ane_h15_soc *s = rows[r].soc;
		const char *t = s->name;

		/* WR1: default off. The run is what it was before: two CPU_CONTROL writes, no PWGATE access. */
		run_setup(s, rows[r].compat, 0x0);
		ret = probe_now();
		snprintf(name, sizeof(name), "WR1 %s default off: PASS core-started-and-stopped, no suffix", t);
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 5 && !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"), name);
		n = run_writes(s, w, 8, &first);
		snprintf(name, sizeof(name), "WR1 %s default off: no PWGATE access, engine writes [0x10, 0], two windows mapped", t);
		chk(events_in(rows[r].window, 0x4000, 0) == 0 && log_count("PWGATE") == 0 && n == 2 && w[0] == 0x10 &&
		    w[1] == 0 && sim_n_iomap == 2 && sim_n_iounmap == 0, name);
		ane_h15_remove(&pdev);

		/* WR2: mode 1, bits 1:0 set. One read, one write of 0 at the literal address, a poll, then RUN. */
		run_setup(s, rows[r].compat, 0x0);
		pwgate_t6031 = 1;
		dm_pw_word = 0x1237u;
		ret = probe_now();
		snprintf(name, sizeof(name), "WR2 %s word 0x1237: PASS, reason core-started-and-stopped pwgate=opened word=0x1234", t);
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 5 && !strcmp(verdict, "PASS") &&
		    !strcmp(reason, "core-started-and-stopped pwgate=opened word=0x1234"), name);
		pw_order(s, rows[r].wpa, &ri, &wi, &cc);
		snprintf(name, sizeof(name), "WR2 %s one write of 0 to the literal word, preceded by a read, before CPU_CONTROL", t);
		chk(wi >= 0 && ri >= 0 && ri < wi && sim_events[wi].val == 0 && cc > wi &&
		    events_in(rows[r].wpa, 4, 1) == 1 && events_in(rows[r].window, 0x4000, 1) == 1, name);
		snprintf(name, sizeof(name), "WR2 %s the read and write are logged before they happen, the poll opens at pass 1", t);
		chk(log_count(rows[r].read_log) == 2 && log_count(rows[r].write_log) == 1 &&
		    log_count("ane_h15 pwgate open after 1 poll") == 1, name);
		n = run_writes(s, w, 8, &first);
		snprintf(name, sizeof(name), "WR2 %s the engine writes stay [0x10, 0]; claims released", t);
		chk(n == 2 && w[0] == 0x10 && w[1] == 0 && sim_pm_get == 1 && sim_pm_put == 1, name);
		ane_h15_remove(&pdev);
		snprintf(name, sizeof(name), "WR2 %s the window was mapped and unmapped again", t);
		chk(sim_n_iomap == 2 && sim_n_iounmap == 3 && sim_bus_faults == 0, name);

		/* WR3: mode 1, the word already 0: a read, no write, no poll. */
		run_setup(s, rows[r].compat, 0x0);
		pwgate_t6031 = 1;
		dm_pw_word = 0;
		ret = probe_now();
		snprintf(name, sizeof(name), "WR3 %s word already 0: PASS pwgate=already-open, nothing written to the window", t);
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(reason, "core-started-and-stopped pwgate=already-open") &&
		    events_in(rows[r].window, 0x4000, 1) == 0 && log_count(rows[r].read_log) == 1, name);
		ane_h15_remove(&pdev);

		/* WR4: mode 2, word 0: the unconditional order writes 0 once. */
		run_setup(s, rows[r].compat, 0x0);
		pwgate_t6031 = 2;
		dm_pw_word = 0;
		ret = probe_now();
		snprintf(name, sizeof(name), "WR4 %s mode 2: one forced write to the literal word", t);
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(reason, "core-started-and-stopped pwgate=forced word=0x0") &&
		    events_in(rows[r].wpa, 4, 1) == 1 && events_in(rows[r].window, 0x4000, 1) == 1 &&
		    log_count(rows[r].write_log) == 1, name);
		ane_h15_remove(&pdev);

		/* WR5: the register ignores the write. The poll runs out, the run stops before any CPU_CONTROL write. */
		run_setup(s, rows[r].compat, 0x0);
		pwgate_t6031 = 1;
		dm_pw_word = 0x1237u;
		dm_pw_stuck = true;
		dm_nostart = true;
		ret = probe_now();
		snprintf(name, sizeof(name), "WR5 %s stuck gate: FAIL pwgate-not-open, zero CPU_CONTROL writes, one PWGATE write", t);
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 5 && !strcmp(verdict, "FAIL") && !strcmp(reason, "pwgate-not-open") &&
		    run_writes(s, w, 16, &first) == 0 && events_in(rows[r].wpa, 4, 1) == 1 &&
		    log_count("bits 1:0 still set after 5000 polls") == 1 && dm_pw_reads == 5001, name);
		snprintf(name, sizeof(name), "WR5 %s stuck gate: nothing pinned, claims released, windows unmapped", t);
		chk(sim_module_gets == 0 && sim_pm_get == sim_pm_put && sim_n_iomap == 2 && sim_n_iounmap == 3 &&
		    log_count("do NOT unload") == 0, name);
		ane_h15_remove(&pdev);

		/* WR6: a value other than 0, 1 or 2 refuses in probe before any bus access. */
		run_setup(s, rows[r].compat, 0x0);
		pwgate_t6031 = 3;
		ret = probe_now();
		snprintf(name, sizeof(name), "WR6 %s pwgate_t6031=3: refused before any bus access and any mapping", t);
		chk(ret != 0 && log_count("REFUSED reason=pwgate_t6031 0 to 2") == 1 && events_in(0, ~0ULL, 0) == 0 &&
		    sim_n_iomap == 0, name);

		/* WR7: the step is stage 5 only. Stage 2 with the parameter set refuses before any bus access. */
		stage2_setup(s, rows[r].compat, -1);
		pwgate_t6031 = 1;
		ret = probe_now();
		snprintf(name, sizeof(name), "WR7 %s pwgate_t6031=1 at stage 2: refused before any bus access and any mapping", t);
		chk(ret != 0 && log_count("REFUSED reason=pwgate_t6031: stage=run (5) only") == 1 &&
		    events_in(0, ~0ULL, 0) == 0 && sim_n_iomap == 0, name);
	}
	printf("  %-58s %s\n", "pos_run_pwgate_rows summary", fails == before ? "all WR checks PASS" : "FAILURES above");
}

/* ---- stage 2 with pwgate_read=1: the PWGATE word, read only ----
 * The word sits in the window the ADT ane0 node lists as its third reg entry on T6030 and T8122 (the
 * fifth on T6031). The step reads it once, logs the address first and the value after, reports closed
 * when bits 1:0 are nonzero, and writes nothing, on every row that has a window. */
static void pw_word_set(const struct ane_h15_soc *s, u32 v)
{
	sim_bus_map(s->pwgate_pa, s->pwgate_size);
	sim_bus[sim_region_find(s->pwgate_pa) - sim_bus].mem[s->pwgate_word >> 2] = v;
}

static void pos_pwgate_read(void)
{
	static const struct {
		const struct ane_h15_soc *soc;
		const char *compat;
	} rows[] = {
		{ &ane_t8122_soc, "apple,t8122-ane" },
		{ &ane_t6030_soc, "apple,t6030-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
	};
	/* Literal per-row facts, not read back from the rows: a wrong window or offset in the SoC table fails here. */
	static const struct { u64 pa; u32 word; u64 wpa; } lit[] = {
		{ 0x2d0724000ull, 0x272c, 0x2d072672cull },
		{ 0x350724000ull, 0x2b6c, 0x350726b6cull },
		{ 0x292290000ull, 0x199c, 0x29229199cull },
	};
	static const struct { u32 word; const char *gate; } vals[] = {
		{ 0x30000003u, "closed" },
		{ 0x30000001u, "closed" },
		{ 0x30000002u, "closed" },
		{ 0x30000004u, "open" },
		{ 0x30000000u, "open" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[192], want[128], line[96];
	size_t r, v;
	int ret, before = fails;

	for (r = 0; r < ARRAY_SIZE(rows); r++) {
		const struct ane_h15_soc *s = rows[r].soc;
		u64 word_pa = lit[r].wpa;

		snprintf(name, sizeof(name), "PR0 %s: window, word offset and word address are the literals", s->name);
		chk(s->pwgate_pa == lit[r].pa && s->pwgate_size == 0x4000 && s->pwgate_word == lit[r].word &&
		    lit[r].pa + lit[r].word == lit[r].wpa, name);

		for (v = 0; v < ARRAY_SIZE(vals); v++) {
			stage2_setup(s, rows[r].compat, -1);
			pwgate_read = 1;
			pw_word_set(s, vals[v].word);
			ret = probe_now();
			snprintf(want, sizeof(want), "preflight-read-only pwgate=%s word=0x%x",
				 vals[v].gate, vals[v].word);
			snprintf(name, sizeof(name), "PR1 %s word %#x: PASS %s", s->name, vals[v].word, want);
			chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
			    rstage == 2 && !strcmp(verdict, "PASS") && !strcmp(reason, want), name);
			snprintf(name, sizeof(name), "PR1 %s word %#x: one read of the word, nothing else in the window",
				 s->name, vals[v].word);
			chk(events_in(word_pa, 4, 0) == 1 && events_in(s->pwgate_pa, s->pwgate_size, 0) == 1, name);
			snprintf(name, sizeof(name), "PR1 %s word %#x: zero bus writes anywhere, zero faults",
				 s->name, vals[v].word);
			chk(events_in(0, ~0ULL, 1) == 0 && sim_bus_faults == 0, name);
			snprintf(name, sizeof(name), "PR1 %s word %#x: window unmapped again", s->name, vals[v].word);
			chk(sim_n_iounmap == 1, name);
			snprintf(line, sizeof(line), "ane_h15 read PWGATE pa=%#llx", (unsigned long long)word_pa);
			snprintf(name, sizeof(name), "PR1 %s word %#x: address logged once, before the read", s->name,
				 vals[v].word);
			chk(log_count(line) == 1, name);
			snprintf(line, sizeof(line), "word=PWGATE pa=%#llx value=%#x gate=%s",
				 (unsigned long long)word_pa, vals[v].word, vals[v].gate);
			snprintf(name, sizeof(name), "PR1 %s word %#x: value logged with the gate state", s->name,
				 vals[v].word);
			chk(log_count(line) == 1, name);
			snprintf(name, sizeof(name), "PR1 %s word %#x: CPU_CONTROL never touched", s->name,
				 vals[v].word);
			chk(eng_events_at(s, s->cpu_control) == 0, name);
			ane_h15_remove(&pdev);
		}

		/* PR2: default off. The stage 2 run is byte-identical: no window mapped, no suffix. */
		stage2_setup(s, rows[r].compat, -1);
		pw_word_set(s, 0x30000003u);
		ret = probe_now();
		snprintf(name, sizeof(name), "PR2 %s default off: PASS preflight-read-only, window untouched", s->name);
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "PASS") && !strcmp(reason, "preflight-read-only") &&
		    events_in(s->pwgate_pa, s->pwgate_size, 0) == 0 && sim_n_iounmap == 0, name);
		ane_h15_remove(&pdev);
	}

	/* PR3: the refusals, each before any bus access and any mapping. */
	for (r = 0; r < 5; r++) {
		const struct ane_h15_soc *s = r == 4 ? &ane_t6034_soc : &ane_t8122_soc;
		const char *compat = r == 4 ? "apple,t6034-ane" : "apple,t8122-ane";
		const char *want_log[] = { "pwgate_read 0 or 1", "pwgate_read: stage=wrapper (2) only",
					   "pwgate_read: stage=wrapper (2) only", "pwgate_read: stage=wrapper (2) only",
					   "pwgate_read: the t6034 row has no PWGATE window" };

		stage2_setup(s, compat, -1);
		pwgate_read = r == 0 ? 2 : 1;
		if (r == 1)
			stage = "run";
		if (r == 2)
			fw_seg = "10000ac4000:b0000";
		if (r == 3) {
			stage = "run";
			pwgate_t6031 = 1;
		}
		ret = probe_now();
		snprintf(name, sizeof(name), "PR3 case %zu: refused (%s) before any MMIO and any mapping", r,
			 want_log[r]);
		chk(ret != 0 && events_in(0, ~0ULL, 0) == 0 && sim_n_iomap == 0 && sim_n_iounmap == 0 &&
		    log_count(want_log[r]) == 1, name);
	}

	printf("  %-58s %s\n", "pos_pwgate_read summary", fails == before ? "all PR checks PASS" : "FAILURES above");
}

/* ---- stage 4 (confirm_read=1): the read-only DART liveness scan ---- */
static void dart_setup(const struct ane_h15_soc *s, const char *compat, bool live)
{
	stage2_setup(s, compat, -1);
	stage = "dart";
	confirm_read = true;
	dart_set(s, live);
}

/* every engine event is a read inside the first DART window span, nothing is written anywhere */
static bool dart_only_reads(const struct ane_h15_soc *s)
{
	int i;

	if (events_in(0, ~0ULL, 1) || sim_bus_faults)
		return false;
	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
		u64 pa = sim_events[i].pa;

		if (pa >= s->engine_pa && pa < s->engine_pa + s->engine_size &&
		    (pa < s->engine_pa + s->dart_off || pa >= s->engine_pa + s->dart_off + 0x24000))
			return false;
	}
	return true;
}

static void pos_dart(void)
{
	static const struct { const struct ane_h15_soc *soc; const char *compat; } rows[] = {
		{ &ane_t8122_soc, "apple,t8122-ane" },
		{ &ane_t6030_soc, "apple,t6030-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[128], tag[32];
	size_t r;
	int ret;

	for (r = 0; r < ARRAY_SIZE(rows); r++) {
		const struct ane_h15_soc *s = rows[r].soc;

		snprintf(tag, sizeof(tag), "D1 %s", s->name);
		dart_setup(s, rows[r].compat, true);
		ret = probe_now();
		snprintf(name, sizeof(name), "%s: RESULT stage=4 PASS dart-live-stream-found", tag);
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 4 && !strcmp(verdict, "PASS") && !strcmp(reason, "dart-live-stream-found"), name);
		snprintf(name, sizeof(name), "%s: stream 3 and the summary logged, other windows not scanned", tag);
		chk(log_count("ane_h15 dart inst=0 sid=3 tcr=0x1 ttbr=0xabc001 enabled=1") == 1 &&
		    log_count("live_streams=1 locked_windows=0") == 1 &&
		    log_count("version differs from inst 0: not scanned") == 3, name);
		snprintf(name, sizeof(name), "%s: reads only, all inside the DART windows, none at CPU_CONTROL", tag);
		chk(dart_only_reads(s) && eng_events_at(s, s->cpu_control) == 0, name);
		snprintf(name, sizeof(name), "%s: groups logged before they are read", tag);
		chk(log_count("ane_h15 read DART inst=0 PARAMS3") == 1 &&
		    log_count("ane_h15 read DART inst=0 TCR[0..]") == 1, name);
		snprintf(name, sizeof(name), "%s: power claim released, windows unmapped on remove", tag);
		ane_h15_remove(&pdev);
		chk(sim_pm_get == 1 && sim_pm_put == 1 && sim_n_iomap == 2 && sim_n_iounmap == 2, name);
	}
	/* nothing enabled: the M6 situation, a PASS that says so */
	dart_setup(&ane_t6030_soc, "apple,t6030-ane", false);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "PASS") && !strcmp(reason, "dart-no-stream-enabled") &&
	    log_count("live_streams=0") == 1 && dart_only_reads(&ane_t6030_soc),
	    "D2 no enabled stream: PASS dart-no-stream-enabled, reads only");
	ane_h15_remove(&pdev);
	/* the lock bit is reported */
	dart_setup(&ane_t6030_soc, "apple,t6030-ane", true);
	eng_set(&ane_t6030_soc, ane_t6030_soc.dart_off + 0x208, 1);
	ret = probe_now();
	chk(ret == 0 && log_count("locked_windows=1") == 1, "D3 PROTECT_LOCK bit 0 is counted");
	ane_h15_remove(&pdev);
	/* enabled stream whose TTBR is not valid, TCR without translate, enable bit missing: not live */
	{
		static const struct { const char *tag; u32 off; u32 val; } nl[] = {
			{ "D4 TTBR not valid", 0x1400 + 12, 0x00abc000 },
			{ "D4 TCR translate clear", 0x1000 + 12, 0x2 },
			{ "D4 ENABLE bit clear", 0xc00, 0 },
		};
		size_t i;

		for (i = 0; i < ARRAY_SIZE(nl); i++) {
			dart_setup(&ane_t6030_soc, "apple,t6030-ane", true);
			eng_set(&ane_t6030_soc, ane_t6030_soc.dart_off + nl[i].off, nl[i].val);
			ret = probe_now();
			snprintf(name, sizeof(name), "%s: not counted as live", nl[i].tag);
			chk(ret == 0 && log_count("live_streams=0") == 1, name);
			ane_h15_remove(&pdev);
		}
	}
}

static void translation_setup(const struct ane_h15_soc *s, const char *compat)
{
	stage2_setup(s, compat, -1);
	node.has_iommus = true;
	fw_seg = "10000ac4000:b0000,10001544000:43c000";
	eng_set(s, s->rvbar, (u32)s->rvbar_expect);
	eng_set(s, s->rvbar + 4, s->rvbar_expect >> 32);
}

static void pos_translation(void)
{
	const struct ane_h15_soc *s = &ane_t6031_soc;
	static const struct {
		u64 pa;
		bool no_domain;
		int ret;
		const char *verdict;
		const char *reason;
	} cases[] = {
		{ 0, false, -ENOENT, "REFUSED", "translation-zero" },
		{ 0x10000ac4000ULL, false, 0, "PASS", "rvbar-text-entry-match" },
		{ 0x10001544000ULL, false, -EIO, "FAIL", "rvbar-text-entry-mismatch" },
		{ 0, true, -ENODEV, "REFUSED", "translation-domain-unavailable" },
	};
	unsigned int rstage;
	char verdict[32], reason[128];
	size_t i;
	int ret, before;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		translation_setup(s, "apple,t6031-ane");
		sim_default_dom_none = cases[i].no_domain;
		if (cases[i].pa) {
			sim_iocalls[0] = (struct sim_iocall) {
				SIM_IO_MAP, s->rvbar_expect & ~1ULL, cases[i].pa,
				SZ_16K, IOMMU_READ, 0
			};
			sim_n_iocalls = 1;
		}
		before = sim_n_iocalls;
		ret = probe_now();
		chk(ret == cases[i].ret &&
		    parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 2 && !strcmp(verdict, cases[i].verdict) &&
		    !strcmp(reason, cases[i].reason), cases[i].reason);
		if (!ret)
			chk(log_count("translation rvbar_iova=0x10000000000 "
				      "expected_pa=0x10000ac4000 resolved_pa=0x10000ac4000") == 1,
			    "T1 matched entry logs the IOVA, expected TEXT and resolved address");
		chk(sim_n_iocalls == before && events_in(0, ~0ULL, 1) == 0 &&
		    events_in(s->engine_pa, s->engine_size, 0) == 5 &&
		    eng_events_at(s, s->cpu_control) == 0 && sim_bus_faults == 0,
		    "T1 query changes no mapping and adds no engine access beyond preflight");
		if (!ret)
			ane_h15_remove(&pdev);
		chk(sim_n_iocalls == before,
		    "T1 teardown leaves the observed domain and mapping untouched");
	}
	translation_setup(&ane_t6034_soc, "apple,t6034-ane");
	ret = probe_now();
	chk(ret == -ENODEV &&
	    parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    !strcmp(verdict, "REFUSED") && !strcmp(reason, "no-measured-rvbar") &&
	    sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0,
	    "T2 absent measured RVBAR refuses translation without writes");
	translation_setup(s, "apple,t6031-ane");
	fw_seg = "invalid";
	ret = probe_now();
	chk(ret == -EINVAL &&
	    parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    !strcmp(verdict, "REFUSED") && !strcmp(reason, "fw_seg missing or unsafe") &&
	    sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0,
	    "T3 invalid firmware evidence refuses translation without writes");
	translation_setup(s, "apple,t6031-ane");
	eng_set(s, s->rvbar, (u32)s->rvbar_expect + 0x4000);
	ret = probe_now();
	chk(ret == -EINVAL &&
	    parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    !strcmp(verdict, "REFUSED") && !strcmp(reason, "rvbar-entry-changed") &&
	    sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0,
	    "T4 changed current RVBAR refuses the old expected entry");
}

static int neg_dart(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	/* no confirm_read: nothing touched */
	dart_setup(s, "apple,t6030-ane", true);
	confirm_read = false;
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "REFUSED") && sim_n_events == 0,
	    "ND1 no confirm_read: REFUSED, zero bus events");
	/* t6034: the ADT has no DART window for it */
	dart_setup(&ane_t6034_soc, "apple,t6034-ane", false);
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "REFUSED") && strstr(reason, "no ADT DART window") &&
	    sim_n_events == 0, "ND2 t6034: REFUSED, zero bus events");
	/* stuck pmgr word: no engine access at all */
	dart_setup(s, "apple,t6030-ane", true);
	set_ps_words(s, 1);
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "FAIL") && !strcmp(reason, "pmgr-actual-stuck") &&
	    events_in(s->engine_pa, s->engine_size, 0) == 0, "ND3 pmgr stuck: FAIL, zero engine events");
	/* shifted reg[0]: refused before any bus access */
	dart_setup(s, "apple,t6030-ane", true);
	node.reg_pa[0] += 0x1000;
	ret = probe_now();
	chk(ret != 0 && strstr(sim_log, "window mismatch") && sim_n_events == 0,
	    "ND4 reg[0] shifted: refused, zero bus events");
	/* window 0 does not read as a DART: stop after one read */
	dart_setup(s, "apple,t6030-ane", true);
	eng_set(s, s->dart_off + 0x08, 0);
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "FAIL") && events_in(s->engine_pa, s->engine_size, 0) == 1,
	    "ND5 PARAMS3 zero: FAIL after exactly one DART read");
	/* run (stage 5) with no live stream refuses without writing; run_unmapped=1 overrides */
	run_setup(s, "apple,t6030-ane", 0);
	dart_set(s, false);
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "no-dart-stream-enabled", "ND6 run, no live DART stream");
	chk(events_in(s->engine_pa + s->cpu_control, 4, 0) == 0, "ND6 run, no live DART stream: CPU_CONTROL never read");
	run_setup(s, "apple,t6030-ane", 0);
	dart_set(s, false);
	run_unmapped = true;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS"), "ND7 run_unmapped=1 runs without a live stream");
	ane_h15_remove(&pdev);
	/* run with an unreadable DART: refuses before the preflight */
	run_setup(s, "apple,t6030-ane", 0);
	eng_set(s, s->dart_off + 0x08, 0);
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "dart-read-refused-or-not-a-dart", "ND8 run, window 0 is not a DART");
	return fails;
}

/* ---- stage 5 with map_fw=1: attach to the DART and iommu_map the two firmware segments before RUN ---- */
#define SEGS_OK "100014e8000:c4000,100030c4000:2ac000"

static void attach_live(void)
{
	dart_set(dm_soc, true);	/* the DART driver programmed stream 3 at the attach */
}

static void map_setup(const struct ane_h15_soc *s, const char *compat)
{
	run_setup(s, compat, 0);
	dart_set(s, false);	/* iBoot left nothing enabled (the M6 measurement) */
	node.has_iommus = true;
	map_fw = true;
	fw_seg = SEGS_OK;
	sim_attach_hook = attach_live;
}

/* the iommu call sequence, as a string of codes */
static void io_seq(char *out, size_t sz)
{
	int k;
	size_t o = 0;

	for (k = 0; k < sim_n_iocalls && k < 64 && o + 2 < sz; k++)
		out[o++] = "?ATMUDF"[sim_iocalls[k].code];
	out[o] = 0;
}

static void pos_map(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], seq[80];
	u32 w[8];
	int ret, first, n;

	map_setup(s, "apple,t6030-ane");
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"),
	    "M1 map_fw: RESULT stage=5 PASS core-started-and-stopped");
	io_seq(seq, sizeof(seq));
	chk(!strcmp(seq, "AT" "MM" "UUDF"), "M1 map_fw: alloc, attach, map, map, then after the stop unmap, unmap, detach, free");
	chk(sim_n_iocalls >= 4 &&
	    sim_iocalls[2].iova == 0x10000000000ULL && sim_iocalls[2].pa == 0x100014e8000ULL &&
	    sim_iocalls[2].size == 0xc4000 && sim_iocalls[2].prot == IOMMU_READ &&
	    sim_iocalls[3].iova == 0x100000c4000ULL && sim_iocalls[3].pa == 0x100030c4000ULL &&
	    sim_iocalls[3].size == 0x2ac000 && sim_iocalls[3].prot == (IOMMU_READ | IOMMU_WRITE),
	    "M1 map_fw: TEXT 0x100014e8000 at the RVBAR entry 0x10000000000 read only, DATA 0x100030c4000 right behind it read+write");
	n = run_writes(s, w, 8, &first);
	chk(n == 2 && w[0] == 0x10 && w[1] == 0 && first >= 0 && sim_iocalls[3].ev <= first &&
	    sim_iocalls[1].ev >= 5, "M1 map_fw: both maps and the attach happen after the pmgr gate and before the RUN write");
	chk(log_count("live_streams=1") == 1 && sim_n_dom_live == 0, "M1 map_fw: the scan after the attach finds the live stream; the domain is gone after the stop");
	chk(events_in(0, ~0ULL, 1) == n && sim_pm_get == 1 && sim_pm_put == 1, "M1 map_fw: only CPU_CONTROL written, power claim released");
	ane_h15_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2, "M1 map_fw: remove unmaps both windows");
}

static int neg_map(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	static const struct { const char *tag; const char *seg; } bad[] = {
		{ "NM2 fw_seg absent", NULL },
		{ "NM3 fw_seg malformed", "zz" },
		{ "NM4 TEXT pa not 16 KiB aligned", "100014e9000:c4000,100030c4000:2ac000" },
		{ "NM4 DATA size not 16 KiB aligned", "100014e8000:c4000,100030c4000:2ac001" },
		{ "NM4 TEXT size zero", "100014e8000:0,100030c4000:2ac000" },
		{ "NM4 DATA over 64 MiB", "100014e8000:c4000,100030c4000:4004000" },
		{ "NM4 segments overlap", "100014e8000:c4000,100014f0000:2ac000" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], seq[80], name[128];
	u32 w[8];
	int ret, first, i;

	/* NM1 no iommus in the node: refused before any bus access, no iommu call */
	map_setup(s, "apple,t6030-ane");
	node.has_iommus = false;
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "REFUSED") && sim_n_events == 0 && sim_n_iocalls == 0,
	    "NM1 no iommus: REFUSED, zero bus events, zero iommu calls");
	for (i = 0; i < (int)ARRAY_SIZE(bad); i++) {
		map_setup(s, "apple,t6030-ane");
		fw_seg = (char *)bad[i].seg;
		ret = probe_now();
		snprintf(name, sizeof(name), "%s: REFUSED, zero bus events, zero iommu calls", bad[i].tag);
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "REFUSED") && sim_n_events == 0 && sim_n_iocalls == 0, name);
	}
	/* NM5 a segment inside system RAM */
	for (i = REGION_INTERSECTS; i <= REGION_MIXED; i += 2) {
		map_setup(s, "apple,t6030-ane");
		sim_region_ram = i;
		ret = probe_now();
		snprintf(name, sizeof(name), "NM5 region_intersects=%d: REFUSED, zero bus events, zero iommu calls", i);
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "REFUSED") && sim_n_events == 0 && sim_n_iocalls == 0, name);
	}
	/* NM15 the core is already running (STOPPED clear, then RUNNING set): nothing mapped, nothing written */
	for (i = 0; i < 2; i++) {
		map_setup(s, "apple,t6030-ane");
		eng_set(s, s->cpu_status, i ? 0x30002b : 0x300028);
		ret = probe_now();
		snprintf(name, sizeof(name), "NM15 core %s: FAIL core-not-stopped-before-map, no iommu call, no write",
			 i ? "RUNNING" : "not STOPPED");
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "FAIL") && !strcmp(reason, "core-not-stopped-before-map") &&
		    sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0, name);
	}
	{
		static const struct { u32 off, val; const char *tag; } preflight[] = {
			{ 0x1400044, 0x10, "RUN set" },
			{ 0x1050000, 0x2, "RVBAR low changed" },
			{ 0x1050004, 0x101, "RVBAR high changed" },
			{ 0x1408110, 0x1, "A2I not empty" },
			{ 0x1408114, 0x1, "I2A not empty" },
		};
		const struct ane_h15_soc *max = &ane_t6031_soc;

		for (i = 0; i < (int)ARRAY_SIZE(preflight); i++) {
			map_setup(max, "apple,t6031-ane");
			eng_set(max, preflight[i].off, preflight[i].val);
			ret = probe_now();
			snprintf(name, sizeof(name), "NM16 %s: refuse before any IOMMU call", preflight[i].tag);
			chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
			    !strcmp(verdict, "FAIL") && !strcmp(reason, "preflight-not-as-iboot-left-it") &&
			    sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0, name);
		}
	}
	/* NM14 a SoC row without a measured RVBAR: refused first */
	map_setup(&ane_t6034_soc, "apple,t6034-ane");
	ret = probe_now();
	chk(ret != 0 && sim_n_events == 0 && sim_n_iocalls == 0, "NM14 t6034 map_fw: refused (no measured RVBAR), no iommu call");
	/* NM6..NM10 failures inside the map: nothing written, everything undone */
	{
		static const struct { int fail; const char *seq; const char *tag; } f[] = {
			{ 1, "A", "NM6 domain alloc fails" },
			{ 2, "ATF", "NM7 attach fails" },
			{ 4, "ATMDF", "NM8 map of TEXT fails" },
			{ 3, "ATMMUDF", "NM9 map of DATA fails" },
			{ 5, "ATMUDF", "NM10 TEXT reads back wrong" },
		};

		for (i = 0; i < (int)ARRAY_SIZE(f); i++) {
			map_setup(s, "apple,t6030-ane");
			sim_io_fail = f[i].fail;
			ret = probe_now();
			io_seq(seq, sizeof(seq));
			snprintf(name, sizeof(name), "%s: FAIL fw-map-failed, call sequence %s, no write, no domain left", f[i].tag, f[i].seq);
			chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
			    !strcmp(verdict, "FAIL") && !strcmp(reason, "fw-map-failed") &&
			    !strcmp(seq, f[i].seq) &&
			    events_in(0, ~0ULL, 1) == 0 && sim_n_dom_live == 0, name);
		}
	}
	/* NM11 attached, but the DART still shows no live stream: refuse, unmap, write nothing */
	map_setup(s, "apple,t6030-ane");
	sim_attach_hook = NULL;
	ret = probe_now();
	io_seq(seq, sizeof(seq));
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    !strcmp(reason, "no-dart-stream-enabled") && !strcmp(seq, "ATMMUUDF") &&
	    events_in(0, ~0ULL, 1) == 0 && sim_n_dom_live == 0,
	    "NM11 attached, no live stream: FAIL, mapping undone, nothing written");
	/* NM12 a core that does not stop keeps the mapping, the power claim and the windows */
	map_setup(s, "apple,t6030-ane");
	dm_stuck = true;
	ret = probe_now();
	io_seq(seq, sizeof(seq));
	chk(ret == 0 && !strcmp(seq, "ATMM") && sim_n_dom_live == 1 && sim_pm_put == 0 &&
	    sim_module_gets == 1 && run_writes(s, w, 8, &first) == 2,
	    "NM12 stuck core: the DART mapping is kept (no unmap, detach or free)");
	ane_h15_remove(&pdev);
	chk(sim_n_dom_live == 1 && sim_n_iounmap == 0, "NM12 stuck core: remove() refuses, mapping and windows stay");
	{
		const struct ane_h15_soc *max = &ane_t6031_soc;
		struct ane_h15 *owned;

		map_setup(max, "apple,t6031-ane");
		dm_ambiguous_stop = true;
		ret = probe_now();
		owned = platform_get_drvdata(&pdev);
		io_seq(seq, sizeof(seq));
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "FAIL") && !strcmp(reason, "core-not-stopped") &&
		    (eng_get(max, max->cpu_status) & 3u) == 3u,
		    "NM17 STOPPED and RUNNING: report core-not-stopped, never clean success");
		chk(!strcmp(seq, "ATMM") && sim_n_dom_live == 1 && sim_module_gets == 1 &&
		    sim_pm_put == 0 && sim_n_iounmap == 0 && owned->fw_started &&
		    owned->fw_touched && owned->map_n == 2,
		    "NM17 ambiguous stop retains both mappings, module pin, power and windows");
		ane_h15_remove(&pdev);
		chk(sim_n_dom_live == 1 && sim_n_iounmap == 0 && sim_pm_put == 0 &&
		    sim_module_gets == 1,
		    "NM17 remove cannot release ambiguous-state ownership");
	}
	return fails;
}

/* ---- stage 6 (confirm_run=1 confirm_rtkit=1): the RTKit session over the wrapper mailbox ----
 *
 * Firmware model: RUN queues a HELLO (versions 11..12); the host's HELLO reply queues two endpoint-map messages
 * (endpoints 0 and 1, then 0x20 with the last flag); STARTEP of endpoint 1 queues a crash-log buffer request (endpoint 1,
 * never answered) and the IOP power ON acknowledgement; the host's AP power ON is acknowledged; STARTEP of 0x20 queues
 * an application message (never answered); AP QUIESCED/OFF and IOP SLEEP/OFF are acknowledged. The I2A words and the
 * RECV registers are derived from the queue; RECV1 is pop on read.
 */
struct fwmsg { u64 m0, m1; };
static struct fwmsg fq[64];
static int fq_n;
static u64 fw_pend0;
static u64 sent0[64], sent1[64];
static int n_sent, fw_ep1, fw_ep20;
static struct {
	bool silent, no_ap_ack, no_iop_ack, flood, a2i_full, no_epmap, quirk_ver;
} fwo;

#define RT(t) ((u64)(t) << 52)

static void fw_regs(void)
{
	const struct ane_h15_soc *s = dm_soc;

	eng_set(s, s->mbox + 0x114, fq_n ? 0x1 : DM_EMPTY_WORD);
	eng_set(s, s->mbox + 0x830, fq_n ? (u32)fq[0].m0 : 0);
	eng_set(s, s->mbox + 0x834, fq_n ? (u32)(fq[0].m0 >> 32) : 0);
	eng_set(s, s->mbox + 0x838, fq_n ? (u32)fq[0].m1 : 0);
	eng_set(s, s->mbox + 0x83c, fq_n ? (u32)(fq[0].m1 >> 32) : 0);
}

static void fw_push(u64 m0, u64 m1)
{
	if (fq_n < 64)
		fq[fq_n++] = (struct fwmsg){ m0, m1 };
	fw_regs();
}

static void fw_host_msg(u64 m0, u64 m1)
{
	unsigned int type = (m0 >> 52) & 0xff, ep, i;

	if (n_sent < 64) {
		sent0[n_sent] = m0;
		sent1[n_sent] = m1;
	}
	n_sent++;
	if (type == 2 && !fwo.no_epmap) {
		fw_push(RT(8) | 0x3, 0);
		fw_push(RT(8) | (1ULL << 32) | 1 | (1ULL << 51), 0);
		if (fwo.flood)
			for (i = 0; i < 30; i++)
				fw_push(RT(8) | (2ULL << 32) | 2, 0);
	} else if (type == 5) {
		ep = (m0 >> 32) & 0xff;
		if (ep == 1) {
			fw_ep1++;
			fw_push(RT(1) | 0x23c000, 1);		/* crash log buffer request on endpoint 1 */
			fw_push(RT(7) | 0x20, 0);		/* IOP power state ON acknowledged */
		} else if (ep == 0x20) {
			fw_ep20++;
			fw_push(RT(1) | (12ULL << 16) | 11, 0x20);	/* an application endpoint message that looks like a HELLO: must stay unanswered */
		}
	} else if (type == 0xb && !fwo.no_ap_ack) {
		fw_push(RT(0xb) | (m0 & 0xffff), 0);
	} else if (type == 6 && !fwo.no_iop_ack) {
		fw_push(RT(7) | (m0 & 0xffff), 0);
	}
}

/* ---- stage 7 firmware model: SCRATCH words, the channel table, rings, the doorbell ---- */
#define SCR_OFF 0x1840050u
#define SCR_READY 0x08042006u
#define SCR_WAKE 0xf7fbdff9u
#define DOORBELL_OFF 0x1844000u
#define IPI_PENDING_OFF 0x184c000u
#define IPI_ACK_OFF 0x1850000u
static bool fw_chan;
static u32 fw_ipi_pend;		/* the pending word the model answers with */
static u32 ipi_raise;		/* what the model raises at publish and on every doorbell */
static bool fw_ipi_hold;	/* the acknowledge never clears pending (the cap control) */
static struct {
	bool s7_zero, s0_big, s3_big, no_table, s3_stuck, no_answer, bad_ack, bad_ring, overlap_ring;
} hso;
static bool fw_h2t_ok;
static int fw_cmds, fw_io_cursor;
static u64 fw_ipc_iova, fw_ipc_size;

static void *dma_host(u64 iova)
{
	int k;

	for (k = 0; k < sim_n_dma; k++)
		if (iova >= sim_dma[k].iova && iova < sim_dma[k].iova + sim_dma[k].size)
			return (u8 *)sim_dma[k].va + (iova - sim_dma[k].iova);
	return NULL;
}

static u64 dma_size_at(u64 iova)
{
	int k;

	for (k = 0; k < sim_n_dma; k++)
		if (iova >= sim_dma[k].iova && iova < sim_dma[k].iova + sim_dma[k].size)
			return sim_dma[k].size;
	return 0;
}

static void scr_set(const struct ane_h15_soc *s, unsigned int i, u32 v)
{
	eng_set(s, SCR_OFF + 4 * i, v);
}

static const struct { const char *name; u32 type, bit, slots, off; } fw_tab[8] = {
	{ "TERMINAL", 2, 0, 0x20, 0x800 }, { "SHAREDMALLOC", 1, 2, 0x10, 0x1000 }, { "BUF_H2T", 0, 3, 0x10, 0x1400 },
	{ "BUF_T2H", 1, 4, 0x10, 0x1800 }, { "IO", 0, 1, 0x10, 0x8800 }, { "IO_T2H", 1, 5, 0x10, 0x1c00 },
	{ "DEBUG", 1, 6, 0x10, 0x2000 }, { "DATA_CHAIN_H2T", 0, 7, 0x10, 0x2400 },
};

static void fw_publish(const struct ane_h15_soc *s)
{
	u64 pub = ((u64)eng_get(s, SCR_OFF + 4) << 32) | eng_get(s, SCR_OFF);
	u8 *blk = dma_host(pub), *ipc, *e;
	unsigned int i;

	if (!blk || hso.no_table)
		return;
	memcpy(&fw_ipc_iova, blk + 8, 8);
	ipc = dma_host(fw_ipc_iova);
	fw_ipc_size = dma_size_at(fw_ipc_iova);
	if (!ipc)
		return;
	for (i = 0; i < 8; i++) {
		u64 slots = fw_tab[i].slots, ring = fw_ipc_iova + fw_tab[i].off;

		if (hso.bad_ring && i == 2)
			ring = fw_ipc_iova + fw_ipc_size + 0x1000;
		if (hso.overlap_ring && i == 3)
			ring = fw_ipc_iova + fw_tab[2].off;
		e = ipc + i * 0x100;
		memset(e, 0, 0x100);
		strcpy((char *)e, fw_tab[i].name);
		memcpy(e + 0x40, &fw_tab[i].type, 4);
		memcpy(e + 0x44, &fw_tab[i].bit, 4);
		memcpy(e + 0x48, &slots, 8);
		memcpy(e + 0x50, &ring, 8);
	}
	scr_set(s, 0, (u32)fw_ipc_iova);
	scr_set(s, 1, (u32)(fw_ipc_iova >> 32));
	scr_set(s, 7, SCR_READY);
	fw_ipi_pend |= ipi_raise;
}

static void fw_s3_ready(const struct ane_h15_soc *s)
{
	u8 *ipc = dma_host(fw_ipc_iova);
	unsigned int i, k;

	fw_h2t_ok = ipc != NULL;
	for (i = 0; ipc && i < 8; i++) {
		if (fw_tab[i].type)
			continue;
		for (k = 0; k < fw_tab[i].slots; k++) {
			u64 hdr;

			memcpy(&hdr, ipc + fw_tab[i].off + k * 0x40, 8);
			if (hdr != 1)
				fw_h2t_ok = false;
		}
	}
	if (!hso.s3_stuck)
		scr_set(s, 3, 0);
}

static void fw_doorbell(u32 v)
{
	u8 *ipc = dma_host(fw_ipc_iova), *slotp, *cmd;
	u64 hdr, len1, zero = 0;
	u16 id, ack = 0;
	u32 tick = 0x16e3600;

	if (!ipc || !(v & 2))
		return;
	slotp = ipc + fw_tab[4].off + (fw_io_cursor % fw_tab[4].slots) * 0x40;
	memcpy(&hdr, slotp, 8);
	if (hdr & 1)
		return;
	cmd = dma_host(hdr);
	if (!cmd)
		return;
	fw_cmds++;
	fw_io_cursor++;
	fw_ipi_pend |= ipi_raise;
	if (hso.no_answer)
		return;
	memcpy(&id, cmd + 4, 2);
	if (hso.bad_ack)
		ack = 1;
	memcpy(cmd + 6, &ack, 2);
	if (id == 3)
		memcpy(cmd + 8, &tick, 4);
	memcpy(&len1, slotp + 8, 8);
	memcpy(slotp + 16, &zero, 8);
	hdr |= 1;
	memcpy(slotp, &hdr, 8);
}

static void rt_write(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;
	bool run = pa == s->engine_pa + s->cpu_control && (v & DM_RUN) && !dm_nostart;

	dm_write(pa, v);
	if (fw_chan && pa >= s->engine_pa + SCR_OFF && pa < s->engine_pa + SCR_OFF + 0x20) {
		unsigned int i = (pa - s->engine_pa - SCR_OFF) / 4;

		if (i == 7 && v == SCR_WAKE)
			fw_publish(s);
		else if (i == 3 && v == SCR_READY)
			fw_s3_ready(s);
	}
	if (fw_chan && pa == s->engine_pa + DOORBELL_OFF)
		fw_doorbell(v);
	if (fw_chan && pa == s->engine_pa + IPI_ACK_OFF && !fw_ipi_hold)
		fw_ipi_pend &= ~v;	/* write-1-to-clear, as macOS's acknowledge */
	if (run && fw_chan) {
		scr_set(s, 0, hso.s0_big ? 0x21 : 8);
		scr_set(s, 1, 0xc440);
		scr_set(s, 2, 0x100);
		scr_set(s, 3, hso.s3_big ? 0x8000000 : 0xf00000);
		scr_set(s, 5, 0x100090);
		scr_set(s, 7, hso.s7_zero ? 0 : SCR_READY);
	}
	if (run) {
		if (fwo.a2i_full)
			eng_set(s, s->mbox + 0x110, 0x10000);
		if (!fwo.silent)
			fw_push(RT(1) | ((u64)(fwo.quirk_ver ? 14 : 12) << 16) | (fwo.quirk_ver ? 13 : 11), 0);
	}
}

static void rt_write64(u64 pa, u64 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (pa == s->engine_pa + s->mbox + 0x800)
		fw_pend0 = v;
	else if (pa == s->engine_pa + s->mbox + 0x808)
		fw_host_msg(fw_pend0, v);
}

static u32 rt_read(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (pa == s->engine_pa + IPI_PENDING_OFF)
		return fw_ipi_pend;
	if (pa == s->engine_pa + s->mbox + 0x838 && fq_n) {	/* pop on the read of RECV1 */
		int i;

		for (i = 1; i < fq_n; i++)
			fq[i - 1] = fq[i];
		fq_n--;
		fw_regs();
	}
	return v;
}

static void rt_setup(const struct ane_h15_soc *s, const char *compat)
{
	run_setup(s, compat, 0);
	stage = "rtkit";
	confirm_rtkit = true;
	fq_n = n_sent = fw_ep1 = fw_ep20 = 0;
	fw_chan = false;
	memset(&fwo, 0, sizeof(fwo));
	sim_write_hook = rt_write;
	sim_write64_hook = rt_write64;
	sim_read_hook = rt_read;
}

static int mbox_writes(const struct ane_h15_soc *s)
{
	return events_in(s->engine_pa + s->mbox + 0x800, 0x10, 1);
}

static void pos_rtkit_case(const char *tag, unsigned int stop, const u64 *want, int n_want, int map)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	u32 w[8];
	int ret, first, i, ok, cw;

	if (map) {
		map_setup(s, "apple,t6030-ane");
		stage = "rtkit";
		confirm_rtkit = true;
		fq_n = n_sent = fw_ep1 = fw_ep20 = 0;
		memset(&fwo, 0, sizeof(fwo));
		sim_write_hook = rt_write;
		sim_write64_hook = rt_write64;
		sim_read_hook = rt_read;
	} else {
		rt_setup(s, "apple,t6030-ane");
	}
	rtkit_stop = stop;
	ret = probe_now();
	snprintf(name, sizeof(name), "%s: probe 0, RESULT stage=6 PASS rtkit-session-and-shutdown", tag);
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 6 && !strcmp(verdict, "PASS") && !strcmp(reason, "rtkit-session-and-shutdown"), name);
	ok = n_sent == n_want;
	for (i = 0; ok && i < n_want; i++)
		ok = sent0[i] == want[i] && sent1[i] == 0;
	snprintf(name, sizeof(name), "%s: the host sent exactly these %d management messages, SEND1 always 0", tag, n_want);
	chk(ok, name);
	cw = run_writes(s, w, 8, &first);
	snprintf(name, sizeof(name), "%s: every engine write is CPU_CONTROL [0x10, 0] or a send register (SEND0 then SEND1), nothing else", tag);
	{
		int cpu = events_in(s->engine_pa + s->cpu_control, 4, 1), mb = mbox_writes(s);

		chk(cpu == 2 && mb == 2 * n_want && cw == cpu + mb && events_in(0, ~0ULL, 1) == cw && sim_bus_faults == 0 &&
		    eng_get(s, s->cpu_control) == 0, name);
	}
	snprintf(name, sizeof(name), "%s: the crash-log request and the application message were not answered", tag);
	chk(fw_ep1 == 1 && fw_ep20 == 1 && n_sent == n_want, name);
	snprintf(name, sizeof(name), "%s: first mailbox write comes after the RUN write; core stopped; claim released", tag);
	{
		int run_idx = -1, mb_idx = -1;

		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
			if (!sim_events[i].write)
				continue;
			if (run_idx < 0 && sim_events[i].pa == s->engine_pa + s->cpu_control)
				run_idx = i;
			if (mb_idx < 0 && sim_events[i].pa == s->engine_pa + s->mbox + 0x800)
				mb_idx = i;
		}
		chk(run_idx >= 0 && mb_idx > run_idx && (eng_get(s, s->cpu_status) & DM_STOPPED) && sim_pm_get == 1 && sim_pm_put == 1 &&
		    sim_module_gets == 0, name);
	}
	ane_h15_remove(&pdev);
}

static void pos_rtkit(void)
{
	const u64 base[] = {
		RT(2) | (12ULL << 16) | 12,			/* HELLO reply */
		RT(8) | 1,					/* map reply, base 0, more */
		RT(8) | (1ULL << 32) | (1ULL << 51),		/* map reply, base 1, last */
		RT(5) | (1ULL << 32) | 2,			/* STARTEP 1 */
		RT(0xb) | 0x20,				/* AP power ON */
		RT(5) | (0x20ULL << 32) | 2,			/* STARTEP 0x20 */
		RT(0xb) | 0x10,				/* AP QUIESCED */
		RT(6) | 0x01,				/* IOP SLEEP */
	};
	u64 off[8];

	memcpy(off, base, sizeof(off));
	off[6] = RT(0xb);				/* AP OFF */
	off[7] = RT(6);					/* IOP OFF */
	pos_rtkit_case("S1 rtkit_stop=1 (QUIESCED, SLEEP)", 1, base, 8, 0);
	pos_rtkit_case("S2 rtkit_stop=2 (OFF, OFF)", 2, off, 8, 0);
	pos_rtkit_case("S3 with map_fw=1", 1, base, 8, 1);
}

static void rt_refused(const char *tag, const char *want_verdict, const char *want_reason, int want_ret_nonzero, bool no_mbox, bool no_events)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	int ret = probe_now();

	snprintf(name, sizeof(name), "%s: RESULT stage=6 %s %s", tag, want_verdict, want_reason);
	chk((ret != 0) == want_ret_nonzero && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 6 && !strcmp(verdict, want_verdict) && strstr(reason, want_reason), name);
	snprintf(name, sizeof(name), "%s: no mailbox write%s", tag, no_events ? ", zero bus events" : "");
	chk((!no_mbox || mbox_writes(s) == 0) && (!no_events || sim_n_events == 0) && sim_bus_faults == 0, name);
}

static int neg_rtkit(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	u32 w[8];
	int first, i;

	/* NR1 no confirm_rtkit */
	rt_setup(s, "apple,t6030-ane");
	confirm_rtkit = false;
	rt_refused("NR1 no confirm_rtkit", "REFUSED", "confirm_rtkit=1", 1, true, true);
	/* NR2 out-of-range budget and stop mode */
	for (i = 0; i < 3; i++) {
		rt_setup(s, "apple,t6030-ane");
		if (i == 0)
			rtkit_wait_ms = 50;
		else if (i == 1)
			rtkit_wait_ms = 20000;
		else
			rtkit_stop = 3;
		rt_refused("NR2 bad rtkit parameter", "REFUSED", "rtkit_wait_ms", 1, true, true);
	}
	/* NR3 silent firmware: only CPU_CONTROL is written, the verdict says so */
	rt_setup(s, "apple,t6030-ane");
	fwo.silent = true;
	rtkit_wait_ms = 300;
	rt_refused("NR3 silent firmware", "FAIL", "rtkit-no-hello", 1, true, false);
	chk(run_writes(s, w, 8, &first) == 2 && w[0] == 0x10 && w[1] == 0 && sim_pm_put == 1,
	    "NR3 silent firmware: RUN set and cleared, nothing else written, claim released");
	/* NR4 firmware versions that exclude 12: no reply */
	rt_setup(s, "apple,t6030-ane");
	fwo.quirk_ver = true;
	rtkit_wait_ms = 300;
	rt_refused("NR4 HELLO 13..14", "FAIL", "rtkit-session-incomplete", 1, true, false);
	chk(n_sent == 0, "NR4 HELLO 13..14: not one message sent");
	/* NR5 A2I mailbox FULL: nothing may be written to the send registers */
	rt_setup(s, "apple,t6030-ane");
	fwo.a2i_full = true;
	rtkit_wait_ms = 300;
	rt_refused("NR5 A2I FULL", "FAIL", "rtkit-session-incomplete", 1, true, false);
	/* NR6 the firmware never acknowledges the AP power state: shutdown incomplete, core still stopped */
	rt_setup(s, "apple,t6030-ane");
	fwo.no_ap_ack = true;
	rtkit_wait_ms = 300;
	rt_refused("NR6 no AP ack", "FAIL", "rtkit-session-incomplete", 1, false, false);
	/* NR7 no IOP ack at shutdown */
	rt_setup(s, "apple,t6030-ane");
	fwo.no_iop_ack = true;
	rt_refused("NR7 no IOP ack", "FAIL", "rtkit-shutdown-incomplete", 1, false, false);
	chk(eng_get(s, s->cpu_control) == 0 && (eng_get(s, s->cpu_status) & DM_STOPPED), "NR7 no IOP ack: RUN cleared and the core stopped regardless");
	/* NR8 an endpoint-map flood: the send cap holds */
	rt_setup(s, "apple,t6030-ane");
	fwo.flood = true;
	rtkit_wait_ms = 500;
	probe_now();
	chk(n_sent <= 16 + 2 && mbox_writes(s) <= 2 * (16 + 2), "NR8 map flood: at most 16 messages in the start phase and the shutdown budget");
	/* NR9 a stuck core after a complete session: kept, pinned, no PASS */
	rt_setup(s, "apple,t6030-ane");
	dm_stuck = true;
	rt_refused("NR9 stuck core", "FAIL", "core-not-stopped", 0, false, false);
	chk(sim_module_gets == 1 && sim_pm_put == 0, "NR9 stuck core: module pinned, power claim kept");
	/* NR10 stage 5 never writes the mailbox, with the same firmware model */
	rt_setup(s, "apple,t6030-ane");
	stage = "run";
	probe_now();
	chk(mbox_writes(s) == 0 && n_sent == 0, "NR10 stage 5 on a talking firmware: zero mailbox writes");
	return fails;
}

/* ---- stage 7 (confirm_chan=1): the SCRATCH handshake and the channel step on top of the session ---- */
static void ch_setup(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;

	map_setup(s, "apple,t6030-ane");
	dart_set(s, true);		/* default DMA domain: the DART driver enabled the stream at boot, no attach */
	sim_attach_hook = NULL;
	stage = "chan";
	confirm_rtkit = true;
	confirm_chan = true;
	fq_n = n_sent = fw_ep1 = fw_ep20 = 0;
	memset(&fwo, 0, sizeof(fwo));
	memset(&hso, 0, sizeof(hso));
	fw_chan = true;
	fw_h2t_ok = false;
	fw_cmds = fw_io_cursor = 0;
	fw_ipc_iova = fw_ipc_size = 0;
	fw_ipi_pend = ipi_raise = 0;
	fw_ipi_hold = false;
	sim_write_hook = rt_write;
	sim_write64_hook = rt_write64;
	sim_read_hook = rt_read;
	rtkit_wait_ms = 500;
}

static int scr_writes(const struct ane_h15_soc *s)
{
	return events_in(s->engine_pa + SCR_OFF, 0x20, 1);
}

/* the value written to SCRATCHi, the n-th write there (0 = first) */
static bool scr_write_seq(const struct ane_h15_soc *s, const u32 *idx, const u32 *val, int n)
{
	int i, k = 0;

	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
		u64 pa = sim_events[i].pa;

		if (!sim_events[i].write || pa < s->engine_pa + SCR_OFF || pa >= s->engine_pa + SCR_OFF + 0x20)
			continue;
		if (k >= n || (pa - s->engine_pa - SCR_OFF) / 4 != idx[k] || sim_events[i].val != val[k])
			return false;
		k++;
	}
	return k == n;
}

static void pos_chan(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], seq[64];
	u8 *blk;
	u64 v64;
	u32 v32;
	int ret, c;

	for (c = 0; c < 2; c++) {
		ch_setup();
		chan_cmds = c;
		ret = probe_now();
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 7 && !strcmp(verdict, "PASS") && !strcmp(reason, "rtkit-channels-and-shutdown"),
		    c ? "C2 chan_cmds=1: RESULT stage=7 PASS rtkit-channels-and-shutdown" : "C1 RESULT stage=7 PASS rtkit-channels-and-shutdown");
		{
			static const u32 idx[] = { 0, 1, 7, 3 };
			u32 val[4] = { sim_n_dma >= 1 ? (u32)sim_dma[0].iova : 0, sim_n_dma >= 1 ? (u32)(sim_dma[0].iova >> 32) : 0, SCR_WAKE, SCR_READY };

			chk(scr_write_seq(s, idx, val, 4) && scr_writes(s) == 4,
			    "C: SCRATCH writes are exactly S0 (pool low), S1 (pool high), S7 (wake), S3 (READY), in that order, nothing else in the window");
		}
		io_seq(seq, sizeof(seq));
		chk(!strcmp(seq, "MMUU") && sim_n_dom_live == 0, "C: firmware mapped into the default DMA domain, no attach, no domain freed, unmapped after the stop");
		chk(sim_n_dma == (c ? 4 : 3) && sim_dma[0].size == 0x40000 && sim_dma[1].size == 0x10000 && sim_dma[2].size == 0xf00000 &&
		    (!c || sim_dma[3].size == 0x4000), "C: surfaces POOL 0x40000, IPC 0x10000 (max(page, S1+1) rounded), HEAP 0xf00000 (and CMD 0x4000 with commands)");
		{
			int k, live = 0;

			for (k = 0; k < sim_n_dma; k++)
				live += sim_dma[k].live;
			chk(live == 0, "C: every surface freed after the core stopped");
		}
		blk = sim_n_dma ? sim_dma[0].va : NULL;
		chk(blk != NULL, "C: init block present");
		if (blk) {
			memcpy(&v64, blk + 0x00, 8);
			chk(v64 == 0x10000000000ULL, "C: block +0x00 = TEXT iova (the firmware surface)");
			memcpy(&v64, blk + 0x08, 8);
			chk(v64 == sim_dma[1].iova, "C: block +0x08 = IPC surface iova");
			memcpy(&v64, blk + 0x10, 8);
			chk(v64 == 0x380000, "C: block +0x10 = 0x380000 (the h15g config size)");
			memcpy(&v64, blk + 0x18, 8);
			chk(v64 == 0x0fc80000, "C: block +0x18 = 0x10000000 - 0x380000");
			memcpy(&v64, blk + 0x20, 8);
			chk(v64 == sim_dma[2].iova, "C: block +0x20 = HEAP iova");
			memcpy(&v64, blk + 0x28, 8);
			chk(v64 == 0xf00000, "C: block +0x28 = the heap request, 0xf00000");
			memcpy(&v64, blk + 0x50, 8);
			chk(v64 == 0xc441, "C: block +0x50 = SCRATCH1 + 1 = 0xc441");
			memcpy(&v64, blk + 0x58, 8);
			chk(v64 == sim_dma[0].iova, "C: block +0x58 = POOL iova");
			memcpy(&v32, blk + 0x68, 4);
			chk(v32 == 0x40, "C: block +0x68 = 0x40");
			memcpy(&v32, blk + 0x6c + 0xc0, 4);
			chk(v32 == 4, "C: template word +0xc0 = 4");
		}
		chk(fw_h2t_ok, "C: every host-to-firmware ring slot was host owned (header 1) when SCRATCH3 was written");
		chk(c ? (fw_cmds == 2 && events_in(s->engine_pa + DOORBELL_OFF, 4, 1) == 2) : (fw_cmds == 0 && events_in(s->engine_pa + DOORBELL_OFF, 4, 1) == 0),
		    c ? "C2: PRINT_ENABLE and CONFIG_GET answered, the doorbell written exactly twice (BIT(1))" : "C1: the doorbell is never written without chan_cmds");
		chk((eng_get(s, s->cpu_status) & DM_STOPPED) && sim_pm_get == 1 && sim_pm_put == 1 && sim_module_gets == 0, "C: core stopped, claim released");
		ane_h15_remove(&pdev);
	}
	/* stage 6 on the same firmware never touches the SCRATCH window */
	ch_setup();
	stage = "rtkit";
	probe_now();
	chk(scr_writes(s) == 0 && sim_n_dma == 0, "C3 stage 6 writes no SCRATCH word and allocates no surface");
	ane_h15_remove(&pdev);
}

/* ---- stage 7 IPI pair: pending engine + 0x184c000 (read), acknowledge engine + 0x1850000 (write the value back) ---- */
static int ipi_ack_writes(const struct ane_h15_soc *s)
{
	return events_in(s->engine_pa + IPI_ACK_OFF, 4, 1);
}

static int ipi_pend_reads(const struct ane_h15_soc *s)
{
	return events_in(s->engine_pa + IPI_PENDING_OFF, 4, 0);
}

/* every acknowledge write so far carried exactly this value */
static bool ipi_ack_all(const struct ane_h15_soc *s, u32 v)
{
	int i;

	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++)
		if (sim_events[i].write && sim_events[i].pa == s->engine_pa + IPI_ACK_OFF && sim_events[i].val != v)
			return false;
	return true;
}

static void ipi_chan(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	/* I1: the pending word stays 0, so nothing is ever written to the acknowledge */
	ch_setup();
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "PASS"), "I1 stage 7 with a silent pending: PASS unchanged");
	chk(ipi_pend_reads(s) > 0, "I1 the stage 7 wait loops read the pending word");
	chk(ipi_ack_writes(s) == 0, "I1 pending 0: no acknowledge write");
	ane_h15_remove(&pdev);

	/* I2: the firmware raises 0x5 with the table: exactly one write of 0x5 back, after its log line */
	ch_setup();
	ipi_raise = 0x5;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "PASS"), "I2 pending 0x5: stage 7 still PASS");
	chk(ipi_ack_writes(s) == 1 && ipi_ack_all(s, 0x5), "I2 exactly one acknowledge write and its value is 0x5");
	chk(log_count("ane_h15 write IPI_ACK=0x5 pa=") == 1, "I2 the write was logged before it happened");
	chk(log_count("ane_h15 read IPI_PENDING=0x5 pa=") >= 1, "I2 the pending change was logged");
	ane_h15_remove(&pdev);

	/* I2b: the same with an even value, 0x6: one write of 0x6 back, after its log line */
	ch_setup();
	ipi_raise = 0x6;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "PASS"), "I2b pending 0x6: stage 7 still PASS");
	chk(ipi_ack_writes(s) == 1 && ipi_ack_all(s, 0x6), "I2b exactly one acknowledge write and its value is 0x6");
	chk(log_count("ane_h15 write IPI_ACK=0x6 pa=") == 1, "I2b the write was logged before it happened");
	chk(log_count("ane_h15 read IPI_PENDING=0x6 pa=") >= 1, "I2b the pending change was logged");
	ane_h15_remove(&pdev);

	/* I3: chan_cmds=1 with the model raising 0x5 at publish and on every doorbell: one plus two acknowledges */
	ch_setup();
	chan_cmds = true;
	ipi_raise = 0x5;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "PASS"), "I3 chan_cmds=1 with a raising pending: PASS");
	chk(ipi_ack_writes(s) == 3 && ipi_ack_all(s, 0x5), "I3 publish plus the two doorbells: three 0x5 acknowledges");
	ane_h15_remove(&pdev);

	/* I4: the pending word is held (the acknowledge never clears it) through a stuck SCRATCH3 wait:
	 * the writes stop at the per run cap of 64 */
	ch_setup();
	hso.s3_stuck = true;
	fw_ipi_hold = true;
	ipi_raise = 0x1;
	hs_wait_ms = 200;
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "FAIL") && strstr(reason, "handshake-not-done"),
	    "I4 stuck SCRATCH3 still fails the run");
	chk(ipi_ack_writes(s) == 64, "I4 the acknowledges stop at 64 per run");
	chk(log_count("ane_h15 write IPI_ACK=") == 64, "I4 every acknowledge write carries its log line, no line past the cap");
	ane_h15_remove(&pdev);

	/* I5: stage 6 and earlier never read the pending word and never write the acknowledge */
	ch_setup();
	stage = "rtkit";
	ipi_raise = 0x5;
	fw_ipi_pend = 0x5;	/* raised before the session; the model answers it whenever it is read */
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 6 && !strcmp(verdict, "PASS"), "I5 stage 6 with a raised pending: PASS unchanged");
	chk(ipi_pend_reads(s) == 0 && ipi_ack_writes(s) == 0, "I5 stage 6: no pending read, no acknowledge write");
	ane_h15_remove(&pdev);
}

static void ch_refused(const char *tag, const char *want_reason, bool no_scr, bool no_dma, bool ret_nonzero)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	int ret = probe_now();

	snprintf(name, sizeof(name), "%s: RESULT stage=7 %s", tag, want_reason);
	chk((ret != 0) == ret_nonzero && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && strstr(reason, want_reason), name);
	snprintf(name, sizeof(name), "%s: %s%s", tag, no_scr ? "no SCRATCH write" : "SCRATCH writes bounded", no_dma ? ", no surface allocated" : "");
	chk((!no_scr || scr_writes(s) == 0) && (!no_dma || sim_n_dma == 0) && sim_bus_faults == 0, name);
}

static int neg_chan(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	int k, live;

	/* NC1 .. NC3: refusals before any bus access */
	ch_setup();
	confirm_chan = false;
	ch_refused("NC1 no confirm_chan", "confirm_chan=1", true, true, true);
	chk(sim_n_events == 0, "NC1 no confirm_chan: zero bus events");
	ch_setup();
	map_fw = false;
	ch_refused("NC2 no map_fw", "stage 7 needs map_fw", true, true, true);
	ch_setup();
	hs_wait_ms = 5;
	ch_refused("NC3 hs_wait_ms=5", "stage 7 needs map_fw", true, true, true);
	/* NC4 SCRATCH7 is not READY */
	ch_setup();
	hso.s7_zero = true;
	ch_refused("NC4 SCRATCH7 not READY", "handshake-not-ready", true, true, true);
	/* NC5 SCRATCH0 >= 0x21 */
	ch_setup();
	hso.s0_big = true;
	ch_refused("NC5 SCRATCH0 = 0x21", "handshake-not-done", true, true, true);
	/* NC6 a heap request above the limit */
	ch_setup();
	hso.s3_big = true;
	ch_refused("NC6 heap request 128 MiB", "handshake-not-done", true, true, true);
	/* NC7 the firmware never publishes a table */
	ch_setup();
	hso.no_table = true;
	hs_wait_ms = 50;
	ch_refused("NC7 no table address", "handshake-no-table-address", false, false, true);
	chk(scr_writes(s) == 3, "NC7 no table address: S0, S1 and S7 written, SCRATCH3 never");
	/* NC8 a ring outside the table's surface: nothing written before the check, no READY */
	ch_setup();
	hso.bad_ring = true;
	ch_refused("NC8 ring outside the surface", "handshake-not-done", false, false, true);
	chk(scr_writes(s) == 3 && !fw_h2t_ok, "NC8 ring outside the surface: no SCRATCH3 write, no slot header set");
	/* NC9 overlapping rings */
	ch_setup();
	hso.overlap_ring = true;
	ch_refused("NC9 rings overlap", "handshake-not-done", false, false, true);
	chk(scr_writes(s) == 3 && !fw_h2t_ok, "NC9 rings overlap: no SCRATCH3 write, no slot header set");
	/* NC10 SCRATCH3 is never cleared */
	ch_setup();
	hso.s3_stuck = true;
	hs_wait_ms = 50;
	ch_refused("NC10 SCRATCH3 never cleared", "handshake-not-done", false, false, true);
	/* NC11 commands: no answer, then a failing acknowledgement */
	ch_setup();
	chan_cmds = true;
	hso.no_answer = true;
	hs_wait_ms = 50;
	ch_refused("NC11 no command answer", "handshake-not-done", false, false, true);
	chk(events_in(s->engine_pa + DOORBELL_OFF, 4, 1) == 1, "NC11 no command answer: the doorbell was written once, not retried");
	ch_setup();
	chan_cmds = true;
	hso.bad_ack = true;
	ch_refused("NC12 failing acknowledgement", "handshake-not-done", false, false, true);
	/* NC13 a surface allocation fails (the IPC surface): no SCRATCH write */
	ch_setup();
	sim_dma_fail = 2;
	ch_refused("NC13 IPC allocation fails", "handshake-not-done", true, false, true);
	/* NC14 no default DMA domain */
	ch_setup();
	sim_default_dom_none = 1;
	ch_refused("NC14 no default DMA domain", "fw-map-failed", true, true, true);
	/* NC15 a stuck core after a complete handshake: surfaces kept, module pinned */
	ch_setup();
	dm_stuck = true;
	ch_refused("NC15 stuck core", "core-not-stopped", false, false, false);
	for (k = 0, live = 0; k < sim_n_dma; k++)
		live += sim_dma[k].live;
	chk(sim_n_dma == 3 && live == 3 && sim_module_gets == 1, "NC15 stuck core: the surfaces stay allocated, the module is pinned");
	return fails;
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
		pos_multi_pd();
		pos_stage2();
		pos_run();
		pos_run_postwait();
		pos_run_scratch();
		pos_run_pmgr();
		pos_run_scratch_boot();
		pos_run_pwgate();
		pos_run_pwgate_rows();
		pos_steplog_stop();
		pos_pwgate_read();
		pos_dart();
		pos_translation();
		pos_map();
		pos_rtkit();
		pos_chan();
		ipi_chan();
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
#ifdef ANE15_SIM_FILL_FACTS
	} else if (!strcmp(mode, "pos-filled")) {
		printf("sim-h15 filled-table probe control (P7):\n");
		pos_filled_probe();
		printf("%s\n", fails ? "SIM-TEST: FAILED"
				     : "SIM-TEST: filled-table probe control PASS");
#endif
	} else if (!strcmp(mode, "neg-read-ps-stuck")) {
		printf("sim-h15 negative control: stage 2 with a stuck pmgr word\n");
		fails = neg_read_ps_stuck();
	} else if (!strcmp(mode, "neg-read-window")) {
		printf("sim-h15 negative control: stage 2 with a reg[0] that is not the SoC row's window\n");
		fails = neg_read_window();
	} else if (!strcmp(mode, "neg-read-forbidden")) {
		printf("sim-h15 negative control: stage 2 words outside the ADT windows or on CPU_CONTROL\n");
		fails = neg_read_forbidden();
	} else if (!strcmp(mode, "neg-rpm-fail")) {
		printf("sim-h15 negative control: pm_runtime_get_sync failure\n");
		fails = neg_rpm_fail();
	} else if (!strcmp(mode, "neg-pd-attach-fail")) {
		printf("sim-h15 negative control: power-domain attach fails at index i\n");
		fails = neg_pd_attach_fail();
	} else if (!strcmp(mode, "neg-pd-link-fail")) {
		printf("sim-h15 negative control: device_link_add fails at index i\n");
		fails = neg_pd_link_fail();
	} else if (!strcmp(mode, "neg-pd-rpm-err")) {
		printf("sim-h15 negative control: get_sync error other than -EACCES with domains attached\n");
		fails = neg_pd_rpm_err();
	} else if (!strcmp(mode, "neg-pd-eacces-alone")) {
		printf("sim-h15 negative control: -EACCES with zero or one domain\n");
		fails = neg_pd_eacces_alone();
	} else if (!strcmp(mode, "neg-chan")) {
		printf("sim-h15 negative control: stage 7 refusals, an odd table, a silent firmware, the stuck core\n");
		fails = neg_chan();
	} else if (!strcmp(mode, "neg-rtkit")) {
		printf("sim-h15 negative control: stage 6 refusals, a silent or odd firmware, the stuck core\n");
		fails = neg_rtkit();
	} else if (!strcmp(mode, "neg-map")) {
		printf("sim-h15 negative control: stage 5 map_fw refusals, undo on each failure, the stuck core\n");
		fails = neg_map();
	} else if (!strcmp(mode, "neg-dart")) {
		printf("sim-h15 negative control: stage 4 DART scan refusals and the run stage DART gate\n");
		fails = neg_dart();
	} else if (!strcmp(mode, "neg-run")) {
		printf("sim-h15 negative control: stage 4 refusals and the stuck core\n");
		fails = neg_run();
	} else {
		fprintf(stderr,
			"usage: sim_h15 [pos|neg-run|neg-dart|neg-map|neg-rtkit|neg-chan|neg-ps-stuck|neg-wrong-stage|neg-truncated-dt|neg-rpm-fail|neg-pd-attach-fail|neg-pd-link-fail|neg-pd-rpm-err|neg-pd-eacces-alone|neg-read-ps-stuck|neg-read-window|neg-read-forbidden]\n");
		return 2;
	}
	return fails ? 1 : 0;
}
