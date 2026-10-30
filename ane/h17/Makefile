# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Manual, opt-in ADT inspection. Not built by the top-level Makefile or DKMS.
ifneq ($(KERNELRELEASE),)
obj-m := ane_h17_adt_probe.o
else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) W=1 modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: all clean
endif
