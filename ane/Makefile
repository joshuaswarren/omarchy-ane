ifneq ($(KERNELRELEASE),)
	obj-m := ane.o
	ane-objs := src/ane_drv.o src/ane_tm.o
	# Resolve <uapi/drm/ane_accel.h> from this tree; never copy headers
	# into the kernel build directory.
	ccflags-y += -I$(src)/src
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
