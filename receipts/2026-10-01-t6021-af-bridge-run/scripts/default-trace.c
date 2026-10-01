/* Throwaway: print every io call of ane_t6021_boot_run for default configs. */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#include "ane_t6021_boot.h"

static int polls;
static u32 rd32(void *c, unsigned int o)
{
	(void)c;
	printf("rd32 %#x\n", o);
	if (o == ANE_T6021_BOOT_REG_SCRATCH7)
		return ++polls % 3 ? 0 : ANE_T6021_BOOT_ACK;
	return 0x1234;
}
static u64 rd64(void *c, unsigned int o) { (void)c; printf("rd64 %#x\n", o); return 1; }
static void wr32(void *c, unsigned int o, u32 v) { (void)c; printf("wr32 %#x %#x\n", o, v); }
static void wr64(void *c, unsigned int o, u64 v) { (void)c; printf("wr64 %#x %#llx\n", o, (unsigned long long)v); }
static void bar(void *c) { (void)c; printf("barrier\n"); }
static void ph(void *c, const char *w) { (void)c; printf("phase %s\n", w); }
static void pw(void *c) { (void)c; printf("poll\n"); }
static int prep(void *c, u32 *lo, u32 *hi) { (void)c; *lo = 0xa; *hi = 0xb; printf("prepare\n"); return 0; }

int main(void)
{
	static const struct ane_t6021_boot_io io = {
		.rd32 = rd32, .rd64 = rd64, .wr32 = wr32, .wr64 = wr64,
		.publish_barrier = bar, .phase = ph, .poll_wait = pw, .prepare = prep,
	};
	int tm, rtb, sa;

	for (tm = 0; tm <= 2; tm++)
		for (rtb = 0; rtb <= 1; rtb++)
			for (sa = 0; sa <= 4; sa++) {
				struct ane_t6021_boot_cfg cfg = {
					.preflight_ok = 1, .preboot_table_mode = tm,
					.fw_dva = 0xdeadbeef000ULL, .rtb_mode = rtb,
					.stop_after = sa,
				};
				int cs, fa, bo, r;
				u64 s;

				polls = 0;
				printf("== tm %d rtb %d stop %d\n", tm, rtb, sa);
				r = ane_t6021_boot_run(&io, &cfg, &cs, &fa, &bo, &s);
				printf("r %d cs %d fa %d bo %d s %#llx\n", r, cs, fa, bo, (unsigned long long)s);
			}
	return 0;
}
