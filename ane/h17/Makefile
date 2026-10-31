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

# Run the actual module against instrumented kernel stubs: the probe runs
# (the map is qualified), every MMIO access must sit inside the qualified
# map, the containment watch must arm before the first engine access and
# disarm after the last, and the hang mode must panic naming the pending
# access. A sim is not silicon: it never qualifies T8140 behaviour.
sim/sim_h17_wake: sim/sim_harness.c sim/shim.h ane_h17_wake.c $(wildcard sim/inc/linux/*.h) sim/inc/linux/mfd/syscon.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim/inc -o $@ sim/sim_harness.c

sim-test: sim/sim_h17_wake
	./sim/sim_h17_wake snapshot
	./sim/sim_h17_wake wake
	./sim/sim_h17_wake peek

sim-negative: sim/sim_h17_wake
	./sim/sim_h17_wake legacy-overrides
	./sim/sim_h17_wake no-optin
	./sim/sim_h17_wake wrong-board

sim-hang: sim/sim_h17_wake
	./sim/sim_h17_wake hang

# Prove the harness catches both regressions. Build errors do not count as
# a killed mutant. Use a unique directory, clean on exit.
sim-mutation-map:
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	cp -r sim "$$tmp/sim"; \
	sed 's/^#define H17_REGISTER_MAP_QUALIFIED 1$$/#define H17_REGISTER_MAP_QUALIFIED 0/' ane_h17_wake.c > "$$tmp/ane_h17_wake.c"; \
	grep -q '^#define H17_REGISTER_MAP_QUALIFIED 0$$' "$$tmp/ane_h17_wake.c"; \
	$(SIM_CC) $(SIM_CFLAGS) -I "$$tmp/sim/inc" -o "$$tmp/sim_h17" "$$tmp/sim/sim_harness.c"; \
	if "$$tmp/sim_h17" snapshot 2>/dev/null; then echo 'FAIL: unqualified mutant escaped'; exit 1; \
	else echo 'PASS: harness rejects the unqualified module'; fi

sim-mutation-contain:
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	cp -r sim "$$tmp/sim"; \
	sed 's/add_timer_on(&h17_contain_timer, h17_contain_cpu);/(void)h17_contain_cpu;/' ane_h17_wake.c > "$$tmp/ane_h17_wake.c"; \
	grep -q 'void)h17_contain_cpu' "$$tmp/ane_h17_wake.c"; \
	$(SIM_CC) $(SIM_CFLAGS) -I "$$tmp/sim/inc" -o "$$tmp/sim_h17" "$$tmp/sim/sim_harness.c"; \
	if "$$tmp/sim_h17" hang 2>/dev/null; then echo 'FAIL: containment-less mutant escaped'; exit 1; \
	else echo 'PASS: harness rejects the uncontained module'; fi

sim-all: sim-test sim-negative sim-hang sim-mutation-map sim-mutation-contain

# Off-device check of run-stage4.sh: fake uname, model, DT node, preflight,
# insmod, rmmod, dmesg and lsmod. Loads no module.
script-test:
	./test-stage4.sh

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f sim/sim_h17_wake

.PHONY: all sim-test sim-negative sim-hang sim-mutation-map sim-mutation-contain sim-all script-test clean
endif
