# Out-of-tree test module. Build against a prepared arm64 kernel tree:
#   make -C <kernel-tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#        M=$(PWD) modules [W=1]
# The <kernel-tree> must match the kernel deployed on the target machine,
# or insmod refuses the vermagic.

obj-m += wdt_hang_test.o

KDIR ?= ../../..

.PHONY: modules clean
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
