/* Fake <linux/types.h> for sim/sim_overlay.c: just the types the overlay code uses. */
#ifndef SIM_OVL_TYPES_H
#define SIM_OVL_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef u32 phandle;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
