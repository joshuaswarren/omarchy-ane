ifneq ($(KERNELRELEASE),)
	obj-m := ane_t6021.o
	ane_t6021-objs := ane_t6021_rtclient_main.o ane_t6021_fwload.o ane_t6021_boot.o ../ane_stats_show.o
	# One module: the installed-path rtclient surface plus the shared
	# boot/fwload contract units. The probe file is _main.c so no
	# object shares the module name (composite/single-object collision).
	# DKMS sources carry no .git: dkms.conf passes ANE_VERSION instead.
	ANE_VERSION ?= $(shell git -C $(src) describe --always --dirty 2>/dev/null || echo unknown)
	ccflags-y += -DANE_T6021_MODULE_VERSION='"$(ANE_VERSION)"'
	# The ABI-2 UAPI lives at ane/src/uapi/drm/ane_accel.h; the quoted
	# include "uapi/drm/ane_accel.h" resolves through this path.
	ccflags-y += -I$(src)/../src -I$(src)/../include
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
 	PWD := $(shell pwd)

# Same out-of-tree contract as the H13 ane module: plain module build,
# no installation, no modprobe. Cross build for the T6021 kernel tree:
#   make KERNELDIR=.../omarchy-linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
default: modules

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

.PHONY: default modules clean

endif
