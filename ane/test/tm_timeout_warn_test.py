#!/usr/bin/env python3
"""Compile the real tm_timeout param setter and slow-complete predicate; no device proof.

Checks the #447 contract on host: the sysfs setter accepts 100..10000 ms and
rejects out-of-range values without writing, and the 75% slow-completion
predicate fires only strictly above the threshold.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    return text[start:end]


tm = (ROOT / 'ane/src/ane_tm.c').read_text()
defines = '\n'.join(
    line for line in tm.splitlines()
    if line.startswith('#define ANE_TM_TIMEOUT_'))
parts = [
    defines,
    function(tm, 'static int ane_tm_timeout_set('),
    function(tm, 'static bool ane_tm_slow_complete('),
]
HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t u64;
#define EINVAL 22
#define WRITE_ONCE(x, v) ((x) = (v))
struct kernel_param { void *arg; };
static int kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	char *end;
	unsigned long v;
	if (!s || !*s)
		return -EINVAL;
	v = strtoul(s, &end, base);
	if (*end && *end != '\n')
		return -EINVAL;
	if (v > 0xfffffffful)
		return -EINVAL;
	*res = (unsigned int)v;
	return 0;
}
%s
int main(void)
{
	unsigned int value;
	struct kernel_param kp = { .arg = &value };
	int failures = 0;

	value = 1000;
	if (ane_tm_timeout_set("99", &kp) != -EINVAL || value != 1000) { puts("FAIL: 99 accepted or wrote"); failures++; }
	if (ane_tm_timeout_set("10001", &kp) != -EINVAL || value != 1000) { puts("FAIL: 10001 accepted or wrote"); failures++; }
	if (ane_tm_timeout_set("0", &kp) != -EINVAL || value != 1000) { puts("FAIL: 0 accepted or wrote"); failures++; }
	if (ane_tm_timeout_set("abc", &kp) == 0 || value != 1000) { puts("FAIL: garbage accepted or wrote"); failures++; }
	if (ane_tm_timeout_set("100", &kp) != 0 || value != 100) { puts("FAIL: 100 rejected or not written"); failures++; }
	if (ane_tm_timeout_set("10000", &kp) != 0 || value != 10000) { puts("FAIL: 10000 rejected or not written"); failures++; }
	if (ane_tm_timeout_set("2500", &kp) != 0 || value != 2500) { puts("FAIL: 2500 rejected or not written"); failures++; }

	if (ane_tm_slow_complete(0, 1000)) { puts("FAIL: zero elapsed warns"); failures++; }
	if (ane_tm_slow_complete(750000, 1000)) { puts("FAIL: exactly 75%% warns"); failures++; }
	if (!ane_tm_slow_complete(750001, 1000)) { puts("FAIL: just over 75%% does not warn"); failures++; }
	if (ane_tm_slow_complete(7500000, 10000)) { puts("FAIL: exactly 75%% of max warns"); failures++; }
	if (!ane_tm_slow_complete(7500001, 10000)) { puts("FAIL: just over 75%% of max does not warn"); failures++; }
	if (!ane_tm_slow_complete(3990000, 5000)) { puts("FAIL: 3990ms at 5000ms budget does not warn"); failures++; }
	if (ane_tm_slow_complete(75000, 100)) { puts("FAIL: exactly 75%% of min warns"); failures++; }
	printf("RESULT: %%d failures\n", failures);
	return failures != 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ane-tm-timeout-') as work:
    work = pathlib.Path(work)
    source = work / 'test.c'
    source.write_text(HARNESS % '\n'.join(parts))
    build = subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-o', str(work / 'test'), str(source)], capture_output=True, text=True, check=False)
    if build.returncode:
        print(build.stderr, file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(subprocess.run([str(work / 'test')], check=False).returncode)
