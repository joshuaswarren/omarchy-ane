// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_dartraw.c — Gap9 window A: read the never-touched ANE dart register
 * state on the unbound-DT variant boot (apple-dart never probes there).
 *
 * Gate law (ane_clk_probe precedent): the pmgr ACTUAL nibble of the owning
 * power domains must read 0xf before any dart MMIO read; otherwise the
 * module refuses (reads on unpowered MMIO wedge the interconnect).
 * Log-then-read: every address is printed BEFORE the readl.
 * Read-only. ioremap_np on raw PAs because the dart platform devices do
 * not exist on the variant boot (status=disabled).
 */
#include <linux/io.h>
#include <linux/module.h>

static unsigned long dart0 = 0x285800000UL;
module_param(dart0, ulong, 0444);
static unsigned long dart1 = 0x285810000UL;
module_param(dart1, ulong, 0444);
static unsigned long dart2 = 0x285820000UL;
module_param(dart2, ulong, 0444);
MODULE_PARM_DESC(dart0, "dart0 PA (0=skip)");
MODULE_PARM_DESC(dart1, "dart1 PA (0=skip)");
MODULE_PARM_DESC(dart2, "dart2 PA (0=skip)");

/* pmgr pwrstate registers whose ACTUAL nibble (bits 7:4) gates reads */
static unsigned long gate_sys = 0x28e080268UL; /* ps_ane_sys  (dart0 pd)  */
static unsigned long gate_cpu = 0x28e0802c8UL; /* ane_cpu_pd  (dart1/2 pd)*/
static unsigned long gate_set0 = 0x28e08c000UL; /* ps_ane_set0 (engine)   */

static const unsigned long offs[] = {
	0x00, 0x04, 0x08, 0x0c,		/* PARAMS1..4 */
	0x40, 0x50, 0x54,		/* ERROR, ERRADDR_LO/HI */
	0x60,				/* CONFIG */
	0x80, 0x84, 0x88, 0x8c,		/* REMAP0..3 */
	0xfc,				/* ENABLED_STREAMS */
	0x100, 0x104, 0x108, 0x10c,	/* TCR[0..3] */
	0x200, 0x204, 0x208, 0x20c,	/* TTBR[0][0..3] (stream 0) */
	0x1000,				/* t8110 TCR shadow */
	0x1400,				/* t8110 TTBR shadow */
};

static int gate_check(const char *name, unsigned long pa)
{
	void __iomem *r = ioremap_np(pa, 4);
	u32 w;

	if (!r) {
		pr_err("dartraw: gate %s ioremap failed\n", name);
		return -1;
	}
	w = readl(r);
	iounmap(r);
	pr_info("dartraw: gate %s @%#lx = %#x (ACTUAL=%x)\n",
		name, pa, w, (w >> 4) & 0xf);
	return ((w >> 4) & 0xf) == 0xf ? 0 : -1;
}

static void peek_dart(const char *name, unsigned long pa)
{
	void __iomem *base;
	unsigned int i;

	pr_info("dartraw: %s @%#lx: mapping next\n", name, pa);
	base = ioremap_np(pa, 0x4000);
	if (!base) {
		pr_info("dartraw: %s: ioremap_np failed\n", name);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(offs); i++) {
		pr_info("dartraw: %s pre-read +0x%03lx\n", name, offs[i]);
		pr_info("dartraw: %s +0x%03lx = 0x%08x\n",
			name, offs[i], readl(base + offs[i]));
	}
	iounmap(base);
	pr_info("dartraw: %s done\n", name);
}

static int __init dartraw_init(void)
{
	int g1, g2, g3;

	g1 = gate_check("ane_sys", gate_sys);
	g2 = gate_check("ane_sys_cpu", gate_cpu);
	g3 = gate_check("ane_set0", gate_set0);
	if (g1 || g2 || g3) {
		pr_err("dartraw: domain off (sys=%d cpu=%d set0=%d) — refusing dart reads\n",
		       g1, g2, g3);
		return -EIO;
	}
	if (dart0)
		peek_dart("dart0", dart0);
	if (dart1)
		peek_dart("dart1", dart1);
	if (dart2)
		peek_dart("dart2", dart2);
	return 0;
}
static void __exit dartraw_exit(void) { }
module_init(dartraw_init);
module_exit(dartraw_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gap9 read-only never-touched ANE dart state (gate-checked)");
