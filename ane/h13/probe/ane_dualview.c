// ane_dualview.c — Gap8 EXP-1: carveout mapping alias check, read-only.
// Maps the two 22G74 staging PAs through TWO independent kernel mappings
// (memremap WC and ioremap) and compares content. Gap7's restore
// readbacks went through a single mapping — self-consistent even when
// the alias is wrong (Gap5 proved an alien-alias memremap PTE on 7.1.13).
// Two views disagreeing is the discriminator.
// Also dumps the TEXT live words at the vector slots (0x0/0x80/0x100/
// 0x180/0x200/0x204/0x234) for the fw vector-poisoning check vs the
// eos-head file reference.
#include <linux/module.h>
#include <linux/io.h>
#include <linux/memremap.h>

struct span { u64 pa; u64 len; const char *name; };
static const struct span spans[] = {
	{ 0x10000a54000ULL, 0x400, "TEXT-head" },
	{ 0x10001684000ULL, 0x400, "DATA-head" },
};
/* TEXT vector-slot words (u32 at PA base + off) */
static const u64 slots[] = { 0x0, 0x80, 0x100, 0x180, 0x200, 0x204, 0x234 };

static void fletcher(const void *p, u64 len, u64 *a, u64 *b)
{
	const u8 *q = p;
	u64 s1 = 0, s2 = 0, i;

	for (i = 0; i < len; i++) {
		s1 = (s1 + q[i]) & 0xffffffffffffULL;
		s2 = (s2 + s1) & 0xffffffffffffULL;
	}
	*a = s2;
	*b = s1;
}

static int __init dualview_init(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(spans); i++) {
		const struct span *s = &spans[i];
		void *va1, *va2;
		u64 a1, b1, a2, b2;

		va1 = memremap(s->pa, s->len, MEMREMAP_WC);
		va2 = ioremap(s->pa, s->len);
		if (!va1 || !va2) {
			pr_info("dualview: %s: map failed (memremap=%px ioremap=%px)\n",
				s->name, va1, va2);
			if (va1) memunmap(va1);
			if (va2) iounmap(va2);
			continue;
		}
		fletcher(va1, s->len, &a1, &b1);
		fletcher(va2, s->len, &a2, &b2);
		pr_info("dualview: %s pa=0x%llx memremap=%012llx:%012llx ioremap=%012llx:%012llx %s\n",
			s->name, s->pa, a1, b1, a2, b2,
			(a1 == a2 && b1 == b2) ? "VIEWS-AGREE" : "VIEWS-DIFFER");
		if (i == 0) {		/* TEXT slot words, both views */
			unsigned int k;

			for (k = 0; k < ARRAY_SIZE(slots); k++)
				pr_info("dualview: TEXT vm+0x%03llx wc=0x%08x ioremap=0x%08x\n",
					slots[k], *(u32 *)(va1 + slots[k]),
					*(u32 *)(va2 + slots[k]));
		}
		memunmap(va1);
		iounmap(va2);
	}
	return 0;
}
module_init(dualview_init);

static void __exit dualview_exit(void) { }
module_exit(dualview_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gap8 read-only dual-view carveout alias check");
