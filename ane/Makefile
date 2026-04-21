ifneq ($(KERNELRELEASE),)
	ccflags-y += -DMODULE
	obj-m := ane.o
	ane-objs := ./src/ane_drv.o ./src/ane_tm.o
else
	KERNELDIR := /lib/modules/$(shell uname -r)/build
 	PWD := $(shell pwd)

default:
	sudo cp ./src/uapi/drm/ane_accel.h /lib/modules/$(shell uname -r)/build/include/uapi/drm/
	make -C $(KERNELDIR) M=$(PWD) modules

install:
	sudo make -C $(KERNELDIR) M=$(PWD) modules_install
	# sudo depmod -a
	sudo modprobe ane
	if [ -e /dev/accel/accel0 ]; then sudo chmod 666 /dev/accel/accel0; \
	else echo "note: /dev/accel/accel0 not found; device may not have probed"; fi

uninstall:
	sudo modprobe -r ane

clean:
	make -C $(KERNELDIR) M=$(PWD) clean

endif
