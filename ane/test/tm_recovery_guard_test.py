#!/usr/bin/env python3
"""Exercise real recovery and postclose with retained scratch."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1] / 'src'
tm = (root / 'ane_tm.c').read_text()
drv = (root / 'ane_drv.c').read_text()
recovery = tm[tm.index('int ane_tm_recover('):]
post = drv[drv.index('static void ane_drm_postclose('):drv.index('\n/*\n * The driver only owns', drv.index('static void ane_drm_postclose('))]
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint32_t u32;
struct device { int unused; };
struct ane_device { int engine_lock; bool scratch_retained, removed, tm_retention, por_pending, quiesced; int wedged; struct device *dev; char *engine; u32 tm_status_fresh; };
struct drm_device { void *dev_private; };
struct drm_file { int unused; };
static int cycles, module_puts, pm_gets, pm_puts;
#define THIS_MODULE 0
#define ANE_TM_BASE 0
#define TM_STATUS 0
#define TM_IS_IDLE 1
#define lockdep_assert_held(x) ((void)0)
#define mutex_lock(x) ((void)0)
#define mutex_unlock(x) ((void)0)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define atomic_read(p) (*(p))
#define atomic_xchg(p, v) ({ int old = *(p); *(p) = (v); old; })
#define module_put(x) (module_puts++)
#define readl_poll_timeout(addr, status, cond, delay, timeout) ((status) = 1, 0)
static int ane_pd_cycle(struct ane_device *a) { (void)a; cycles++; return 0; }
static int ane_ps_verify_on(struct ane_device *a) { (void)a; return 0; }
static void ane_tm_drain_retained(struct ane_device *a) { (void)a; }
static void ane_tm_enable(struct ane_device *a, bool b) { (void)a; (void)b; }
static u32 ane_rec_read32(struct ane_device *a, const char *s, void *p) { (void)a; (void)s; (void)p; return 1; }
static void ane_reclaim_preserved(struct ane_device *a) { (void)a; }
static int pm_runtime_resume_and_get(struct device *d) { (void)d; pm_gets++; return 0; }
static void pm_runtime_mark_last_busy(struct device *d) { (void)d; }
static void pm_runtime_put_autosuspend(struct device *d) { (void)d; pm_puts++; }
'''
main = r'''
int main(void) {
    struct device dev = {0}; struct ane_device ane = {.dev=&dev, .wedged=1, .scratch_retained=true};
    struct drm_device drm = {.dev_private=&ane}; struct drm_file file = {0};
    int rc = ane_tm_recover(&ane);
    ane_drm_postclose(&drm, &file);
    if (rc != -EBUSY || cycles || module_puts || ane.wedged != 1 || pm_gets != 1 || pm_puts != 1) {
        fprintf(stderr, "guard: rc=%d cycles=%d puts=%d wedged=%d pm=%d/%d\n", rc, cycles, module_puts, ane.wedged, pm_gets, pm_puts);
        return 1;
    }
    printf("real recovery guard and postclose preserve wedge: PASS\n"); return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    binary = pathlib.Path(tmp) / 'recovery-test'
    subprocess.run(['cc', '-std=gnu11', '-x', 'c', '-o', str(binary), '-'],
                   input=prefix + recovery + post + main, text=True, check=True)
    subprocess.run([str(binary)], check=True)
