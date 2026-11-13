// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/* A stand-in server for the process-level cpu floor tests: begin, hold,
 * release. test_cpu_floor.py drives it with the normal exit, SIGTERM,
 * SIGINT and SIGKILL, and with two concurrent instances.
 *
 * usage: ./test_cpu_floor_serve [SECONDS]
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ane_cpu_floor.h"

int main(int argc, char **argv)
{
	double left = argc > 1 ? strtod(argv[1], NULL) : 1.0;

	if (ane_cpu_floor_begin())
		printf("FLOOR raised\n");
	else
		printf("FLOOR skipped\n");
	fflush(stdout);

	while (left > 0) {
		struct timespec ts = { 0, 100 * 1000 * 1000 };

		nanosleep(&ts, NULL);
		left -= 0.1;
	}

	ane_cpu_floor_end();
	return 0;
}
