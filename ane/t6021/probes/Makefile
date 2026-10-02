ifneq ($(KERNELRELEASE),)
	obj-m := ane_afbridge_probe.o ane_dart_probe.o ane_dcs_ps_probe.o \
		 ane_dsid_tm_probe.o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build

# Build only; the run procedures are receipts/2026-10-01-t6021-af-bridge,
# receipts/2026-10-01-t6021-dart-tunables and receipts/2026-10-02-ane-dsid.
modules clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) $@

.PHONY: modules clean
endif
