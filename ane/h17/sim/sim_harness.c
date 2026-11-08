// SPDX-License-Identifier: GPL-2.0-only OR MIT
#include "shim.h"
char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
bool sim_j700 = true;
#include "../ane_h17_wake.c"

int main(int argc, char **argv)
{
	int expected = -EOPNOTSUPP, ret;
	const char *reason = "REFUSED reason=unqualified-register-map";
	if (argc != 2)
		return 2;
	optin = true;
	if (!strcmp(argv[1], "no-optin")) {
		optin = false;
		expected = -EPERM;
	} else if (!strcmp(argv[1], "wrong-board")) {
		sim_j700 = false;
		expected = -EPERM;
	} else if (!strcmp(argv[1], "rehearse-init")) {
		contain_rehearse = true;
	} else if (strcmp(argv[1], "snapshot")) {
		return 2;
	}
	if (expected == -EPERM)
		reason = "REFUSED reason=board-optin-or-dt";
	ret = sim_init_fn();
	sim_exit_fn();
	if (ret != expected || !strstr(sim_log, reason) || strstr(sim_log, "verdict=PASS")) {
		fprintf(stderr, "%s: FAIL rc=%d\n%s", argv[1], ret, sim_log);
		return 1;
	}
	printf("%s: ok\n", argv[1]);
	return 0;
}
