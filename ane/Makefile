ifneq ($(KERNELRELEASE),)
	obj-m := ane.o
	ane-objs := src/ane_drv.o src/ane_tm.o src/ane_dart.o
	ccflags-y += -DANE_MODULE_VERSION='"$(shell git -C $(src) describe --always --dirty 2>/dev/null || echo unknown)"'
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
 	PWD := $(shell pwd)

# Safe by construction: plain out-of-tree module build, no header
# installation, no module installation, no modprobe. Pass KERNELDIR,
# ARCH and CROSS_COMPILE explicitly for cross builds.
default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: default modules clean

endif
