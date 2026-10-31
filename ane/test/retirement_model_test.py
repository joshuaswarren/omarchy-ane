"""Compile real close/recovery functions with CPU kernel seams; no device proof."""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def function(text, signature):
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    return text[start:end]


driver = (ROOT / 'ane/src/ane_drv.c').read_text()
tm = (ROOT / 'ane/src/ane_tm.c').read_text()
parts = [function(tm, 'int ane_tm_recover(')]
if 'void ane_wedge_clear(' in tm:
    parts.append(function(tm, 'void ane_wedge_clear('))
parts.append(function(driver, 'static void ane_drm_postclose('))
COMMON = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define THIS_MODULE 0
#define ANE_TM_BASE 0x20000
#define TM_STATUS 0x54
#define TM_IS_IDLE 1
#define ETIMEDOUT 110
#define EIO 5
typedef uint32_t u32;
struct ane_device {
    int wedged, engine_lock;
    bool removed, tm_retention;
    u32 tm_status_fresh;
    char *engine;
    void *dev;
};
struct drm_device { struct ane_device *dev_private; };
struct drm_file { int unused; };
static int pins, cycle_rc, verify_rc, cycles, verifies, drains, enables;
static int polls, reads, reclaims, violations, failures;
static bool poll_complete;
static u32 status_value;
#define atomic_read(p) (*(p))
static int atomic_xchg(int *p, int value) { int old = *p; *p = value; return old; }
static void mutex_lock(int *p) { if (*p) violations++; *p = 1; }
static void mutex_unlock(int *p) { if (!*p) violations++; *p = 0; }
#define lockdep_assert_held(p) do { if (!*(p)) violations++; } while (0)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
static void module_put(int unused) { if (!poll_complete) violations++; pins--; }
static int ane_pd_cycle(struct ane_device *ane) { cycles++; return cycle_rc; }
static int ane_ps_verify_on(struct ane_device *ane) { verifies++; return verify_rc; }
static void ane_tm_drain_retained(struct ane_device *ane) { drains++; }
static void ane_tm_enable(struct ane_device *ane, bool recovery) { enables++; }
static u32 ane_rec_read32(struct ane_device *ane, const char *name, void *addr) {
    reads++; return status_value;
}
static void ane_reclaim_preserved(struct ane_device *ane) {
    if (!poll_complete || ane->wedged) violations++;
    reclaims++;
}
#define readl_poll_timeout(addr, value, condition, sleep_us, timeout_us) \
    ({ polls++; (value) = status_value; int rc = (condition) ? 0 : -ETIMEDOUT; \
       poll_complete = !rc; rc; })
'''
TEST = r'''
static char engine[0x30000];
static struct ane_device ane;
static struct drm_device drm = { .dev_private = &ane };
#define EXPECT(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); failures++; } } while (0)
static void fresh(void) {
    memset(&ane, 0, sizeof(ane)); ane.engine = engine;
    ane.wedged = 1; ane.tm_retention = true; ane.tm_status_fresh = 0x22222222;
    pins = 1; cycle_rc = verify_rc = cycles = verifies = drains = enables = 0;
    polls = reads = reclaims = violations = 0; poll_complete = false;
    status_value = TM_IS_IDLE;
}
static void close_session(void) { ane_drm_postclose(&drm, NULL); }
int main(void) {
    fresh(); ane.wedged = 0; pins = 0; close_session();
    EXPECT(!cycles && !pins && !reclaims && !violations, "healthy close performs no reset or release");
    fresh(); close_session();
    EXPECT(cycles == 1 && verifies == 1 && polls == 1 && reads == 1, "wedged close reaches real recovery stages");
    EXPECT(!ane.wedged && !pins && reclaims == 1 && !violations, "only successful idle poll releases wedge/pin/mappings");
    close_session();
    EXPECT(cycles == 1 && reclaims == 1 && !pins && !violations, "repeated clean close does not double-release");
    fresh(); cycle_rc = -EIO; close_session();
    EXPECT(ane.wedged && pins == 1 && !reclaims && cycles == 1 && !verifies && !polls && !violations, "power failure preserves wedge/pin/mappings");
    cycle_rc = 0; close_session();
    EXPECT(!ane.wedged && !pins && reclaims == 1 && cycles == 2 && !violations, "later successful recovery can retire preserved state once");
    fresh(); verify_rc = -ETIMEDOUT; close_session();
    EXPECT(ane.wedged && pins == 1 && !reclaims && verifies == 1 && !drains && !enables && !polls && !reads && !violations, "power-on verification failure performs no engine access or release");
    fresh(); status_value = 0; close_session();
    EXPECT(ane.wedged && pins == 1 && !reclaims && polls == 1 && !violations, "idle timeout preserves wedge/pin/mappings");
    fresh(); ane.removed = true; close_session();
    EXPECT(ane.wedged && pins == 1 && !cycles && !reclaims && !ane.engine_lock && !violations, "removed close does not reset or clear retained state");
    fresh(); ane.engine_lock = 1; cycle_rc = -EIO;
    EXPECT(ane_tm_recover(&ane) == -EIO && ane.wedged && pins == 1, "original power-cycle error propagates");
    fresh(); ane.engine_lock = 1; verify_rc = -ETIMEDOUT;
    EXPECT(ane_tm_recover(&ane) == -ETIMEDOUT && ane.wedged && pins == 1, "original power-on error propagates");
    fresh(); ane.engine_lock = 1; status_value = 0;
    EXPECT(ane_tm_recover(&ane) == -ETIMEDOUT && ane.wedged && pins == 1, "original idle-poll error propagates");
    printf("RESULT: %d behavioral failures\n", failures);
    return failures != 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ane-retirement-') as work:
    work = pathlib.Path(work)
    source = work / 'test.c'
    source.write_text(COMMON + '\n'.join(parts) + TEST)
    build = subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-o', str(work / 'test'), str(source)], capture_output=True, text=True, check=False)
    if build.returncode:
        print(build.stderr, file=sys.stderr)
        raise SystemExit(2)
    result = subprocess.run([str(work / 'test')], check=False)
    raise SystemExit(result.returncode)
