// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Independent callback traces and injected errors; no physical addresses,
 * register constants, syscall wrappers or fake firmware from the module. */
#include <stdio.h>
#include <string.h>
#include "../transaction.h"

enum failure { NONE, BEGIN, ACQUIRE, ARM, PENDING, READ, PET, WAIT,
	STOP, RELEASE, FINISH };
struct fixture {
	enum failure fail;
	int error, owner, armed, outcome, id_error, protocol_error;
	unsigned int pending, reads, pets, waits, stops, releases, finishes;
	h17_tx_word id;
	unsigned int sequence, match_after;
	char trace[128];
	unsigned int used;
};
static int failures, tests;

static void check(const char *name, int ok)
{
	tests++;
	if (!ok) {
		fprintf(stderr, "FAILED %s\n", name);
		failures++;
	}
}
static void event(struct fixture *f, char c)
{
	if (f->used < sizeof(f->trace) - 1)
		f->trace[f->used++] = c;
}
static int result(struct fixture *f, enum failure point)
{
	return f->fail == point ? f->error : 0;
}
static int begin(void *ctx, h17_tx_word id)
{
	struct fixture *f = ctx;
	event(f, 'B');
	f->id = id;
	return result(f, BEGIN);
}
static int acquire(void *ctx)
{
	struct fixture *f = ctx;
	int ret = result(f, ACQUIRE);
	event(f, 'O');
	if (!ret)
		f->owner = 1;
	return ret;
}
static int arm(void *ctx, unsigned int ms)
{
	struct fixture *f = ctx;
	event(f, 'A');
	if (!f->owner || ms != 30000)
		f->protocol_error = 1;
	/* An arm failure can leave an owned device partially armed. */
	f->armed = 1;
	return result(f, ARM);
}
static int pending(void *ctx, h17_tx_word id, unsigned int seq,
		   const struct h17_tx_step *s)
{
	struct fixture *f = ctx;
	event(f, 'P');
	f->pending++;
	if (id != f->id || seq != ++f->sequence)
		f->id_error = 1;
	if (!f->armed || s->off != 0x40 || s->width != 4)
		f->protocol_error = 1;
	return result(f, PENDING);
}
static int read_value(void *ctx, unsigned int off, unsigned int width,
		      h17_tx_word *value)
{
	struct fixture *f = ctx;
	event(f, 'R');
	f->reads++;
	if (!f->owner || !f->armed || off != 0x40 || width != 4)
		f->protocol_error = 1;
	*value = f->match_after && f->reads >= f->match_after ? 2 : 0;
	return result(f, READ);
}
static int pet(void *ctx)
{
	struct fixture *f = ctx;
	event(f, 'K');
	f->pets++;
	return result(f, PET);
}
static int wait_ms(void *ctx, unsigned int ms)
{
	struct fixture *f = ctx;
	event(f, 'W');
	f->waits++;
	if (ms != 10)
		f->protocol_error = 1;
	return result(f, WAIT);
}
static int stop(void *ctx)
{
	struct fixture *f = ctx;
	int ret = result(f, STOP);
	event(f, 'S');
	f->stops++;
	if (!f->owner)
		f->protocol_error = 1;
	if (!ret)
		f->armed = 0;
	return ret;
}
static int release(void *ctx)
{
	struct fixture *f = ctx;
	event(f, 'L');
	f->releases++;
	if (!f->owner || f->armed)
		f->protocol_error = 1;
	if (!result(f, RELEASE))
		f->owner = 0;
	return result(f, RELEASE);
}
static int finish(void *ctx, h17_tx_word id, int outcome)
{
	struct fixture *f = ctx;
	event(f, 'F');
	f->finishes++;
	f->outcome = outcome;
	if (id != f->id)
		f->id_error = 1;
	if (!outcome && (f->owner || f->armed))
		f->protocol_error = 1;
	return result(f, FINISH);
}
static const struct h17_tx_ops ops = {
	.begin = begin, .pending = pending, .finish = finish,
	.acquire = acquire, .arm = arm, .pet = pet, .stop = stop,
	.release = release, .read = read_value, .wait_ms = wait_ms,
};

static void run_case(const char *name, enum failure fail, int error,
		     unsigned int polls, unsigned int match_after,
		     int expected, const char *trace)
{
	struct fixture f = { .fail = fail, .error = error,
		.match_after = match_after };
	struct h17_tx_step s = { .off = 0x40, .width = 4,
		.direction = H17_TX_READ, .polls = polls, .interval_ms = 10 };
	struct h17_tx_plan p = { .run_id = 0x123456789ULL, .qualified = 1,
		.resource_bytes = 0x100, .recovery_ms = 30000, .count = 1, .steps = &s };
	h17_tx_word value = ~0ULL;
	unsigned int committed = 99;
	int ret;

	if (polls > 1) {
		s.mask = 3;
		s.expected = 2;
	}
	if (fail == ACQUIRE && error == -EBUSY)
		f.armed = 1; /* Another owner's watchdog must remain untouched. */
	ret = h17_tx_run(&p, &ops, &f, &value, 1, &committed);
	check(name, ret == expected);
	check(name, committed == (expected ? 0U : 1U));
	check(name, !strcmp(f.trace, trace));
	check(name, !f.id_error && !f.protocol_error);
	if (fail == ACQUIRE && error == -EBUSY)
		check("busy-owner-preserved", f.armed && !f.stops && !f.releases);
	if (fail == PENDING)
		check("pending-failure-no-read", !f.reads);
	if (fail == STOP)
		check("failed-stop-retains-owner", f.owner && f.armed && !f.releases);
	if (expected == -ETIMEDOUT)
		check("timeout-is-not-ack", f.reads == polls && f.waits == polls - 1 &&
		      f.outcome == -ETIMEDOUT);
}

static void validation_cases(void)
{
	struct h17_tx_step good = { .off = 0x40, .width = 4,
		.direction = H17_TX_READ, .polls = 1 };
	struct h17_tx_plan good_plan = { .run_id = 7, .qualified = 1,
		.resource_bytes = 0x100, .recovery_ms = 30000, .count = 1 };
	unsigned int n;

	for (n = 0; n < 18; n++) {
		struct fixture f = {0};
		struct h17_tx_step s = good;
		struct h17_tx_plan p = good_plan;
		h17_tx_word value = 0;
		unsigned int committed = 42;
		int expected = -EINVAL, ret;

		p.steps = &s;
		switch (n) {
		case 0: p.qualified = 0; expected = -EOPNOTSUPP; break;
		case 1: p.run_id = 0; break;
		case 2: p.steps = 0; break;
		case 3: p.count = 0; break;
		case 4: p.count = 65; break;
		case 5: p.resource_bytes = 2; break;
		case 6: p.recovery_ms = 1; break;
		case 7: p.recovery_ms = ~0U; break;
		case 8: s.direction = H17_TX_WRITE; expected = -EOPNOTSUPP; break;
		case 9: s.width = 8; s.off = 0x195c284; p.resource_bytes = 0x2000000; break;
		case 10: s.off = 0x100; break;
		case 11: s.off = ~0U - 3; break;
		case 12: s.width = 2; break;
		case 13: s.polls = 0; break;
		case 14: s.mask = 3; s.expected = 4; break;
		case 15: s.mask = 1ULL << 32; break;
		case 16: s.mask = 3; s.polls = 2; s.interval_ms = ~0U; break;
		case 17: s.mask = 3; s.polls = 1025; break;
		}
		ret = h17_tx_run(&p, &ops, &f, &value, 1, &committed);
		check("invalid-plan-refused", ret == expected && committed == 0 && !f.used);
	}
	{
		struct h17_tx_step s = good;
		struct h17_tx_plan p = good_plan;
		p.steps = &s;
		s.off = 0xfc;
		check("exact-resource-end", h17_tx_validate(&p) == 0);
		s.off = 0xf8; s.width = 8;
		check("aligned-eight-byte-end", h17_tx_validate(&p) == 0);
	}
	{
		struct h17_tx_step steps[2] = { good, good };
		struct h17_tx_plan p = good_plan;
		struct fixture f = {0};
		h17_tx_word value[2] = {0};
		unsigned int committed = 42;
		struct h17_tx_ops missing = ops;
		p.steps = steps;
		p.count = 2;
		check("output-capacity", h17_tx_run(&p, &ops, &f, value, 1,
		      &committed) == -EINVAL && !committed && !f.used);
		p.count = 1;
		missing.stop = 0;
		check("mandatory-stop", h17_tx_run(&p, &missing, &f, value, 1,
		      &committed) == -EINVAL && !f.used);
		check("null-plan", h17_tx_run(0, &ops, &f, value, 1,
		      &committed) == -EOPNOTSUPP && !f.used);
		check("null-result", h17_tx_run(&p, &ops, &f, value, 1,
		      0) == -EINVAL && !f.used);
		p.count = 2;
		steps[1].direction = H17_TX_WRITE;
		/* Even a bad later step must be rejected before any operation. */
		check("validate-entire-plan", h17_tx_run(&p, &ops, &f, value, 2,
		      &committed) == -EOPNOTSUPP && !committed && !f.used);
	}
}

int main(void)
{
	run_case("snapshot-success", NONE, -EIO, 1, 0, 0, "BOAPRKSLF");
	run_case("begin-failure", BEGIN, -EIO, 1, 0, -EIO, "B");
	run_case("owner-busy", ACQUIRE, -EBUSY, 1, 0, -EBUSY, "BOF");
	run_case("arm-partial-failure", ARM, -EIO, 1, 0, -EIO, "BOASLF");
	run_case("pending-failure", PENDING, -ENOSPC, 1, 0, -ENOSPC, "BOAPSLF");
	run_case("short-write-failure", PENDING, 1, 1, 0, -EIO, "BOAPSLF");
	run_case("read-failure", READ, -EIO, 1, 0, -EIO, "BOAPRSLF");
	run_case("pet-failure", PET, -EIO, 1, 0, -EIO, "BOAPRKSLF");
	run_case("stop-failure", STOP, -EIO, 1, 0, -EIO, "BOAPRKSF");
	run_case("release-failure", RELEASE, -EIO, 1, 0, -EIO, "BOAPRKSLF");
	run_case("finish-failure", FINISH, -EIO, 1, 0, -EIO, "BOAPRKSLF");
	run_case("poll-timeout", NONE, -EIO, 3, 0, -ETIMEDOUT, "BOAPRKWPRKWPRKSLF");
	run_case("poll-success", NONE, -EIO, 3, 3, 0, "BOAPRKWPRKWPRKSLF");
	run_case("wait-failure", WAIT, -EIO, 3, 0, -EIO, "BOAPRKWSLF");
	validation_cases();
	printf("transaction checks: %d, failures: %d\n", tests, failures);
	return failures ? 1 : 0;
}
