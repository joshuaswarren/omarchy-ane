ifneq ($(KERNELRELEASE),)
	obj-m := ane_t6021.o
	ane_t6021-objs := ane_t6021_drv.o ane_t6021_rtkit.o ane_t6021_fwload.o ane_t6021_boot.o
	ccflags-y += -DANE_T6021_MODULE_VERSION='"$(shell git -C $(src) describe --always --dirty 2>/dev/null || echo unknown)"'
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
 	PWD := $(shell pwd)

# Same out-of-tree contract as the H13 ane module: plain module build,
# no installation, no modprobe. Cross build for the jw14m2 kernel tree:
#   make KERNELDIR=.../omarchy-linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: default modules clean

endif
