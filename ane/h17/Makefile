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

# Userspace sim of the REAL ane_h17_wake.c (sim/shim.h fake bus, genpd and
# firmware model). A green sim proves control flow and gating, not silicon.
sim/sim_h17_wake: sim/sim_harness.c sim/shim.h ane_h17_wake.c $(wildcard sim/inc/linux/*.h) sim/inc/linux/mfd/syscon.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim/inc -o $@ sim/sim_harness.c

sim-test: sim/sim_h17_wake
	./sim/sim_h17_wake pos-snapshot
	./sim/sim_h17_wake pos-wake

sim-negative: sim/sim_h17_wake
	./sim/sim_h17_wake neg-not-stopped
	./sim/sim_h17_wake neg-core-stuck
	./sim/sim_h17_wake neg-bad-optin

# Mutation controls: build a copy of the real module with one sed edit and
# REQUIRE the named harness mode to fail it.
# $(call mutant,<name>,<sed script>,<marker>,<harness mode>)
define mutant
	rm -rf /tmp/ane_h17-$(1) && mkdir -p /tmp/ane_h17-$(1)
	cp -r sim /tmp/ane_h17-$(1)/sim
	cp ane_h17_wake.c /tmp/ane_h17-$(1)/ane_h17_wake.c
	sed '$(2)' /tmp/ane_h17-$(1)/ane_h17_wake.c > /tmp/ane_h17-$(1)/ane_h17_wake.c.new
	mv /tmp/ane_h17-$(1)/ane_h17_wake.c.new /tmp/ane_h17-$(1)/ane_h17_wake.c
	grep -q '$(3)' /tmp/ane_h17-$(1)/ane_h17_wake.c
	$(SIM_CC) $(SIM_CFLAGS) -I /tmp/ane_h17-$(1)/sim/inc -o /tmp/ane_h17-$(1)/sim_h17 /tmp/ane_h17-$(1)/sim/sim_harness.c
	@/tmp/ane_h17-$(1)/sim_h17 $(4) > /tmp/ane_h17-$(1)/run.log 2>&1; \
	s=$$?; \
	echo "--- $(1) mutant harness tail ---"; tail -3 /tmp/ane_h17-$(1)/run.log; \
	if [ $$s -eq 0 ]; then echo "SIM-MUT-$(1) BAD: mutant PASSED the harness (the harness does not discriminate)"; exit 1; \
	else echo "SIM-MUT-$(1) OK: mutant FAILS the harness"; fi
endef

# stage 4 runs without confirm_wake=1
sim-mutation-noconfirm:
	$(call mutant,noconfirm,s/if (!confirm_wake) {/if (0 \/* NEG: confirm ignored *\/) {/,NEG: confirm ignored,pos-snapshot)

# the RUN clear is never written
sim-mutation-nohalt:
	$(call mutant,nohalt,s/writel(0, w->engine + H17_CPU_CONTROL);/(void)0; \/* NEG: RUN never cleared *\//,NEG: RUN never cleared,pos-wake)

# the wake word is never recorded, so the reason degrades to hello-msgs
sim-mutation-nowake:
	$(call mutant,nowake,s/if (scratch7 == H17_WAKE_ACK) {/if (0 \/* NEG: wake word ignored *\/) {/,NEG: wake word ignored,pos-wake)

sim-all: sim-test sim-negative sim-mutation-noconfirm sim-mutation-nohalt sim-mutation-nowake

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f sim/sim_h17_wake

.PHONY: all sim-test sim-negative sim-mutation-noconfirm sim-mutation-nohalt \
	sim-mutation-nowake sim-all clean
endif
