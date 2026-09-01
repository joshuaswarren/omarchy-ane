// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren <816217+joshuaswarren@users.noreply.github.com> */

#include <stdio.h>
#include <unistd.h>

#include "ane.h"

/* Exercises ane_exec_loop on real hardware with a state graph
 * (equal-size input 0 / output 0). Skips without a device or model.
 * Evidence from ane-linux-experiments receipts/ane-static-graph-loop.log:
 * mul.ane 3 iterations ((3 * 2) * 2) * 2 -> 24.0 everywhere. */

int main(int argc, char **argv)
{
	struct ane_nn *nn;
	int err;

	if (argc < 2) {
		printf("usage: %s <model.anec>\n", argv[0]);
		return 77;
	}

	if (access("/dev/accel/accel0", F_OK) != 0) {
		printf("TEST: SKIP: no ANE device\n");
		return 77;
	}

	nn = ane_init(argv[1]);
	if (nn == NULL) {
		printf("TEST: ERR: failed to init %s\n", argv[1]);
		return 1;
	}

	if (ane_kernel_capacity(nn) == 0) {
		printf("TEST: SKIP: no kernel capacity\n");
		ane_free(nn);
		return 77;
	}

	if (__ane_src_size(nn, 0) != __ane_dst_size(nn, 0)) {
		printf("TEST: SKIP: state sizes differ; needs a state graph\n");
		ane_free(nn);
		return 77;
	}

	err = ane_exec_loop(nn, 3, 0, 0);
	printf("TEST: LOG: ane_exec_loop: %d\n", err);

	ane_free(nn);
	return err < 0 ? 1 : 0;
}
