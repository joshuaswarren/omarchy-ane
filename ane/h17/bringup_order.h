/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H17_BRINGUP_ORDER_H
#define ANE_H17_BRINGUP_ORDER_H

#include <linux/errno.h>

/* Source-derived ordering only; NOT hardware qualification. The retained
 * kernelcache SHA256 is 7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe.
 * CE, PS and PWGATE block addresses: recorded in the
 * receipts/2026-11-04-j700-h17-firmware-load-path/ dumps.
 * Tunables: verified 37-entry decode recorded at commit 1e4e9e7.
 *
 * This helper stops BEFORE scratch/CPU release. Provider/PMP votes and
 * domain acquisition are caller prerequisites, not replaced by this code.
 * There is intentionally no production caller until those are qualified.
 */
enum h17_order_bank { H17_ORDER_ENGINE, H17_ORDER_PS, H17_ORDER_PWGATE };
enum h17_order_action { H17_ORDER_READ, H17_ORDER_WRITE };

struct h17_order_ops {
	/* Must durably commit intent before returning 0. Failure prevents MMIO. */
	int (*pending)(void *ctx, enum h17_order_action action,
		       enum h17_order_bank bank, unsigned int off,
		       unsigned int width, unsigned long long value);
	int (*read)(void *ctx, enum h17_order_bank bank, unsigned int off,
		    unsigned int width, unsigned long long *value);
	int (*write)(void *ctx, enum h17_order_bank bank, unsigned int off,
		     unsigned int width, unsigned long long value);
	int (*delay_us)(void *ctx, unsigned int us);
};

static inline int h17_order_result(int ret)
{
	return ret > 0 ? -EIO : ret;
}

static inline int h17_order_read(const struct h17_order_ops *ops, void *ctx,
			       enum h17_order_bank bank, unsigned int off,
			       unsigned int width, unsigned long long *value)
{
	int ret = h17_order_result(ops->pending(ctx, H17_ORDER_READ,
					     bank, off, width, 0));
	if (ret)
		return ret;
	return h17_order_result(ops->read(ctx, bank, off, width, value));
}

static inline int h17_order_write(const struct h17_order_ops *ops, void *ctx,
				enum h17_order_bank bank, unsigned int off,
				unsigned int width, unsigned long long value)
{
	int ret = h17_order_result(ops->pending(ctx, H17_ORDER_WRITE,
					     bank, off, width, value));
	if (ret)
		return ret;
	return h17_order_result(ops->write(ctx, bank, off, width, value));
}

static inline int h17_order_poll(const struct h17_order_ops *ops, void *ctx,
			       enum h17_order_bank bank, unsigned int off,
			       unsigned int mask, unsigned int expected)
{
	unsigned int i;
	int ret;
	unsigned long long value;

	for (i = 0; i < 5001; i++) {
		ret = h17_order_read(ops, ctx, bank, off, 4, &value);
		if (ret)
			return ret;
		if ((value & mask) == expected)
			return 0;
		if (i != 5000) {
			ret = h17_order_result(ops->delay_us(ctx, 10));
			if (ret)
				return ret;
		}
	}
	return -ETIMEDOUT;
}

static inline int h17_order_prepare(const struct h17_order_ops *ops, void *ctx)
{
	static const unsigned int tune[][3] = {
		{ 0x0, 0x800000ff, 0x8000001b },
		{ 0x4, 0x80ff00ff, 0x800c000d },
		{ 0x8, 0x80ff00ff, 0x800c000d },
		{ 0xc, 0x80ff00ff, 0x800c000d },
		{ 0x10, 0x80ff00ff, 0x80000003 },
		{ 0x14, 0x80ff00ff, 0x80000003 },
		{ 0x18, 0x800000ff, 0x80000013 },
		{ 0x1c, 0x800000ff, 0x80000020 },
		{ 0x20, 0x80ff00ff, 0x8012000c },
		{ 0x24, 0x80ff00ff, 0x80180006 },
		{ 0x28, 0x80ff00ff, 0x80070003 },
		{ 0x2c, 0x80ff00ff, 0x80070003 },
		{ 0x30, 0x800000ff, 0x80000015 },
		{ 0x34, 0x800000ff, 0x80000011 },
		{ 0x38, 0x800000ff, 0x80000011 },
		{ 0x100, 0xff, 0xff },
		{ 0x108, 0x6, 0 },
		{ 0x304, 0x3f3f0000, 0x02020000 },
		{ 0x314, 0x80000000, 0x80000000 },
		{ 0x31c, 0x80000000, 0x80000000 },
		{ 0x324, 0x80000000, 0x80000000 },
		{ 0x408, 0xffff0020, 0x00120020 },
		{ 0x700, 0x400, 0x400 },
		{ 0x748, 0x07ff07ff, 0x00200030 },
		{ 0x800, 0x400, 0x400 },
		{ 0x848, 0x07ff07ff, 0x00100030 },
		{ 0x900, 0x400, 0x400 },
		{ 0x948, 0x07ff07ff, 0x01000100 },
		{ 0xb00, 0xffffffff, 0x7fffffff },
		{ 0xb04, 0x100, 0x100 },
		{ 0x4008, 0x1, 0 },
		{ 0x400c, 0x1, 0 },
		{ 0x4014, 0x1, 0 },
		{ 0x4018, 0x1, 0 },
		{ 0x401c, 0x1, 0x1 },
		{ 0x338, 0xffffffff, 0x00050020 },
		{ 0x33c, 0xffffffff, 0x000a0030 },
	};
	unsigned long long state, status, value;
	unsigned int toggle, i;
	int ret;

	if (!ops || !ops->pending || !ops->read || !ops->write || !ops->delay_us)
		return -EINVAL;
	ret = h17_order_read(ops, ctx, H17_ORDER_ENGINE, 0x18a4010, 4, &state);
	if (ret)
		return ret;
	state &= 3;
	ret = h17_order_read(ops, ctx, H17_ORDER_ENGINE, 0x18a4018, 4, &status);
	if (ret)
		return ret;
	toggle = (unsigned int)state ^ 3;
	if (toggle & status)
		return -EIO;
	if (state != 3) {
		ret = h17_order_write(ops, ctx, H17_ORDER_ENGINE, 0x18a4000, 4, toggle);
		if (ret)
			return ret;
		ret = h17_order_poll(ops, ctx, H17_ORDER_ENGINE, 0x18a4008, 3, toggle);
		if (ret)
			return ret;
		ret = h17_order_read(ops, ctx, H17_ORDER_ENGINE, 0x18a4018, 8, &status);
		if (ret)
			return ret;
		if (status & toggle)
			return -EIO;
		ret = h17_order_write(ops, ctx, H17_ORDER_ENGINE, 0x18a4008, 8, toggle);
		if (ret)
			return ret;
	}
	for (i = 0; i < sizeof(tune) / sizeof(tune[0]); i++) {
		ret = h17_order_read(ops, ctx, H17_ORDER_ENGINE, tune[i][0], 4, &value);
		if (ret)
			return ret;
		value = (value & ~tune[i][1]) | (tune[i][2] & tune[i][1]);
		ret = h17_order_write(ops, ctx, H17_ORDER_ENGINE, tune[i][0], 4, value);
		if (ret)
			return ret;
	}
	ret = h17_order_write(ops, ctx, H17_ORDER_PS, 0xc000, 4, 0xf);
	if (ret)
		return ret;
	ret = h17_order_poll(ops, ctx, H17_ORDER_PS, 0xc000, 0xff, 0xff);
	if (ret)
		return ret;
	ret = h17_order_write(ops, ctx, H17_ORDER_PWGATE, 0x268c, 4, 0);
	if (ret)
		return ret;
	ret = h17_order_poll(ops, ctx, H17_ORDER_PWGATE, 0x268c, 3, 0);
	if (ret)
		return ret;
	ret = h17_order_write(ops, ctx, H17_ORDER_PS, 0xc000, 4, 0x1000000f);
	if (ret)
		return ret;
	ret = h17_order_poll(ops, ctx, H17_ORDER_PS, 0xc000, 0x100000ff, 0x100000ff);
	if (ret)
		return ret;
	ret = h17_order_write(ops, ctx, H17_ORDER_PS, 0xc008, 4, 0xf);
	if (ret)
		return ret;
	return h17_order_poll(ops, ctx, H17_ORDER_PS, 0xc008, 0xff, 0xff);
}
#endif
