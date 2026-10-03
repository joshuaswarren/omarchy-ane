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

default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: default modules clean
endif
