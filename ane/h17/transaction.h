/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H17_TRANSACTION_H
#define ANE_H17_TRANSACTION_H

#include <linux/errno.h>

/* Read-only transaction logic, NOT a hardware adapter or qualified map.
 * No production caller exists yet. No watchdog MMIO, power votes, writes,
 * or firmware-start/stop semantics are inferred here.
 *
 * Callback contract: zero success, negative errno failure. acquire must
 * obtain exclusive ownership WITHOUT arming anything; a failed acquire
 * must leave no ownership behind. arm must verify the requested timeout.
 * stop must verify disarm, including after a partially successful arm.
 * release relinquishes only this owner's resources after verified stop.
 * On stop failure ownership is retained in ctx for explicit recovery;
 * a future adapter must keep that context alive. Ownership must be
 * implemented through the owning subsystem, not a register-state check.
 *
 * begin/pending/finish must durably record run ID, sequence and outcome;
 * begin includes file AND parent-directory persistence. A future adapter
 * must fail closed on short writes, stale identities and storage errors.
 * read must enforce the validated access width and device semantics.
 * wait_ms must bound its delay. These contracts need adapter tests too.
 *
 * A polling deadline cannot interrupt a stalled bus transaction, nor can
 * this logic bound a blocking journal write. Recovery independence and
 * storage/recovery timeout interaction remain adapter design requirements.
 */
typedef unsigned long long h17_tx_word;

enum h17_tx_direction { H17_TX_READ, H17_TX_WRITE };

struct h17_tx_step {
	unsigned int off;
	unsigned int width;
	enum h17_tx_direction direction;
	/* mask=0: snapshot once. Otherwise require (value & mask)==expected. */
	h17_tx_word mask;
	h17_tx_word expected;
	unsigned int polls;
	unsigned int interval_ms;
};

struct h17_tx_plan {
	h17_tx_word run_id;
	unsigned int qualified;
	unsigned int resource_bytes;
	unsigned int recovery_ms;
	unsigned int count;
	const struct h17_tx_step *steps;
};

struct h17_tx_ops {
	int (*begin)(void *ctx, h17_tx_word run_id);
	int (*pending)(void *ctx, h17_tx_word run_id, unsigned int sequence,
		       const struct h17_tx_step *step);
	int (*finish)(void *ctx, h17_tx_word run_id, int outcome);
	int (*acquire)(void *ctx);
	int (*arm)(void *ctx, unsigned int timeout_ms);
	int (*pet)(void *ctx);
	int (*stop)(void *ctx);
	int (*release)(void *ctx);
	int (*read)(void *ctx, unsigned int off, unsigned int width,
		    h17_tx_word *value);
	int (*wait_ms)(void *ctx, unsigned int ms);
};

static inline int h17_tx_status(int ret)
{
	return ret > 0 ? -EIO : ret;
}

static inline int h17_tx_validate(const struct h17_tx_plan *p)
{
	unsigned int i;

	if (!p || !p->qualified)
		return -EOPNOTSUPP;
	if (!p->run_id || !p->steps || !p->count || p->count > 64 ||
	    !p->resource_bytes || p->recovery_ms < 1000 || p->recovery_ms > 60000)
		return -EINVAL;
	for (i = 0; i < p->count; i++) {
		const struct h17_tx_step *s = &p->steps[i];

		/* No source-qualified write/quiesce protocol exists yet. */
		if (s->direction != H17_TX_READ)
			return -EOPNOTSUPP;
		if ((s->width != 4 && s->width != 8) || s->off % s->width ||
		    s->width > p->resource_bytes ||
		    s->off > p->resource_bytes - s->width ||
		    !s->polls || s->polls > 1024 ||
		    (s->width == 4 && (s->mask >> 32 || s->expected >> 32)) ||
		    (s->expected & ~s->mask) || (!s->mask && s->polls != 1))
			return -EINVAL;
		/* Bounded arithmetic, with half of recovery window reserved.
		 * This is a delay budget, not proof of total elapsed time. */
		if (s->polls > 1 && (!s->interval_ms ||
		    s->interval_ms > (p->recovery_ms / 2) / (s->polls - 1)))
			return -EINVAL;
	}
	return 0;
}

/* committed remains zero until all reads, disarm, ownership release and
 * durable completion succeed. values are provisional on any error. */
static inline int h17_tx_run(const struct h17_tx_plan *p,
			     const struct h17_tx_ops *ops, void *ctx,
			     h17_tx_word *values, unsigned int capacity,
			     unsigned int *committed)
{
	unsigned int i, poll, sequence = 0;
	int ret, cleanup;

	if (!committed)
		return -EINVAL;
	*committed = 0;
	ret = h17_tx_validate(p);
	if (ret)
		return ret;
	if (!values || capacity < p->count || !ops ||
	    !ops->begin || !ops->pending || !ops->finish ||
	    !ops->acquire || !ops->arm || !ops->pet || !ops->stop ||
	    !ops->release || !ops->read || !ops->wait_ms)
		return -EINVAL;
	ret = h17_tx_status(ops->begin(ctx, p->run_id));
	if (ret)
		return ret;
	ret = h17_tx_status(ops->acquire(ctx));
	if (ret)
		goto finish; /* Never stop/release another owner on EBUSY. */
	ret = h17_tx_status(ops->arm(ctx, p->recovery_ms));
	if (ret)
		goto stop; /* An owned device may be partially armed. */
	for (i = 0; i < p->count; i++) {
		const struct h17_tx_step *s = &p->steps[i];

		for (poll = 0; poll < s->polls; poll++) {
			ret = h17_tx_status(ops->pending(ctx, p->run_id, ++sequence, s));
			if (ret)
				goto stop;
			ret = h17_tx_status(ops->read(ctx, s->off, s->width, &values[i]));
			if (ret)
				goto stop;
			ret = h17_tx_status(ops->pet(ctx));
			if (ret)
				goto stop;
			if (!s->mask || (values[i] & s->mask) == s->expected)
				break;
			if (poll + 1 < s->polls) {
				ret = h17_tx_status(ops->wait_ms(ctx, s->interval_ms));
				if (ret)
					goto stop;
			}
		}
		if (poll == s->polls) {
			ret = -ETIMEDOUT;
			goto stop;
		}
	}
stop:
	cleanup = h17_tx_status(ops->stop(ctx));
	if (cleanup) {
		ret = cleanup;
		goto finish; /* Retain ownership while device may be armed. */
	}
	cleanup = h17_tx_status(ops->release(ctx));
	if (cleanup && !ret)
		ret = cleanup;
finish:
	cleanup = h17_tx_status(ops->finish(ctx, p->run_id, ret));
	if (cleanup && !ret)
		ret = cleanup;
	if (!ret)
		*committed = p->count;
	return ret;
}

#endif
