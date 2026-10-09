/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Pipelined-CALL completion demux — the userspace-testable core.
 *
 * ane_t6021_pipeline.h holds the ticket/counter bookkeeping the driver
 * uses to hand each PROCEDURE_CALL its own finish event while two calls
 * are in flight. Every function is pure: it reads and writes only the
 * demux struct it is given, performs no I/O, and uses no kernel-only
 * call, so the host unit test (tests/pipeline_test.c) exercises the
 * exact accounting the driver runs. The caller brings the locks (the
 * driver guards submit/finish/released with one spinlock).
 *
 * Facts this rests on (artifacts/FwPipeline/fw-crpc-intake-decode.md,
 * 13.5 selene a9c4b771; receipts/2026-09-30-t6021-call-wait):
 * - A CALL's IO_T2H event carries the program id at +0x10 and the
 *   process id at +0x14, so a finish event names its call without the
 *   cookie 0xADD0 alone; completion order across different processes is
 *   not guaranteed (one task queue FIFO).
 * - Events for one (program, process) key arrive in submit order
 *   (same-process FIFO). UNPROVEN on hardware: the per-key positional
 *   tickets below are exactly this assumption; any out-of-order same-key
 *   delivery would mispair releases, so the armed path treats a timeout
 *   as fatal (quarantine) like the serialized path.
 *
 * Guards the test proves, each with a mutation that flips it:
 * - a finish event releases only its own key (wrong (prog,proc) never);
 * - a finish event is credited only while the key is owed one
 *   (finished < submitted): a stale or duplicate event cannot satisfy a
 *   later ticket, and an event for an unsubmitted key creates nothing;
 * - a ticket is released by its own position, not by any finish;
 * - the table is fixed: a full table rejects the submission instead of
 *   corrupting accounting.
 */
#ifndef __ANE_T6021_PIPELINE_H__
#define __ANE_T6021_PIPELINE_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef unsigned int u32;
typedef unsigned long long u64;
#endif

/* Distinct (program, process) keys the demux tracks per module load.
 * There is no eviction: keys accumulate over the armed epoch. The armed
 * path rejects a submission with -EAGAIN when the table is full — a
 * clean per-call failure, not a wedge. 16 covers the lab shapes (one
 * program, one or two process ids) with room to spare.
 */
#define ANE_T6021_PIPE_KEYS	16

struct ane_t6021_pipe_key {
	u32 prog_id;
	u32 proc_id;
	u64 submitted;	/* tickets taken = finish events owed */
	u64 finished;	/* finish events credited to this key */
};

struct ane_t6021_pipe_demux {
	struct ane_t6021_pipe_key keys[ANE_T6021_PIPE_KEYS];
	unsigned int in_flight;	/* submits without a credited finish */
};

/* Take one ticket for a submission of (prog_id, proc_id). Tickets are
 * positional per key: 1 for its first submission, 2 for the second, and
 * so on, so a waiter knows how many finishes must have landed. Returns
 * 0 and sets *ticket, or -1 when the table is full. The caller
 * serializes submits (the driver holds its firmware lock).
 */
static inline int ane_t6021_pipe_submit(struct ane_t6021_pipe_demux *d,
					u32 prog_id, u32 proc_id,
					u64 *ticket)
{
	struct ane_t6021_pipe_key *k = NULL;
	unsigned int i;

	for (i = 0; i < ANE_T6021_PIPE_KEYS; i++) {
		struct ane_t6021_pipe_key *e = &d->keys[i];

		if (e->prog_id == prog_id && e->proc_id == proc_id &&
		    (e->submitted || e->finished)) {
			k = e;
			break;
		}
	}
	if (!k) {
		for (i = 0; i < ANE_T6021_PIPE_KEYS; i++) {
			struct ane_t6021_pipe_key *e = &d->keys[i];

			if (!e->submitted && !e->finished) {
				e->prog_id = prog_id;
				e->proc_id = proc_id;
				k = e;
				break;
			}
		}
	}
	if (!k)
		return -1;
	k->submitted++;
	d->in_flight++;
	*ticket = k->submitted;
	return 0;
}

/* Credit one finish event for (prog_id, proc_id). Only a key that is
 * owed an event (finished < submitted) takes it: an event that arrives
 * with nothing owed is stale or unsubmitted and must not create a key
 * or fast-release a later ticket.
 */
static inline void ane_t6021_pipe_finish(struct ane_t6021_pipe_demux *d,
					 u32 prog_id, u32 proc_id)
{
	unsigned int i;

	for (i = 0; i < ANE_T6021_PIPE_KEYS; i++) {
		struct ane_t6021_pipe_key *e = &d->keys[i];

		if (e->prog_id == prog_id && e->proc_id == proc_id &&
		    e->finished < e->submitted) {
			e->finished++;
			d->in_flight--;
			return;
		}
	}
}

/* True when the submission holding ticket is finished: its key has
 * credited at least ticket finish events. A wrong key or an unknown
 * ticket releases nothing.
 */
static inline bool ane_t6021_pipe_released(const struct ane_t6021_pipe_demux *d,
					   u32 prog_id, u32 proc_id,
					   u64 ticket)
{
	unsigned int i;

	for (i = 0; i < ANE_T6021_PIPE_KEYS; i++) {
		const struct ane_t6021_pipe_key *e = &d->keys[i];

		if (e->prog_id == prog_id && e->proc_id == proc_id &&
		    (e->submitted || e->finished))
			return e->finished >= ticket;
	}
	return false;
}

/* Submissions without a credited finish. The driver routes finish
 * events into the demux only while this is nonzero, so the serialized
 * path (no armed submissions) pays nothing.
 */
static inline unsigned int
ane_t6021_pipe_outstanding(const struct ane_t6021_pipe_demux *d)
{
	return d->in_flight;
}

#endif /* __ANE_T6021_PIPELINE_H__ */
