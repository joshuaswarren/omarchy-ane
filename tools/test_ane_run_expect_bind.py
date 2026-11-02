#!/usr/bin/env python3
"""Host check for ane-run --expect-bind: compile the real functions, no device.

The case is the Grandpa 2026-11-02 probe shape: six inputs and three outputs,
whose converter planned outputs on channels 4..6 and inputs on 7..12 while its
rewritten task stream wrote channels 6, 9 and 10. libane binds roles from the
stream, so --expect-bind with the converter's plan must refuse, and with the
stream's real layout must pass.
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TASK_WORDS = 16
DMA_ON_SRC = 0x33881
DMA_ON_DST = 0xC1


def function(text, signature):
    start = text.index(signature)
    end = text.index("\n}", start) + 2
    return text[start:end]


def task(src, dst, next_offset):
    """One H13 task: ten header words, a source-1 and a destination record."""
    words = [0] * 10
    words[1] = (TASK_WORDS - 1) << 16
    words[7] = next_offset
    words[8] = src | (dst << 12)
    words += [0x13800, DMA_ON_SRC, 0x17800, DMA_ON_DST, 0, 0]
    assert len(words) == TASK_WORDS
    return struct.pack(f"<{TASK_WORDS}I", *words)


def stream(pairs):
    size = TASK_WORDS * 4
    return b"".join(task(s, d, (i + 1) * size) for i, (s, d) in enumerate(pairs))


# Reads 4, 5, 7, 8, 11 and 12; writes 6, 9 and 10: the post-rewire stream.
GRANDPA = [(4, 6), (5, 9), (7, 10), (8, 6), (11, 9), (12, 10)]

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ane_bind.h"
%(functions)s
int main(int argc, char **argv)
{
	static uint8_t raw[sizeof(struct anec)];
	static struct ane_nn nn;
	FILE *f = argc > 2 ? fopen(argv[1], "rb") : NULL;
	static uint8_t task[1 << 16];
	size_t n = f ? fread(task, 1, sizeof(task), f) : 0;
	uint64_t size = n;
	uint32_t td_size = %(td_size)d, td_count = %(td_count)d;
	uint32_t srcs = 6, dsts = 3, tile = 1;

	if (!f)
		return 5;
	fclose(f);
	memcpy(raw + 0, &size, 8);
	memcpy(raw + 8, &td_size, 4);
	memcpy(raw + 12, &td_count, 4);
	memcpy(raw + 32, &srcs, 4);
	memcpy(raw + 36, &dsts, 4);
	for (int ch = 4; ch <= 12; ch++)
		memcpy(raw + 40 + 4 * ch, &tile, 4);
	memcpy(&nn.anec, raw, sizeof(raw));
	if (ane_bind_init(&nn.anec, task, size, &nn.bind) != 1)
		return 3;
	return check_expected_bind(&nn, argv[2]) < 0 ? 4 :
	       check_expected_bind(&nn, argv[2]);
}
'''


def build(work):
    run_c = (ROOT / "tools/ane-run.c").read_text()
    functions = "\n".join(function(run_c, sig) for sig in (
        "static int parse_channel_list(",
        "static void print_channels(",
        "static int check_expected_bind(",
    ))
    source = work / "harness.c"
    source.write_text(HARNESS % {"functions": functions,
                                 "td_size": TASK_WORDS * 4,
                                 "td_count": len(GRANDPA)})
    binary = work / "harness"
    result = subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
         "-I", str(ROOT / "libane"), "-o", str(binary), str(source)],
        capture_output=True, text=True, check=False)
    if result.returncode:
        sys.exit(f"harness build failed:\n{result.stderr}")
    blob = work / "stream.bin"
    blob.write_bytes(stream(GRANDPA))
    return binary, blob


def main():
    failures = 0

    def expect(ok, name):
        nonlocal failures
        print(("PASS " if ok else "FAIL ") + name)
        failures += not ok

    with tempfile.TemporaryDirectory(prefix="expect-bind-") as work:
        binary, blob = build(Path(work))

        def run(spec):
            return subprocess.run([str(binary), str(blob), spec],
                                  capture_output=True, text=True, check=False)

        bad = run("7,8,9,10,11,12:4,5,6")
        expect(bad.returncode == 1
               and "in=4,5,7,8,11,12 out=6,9,10" in bad.stderr
               and "refusing before any submit" in bad.stderr,
               "a converter plan that disagrees with its stream is refused")
        good = run("4,5,7,8,11,12:6,9,10")
        expect(good.returncode == 0 and good.stderr == "",
               "a plan matching the stream passes silently")
        for spec in ("", "4,5", "4,5:", "4,x:6", "4,5:6,32", ":"):
            expect(run(spec).returncode == 4, f"malformed spec {spec!r} is an error")
    print(f"RESULT: {failures} failures")
    return failures != 0


if __name__ == "__main__":
    raise SystemExit(main())
