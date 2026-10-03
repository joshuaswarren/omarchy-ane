# SPDX-License-Identifier: GPL-2.0-only OR MIT
# OPT-IN EXPERIMENTAL module: not part of "make all", not in dkms.conf,
# not autoloaded (no MODULE_DEVICE_TABLE). Build by hand for bring-up:
#   make KERNELDIR=<kernel tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
#   make KERNELDIR=<kernel tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 modules
#   make check                 (host ADT walker self test)
ifneq ($(KERNELRELEASE),)
	obj-m := ane_h15.o
	ane_h15-objs := ane_h15_main.o ane_h15_soc.o
	ccflags-y += -I$(src)
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

check: ane_h15_adt_test
	./ane_h15_adt_test

ane_h15_adt_test: ane_h15_adt_test.c ane_h15_adt.h
	$(CC) $(CFLAGS) -I. ane_h15_adt_test.c -o $@

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -f ane_h15_adt_test

.PHONY: default modules check clean
endif
