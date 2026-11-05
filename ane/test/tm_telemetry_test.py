"""Compile the real ane_tm_collect_events for two fake engines; no device proof.

Engine A reads two events on line 0 then overflows the event count on line 1.
Engine B, collected in between, reads different events. Each engine's overflow
and last-INFO words must stay its own, so a failure log never reports the
other engine's event metadata.
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
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define EIO 5
#define BIT(n) (1u << (n))
#define TM_IRQ_EVTC(line) (0x14 + (line * 0x14))
#define TM_IRQ_INFO(line) (0x18 + (line * 0x14))
#define TM_IRQ_UNK1(line) (0x1c + (line * 0x14))
#define TM_IRQ_TMST(line) (0x20 + (line * 0x14))
#define TM_IRQ_UNK2(line) (0x24 + (line * 0x14))
#define TM_STATUS 0x54
#define TM_IRQ_ACK 0x6c
#define TM_IS_IDLE 1
struct ane_request { u32 nid, td_count; };
struct ane_device { u32 evtc[2], info[2][8], popped[2], last_overflow, last_info; };
static u32 ane_last_tmst;
static u32 tm_read32(struct ane_device *ane, u32 off)
{
	for (unsigned int line = 0; line < 2; line++) {
		if (off == TM_IRQ_EVTC(line))
			return ane->evtc[line];
		if (off == TM_IRQ_INFO(line))
			return ane->info[line][ane->popped[line]++];
	}
	return 0;
}
static void tm_write32(struct ane_device *ane, u32 off, u32 v) { }
%s
int main(void)
{
	struct ane_device a = { .evtc = {2, 100}, .info = {{0x0a000001, 0x0a000002}} };
	struct ane_device b = { .evtc = {1, 0}, .info = {{0x0b400041}} };
	u32 fa = 0, fb = 0;
	int failures = 0;

	if (ane_tm_collect_events(&a, NULL, &fa) != -EIO) { puts("FAIL: A did not report overflow"); failures++; }
	ane_tm_collect_events(&b, NULL, &fb);
	if (a.last_overflow != ((1u << 31) | 100)) { puts("FAIL: A overflow word changed by B"); failures++; }
	if (a.last_info != 0x0a000002) { printf("FAIL: A last_info %%#x\n", a.last_info); failures++; }
	if (b.last_overflow != 0 || b.last_info != 0x0b400041) { puts("FAIL: B metadata wrong"); failures++; }
	printf("RESULT: %%d failures\n", failures);
	return failures != 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ane-tm-telemetry-') as work:
    work = pathlib.Path(work)
    source = work / 'test.c'
    source.write_text(HARNESS % function(tm, 'static int ane_tm_collect_events('))
    build = subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-o', str(work / 'test'), str(source)], capture_output=True, text=True, check=False)
    if build.returncode:
        print(build.stderr, file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(subprocess.run([str(work / 'test')], check=False).returncode)
