ifneq ($(KERNELRELEASE),)
	obj-m := ane_afbridge_probe.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Build only; the run procedure is receipts/2026-10-01-t6021-af-bridge.
modules clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) $@

.PHONY: modules clean
endif
