// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Pure host model: no module loading, device access, or real MMIO. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../bringup_order.h"

struct slot { enum h17_order_bank bank; unsigned int off; unsigned long long value; };
struct model {
	struct slot slots[64];
	unsigned int slots_n, pending, io, fail_pending, fail_io, writes, tunes;
	unsigned int delays, state, toggle;
	int stuck;
	int status_error;
	int post_status_error;
	int ce_done;
	unsigned int width_ack;
	enum h17_order_action action;
	enum h17_order_bank bank;
	unsigned int off, width;
};

static unsigned long long *slot(struct model *m, enum h17_order_bank bank,
				unsigned int off)
{
	unsigned int i;
	for (i = 0; i < m->slots_n; i++)
		if (m->slots[i].bank == bank && m->slots[i].off == off)
			return &m->slots[i].value;
	assert(m->slots_n < 64);
	m->slots[m->slots_n] = (struct slot){bank, off, 0xdeadbeef};
	return &m->slots[m->slots_n++].value;
}

static int pending(void *ctx, enum h17_order_action action,
		   enum h17_order_bank bank, unsigned int off,
		   unsigned int width, unsigned long long value)
{
	struct model *m = ctx;
	(void)value;
	assert(width == 4 || width == 8);
	assert(off % width == 0);
	assert(off != 0x1600044 && off != 0x1050000); /* no CPU/RVBAR writes */
	m->action = action; m->bank = bank; m->off = off; m->width = width;
	return ++m->pending == m->fail_pending ? -ENOSPC : 0;
}

static int before_io(struct model *m, enum h17_order_action action,
		     enum h17_order_bank bank, unsigned int off, unsigned int width)
{
	assert(m->action == action && m->bank == bank);
	assert(m->off == off && m->width == width);
	return ++m->io == m->fail_io ? -EIO : 0;
}

static int read_reg(void *ctx, enum h17_order_bank bank, unsigned int off,
		    unsigned int width, unsigned long long *value)
{
	struct model *m = ctx;
	int ret = before_io(m, H17_ORDER_READ, bank, off, width);
	if (ret) return ret;
	if (bank == H17_ORDER_ENGINE && off == 0x18a4010)
		*value = m->state;
	else if (bank == H17_ORDER_ENGINE && off == 0x18a4018)
		*value = (m->status_error || (width == 8 && m->post_status_error)) ? 3 : 0;
	else if (bank == H17_ORDER_ENGINE && off == 0x18a4008)
		*value = m->stuck == 1 ? 0 : m->toggle;
	else if (bank == H17_ORDER_PS)
		*value = m->stuck == 2 ? 0 : *slot(m, bank, off) | 0xf0;
	else if (bank == H17_ORDER_PWGATE)
		*value = m->stuck == 3 ? 3 : *slot(m, bank, off);
	else
		*value = *slot(m, bank, off);
	return 0;
}

static int write_reg(void *ctx, enum h17_order_bank bank, unsigned int off,
		     unsigned int width, unsigned long long value)
{
	struct model *m = ctx;
	int ret = before_io(m, H17_ORDER_WRITE, bank, off, width);
	if (ret) return ret;
	m->writes++;
	if (bank == H17_ORDER_ENGINE && off == 0x18a4000) {
		assert(m->writes == 1 && width == 4);
		m->toggle = value;
	} else if (bank == H17_ORDER_ENGINE && off == 0x18a4008) {
		assert(value == m->toggle && width == 8);
		m->ce_done = 1;
		m->width_ack = width;
	} else if (bank == H17_ORDER_ENGINE) {
		assert(m->ce_done);
		m->tunes++;
	} else {
		unsigned int phase = m->writes - (m->state == 3 ? 37 : 39);
		assert(m->tunes == 37);
		if (phase == 1)
			assert(bank == H17_ORDER_PS && off == 0xc000 && value == 0xf);
		else if (phase == 2)
			assert(bank == H17_ORDER_PWGATE && off == 0x268c && value == 0);
		else if (phase == 3)
			assert(bank == H17_ORDER_PS && off == 0xc000 && value == 0x1000000f);
		else
			assert(phase == 4 && bank == H17_ORDER_PS && off == 0xc008 && value == 0xf);
	}
	*slot(m, bank, off) = value;
	return 0;
}

static int delay(void *ctx, unsigned int us)
{
	struct model *m = ctx;
	assert(us == 10);
	m->delays++;
	return 0;
}

static const struct h17_order_ops ops = { pending, read_reg, write_reg, delay };

int main(void)
{
	struct model m = {0};
	unsigned int i, operations;
	assert(h17_order_prepare(NULL, &m) == -EINVAL);
	assert(h17_order_prepare(&ops, &m) == 0);
	assert(m.tunes == 37 && m.width_ack == 8 && m.writes == 43);
	assert(*slot(&m, H17_ORDER_ENGINE, 0x108) == (0xdeadbeefU & ~6U));
	assert(*slot(&m, H17_ORDER_PS, 0xc000) == 0x1000000f);
	assert(*slot(&m, H17_ORDER_PS, 0xc008) == 0xf);
	assert(*slot(&m, H17_ORDER_PWGATE, 0x268c) == 0);
	operations = m.pending;
	for (i = 1; i <= operations; i++) {
		memset(&m, 0, sizeof(m)); m.fail_pending = i;
		assert(h17_order_prepare(&ops, &m) == -ENOSPC);
		assert(m.pending == i && m.io == i - 1);
		memset(&m, 0, sizeof(m)); m.fail_io = i;
		assert(h17_order_prepare(&ops, &m) == -EIO);
		assert(m.pending == i && m.io == i);
	}
	memset(&m, 0, sizeof(m)); m.state = 3; m.ce_done = 1;
	assert(h17_order_prepare(&ops, &m) == 0);
	assert(m.writes == 41 && m.width_ack == 0); /* already-open CE */
	for (i = 1; i < 3; i++) {
		memset(&m, 0, sizeof(m)); m.state = i;
		assert(h17_order_prepare(&ops, &m) == 0 && m.toggle == (i ^ 3));
	}
	memset(&m, 0, sizeof(m)); m.status_error = 1;
	assert(h17_order_prepare(&ops, &m) == -EIO && m.writes == 0);
	memset(&m, 0, sizeof(m)); m.post_status_error = 1;
	assert(h17_order_prepare(&ops, &m) == -EIO && m.writes == 1 && m.tunes == 0);
	for (i = 1; i <= 3; i++) {
		memset(&m, 0, sizeof(m)); m.stuck = i;
		assert(h17_order_prepare(&ops, &m) == -ETIMEDOUT);
		assert(m.delays == 5000);
		if (i == 1) assert(m.tunes == 0 && m.writes == 1);
	}
	printf("PASS: H17 CE/tunable/PS/PWGATE ordering, %u intent and %u I/O failures, three bounded timeouts; no CPU release\n",
	       operations, operations);
	return 0;
}
