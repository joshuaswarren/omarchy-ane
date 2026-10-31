# SPDX-License-Identifier: GPL-2.0-only OR MIT
ifneq ($(KERNELRELEASE),)
obj-m := ane_h17_adt_probe.o ane_h17_ps_probe.o ane_h17_wake.o
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
SIM_CC ?= cc
SIM_CFLAGS ?= -g -O1 -Wall -Wextra -Werror=return-type -std=gnu11

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) W=1 modules

sim/sim_h17_wake: sim/sim_harness.c sim/shim.h ane_h17_wake.c
	$(SIM_CC) $(SIM_CFLAGS) -I sim/inc -o $@ sim/sim_harness.c

sim-test: sim/sim_h17_wake
	./sim/sim_h17_wake snapshot

sim-negative: sim/sim_h17_wake
	./sim/sim_h17_wake no-optin
	./sim/sim_h17_wake wrong-board

sim-rehearse: sim/sim_h17_wake
	./sim/sim_h17_wake rehearse

# Both mutants must be rejected: unqualifying the map would let the probe
# run against the fake bus, and removing the rehearsal timer arm must
# fail the rehearsal assertions.
sim-mutation-map:
	@set -eu; tmp=$$(mktemp -d "${TMPDIR:-/tmp/opencode}/h17-mutation.XXXXXX" 2>/dev/null || mktemp -d "/tmp/opencode/h17-mutation.XXXXXX"); trap 'rm -rf "$$tmp"' EXIT; \
	cp -r sim "$$tmp/sim"; \
	sed 's/^#define H17_REGISTER_MAP_QUALIFIED 0$$/#define H17_REGISTER_MAP_QUALIFIED 1/' ane_h17_wake.c > "$$tmp/ane_h17_wake.c"; \
	grep -q '^#define H17_REGISTER_MAP_QUALIFIED 1$$' "$$tmp/ane_h17_wake.c"; \
	$(SIM_CC) $(SIM_CFLAGS) -I "$$tmp/sim/inc" -o "$$tmp/sim_h17" "$$tmp/sim/sim_harness.c"; \
	if "$$tmp/sim_h17" snapshot 2>/dev/null; then echo 'FAIL: gate mutant escaped'; exit 1; \
	else echo 'PASS: harness rejects removal of refusal'; fi

sim-mutation-contain:
	@set -eu; tmp=$$(mktemp -d "${TMPDIR:-/tmp/opencode}/h17-mutation.XXXXXX" 2>/dev/null || mktemp -d "/tmp/opencode/h17-mutation.XXXXXX"); trap 'rm -rf "$$tmp"' EXIT; \
	cp -r sim "$$tmp/sim"; \
	sed 's/add_timer_on(&h17_rehearse_timer, cpu);/(void)cpu;/' ane_h17_wake.c > "$$tmp/ane_h17_wake.c"; \
	grep -q 'void)cpu;' "$$tmp/ane_h17_wake.c"; \
	$(SIM_CC) $(SIM_CFLAGS) -I "$$tmp/sim/inc" -o "$$tmp/sim_h17" "$$tmp/sim/sim_harness.c"; \
	if "$$tmp/sim_h17" rehearse 2>/dev/null; then echo 'FAIL: containment mutant escaped'; exit 1; \
	else echo 'PASS: harness rejects the uncontained rehearsal'; fi

sim-all: sim-test sim-negative sim-rehearse sim-mutation-map sim-mutation-contain

script-test:
	bash ./test-stage4.sh

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f sim/sim_h17_wake

.PHONY: all sim-test sim-negative sim-rehearse sim-mutation-map sim-mutation-contain sim-all script-test clean
endif
