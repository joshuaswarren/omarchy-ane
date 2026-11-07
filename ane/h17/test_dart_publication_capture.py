#!/usr/bin/env python3
"""Compile and exercise the actual candidate capture helper, without a device.

Usage: python3 test_dart_publication_capture.py /path/to/patched/apple-dart.c
The minimal C types model only the fields read by the helper, not hardware.
"""

import pathlib
import subprocess
import sys
import tempfile


def function(source, name):
    start = source.index("static void " + name + "(")
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


source = pathlib.Path(sys.argv[1]).read_text()
start = source.index("struct apple_dart_h17_pub_sample {")
end = source.index("static bool apple_dart_h17_pub_enabled", start)
capture = source[start:end] + function(source, "apple_dart_h17_pub_capture")
preamble = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BIT(n) (1U << (n))
#define READ_ONCE(x) (x)
#define ilog2(x) (63 - __builtin_clzll((unsigned long long)(x)))
#define DART_MAX_TTBR 4
struct apple_dart {
    u32 pgsize;
    u64 *locked_ttbr[1][4];
    u64 *locked_owned[1][4];
};
struct io_pgtable_cfg {
    struct { u32 n_levels, n_ttbrs; u64 *ttbr[4]; } apple_dart_cfg;
};
"""
tests = r"""
int main(void) {
    u64 live[2048], owned[2048], host[2048];
    struct apple_dart d = { .pgsize = 16384 };
    struct io_pgtable_cfg cfg = { .apple_dart_cfg = {
        .n_levels = 4, .n_ttbrs = 1, .ttbr = { host } } };
    struct apple_dart_h17_pub_sample s[3] = {};
    for (unsigned int i = 0; i < 2048; i++) {
        live[i] = 10000 + i; owned[i] = 20000 + i; host[i] = 30000 + i;
    }
    d.locked_ttbr[0][0] = live; d.locked_owned[0][0] = owned;
    apple_dart_h17_pub_capture(&d, &cfg, s);
    unsigned int slots[] = {16, 63, 0};
    for (unsigned int i = 0; i < 3; i++) {
        assert(s[i].index == 0 && s[i].slot == slots[i]);
        assert(s[i].present == 7);
        assert(s[i].host == 30000 + slots[i]);
        assert(s[i].owned == 20000 + slots[i]);
        assert(s[i].live == 10000 + slots[i]);
    }
    /* Three-level high candidates must not index beyond the TTBR array. */
    cfg.apple_dart_cfg.n_levels = 3;
    memset(s, 0, sizeof(s)); apple_dart_h17_pub_capture(&d, &cfg, s);
    assert(s[0].index == 16 && s[0].present == 0);
    assert(s[1].index == 63 && s[1].present == 0);
    assert(s[2].index == 0 && s[2].slot == 127 && s[2].present == 7);
    /* Missing roots are reported without dereferencing NULL. */
    cfg.apple_dart_cfg.n_levels = 4;
    d.locked_ttbr[0][0] = NULL; d.locked_owned[0][0] = NULL;
    memset(s, 0, sizeof(s)); apple_dart_h17_pub_capture(&d, &cfg, s);
    assert(s[1].present == 1 && s[1].host == 30063);
    /* Retirement may leave a dangling owned pointer after clearing live. */
    d.locked_owned[0][0] = (u64 *)(uintptr_t)1;
    memset(s, 0, sizeof(s)); apple_dart_h17_pub_capture(&d, &cfg, s);
    assert(s[1].present == 1 && s[1].host == 30063);
    cfg.apple_dart_cfg.ttbr[0] = NULL;
    memset(s, 0, sizeof(s)); apple_dart_h17_pub_capture(&d, &cfg, s);
    assert(s[1].present == 0);
    /* No claimed tables or unsupported geometry: no table reads. */
    cfg.apple_dart_cfg.n_ttbrs = 0;
    memset(s, 0, sizeof(s)); apple_dart_h17_pub_capture(&d, &cfg, s);
    assert(s[0].present == 0 && s[1].present == 0 && s[2].present == 0);
    cfg.apple_dart_cfg.n_levels = 2;
    memset(s, 0, sizeof(s)); apple_dart_h17_pub_capture(&d, &cfg, s);
    assert(s[0].present == 0 && s[1].present == 0 && s[2].present == 0);
    puts("PASS: four-level slots; three-level bounds; missing roots; zero tables; unsupported levels");
}
"""
with tempfile.TemporaryDirectory(prefix="h17-pub-", dir="/var/tmp/opencode") as tmp:
    c = pathlib.Path(tmp) / "test.c"
    binary = pathlib.Path(tmp) / "test"
    c.write_text(preamble + capture + tests)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=undefined", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
