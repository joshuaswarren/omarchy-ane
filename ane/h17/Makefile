# SPDX-License-Identifier: GPL-2.0-only OR MIT
ifneq ($(KERNELRELEASE),)
obj-m := ane_h17_adt_probe.o ane_h17_ps_probe.o ane_h17_cpu_ps_probe.o ane_h17_cpu_vote_probe.o ane_h17_engine_probe.o ane_h17_read_probe.o ane_h17_single_read_probe.o ane_h17_poweroff_probe.o ane_h17_survey_probe.o ane_h17_run_probe.o ane_h17_write_probe.o ane_h17_poweron_probe.o ane_h17_tunable_probe.o ane_h17_appleorder_probe.o ane_h17_boot_probe.o ane_h17_full_probe.o ane_h17_rtkit_probe.o ane_h17_hal_probe.o ane_h17_clk_scan.o ane_h17_wake.o
obj-m += ane_h17_af_scan.o
obj-m += ane_h17_clk_wide.o
obj-m += ane_h17_clkgen_scan.o
obj-m += ane_h17_fwboot_probe.o
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
SIM_CC ?= cc
SIM_CFLAGS ?= -g -O1 -Wall -Wextra -Werror -std=gnu11

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) W=1 modules

sim/sim_h17_wake: sim/sim_harness.c sim/shim.h ane_h17_wake.c transaction.h $(wildcard sim/inc/linux/*.h)
	$(SIM_CC) $(SIM_CFLAGS) -I sim/inc -o $@ sim/sim_harness.c

sim/transaction_test: sim/transaction_test.c transaction.h sim/inc/linux/errno.h
	$(SIM_CC) $(SIM_CFLAGS) -I sim/inc -o $@ sim/transaction_test.c

sim/bringup_order_test: sim/bringup_order_test.c bringup_order.h
	$(SIM_CC) $(SIM_CFLAGS) -o $@ sim/bringup_order_test.c

order-test: sim/bringup_order_test
	./sim/bringup_order_test

sim-test: sim/sim_h17_wake sim/transaction_test
	./sim/sim_h17_wake snapshot
	./sim/transaction_test

sim-negative: sim/sim_h17_wake
	./sim/sim_h17_wake no-optin
	./sim/sim_h17_wake wrong-board
	./sim/sim_h17_wake rehearse-init

sim-mutations:
	SIM_CC='$(SIM_CC)' SIM_CFLAGS='$(SIM_CFLAGS)' python3 sim/test_mutations.py

sim-all: sim-test sim-negative sim-mutations planner-test order-test chanreg-model-test dart-publication-test diagnostic-preflight-test table-preflight-test chanreg-layout-test

sim/chanreg_layout_test: sim/chanreg_layout_test.c chanreg.h
	$(SIM_CC) $(SIM_CFLAGS) -o $@ $<

chanreg-layout-test: sim/chanreg_layout_test
	./sim/chanreg_layout_test
	python3 -m unittest -v test_chan_alias_overlay.py

diagnostic-preflight-test:
	python3 -m unittest -v test_diagnostic_preflight.py

table-preflight-test:
	python3 -m unittest -v test_table_preflight.py

dart-publication-test:
	python3 -m unittest -v test_dart_publication_observations.py

chanreg-model-test:
	python3 -m unittest -v test_chanreg_model.py test_table_discovery_verdict.py test_registration_model.py

planner-test:
	python3 sim/test_plan_stage4.py

script-test:
	bash ./test-stage4.sh

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f sim/sim_h17_wake sim/transaction_test sim/bringup_order_test sim/chanreg_layout_test

.PHONY: all sim-test sim-negative sim-mutations sim-all planner-test script-test order-test chanreg-model-test dart-publication-test diagnostic-preflight-test table-preflight-test chanreg-layout-test clean
endif
