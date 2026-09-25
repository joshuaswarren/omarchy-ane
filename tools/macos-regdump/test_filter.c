/* Unit test for the banned-offset filter and the pmgr gate. */
#include <stdio.h>
#include "ane_regdump_filter.h"

static int fails;

static void expect(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL %s\n", what);
		fails++;
	}
}

int main(void)
{
	uint32_t ps[ANE_ISLAND_COUNT];
	size_t i;

	expect(ane_word_banned(ANE_BAN_IRQ_POP0, 4), "pop word itself");
	expect(ane_word_banned(ANE_BAN_IRQ_POP1, 4), "pop word 0x81c");
	expect(ane_word_banned(ANE_BAN_IRQ_POP2, 4), "pop word 0x820");
	expect(ane_word_banned(ANE_BAN_IRQ_POP0 - 2, 4), "overlap below");
	expect(ane_word_banned(ANE_BAN_IRQ_POP2 + 2, 4), "overlap above");
	expect(!ane_word_banned(ANE_BAN_IRQ_POP0 - 4, 4), "word before");
	expect(!ane_word_banned(ANE_BAN_IRQ_POP2 + 4, 4), "word after");

	expect(ane_word_banned(ANE_BAN_A2I_RECV0, 8), "A2I recv pair");
	expect(ane_word_banned(ANE_BAN_I2A_RECV0, 8), "I2A recv pair");
	expect(!ane_word_banned(0x01408800, 8), "A2I send is safe");
	expect(!ane_word_banned(0x01408820, 4), "I2A control is safe");

	expect(ane_word_banned(ANE_CORESIGHT_OFF, 4), "coresight base");
	expect(ane_word_banned(ANE_CORESIGHT_OFF + ANE_CORESIGHT_LEN - 4, 4),
	       "coresight tail");
	expect(!ane_word_banned(ANE_CORESIGHT_OFF - 4, 4), "before coresight");
	expect(!ane_word_banned(ANE_CORESIGHT_OFF + ANE_CORESIGHT_LEN, 4),
	       "after coresight");

	for (i = 0; i < ANE_ISLAND_COUNT; i++)
		ps[i] = 0xf0;
	expect(ane_islands_up(ps), "all actual 0xf");
	ps[3] = 0x00;
	expect(!ane_islands_up(ps), "one island down");
	ps[3] = 0x0f;
	expect(!ane_islands_up(ps), "actual in the wrong nibble");
	ps[3] = 0xff;
	expect(ane_islands_up(ps), "target bits ignored");

	if (fails) {
		fprintf(stderr, "%d failed\n", fails);
		return 1;
	}
	printf("filter ok\n");
	return 0;
}
