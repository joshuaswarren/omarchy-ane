#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Mutation proof for test_ane_session_state.c (host only, no device).

Each mutant is ane-session.c with one textual change. The mutated copy is built next to a copy of the test and its
headers in a temp dir and run from tools/ (the fixtures are relative to it). The control is the unmutated source: it must
pass. Every mutant must fail the test (nonzero exit or a FAIL line); a mutant that passes is a test that cannot see that
fault and this script exits 1."""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
SRC = (TOOLS / "ane-session.c").read_text()
LIBANE = TOOLS.parent / "libane"
UAPI = TOOLS.parent / "ane/src/uapi/drm"

MUTANTS = {
    "whole_tile_copy_instead_of_runs": (
        """		for (r = 0; r < rs->nruns; r++) {
			memcpy(rs->buf + rs->offs[r], scratch + rs->offs[r],
			       (size_t)rs->run_bytes);
		}""",
        """		(void)r;
		memcpy(rs->buf, scratch, (size_t)rs->tile_bytes);"""),
    "run_truncated_by_one": ("(size_t)rs->run_bytes);", "(size_t)rs->run_bytes - 1);"),
    "reset_is_a_noop": ("		memset(p->st[i].buf, 0, (size_t)p->st[i].tile_bytes);", "		(void)p->st[i].buf;"),
    "reset_name_resets_all_but_not_all": (
        """		for (i = 0; i < ANE_SESSION_MAX_PROGS; i++) {
			if (s->progs[i].name) {
				state_zero(&s->progs[i]);
			}
		}""", """		for (i = 0; i < 0; i++) {
			state_zero(&s->progs[i]);
		}"""),
    "state_shared_across_names": (
        "	st->buf = calloc(1, (size_t)st->tile_bytes);",
        "	{ static uint8_t *shared; if (!shared) shared = calloc(1, (size_t)st->tile_bytes); st->buf = shared; }"),
    "output_matched_by_input_index": ("if (p->st[i].out_idx == idx) {", "if (p->st[i].in_idx == idx) {"),
    "payload_still_expects_state": ("	in_wire = p->in_total;\n	out_wire = p->out_total;\n	for (k2 = 0; k2 < p->nst; k2++) {\n		in_wire -= p->st[k2].tile_bytes;\n		out_wire -= p->st[k2].tile_bytes;\n	}",
                                    "	in_wire = p->in_total;\n	out_wire = p->out_total;"),
    "reply_still_carries_state": ("		out_wire -= p->st[k2].tile_bytes;\n", ""),
    "no_tile_check": ("pr->ports[ki].tile_bytes != pr->ports[ko].tile_bytes", "0"),
    "no_run_range_check": ("run_bytes > pr->ports[ki].tile_bytes - offs[i]", "0"),
    "no_double_registration_check": ("state_by_in(p, (uint32_t)in_idx) ||\n\t\t\t   state_by_out(p, (uint32_t)out_idx)", "0"),
    "no_loaded_check": ("	} else if (!p) {\n		why = \"not-loaded\";", "	} else if (0) {\n		why = \"not-loaded\";"),
    "state_not_zero_initialised": ("st->buf = calloc(1, (size_t)st->tile_bytes);", "st->buf = malloc((size_t)st->tile_bytes); if (st->buf) memset(st->buf, 0xff, (size_t)st->tile_bytes);"),
    "u64_digit_check_removed": ("if (!s || !*s || *s < '0' || *s > '9') {", "if (!s || !*s) {"),
    "u64_trailing_check_removed": ("if (errno || !end || *end) {", "if (errno || !end) {"),
    "u32_range_check_removed": ("if (!fields_ok || in_idx > UINT32_MAX || out_idx > UINT32_MAX) {", "if (!fields_ok) {"),
    "bad_field_flag_ignored": ("if (!fields_ok || in_idx > UINT32_MAX || out_idx > UINT32_MAX) {", "if (in_idx > UINT32_MAX || out_idx > UINT32_MAX) {"),
    "run_count_garbage_accepted": ("if (!d || extra || parse_u64(d, &nr)) {", "if (!d || extra) {"),
    "extra_token_accepted": ("if (!d || extra || parse_u64(d, &nr)) {", "if (!d || parse_u64(d, &nr)) {"),
    "state_sent_from_wrong_buffer": ("ane_m2_send(k->nn, rs->buf, rs->in_idx);", "ane_m2_send(k->nn, buf, rs->in_idx);"),
}


def build_and_run(src, work):
    (work / "ane-session.c").write_text(src)
    for name in ("test_ane_session_state.c", "ane-run.c", "ane_f16_add.h", "ane_check_band.h", "ane_timing.h",
                 "ane_cpu_floor.h"):
        shutil.copy(TOOLS / name, work / name)
    exe = work / "t"
    cc = subprocess.run(
        ["gcc", "-O1", "-std=gnu99", "-w", f"-I{LIBANE}", f"-I{UAPI}", "-I/usr/include/libdrm",
         "-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap", "-o", str(exe), str(work / "test_ane_session_state.c"),
         str(LIBANE / "ane.c"), str(LIBANE / "ane_m2.c"), str(LIBANE / "ane_sha256.c"), "-lm"],
        capture_output=True, text=True, cwd=TOOLS)
    if cc.returncode:
        return "BUILD-ERROR", cc.stderr[-300:]
    run = subprocess.run([str(exe)], capture_output=True, text=True, errors="replace", cwd=TOOLS, timeout=120, check=False)
    return ("PASS" if run.returncode == 0 and "FAIL" not in run.stderr + run.stdout else "FAIL"), run.stderr[-300:] + run.stdout[-100:]


def main():
    with tempfile.TemporaryDirectory() as d:
        control, detail = build_and_run(SRC, Path(d))
        print(f"control (unmutated): {control}")
        if control != "PASS":
            print(detail)
            return 1
    bad = 0
    for name, (old, new) in MUTANTS.items():
        if SRC.count(old) != 1:
            print(f"MUTANT {name}: pattern found {SRC.count(old)} times, expected 1")
            bad += 1
            continue
        with tempfile.TemporaryDirectory() as d:
            res, detail = build_and_run(SRC.replace(old, new), Path(d))
        killed = res in ("FAIL", "BUILD-ERROR") if res != "BUILD-ERROR" else False
        print(f"mutant {name}: {res}{'  (killed)' if killed else '  SURVIVED' if res == 'PASS' else '  (build error, not counted)'}")
        if res != "FAIL":
            bad += 1
            if res == "BUILD-ERROR":
                print(detail)
    print(f"{len(MUTANTS) - bad} of {len(MUTANTS)} mutants killed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
