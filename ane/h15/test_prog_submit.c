// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Stage 7 hs_prog=1: the bounded ANEP submit end to end, plus the file and timeout fault controls.
// One case per process: the sim DMA table never reuses slots, and the submit allocates 8 surfaces.
//   ./test_prog_submit pos|bad-magic|truncated|oversize|timeout
#include <assert.h>
#define main existing_sim_main
#include "sim/sim_harness.c"
#undef main

#define T6030_COMPAT "apple,t6030-ane"
#define PROG_NAME "sim_prog.bin"

static u8 prog_file[0x1000];
static size_t prog_len;

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }

/* One small ANEP version 1 file, the layout tools/ane-prog-pack.c writes: a 0x20 byte header, two 0x100 byte
 * sections (ids 1 and 2), one input buffer (id 1, 0x20 bytes of data) and one output buffer (id 2). */
static void build_prog(u32 magic)
{
	const size_t data_off = 0xa0;

	memset(prog_file, 0, sizeof(prog_file));
	put32(prog_file + 0x00, magic);
	put32(prog_file + 0x04, 1);		/* version */
	put32(prog_file + 0x08, 2);		/* nsec */
	put32(prog_file + 0x0c, 2);		/* nio */
	put32(prog_file + 0x20, 1);
	put64(prog_file + 0x28, 0x100);		/* section 1 size */
	put64(prog_file + 0x30, data_off);	/* section 1 offset */
	put32(prog_file + 0x40, 2);
	put64(prog_file + 0x48, 0x100);		/* section 2 size */
	put64(prog_file + 0x50, data_off + 0x100);
	put32(prog_file + 0x60, 1);
	put32(prog_file + 0x64, 0);		/* direction: input */
	put64(prog_file + 0x68, 0x100);		/* buffer size */
	put64(prog_file + 0x70, data_off + 0x200);
	put64(prog_file + 0x78, 0x20);		/* data size */
	put32(prog_file + 0x80, 2);
	put32(prog_file + 0x84, 1);		/* direction: output */
	put64(prog_file + 0x88, 0x100);
	prog_len = data_off + 0x300;
	put64(prog_file + 0x10, prog_len);	/* the file size in the header, last so the patches above hold */
	memset(prog_file + data_off + 0x200, 0xab, 0x20);
}

static void write_prog(const u8 *bytes, size_t n)
{
	FILE *f = fopen(PROG_NAME, "wb");

	assert(f);
	assert(fwrite(bytes, 1, n, f) == n);
	assert(fclose(f) == 0);
}

static void prog_setup(void)
{
	ch_setup();
	chan_cmds = true;
	hs_prog = true;
	prog_fw = PROG_NAME;
	hs_wait_ms = 200;
	rtkit_wait_ms = 500;
}

static int surfaces_live(void)
{
	int k, live = 0;

	for (k = 0; k < sim_n_dma; k++)
		live += sim_dma[k].live;
	return live;
}

static void clean_shutdown(const struct ane_h15_soc *s, const char *tag)
{
	char name[160];

	snprintf(name, sizeof(name), "%s: every surface freed, the program file released, no bus fault, core stopped", tag);
	chk(surfaces_live() == 0 && sim_fw_live == 0 && (eng_get(s, s->cpu_status) & DM_STOPPED) && sim_bus_faults == 0, name);
	snprintf(name, sizeof(name), "%s: the runtime claim is released and the module is not pinned", tag);
	chk(sim_pm_get == sim_pm_put && sim_module_gets == 0, name);
}

static void pos_submit(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	u8 *ipc;
	u64 hdr;
	int ret;

	build_prog(0x50454e41u);		/* 'ANEP' */
	write_prog(prog_file, prog_len);
	prog_setup();
	ret = probe_now();
	snprintf(name, sizeof(name), "PS1 hs_prog submit: RESULT stage=7 PASS program-submitted-and-shutdown");
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "PASS") && !strcmp(reason, "program-submitted-and-shutdown"), name);
	chk(events_in(s->engine_pa + DOORBELL_OFF, 4, 1) == 8,
	    "PS2 the doorbell rang eight times: seven commands (PRINT_ENABLE, CONFIG_GET, LOAD_PROGRAM, CREATE_PROCESS, PROCEDURE_CALL, TERMINATE_PROCESS, DESTROY_PROGRAM) plus the IO_T2H give-back");
	chk(log_count("LOAD_PROGRAM program id 0x1") == 1 && log_count("CREATE_PROCESS process id 0x11") == 1,
	    "PS3 LOAD_PROGRAM and CREATE_PROCESS replies carried the program and process ids");
	chk(log_count("PROCEDURE_CALL finished after") == 1, "PS4 the IO_T2H finish event arrived");
	chk(log_count("cmd 0x203 answered") == 1 && log_count("cmd 0x201 answered") == 1,
	    "PS5 TERMINATE_PROCESS and DESTROY_PROGRAM were answered");
	ipc = dma_host(fw_ipc_iova);
	memcpy(&hdr, ipc + fw_tab[5].off, 8);
	chk(hdr && (hdr & 1), "PS6 the finish event's IO_T2H slot was given back to the firmware");
	chk(sim_n_dma == 8, "PS7 exactly eight surfaces were allocated (POOL, IPC, HEAP, CMD, two sections, two buffers)");
	clean_shutdown(s, "PS8");
	remove_if_probed();
}

/* A refused or truncated file must be refused before the first program command and before any allocation. */
static void file_refused(const char *tag, const char *want_reason, u8 *bytes, size_t n)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	int ret;

	write_prog(bytes, n);
	prog_setup();
	ret = probe_now();
	snprintf(name, sizeof(name), "%s: RESULT stage=7 FAIL %s", tag, want_reason);
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "FAIL") && strstr(reason, want_reason), name);
	snprintf(name, sizeof(name), "%s: no program command was sent (PRINT_ENABLE and CONFIG_GET only)", tag);
	chk(events_in(s->engine_pa + DOORBELL_OFF, 4, 1) == 2 && log_count("ane_h15 prog: LOAD_PROGRAM") == 0, name);
	snprintf(name, sizeof(name), "%s: no program surface was allocated (POOL, IPC, HEAP, CMD only)", tag);
	chk(sim_n_dma == 4, name);
	clean_shutdown(s, tag);
	remove_if_probed();
}

static void neg_bad_magic(void)
{
	build_prog(0x50454e42u);		/* not 'ANEP' */
	file_refused("PN1 bad magic", "program-file-rejected", prog_file, prog_len);
}

static void neg_truncated(void)
{
	build_prog(0x50454e41u);
	file_refused("PN2 truncated", "program-file-rejected", prog_file, prog_len - 16);
}

static void neg_oversize(void)
{
	build_prog(0x50454e41u);
	put64(prog_file + 0x28, 0xb000000);	/* a section over the 160 MiB single buffer bound */
	file_refused("PN3 oversize", "program-file-rejected", prog_file, prog_len);
}

/* The firmware answers PROCEDURE_CALL but never posts the finish event: the run times out into the cleanup
 * path, which unloads the process and the program and frees every program surface. */
static void neg_timeout(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	int ret;

	build_prog(0x50454e41u);
	write_prog(prog_file, prog_len);
	prog_setup();
	hso.prog_no_finish = true;
	ret = probe_now();
	snprintf(name, sizeof(name), "PN4 timeout: RESULT stage=7 FAIL program-finish-timeout");
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 7 && !strcmp(verdict, "FAIL") && !strcmp(reason, "program-finish-timeout"), name);
	chk(log_count("PROCEDURE_CALL gave no finish event") == 1, "PN4 the finish wait timed out");
	chk(log_count("cmd 0x203 answered") == 1 && log_count("cmd 0x201 answered") == 1,
	    "PN4 the cleanup unloaded the process and the program, both answered");
	chk(sim_n_dma == 8, "PN4 the four program surfaces were allocated and freed with the rest");
	clean_shutdown(s, "PN4");
	remove_if_probed();
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";

	if (!strcmp(mode, "pos")) {
		pos_submit();
		printf("%s\n", fails ? "SIM-PROG: FAILED" : "SIM-PROG: pos controls PASS");
	} else if (!strcmp(mode, "bad-magic")) {
		neg_bad_magic();
		printf("%s\n", fails ? "SIM-PROG: FAILED" : "SIM-PROG: bad-magic control PASS");
	} else if (!strcmp(mode, "truncated")) {
		neg_truncated();
		printf("%s\n", fails ? "SIM-PROG: FAILED" : "SIM-PROG: truncated control PASS");
	} else if (!strcmp(mode, "oversize")) {
		neg_oversize();
		printf("%s\n", fails ? "SIM-PROG: FAILED" : "SIM-PROG: oversize control PASS");
	} else if (!strcmp(mode, "timeout")) {
		neg_timeout();
		printf("%s\n", fails ? "SIM-PROG: FAILED" : "SIM-PROG: timeout control PASS");
	} else {
		fprintf(stderr, "usage: test_prog_submit [pos|bad-magic|truncated|oversize|timeout]\n");
		return 2;
	}
	reset_all();
	return fails ? 1 : 0;
}
