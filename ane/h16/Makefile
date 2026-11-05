# SPDX-License-Identifier: GPL-2.0-only OR MIT
# OPT-IN EXPERIMENTAL module: not part of "make all", not in dkms.conf,
# not autoloaded (no MODULE_DEVICE_TABLE). Build by hand for bring-up:
#   make KERNELDIR=.../omarchy-linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
ifneq ($(KERNELRELEASE),)
	obj-m := ane_h16.o
	ane_h16-objs := ane_h16_main.o ane_h16_soc.o ../h17/ane_h17_soc.o
	ccflags-y += -I$(src)
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

CC ?= cc
SIM_OUT ?= sim/sim_h16
SIM_SRCS = sim/sim_harness.c sim/shim.h ane_h16_main.c ane_h16_soc.c ane_h16.h \
	../h17/chanreg.h

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

# Userspace sim of the REAL module (see sim/sim_harness.c header). A
# green sim proves harness logic only, never silicon. Not part of
# "make all"/modules.
POS = pos-dt pos-dt-noadt pos-status pos-boot pos-boot-hello pos-four-ps \
	pos-h17-boot pos-h17-irq pos-h17-scratch pos-h17-table
NEG = neg-ps-stuck neg-mbox-silent neg-bad-pin neg-foreign-preload \
	neg-dart-map neg-short-unmap neg-unknown-stage neg-adt-bounds \
	neg-unreserved-alias neg-non-dma-domain \
	neg-preload-bss neg-preload-boundary neg-preload-snapshot \
	neg-preload-sysram neg-table-timeout neg-table-overrun neg-table-foreign \
	neg-table-padding neg-table-envelope neg-table-alloc neg-table-dma-range \
	neg-table-heap-alloc neg-table-scope neg-table-reservation

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
	@rm -rf /tmp/ane-h16-discrim && mkdir -p /tmp/ane-h16-discrim/sim /tmp/ane-h16-discrim/h17 /tmp/ane-h16-discrim/h16
	@cp sim/sim_harness.c sim/shim.h /tmp/ane-h16-discrim/sim/ && cp -r sim/inc /tmp/ane-h16-discrim/sim/
	@cp ane_h16_soc.c ane_h16.h /tmp/ane-h16-discrim/
	@cp ../h17/ane_h17_soc.c ../h17/ane_h17_soc.h ../h17/ane_h17_fw.h ../h17/bringup_order.h ../h17/chanreg.h /tmp/ane-h16-discrim/h17/
	@cp ane_h16.h /tmp/ane-h16-discrim/h16/
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
	@sed 's/ane_h16_result(ane, 3, "REFUSED", "preload-diff");/ane_h16_result(ane, 3, "REFUSED", "preload-diff"); goto rel_fw;/' \
	  ane_h16_main.c > /tmp/ane-h16-discrim/ane_h16_main.c
	@$(CC) -g -O1 -I /tmp/ane-h16-discrim/sim -I /tmp/ane-h16-discrim/sim/inc \
	  -o $(SIM_OUT).d3 /tmp/ane-h16-discrim/sim/sim_harness.c
	@if $(SIM_OUT).d3 neg-foreign-preload >/dev/null 2>&1; then \
		echo "DISCRIMINATION FAILED: reintroduced DMA leak was not caught"; exit 1; \
	else echo "ok: leak mutation caught by neg-foreign-preload"; fi
	@sed '/"REFUSED", "dart-map");/{n;s/goto unmap;/goto free_stage;/}' \
	  ane_h16_main.c > /tmp/ane-h16-discrim/ane_h16_main.c
	@$(CC) -g -O1 -I /tmp/ane-h16-discrim/sim -I /tmp/ane-h16-discrim/sim/inc \
	  -o $(SIM_OUT).d4 /tmp/ane-h16-discrim/sim/sim_harness.c
	@if $(SIM_OUT).d4 neg-dart-map >/dev/null 2>&1; then \
		echo "DISCRIMINATION FAILED: reintroduced partial-map leak was not caught"; exit 1; \
	else echo "ok: partial-map-leak mutation caught by neg-dart-map"; fi
	@rm -rf sim/sim_h16.d1 sim/sim_h16.d2 sim/sim_h16.d3 sim/sim_h16.d4

sim-adt-mutations:
	python3 sim/test_adt_mutations.py

sim: sim-test sim-negative sim-discriminate sim-adt-mutations

$(NEG): $(SIM_OUT)
	$(SIM_OUT) $@

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f $(SIM_OUT)

.PHONY: default modules sim sim-test sim-negative sim-discriminate sim-adt-mutations $(NEG) clean
endif
