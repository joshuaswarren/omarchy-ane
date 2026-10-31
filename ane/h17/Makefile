# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Manual, opt-in probes for the J700 H17 bring-up ladder. Not built by the
# top-level Makefile or DKMS.
ifneq ($(KERNELRELEASE),)
obj-m := ane_h17_adt_probe.o ane_h17_ps_probe.o ane_h17_wake.o
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

SIM_CC ?= cc
SIM_CFLAGS ?= -g -O1 -Wall -Wextra -Werror=return-type -std=gnu11

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) W=1 modules

# Run the actual module-init gate against instrumented kernel stubs.
# No fake firmware response qualifies a register map or device support.
sim/sim_h17_wake: sim/sim_harness.c sim/shim.h ane_h17_wake.c $(wildcard sim/inc/linux/*.h) sim/inc/linux/mfd/syscon.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim/inc -o $@ sim/sim_harness.c

# The qualified build flips the quarantine gate inside the sim only: the
# probe runs against the fake bus and every MMIO access is asserted to sit
# inside the qualified map (sim/sim_harness.c: scan_unqualified).
sim/sim_h17_wake_q: sim/sim_harness.c sim/shim.h ane_h17_wake.c $(wildcard sim/inc/linux/*.h) sim/inc/linux/mfd/syscon.h
	$(SIM_CC) $(SIM_CFLAGS) -DH17_REGISTER_MAP_QUALIFIED=1 -I sim/inc -o $@ sim/sim_harness.c

sim-test: sim/sim_h17_wake
	./sim/sim_h17_wake snapshot
	./sim/sim_h17_wake wake

sim-negative: sim/sim_h17_wake
	./sim/sim_h17_wake legacy-overrides
	./sim/sim_h17_wake no-optin
	./sim/sim_h17_wake wrong-board

sim-qualified: sim/sim_h17_wake_q
	./sim/sim_h17_wake_q q-snapshot
	./sim/sim_h17_wake_q q-wake
	./sim/sim_h17_wake_q q-peek

# Prove the harness detects removal of the live hardware gate. Build errors
# do not count as a killed mutant. Use a unique directory, clean on exit.
sim-mutation-ungated:
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	cp -r sim "$$tmp/sim"; \
	sed 's/^#define H17_REGISTER_MAP_QUALIFIED 0$$/#define H17_REGISTER_MAP_QUALIFIED 1/' ane_h17_wake.c > "$$tmp/ane_h17_wake.c"; \
	grep -q '^#define H17_REGISTER_MAP_QUALIFIED 1$$' "$$tmp/ane_h17_wake.c"; \
	$(SIM_CC) $(SIM_CFLAGS) -I "$$tmp/sim/inc" -o "$$tmp/sim_h17" "$$tmp/sim/sim_harness.c"; \
	if "$$tmp/sim_h17" snapshot 2>/dev/null; then echo 'FAIL: ungated mutant escaped'; exit 1; \
	else echo 'PASS: harness rejects ungated hardware access'; fi

sim-all: sim-test sim-negative sim-qualified sim-mutation-ungated

# Off-device check of run-stage4.sh: fake uname, model, DT node, preflight,
# insmod, rmmod, dmesg and lsmod. Loads no module.
script-test:
	./test-stage4.sh

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f sim/sim_h17_wake

.PHONY: all sim-test sim-negative sim-qualified sim-mutation-ungated sim-all script-test clean
endif
