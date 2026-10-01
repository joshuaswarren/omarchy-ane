ifneq ($(KERNELRELEASE),)
	obj-m := ane_afbridge_probe.o ane_dart_probe.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Build only; the run procedures are receipts/2026-10-01-t6021-af-bridge
# and receipts/2026-10-01-t6021-dart-tunables.
modules clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) $@

.PHONY: modules clean
endif
