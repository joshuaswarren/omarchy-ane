#!/usr/bin/env python3
"""Exercise the real task-manager poll and execute functions with simulated MMIO."""
import pathlib
import subprocess
import tempfile

source = (pathlib.Path(__file__).resolve().parents[1] / 'src/ane_tm.c').read_text()
start = source.index('static int ane_tm_poll_step(')
end = source.index('\n/*\n * Bounded recovery', start)
functions = source[start:end]
harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
struct ane_device { int engine_lock; int wedged; int tm_retention; int dev; bool scratch_retained, quiesced; u32 last_overflow, last_info; };
struct ane_request { int qid; int nid; };
struct ane_dart_scratch { int unused; };
#define ANE_DART_SCRATCH_MAX 4
#define THIS_MODULE 0
#define TQ_NID1(qid) 0
#define TQ_STATUS(qid) 1
#define lockdep_assert_held(x) ((void)0)
#define wmb() ((void)0)
#define rmb() ((void)0)
#define dev_err(...) ((void)0)
#define __module_get(x) ((void)0)
#define atomic_xchg(ptr, value) (*(ptr) = (value), 0)
#define read_poll_timeout(op, status, cond, delay, timeout, sleep, ...) \
    ((status) = op(__VA_ARGS__), 0)
static int reads, scenario, recovery, recovery_result, pages_released, released_before_recovery;
static bool ane_dart_faulted(struct ane_device *ane, u64 *iova, u32 *status) {
    (void)ane; (void)iova; (void)status;
    if (scenario == 1) return reads++ == 0;
    if (scenario == 2 || scenario == 3) { int n = reads++; return n < (scenario == 2 ? 3 : 2); }
    return false;
}
static int ane_tm_collect_events(struct ane_device *ane, struct ane_request *req, u32 *finished) {
    (void)ane; if (!req) return 0; *finished = 3; return 1;
}
static void ane_dart_mask(struct ane_device *a) { (void)a; }
static void ane_dart_unmask(struct ane_device *a) { (void)a; }
static void ane_tm_push_tq(struct ane_device *a, struct ane_request *r) { (void)a; (void)r; }
static int ane_dart_drain_fault(struct ane_device *a, u64 i, struct ane_dart_scratch *s, int *p) {
    (void)a; (void)i; (void)s; (*p)++; return 0;
}
static void ane_dart_release_scratch(struct ane_device *a, struct ane_dart_scratch *s, int p) {
    (void)a; (void)s; pages_released += p;
    if (p && !recovery) released_before_recovery = 1;
}
static int ane_tm_recover(struct ane_device *a) { (void)a; recovery++; return recovery_result; }
static u32 tq_read32(struct ane_device *a, int r) { (void)a; (void)r; return 4 << 8; }
static void tq_write32(struct ane_device *a, int r, u32 v) { (void)a; (void)r; (void)v; }
'''
main = r'''
int main(void) {
    struct ane_device dev = {0};
    struct ane_request req = {.qid = 4, .nid = 4};
    int result;
    scenario = 0; reads = 0; recovery = 0;
    result = ane_tm_execute(&dev, &req);
    if (result != 0 || recovery != 0) { fprintf(stderr, "normal completion failed: %d\n", result); return 1; }
    scenario = 1; reads = 0; recovery = 0;
    result = ane_tm_execute(&dev, &req);
    if (result == 0 || recovery != 1) {
        fprintf(stderr, "transient DART fault returned %d; recovery=%d\n", result, recovery);
        return 1;
    }
    scenario = 3; reads = 0; recovery = 0; pages_released = 0;
    result = ane_tm_execute(&dev, &req);
    if (result != -EIO || pages_released != 1 || recovery || dev.scratch_retained) {
        fprintf(stderr, "completed drain: rc=%d released=%d recovery=%d\n", result, pages_released, recovery);
        return 1;
    }
    scenario = 2; reads = 0; recovery = 0; pages_released = 0; released_before_recovery = 0;
    result = ane_tm_execute(&dev, &req);
    if (result != -EIO || pages_released || recovery || !dev.scratch_retained) {
        fprintf(stderr, "fault after scratch: rc=%d released=%d recovery=%d retained=%d\n",
                result, pages_released, recovery, dev.scratch_retained);
        return 1;
    }
    puts("normal, transient and scratch lifetime: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    binary = pathlib.Path(tmp) / 'tm-test'
    subprocess.run(['cc', '-std=gnu11', '-x', 'c', '-o', str(binary), '-'],
                   input=harness + functions + main, text=True, check=True)
    subprocess.run([str(binary)], check=True)
