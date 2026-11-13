// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/* A stand-in server for the process-level cpu floor tests: begin, hold,
 * release. test_cpu_floor.py drives it with the normal exit, SIGTERM,
 * SIGINT and SIGKILL, and with two concurrent instances.
 *
 * usage: ./test_cpu_floor_serve [SECONDS]
 *	  ./test_cpu_floor_serve slowclose [SECONDS]
 *
 * slowclose cuts the watchdog's pipe while the process is alive and dies
 * SECONDS later (default 0.5): a stand-in for the dying-parent window where
 * the pipe write end is already closed but the pid is not a zombie yet (fds
 * release inside do_exit, before exit_notify). The watchdog must ride that
 * out instead of restoring at the first sight of a live holder.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ane_cpu_floor.h"

static void nap(double seconds)
{
	while (seconds > 0) {
		struct timespec ts = { 0, 100 * 1000 * 1000 };

		nanosleep(&ts, NULL);
		seconds -= 0.1;
	}
}

int main(int argc, char **argv)
{
	double left = 1.0;
	int slowclose = argc > 1 && !strcmp(argv[1], "slowclose");

	if (argc > (slowclose ? 2 : 1))
		left = strtod(argv[slowclose ? 2 : 1], NULL);
	else if (slowclose)
		left = 0.5;

	if (ane_cpu_floor_begin())
		printf("FLOOR raised\n");
	else
		printf("FLOOR skipped\n");
	fflush(stdout);

	if (slowclose) {
		close(ane_floor_pipe_w);
		ane_floor_pipe_w = -1;
		nap(left);
		raise(SIGKILL);
		return 0;
	}

	nap(left);
	ane_cpu_floor_end();
	return 0;
}
