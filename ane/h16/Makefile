# SPDX-License-Identifier: GPL-2.0-only OR MIT
# OPT-IN EXPERIMENTAL module: not part of "make all", not in dkms.conf,
# not autoloaded (no MODULE_DEVICE_TABLE). Build by hand for bring-up:
#   make KERNELDIR=.../omarchy-linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
ifneq ($(KERNELRELEASE),)
	obj-m := ane_h16.o
	ane_h16-objs := ane_h16_main.o ane_h16_soc.o
	ccflags-y += -I$(src)
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

CC ?= cc
SIM_OUT ?= sim/sim_h16
SIM_SRCS = sim/sim_harness.c sim/shim.h ane_h16_main.c ane_h16_soc.c ane_h16.h

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

# Userspace sim of the REAL module (see sim/sim_harness.c header). A
# green sim proves harness logic only, never silicon. Not part of
# "make all"/modules.
POS = pos-dt pos-dt-noadt pos-status pos-boot pos-boot-hello
NEG = neg-ps-stuck neg-mbox-silent neg-bad-pin neg-foreign-preload neg-unknown-stage

$(SIM_OUT): $(SIM_SRCS)
	$(CC) -g -O1 -Wall -Wextra -Wno-unused-parameter -I sim -I sim/inc -o $@ sim/sim_harness.c

sim-test: $(SIM_OUT)
	@for s in $(POS); do $(SIM_OUT) $$s || exit 1; done

sim-negative: $(SIM_OUT)
	@for s in $(NEG); do $(SIM_OUT) $$s || exit 1; done

# Discrimination proof: mutate a COPY of the module (pin check neutered,
# ps gate skipped) and require the matching negative to FAIL the harness.
# If a mutated run passes, the harness is not discriminating: fix it.
sim-discriminate: $(SIM_OUT)
	@rm -rf /tmp/ane-h16-discrim && mkdir -p /tmp/ane-h16-discrim/sim
	@cp sim/sim_harness.c sim/shim.h /tmp/ane-h16-discrim/sim/ && cp -r sim/inc /tmp/ane-h16-discrim/sim/
	@cp ane_h16_soc.c ane_h16.h /tmp/ane-h16-discrim/
	@sed 's/if (memcmp(digest, img->sha256, sizeof(digest))) {/if (0) {/' \
	  ane_h16_main.c > /tmp/ane-h16-discrim/ane_h16_main.c
	@grep -q 'if (0) {' /tmp/ane-h16-discrim/ane_h16_main.c
	@$(CC) -g -O1 -I /tmp/ane-h16-discrim/sim -I /tmp/ane-h16-discrim/sim/inc \
	  -o $(SIM_OUT).d1 /tmp/ane-h16-discrim/sim/sim_harness.c
	@if $(SIM_OUT).d1 neg-bad-pin >/dev/null 2>&1; then \
		echo "DISCRIMINATION FAILED: neutered pin check was not caught"; exit 1; \
	else echo "ok: pin-check mutation caught by neg-bad-pin"; fi
	@sed 's/ret = ane_h16_ps_wait(ane);/ret = 0;/' \
	  ane_h16_main.c > /tmp/ane-h16-discrim/ane_h16_main.c
	@grep -q 'ret = 0;' /tmp/ane-h16-discrim/ane_h16_main.c
	@$(CC) -g -O1 -I /tmp/ane-h16-discrim/sim -I /tmp/ane-h16-discrim/sim/inc \
	  -o $(SIM_OUT).d2 /tmp/ane-h16-discrim/sim/sim_harness.c
	@if $(SIM_OUT).d2 neg-ps-stuck >/dev/null 2>&1; then \
		echo "DISCRIMINATION FAILED: skipped ps gate was not caught"; exit 1; \
	else echo "ok: ps-gate mutation caught by neg-ps-stuck"; fi
	@rm -rf sim/sim_h16.d1 sim/sim_h16.d2

sim: sim-test sim-negative sim-discriminate

$(NEG): $(SIM_OUT)
	$(SIM_OUT) $@

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(SIM_OUT)

.PHONY: default modules sim sim-test sim-negative sim-discriminate $(NEG) clean
endif
