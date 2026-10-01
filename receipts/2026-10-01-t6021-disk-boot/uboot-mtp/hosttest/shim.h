/* Host shim: just enough of U-Boot to compile drivers/input/apple_{mtp_,}kbd.c unchanged. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/input-event-codes.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef uint16_t __le16;
typedef uint32_t __le32;
typedef unsigned long ulong;
typedef unsigned long fdt_addr_t;
typedef int ofnode;

#define __packed __attribute__((packed))
#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h, l) (((~0U) << (l)) & (~0U >> (31 - (h))))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctz(m))
#define le16_to_cpu(x) (x)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define printk printf
#define dev_err(dev, ...) printf(__VA_ARGS__)
#define FDT_ADDR_T_NONE ((fdt_addr_t)-1)
#define of_match_ptr(x) (x)
#define DM_FLAG_OS_PREPARE 1
#define DM_REMOVE_NORMAL 0
#define udelay(x) ((void)0)

enum { UCLASS_MISC, UCLASS_KEYBOARD, UCLASS_SIMPLE_BUS };

static inline u32 get_unaligned_le32(const void *p)
{
	const u8 *b = p;

	return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24;
}

static inline void *memscan(void *addr, int c, size_t size)
{
	u8 *p = addr;

	while (size && *p != c) {
		p++;
		size--;
	}
	return p;
}

static inline void *memchr_inv(const void *s, int c, size_t n)
{
	const u8 *p = s;

	for (; n; n--, p++)
		if (*p != (u8)c)
			return (void *)p;
	return NULL;
}

struct udevice {
	void *priv;
	void *uc_priv;
};

struct udevice_id {
	const char *compatible;
	ulong data;
};

struct driver {
	const char *name;
	int id;
	const struct udevice_id *of_match;
	int (*probe)(struct udevice *dev);
	int (*remove)(struct udevice *dev);
	int priv_auto;
	const void *ops;
	int flags;
};

#define U_BOOT_DRIVER(n) struct driver drv_##n

struct stdio_dev {
	char name[32];
};

struct input_config {
	struct udevice *dev;
	int (*read_keys)(struct input_config *config);
};

struct keyboard_priv {
	struct stdio_dev sdev;
	struct input_config input;
};

struct keyboard_ops {
	int dummy;
};

static inline void *dev_get_priv(struct udevice *dev) { return dev->priv; }
static inline void *dev_get_uclass_priv(struct udevice *dev) { return dev->uc_priv; }

/* Fake DockChannel FIFO: "data" at 0x1000, "rmt-data" at 0x2000. */
#define FAKE_LOCAL 0x1000UL
#define FAKE_RMT 0x2000UL
extern u8 fifo[8192];
extern size_t fifo_rd, fifo_wr;
extern ulong fake_ms;

static inline u32 readl(const void *addr)
{
	ulong a = (ulong)addr;

	if (a == FAKE_LOCAL + 0x2c)
		return fifo_wr - fifo_rd;
	if (a == FAKE_LOCAL + 0x1c)
		return fifo_rd < fifo_wr ? (u32)fifo[fifo_rd++] << 8 : 0;
	return 0;
}

static inline void writel(u32 v, void *addr) { (void)v; (void)addr; }
static inline ulong get_timer(ulong base) { return ++fake_ms - base; }

static inline fdt_addr_t dev_read_addr_name(struct udevice *dev, const char *name)
{
	return !strcmp(name, "data") ? FAKE_LOCAL : FAKE_RMT;
}

static inline int dev_read_u32(struct udevice *dev, const char *name, u32 *v)
{
	*v = !strcmp(name, "apple,fifo-size") ? 0x800 : 1;
	return 0;
}

static inline ofnode ofnode_get_by_phandle(u32 ph) { return (ofnode)ph; }

static struct udevice fake_helper;
static inline int uclass_get_device_by_ofnode(int id, ofnode n, struct udevice **devp)
{
	*devp = &fake_helper;
	return 0;
}

static inline int device_remove(struct udevice *dev, int flags) { return 0; }
static inline int apple_rtkit_helper_poll(struct udevice *dev, ulong t) { return 0; }
static inline int input_add_tables(struct input_config *c, bool ch) { return 0; }
static inline int input_stdio_register(struct stdio_dev *d) { return 0; }

/* Key presses that reach the input layer (releases give no character). */
int input_add_keycode(struct input_config *config, int new_keycode, bool release);
