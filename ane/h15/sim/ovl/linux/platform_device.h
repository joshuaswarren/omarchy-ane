/* Fake <linux/platform_device.h> for sim/sim_overlay.c: the harness records the calls and their order. */
#ifndef SIM_OVL_PLATFORM_DEVICE_H
#define SIM_OVL_PLATFORM_DEVICE_H
struct platform_driver {
	const char *name;
};
int platform_driver_register(struct platform_driver *drv);
void platform_driver_unregister(struct platform_driver *drv);
#endif
