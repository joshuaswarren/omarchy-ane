// SPDX-License-Identifier: GPL-2.0+ OR MIT
/*
 * Replay DockChannel HID packets into the unchanged U-Boot apple_mtp_kbd.c
 * (and apple_kbd.c) on the host, and check which key presses reach the
 * input layer. Packets follow the Linux dockchannel-hid framing.
 *
 * build: cc -I shim -include shim.h -DVARIANT_<ORIG|A|B> -DSRC=<dir> test_mtp_kbd.c
 */
#include "shim.h"

#define STR2(x) #x
#define STR(x) STR2(x)
#include STR(SRC/apple_kbd.c)
#include STR(SRC/apple_mtp_kbd.c)

u8 fifo[8192];
size_t fifo_rd, fifo_wr;
ulong fake_ms;

static int presses[64], npress;

int input_add_keycode(struct input_config *config, int new_keycode, bool release)
{
	if (!release && npress < 64)
		presses[npress++] = new_keycode;
	return 0;
}

static u8 seq;

/* One packet: 8-byte header, sub header, payload padded to 4, checksum. */
static void pkt(u8 iface, u8 flags, const u8 *payload, int len, bool bad_sum)
{
	u8 p[512] = { 0 };
	int body = 8 + ((len + 3) & ~3);
	u32 sum = 0;
	int i;

	p[0] = 8;
	p[1] = 0x12;
	p[2] = body & 0xff;
	p[3] = body >> 8;
	p[4] = seq++;
	p[5] = iface;
	p[8] = flags;
	p[10] = len & 0xff;
	p[11] = len >> 8;
	memcpy(p + 16, payload, len);
	for (i = 0; i < 8 + body; i += 4)
		sum += get_unaligned_le32(p + i);
	sum = 0xffffffff - sum + (bad_sum ? 1 : 0);
	memcpy(p + 8 + body, &sum, 4);
	memcpy(fifo + fifo_wr, p, 8 + body + 4);
	fifo_wr += 8 + body + 4;
}

static void init(u8 iface, const char *name, u8 more)
{
	u8 d[26] = { 0xf0, 0, 0, iface };

	strncpy((char *)d + 4, name, 16);
	d[20] = more;
	d[22] = 2;	/* INIT_TERMINATOR block, length 0 */
	pkt(0, 0, d, sizeof(d), false);
}

/* Keyboard report 1: id, modifiers, reserved, 6 key codes, fn. */
static void key(u8 iface, u8 code, bool bad_sum)
{
	u8 r[10] = { 0x01, 0, 0, code };

	pkt(iface, 0, r, sizeof(r), bad_sum);
}

static struct input_config *input;

/* Poll as tstc() does until the FIFO is empty; return presses since last call. */
static int poll_all(void)
{
	int n0 = npress;

	while (fifo_rd < fifo_wr)
		input->read_keys(input);
	return npress - n0;
}

static void boot(void)
{
	static struct udevice dev;
	static struct keyboard_priv kp;
	static struct apple_mtp_kbd_priv priv;

	memset(&priv, 0, sizeof(priv));
	memset(&kp, 0, sizeof(kp));
	dev.priv = &priv;
	dev.uc_priv = &kp;
	fifo_rd = fifo_wr = 0;
	npress = 0;
	if (drv_apple_mtp_kbd.probe(&dev))
		exit(2);
	input = &kp.input;
}

/* The MTP announces its interfaces as it starts: stm 1, multi-touch 2, keyboard 3, tp_accel 4. */
static void announce(void)
{
	init(1, "stm", 0);
	init(2, "multi-touch", 0);
	init(3, "keyboard", 0);
	init(4, "tp_accel", 0);
}

#if defined(VARIANT_ORIG)
#define V(o, a, b) (o)
#elif defined(VARIANT_A)
#define V(o, a, b) (a)
#else
#define V(o, a, b) (b)
#endif

static int fails;

static void check(const char *what, int got, int want)
{
	printf("%-62s presses %d (want %d) %s\n", what, got, want,
	       got == want ? "ok" : "FAIL");
	fails += got != want;
}

int main(void)
{
	int n;

	/* HA: a 0x14-byte tp_accel report with first byte 0x01 at start */
	boot();
	announce();
	key(4, 0x29, false);	/* HID 0x29 = Escape */
	check("HA tp_accel report at start (Escape code)", poll_all(), V(1, 0, 0));

	/* HB: keyboard report with Enter down at start, no release */
	boot();
	announce();
	key(3, 0x28, false);
	check("HB keyboard Enter down at start", poll_all(), V(1, 1, 0));
	key(3, 0x00, false);
	key(3, 0x04, false);
	key(3, 0x00, false);
	check("HB then release, then a real 'a' press", poll_all(), 1);
	check("HB the live press is KEY_A", presses[npress - 1] == KEY_A, 1);
	key(3, 0x28, false);
	check("HB Enter pressed again after release", poll_all(), 1);

	/* HC: no key at start, then a key down after the first poll */
	boot();
	announce();
	key(3, 0x00, false);
	poll_all();
	key(3, 0x28, false);
	check("HC live Enter down (cannot tell from a real press)", poll_all(), 1);

	/* HE: keyboard report before the keyboard is announced */
	boot();
	init(1, "stm", 0);
	key(3, 0x28, false);
	init(3, "keyboard", 0);
	check("HE keyboard report before its EVENT_INIT", poll_all(), V(1, 0, 0));

	/* HD: bad checksum on a live keyboard report */
	boot();
	announce();
	poll_all();
	key(3, 0x28, true);
	check("HD live keyboard report, bad checksum", poll_all(), V(1, 0, 0));
	key(3, 0x00, false);
	key(3, 0x04, false);
	check("HD next good report still works", poll_all(), 1);

	/* HD: lost framing (5 stray bytes queued with a report), then good packets */
	boot();
	announce();
	poll_all();
	memset(fifo + fifo_wr, 0xaa, 5);
	fifo_wr += 5;
	key(3, 0x28, false);
	n = poll_all();
	if (V(1, 0, 0))
		printf("%-62s presses %d (no check)\n", "HD stray bytes, original", n);
	else
		check("HD stray bytes: queued bytes dropped", n, 0);
	key(3, 0x00, false);
	key(3, 0x04, false);
	check("HD after stray bytes, a later report works", poll_all(), 1);

	/* Keyboard init in two packets, a live report between them */
	boot();
	init(3, "keyboard", 1);
	poll_all();
	key(3, 0x28, false);
	check("init more_packets=1, then a live keyboard report", poll_all(), V(1, 1, 0));
	init(3, "keyboard", 0);
	key(3, 0x00, false);
	key(3, 0x04, false);
	check("after the last init packet, a real press", poll_all(), 1);

	/* Live traffic of another interface, 0x14 bytes, first byte 0x01 */
	boot();
	announce();
	poll_all();
	key(4, 0x29, false);
	check("live tp_accel 0x14-byte report", poll_all(), V(1, 0, 0));

	printf("%s: %d failure(s)\n", STR(SRC), fails);
	return fails != 0;
}
