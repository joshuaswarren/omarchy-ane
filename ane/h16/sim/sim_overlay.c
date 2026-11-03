// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sim_overlay.c: drives the REAL ane_h16_overlay.c against a fake live device tree (the headers under sim/ovl/linux). Every call the
 * code makes into the tree, the changeset and the driver core is recorded in order, so the tests assert both what it does and that it
 * does nothing at all when a guard refuses. Exit 0 iff every check passes.
 *   make sim-overlay
 */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "../ane_h16_overlay.c"

static int fails;
static char ev[200][100];
static int nev;
static char msgs[60][200];
static int nmsg;

static struct device_node tree[16];
static int ntree;
static const char *machine;
static int sim_cs_apply_rc, sim_drv_rc;
static bool drv_registered;
static struct device_node *n_pmgr, *n_pmgr_ane, *n_fab4, *n_fab5, *n_ane, *n_dart[3], *n_ps[5];

static void ev_add(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (nev < 200)
		vsnprintf(ev[nev++], sizeof(ev[0]), fmt, ap);
	va_end(ap);
}

/* ---- tracked heap: a property belongs to the kernel once the changeset accepted it ---- */
static int sim_prop_misuse, sim_alloc_fail_n, sim_allocs, sim_update_fail_n, sim_updates, sim_distinct_props;
static struct {
	const void *p;
	bool live, held;
} heap[256];
static int nheap;

static int heap_find(const void *p)
{
	for (int i = 0; p && i < nheap; i++)
		if (heap[i].live && heap[i].p == p)
			return i;
	return -1;
}

static void *heap_add(void *p)
{
	if (p && nheap < 256) {
		heap[nheap].p = p;
		heap[nheap].live = true;
		heap[nheap].held = false;
		nheap++;
	}
	return p;
}

static bool heap_fail(void)
{
	return sim_alloc_fail_n && ++sim_allocs == sim_alloc_fail_n;
}

static void heap_hold(const void *p)
{
	int i = heap_find(p);

	if (i >= 0)
		heap[i].held = true;
}

static int heap_unheld_live(void)
{
	int n = 0;

	for (int i = 0; i < nheap; i++)
		n += heap[i].live && !heap[i].held;
	return n;
}

void *kzalloc(size_t size, int gfp)
{
	(void)gfp;
	return heap_fail() ? NULL : heap_add(calloc(1, size));
}

char *kstrdup(const char *s, int gfp)
{
	(void)gfp;
	return heap_fail() ? NULL : heap_add(strdup(s));
}

void *kmemdup(const void *src, size_t len, int gfp)
{
	void *p;

	(void)gfp;
	if (heap_fail())
		return NULL;
	p = malloc(len);
	memcpy(p, src, len);
	return heap_add(p);
}

/* Freeing something that is not a live heap block, or that the kernel already holds, is the module's bug. */
void kfree(const void *p)
{
	int i = heap_find(p);

	if (!p)
		return;
	if (i < 0 || heap[i].held) {
		sim_prop_misuse++;
		ev_add("MISUSE kfree of a block the module does not own");
		return;
	}
	heap[i].live = false;
	free((void *)p);
}

void sim_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (nmsg < 60)
		vsnprintf(msgs[nmsg++], sizeof(msgs[0]), fmt, ap);
	va_end(ap);
}

static void chk(int ok, const char *name)
{
	printf("  %-110s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok)
		fails++;
}

static int ev_count(const char *prefix)
{
	int n = 0;

	for (int i = 0; i < nev; i++)
		if (!strncmp(ev[i], prefix, strlen(prefix)))
			n++;
	return n;
}

static int ev_index(const char *s)
{
	for (int i = 0; i < nev; i++)
		if (!strcmp(ev[i], s))
			return i;
	return -1;
}

static int msg_has(const char *s)
{
	for (int i = 0; i < nmsg; i++)
		if (strstr(msgs[i], s))
			return 1;
	return 0;
}

/* ---- the fake kernel ---- */
struct device_node *of_find_node_by_path(const char *path)
{
	for (int i = 0; i < ntree; i++)
		if (!strcmp(tree[i].path, path)) {
			tree[i].refs++;
			return &tree[i];
		}
	return NULL;
}

struct device_node *of_find_all_nodes(struct device_node *prev)
{
	int i = prev ? (int)(prev - tree) + 1 : 0;

	return i < ntree ? &tree[i] : NULL;
}

bool of_machine_is_compatible(const char *compat)
{
	return machine && !strcmp(machine, compat);
}

bool of_device_is_available(const struct device_node *np)
{
	return np && np->avail;
}

int of_device_is_compatible(const struct device_node *np, const char *compat)
{
	return np && np->compat && !strcmp(np->compat, compat);
}

void of_node_put(struct device_node *np)
{
	if (np)
		np->refs--;
}

static struct sim_prop *prop_slot(struct device_node *np, const char *name, bool create)
{
	struct sim_prop *free_slot = NULL;

	for (int i = 0; i < SIM_MAX_PROPS; i++) {
		if (np->props[i].present && !strcmp(np->props[i].name, name))
			return &np->props[i];
		if (!np->props[i].present && !free_slot)
			free_slot = &np->props[i];
	}
	return create ? free_slot : NULL;
}

struct property *of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	struct sim_prop *p = prop_slot((struct device_node *)np, name, false);

	if (!p)
		return NULL;
	p->pub.name = p->name;
	p->pub.length = p->length;
	p->pub.value = p->value;
	if (lenp)
		*lenp = p->length;
	return &p->pub;
}

static void prop_put(struct device_node *np, const char *name, const void *val, int len)
{
	struct sim_prop *p = prop_slot(np, name, true);

	snprintf(p->name, sizeof(p->name), "%s", name);
	memset(p->value, 0, sizeof(p->value));
	memcpy(p->value, val, len);
	p->length = len;
	p->present = true;
	if (!strcmp(name, "status"))
		np->avail = !strcmp((const char *)val, "okay");
}

static void prop_drop(struct device_node *np, const char *name)
{
	struct sim_prop *p = prop_slot(np, name, false);

	if (p)
		p->present = false;
}

void of_changeset_init(struct of_changeset *ocs)
{
	memset(ocs, 0, sizeof(*ocs));
	ev_add("cs-init");
}

void of_changeset_destroy(struct of_changeset *ocs)
{
	(void)ocs;
	ev_add("cs-destroy");
}

static int cs_record(struct of_changeset *ocs, enum sim_cs_kind kind, struct device_node *np, const char *name, const void *val, int len)
{
	struct sim_prop *cur = prop_slot(np, name, false);
	int i = ocs->n;

	if (i >= 40 || (kind == CS_UPDATE && !cur) || (kind == CS_ADD && cur) || (kind == CS_REMOVE && !cur))
		return -EINVAL;
	ocs->ent[i].kind = kind;
	ocs->ent[i].np = np;
	memset(&ocs->ent[i].before, 0, sizeof(ocs->ent[i].before));
	if (cur)
		ocs->ent[i].before = *cur;
	memset(&ocs->ent[i].after, 0, sizeof(ocs->ent[i].after));
	snprintf(ocs->ent[i].after.name, sizeof(ocs->ent[i].after.name), "%s", name);
	if (val)
		memcpy(ocs->ent[i].after.value, val, len);
	ocs->ent[i].after.length = len;
	ocs->n++;
	ev_add("%s %s %s", kind == CS_UPDATE ? "update" : kind == CS_ADD ? "add" : "remove", np->path, name);
	return 0;
}

int of_changeset_update_property(struct of_changeset *ocs, struct device_node *np, const struct property *prop)
{
	int i = ocs->n, ret;

	if (sim_update_fail_n && ++sim_updates == sim_update_fail_n)
		return -ENOMEM;
	if (heap_find(prop) < 0 || heap_find(prop->name) < 0 || heap_find(prop->value) < 0 ||
	    !(prop->_flags & (1UL << OF_DYNAMIC)) || prop->next) {
		sim_prop_misuse++;
		ev_add("MISUSE %s %s is not a fresh dynamic heap property", np->path, prop->name);
	}
	for (int j = 0; j < i; j++)
		if (ocs->ent[j].prop == prop) {
			sim_prop_misuse++;
			ev_add("MISUSE %s %s reuses a property struct that is already in the change set", np->path, prop->name);
		}
	ret = cs_record(ocs, CS_UPDATE, np, prop->name, prop->value, prop->length);
	if (!ret) {
		ocs->ent[i].prop = prop;
		heap_hold(prop);
		heap_hold(prop->name);
		heap_hold(prop->value);
		sim_distinct_props++;
	}
	return ret;
}

int of_changeset_add_prop_u32(struct of_changeset *ocs, struct device_node *np, const char *name, u32 val)
{
	__be32 be = cpu_to_be32(val);

	return cs_record(ocs, CS_ADD, np, name, &be, 4);
}

int of_changeset_remove_property(struct of_changeset *ocs, struct device_node *np, struct property *prop)
{
	return cs_record(ocs, CS_REMOVE, np, prop->name, NULL, 0);
}

int of_changeset_apply(struct of_changeset *ocs)
{
	ev_add("apply");
	if (sim_cs_apply_rc)
		return sim_cs_apply_rc;
	for (int i = 0; i < ocs->n; i++) {
		if (ocs->ent[i].kind == CS_REMOVE)
			prop_drop(ocs->ent[i].np, ocs->ent[i].after.name);
		else {
			if (ocs->ent[i].prop) {
				struct property *p = (struct property *)ocs->ent[i].prop;

				if (p->next) {
					sim_prop_misuse++;
					ev_add("MISUSE %s %s is already linked into a property list", ocs->ent[i].np->path, p->name);
				}
				p->next = (struct property *)&tree[0];
			}
			prop_put(ocs->ent[i].np, ocs->ent[i].after.name, ocs->ent[i].after.value, ocs->ent[i].after.length);
		}
	}
	ocs->applied = true;
	return 0;
}

int of_changeset_revert(struct of_changeset *ocs)
{
	ev_add("revert");
	for (int i = ocs->n - 1; i >= 0; i--) {
		if (ocs->ent[i].kind == CS_ADD) {
			prop_drop(ocs->ent[i].np, ocs->ent[i].after.name);
		} else {
			prop_put(ocs->ent[i].np, ocs->ent[i].before.name, ocs->ent[i].before.value, ocs->ent[i].before.length);
		}
	}
	ocs->applied = false;
	return 0;
}

int platform_driver_register(struct platform_driver *drv)
{
	(void)drv;
	ev_add("register-driver");
	if (sim_drv_rc)
		return sim_drv_rc;
	drv_registered = true;
	return 0;
}

void platform_driver_unregister(struct platform_driver *drv)
{
	(void)drv;
	ev_add("unregister-driver");
	drv_registered = false;
}

/* ---- the fake tree, as it stands on a T6040 with every ANE node present but disabled ---- */
static struct device_node *mk(const char *path, const char *compat, bool avail)
{
	struct device_node *np = &tree[ntree++];

	memset(np, 0, sizeof(*np));
	np->path = path;
	np->compat = compat;
	np->avail = avail;
	if (!avail)
		prop_put(np, "status", "disabled", 9);
	return np;
}

static void put_cells(struct device_node *np, const char *name, const u32 *c, int n)
{
	__be32 be[16];

	for (int i = 0; i < n; i++)
		be[i] = cpu_to_be32(c[i]);
	prop_put(np, name, be, 4 * n);
}

static void tree_reset(void)
{
	static const char *const psp[5] = {
		"/power-management@502280000/power-controller@3c0", "/power-management@50228c000/power-controller@0",
		"/power-management@50228c000/power-controller@8", "/power-management@50228c000/power-controller@10",
		"/power-management@50228c000/power-controller@18",
	};
	static const char *const dp[3] = { "/iommu@485800000", "/iommu@485820000", "/iommu@485840000" };
	static const u32 reg0[4] = { 4, 0x84000000, 0, 0x2000000 };
	static const u32 pd0[1] = { 12 };
	static const u32 iommus[6] = { 20, 0, 21, 0, 22, 0 };

	ntree = 0;
	nev = 0;
	nmsg = 0;
	sim_cs_apply_rc = 0;
	sim_drv_rc = 0;
	drv_registered = false;
	nheap = 0;
	sim_prop_misuse = sim_alloc_fail_n = sim_allocs = sim_update_fail_n = sim_updates = sim_distinct_props = 0;
	machine = "apple,t6040";
	n_pmgr = mk("/power-management@502280000", "apple,t6041-pmgr", true);
	n_pmgr_ane = mk("/power-management@50228c000", "apple,t6041-pmgr", true);
	n_fab4 = mk("/power-management@502280000/power-controller@128", "apple,t6041-pmgr-pwrstate", true);
	n_fab5 = mk("/power-management@502800000/power-controller@440", "apple,t6041-pmgr-pwrstate", true);
	for (int i = 0; i < 5; i++)
		n_ps[i] = mk(psp[i], "apple,t6041-pmgr-pwrstate", false);
	n_ps[0]->phandle = 12;
	n_ps[2]->phandle = 13;
	for (int i = 0; i < 3; i++) {
		n_dart[i] = mk(dp[i], "apple,t6041-dart", false);
		n_dart[i]->phandle = 20 + i;
	}
	n_ane = mk("/ane@484000000", "apple,t6041-ane", false);
	put_cells(n_ane, "reg", reg0, 4);
	prop_put(n_ane, "reg-names", "engine", 7);
	put_cells(n_ane, "power-domains", pd0, 1);
	put_cells(n_ane, "iommus", iommus, 6);
	prop_put(n_ane, "status", "disabled", 9);
}

/* ---- checks on the live tree ---- */
static u32 cell(struct device_node *np, const char *name, int i)
{
	struct property *p = of_find_property(np, name, NULL);
	u32 v;

	if (!p || p->length < 4 * (i + 1))
		return 0xdeadbeef;
	memcpy(&v, (u8 *)p->value + 4 * i, 4);
	return __builtin_bswap32(v);
}

static bool tree_untouched(void)
{
	bool ok = !n_ane->avail && !n_ps[0]->avail && !n_ps[1]->avail && !n_ps[2]->avail && !n_ps[3]->avail && !n_ps[4]->avail;
	struct property *p;

	ok = ok && cell(n_ane, "reg", 3) == 0x2000000 && cell(n_ane, "reg", 4) == 0xdeadbeef;
	p = of_find_property(n_ane, "reg-names", NULL);
	ok = ok && p && p->length == 7 && !of_find_property(n_ane, "apple,ane-type", NULL);
	ok = ok && cell(n_ane, "power-domains", 0) == 12 && cell(n_ane, "power-domains", 1) == 0xdeadbeef;
	ok = ok && of_find_property(n_ane, "iommus", NULL);
	return ok;
}

static void refusal(const char *what, const char *optin, int want)
{
	char name[160];
	int ret;

	nev = 0;
	nmsg = 0;
	ret = ane_h16_register(NULL, optin, true);
	snprintf(name, sizeof(name), "%s: refused with %d, no changeset, no driver, tree untouched", what, want);
	chk(ret == want && ev_count("cs-init") == 0 && ev_count("apply") == 0 && !drv_registered && nmsg > 0 && tree_untouched(), name);
}

int main(void)
{
	static struct platform_driver drv = { "ane_h16" };
	int ret, a, b, c;

	/* success: every node enabled, the ANE node completed, the status written last, the driver registered after the apply */
	tree_reset();
	ret = ane_h16_register(&drv, "t6040", true);
	chk(ret == 0 && drv_registered, "S1 apply then register succeeds");
	chk(n_ane->avail && n_ps[0]->avail && n_ps[1]->avail && n_ps[2]->avail && n_ps[3]->avail && n_ps[4]->avail,
	    "S2 the five power controllers and the ANE node are enabled");
	chk(cell(n_ane, "reg", 0) == 4 && cell(n_ane, "reg", 1) == 0x84000000 && cell(n_ane, "reg", 3) == 0x2000000 &&
	    cell(n_ane, "reg", 4) == 5 && cell(n_ane, "reg", 5) == 0x02280000 && cell(n_ane, "reg", 7) == 0x18000,
	    "S3 reg lists the engine window and the pmgr window 0x502280000 + 0x18000");
	{
		struct property *p = of_find_property(n_ane, "reg-names", NULL);

		chk(p && p->length == 12 && !memcmp(p->value, "engine\0pmgr\0", 12), "S4 reg-names is engine, pmgr");
	}
	chk(cell(n_ane, "apple,ane-type", 0) == 0x110, "S5 apple,ane-type is 0x110");
	chk(cell(n_ane, "power-domains", 0) == n_ps[0]->phandle && cell(n_ane, "power-domains", 1) == n_ps[1]->phandle &&
	    cell(n_ane, "power-domains", 2) == n_ps[2]->phandle && cell(n_ane, "power-domains", 3) == n_ps[3]->phandle &&
	    cell(n_ane, "power-domains", 4) == n_ps[4]->phandle && cell(n_ane, "power-domains", 5) == 0xdeadbeef,
	    "S6 power-domains lists sys, mpm, cpu, td, base in that order");
	chk(n_ps[0]->phandle == 12 && n_ps[2]->phandle == 13 && n_ps[1]->phandle > 22 && n_ps[3]->phandle > 22 &&
	    n_ps[4]->phandle > 22 && n_ps[1]->phandle != n_ps[3]->phandle && n_ps[3]->phandle != n_ps[4]->phandle &&
	    n_ps[1]->phandle != n_ps[4]->phandle,
	    "S7 existing phandles are kept; the phandle-less ones get fresh, distinct values above the largest in the tree");
	chk(!of_find_property(n_ane, "iommus", NULL), "S8 iommus is removed while its DART providers are disabled");
	chk(!n_dart[0]->avail && !n_dart[1]->avail && !n_dart[2]->avail, "S9 the DART nodes are not touched");
	a = ev_index("update /ane@484000000 status");
	b = ev_index("update /power-management@502280000/power-controller@3c0 status");
	c = ev_index("apply");
	chk(a > b && a > ev_index("update /ane@484000000 power-domains") && a > ev_index("update /ane@484000000 reg") &&
	    a == c - 1 && b >= 0, "S10 status on the ANE node is the last change; the power controllers come before the ANE node");
	chk(ev_index("register-driver") > c && ev_count("cs-init") == 1, "S11 the driver registers after the apply, from one changeset");
	chk(sim_prop_misuse == 0 && sim_distinct_props == 9 && heap_unheld_live() == 0,
	    "S12 each of the nine updated properties is its own dynamic heap struct with heap name and value, none shared or reused");

	/* unload: driver first, then the changeset is reverted; the tree is what it was */
	nev = 0;
	ane_h16_unregister(&drv);
	chk(ev_index("unregister-driver") >= 0 && ev_index("unregister-driver") < ev_index("revert") && ev_count("cs-destroy") == 1,
	    "U1 unload unregisters the driver, then reverts and destroys the changeset");
	chk(tree_untouched(), "U2 after unload every enabled node, property and reg is back as it was");
	nev = 0;
	ane_h16_unregister(&drv);
	chk(ev_count("revert") == 0, "U3 a second unload reverts nothing");
	chk(sim_prop_misuse == 0, "U4 unload frees nothing the kernel holds");

	/* guards: each refuses before any change */
	tree_reset();
	refusal("wrong optin t6041", "t6041", -EINVAL);
	refusal("optin unset", NULL, -EINVAL);
	machine = "apple,t8132";
	refusal("wrong machine", "t6040", -ENODEV);
	tree_reset();
	n_ane->compat = "apple,t6020-ane";
	refusal("ANE node with the wrong compatible", "t6040", -ENODEV);
	tree_reset();
	n_ane->path = "/ane@999";
	refusal("ANE node missing", "t6040", -ENOENT);
	tree_reset();
	n_ps[3]->path = "/nowhere";
	refusal("a power controller node missing", "t6040", -ENOENT);
	tree_reset();
	n_pmgr_ane->avail = false;
	refusal("the pmgr node of the ANE controllers disabled", "t6040", -ENODEV);
	tree_reset();
	n_fab5->avail = false;
	refusal("a fabric parent power controller disabled", "t6040", -ENODEV);
	chk(msg_has("power-controller@440"), "G1 the refusal names the parent that is not available");

	/* apply_overlay=0: only the driver is registered, the tree is not even read */
	tree_reset();
	ret = ane_h16_register(&drv, "t6040", false);
	chk(ret == 0 && drv_registered && ev_count("cs-init") == 0 && tree_untouched(), "N1 apply_overlay=0 registers the driver and changes nothing");
	ane_h16_unregister(&drv);

	/* already complete: nothing to apply */
	tree_reset();
	n_ane->avail = true;
	{
		const u32 t = 0x110;

		put_cells(n_ane, "apple,ane-type", &t, 1);
	}
	ret = ane_h16_register(&drv, "t6040", true);
	chk(ret == 0 && drv_registered && ev_count("cs-init") == 0 && msg_has("node already complete"),
	    "C1 an available ANE node that already has apple,ane-type applies nothing and still registers the driver");
	ane_h16_unregister(&drv);
	chk(ev_count("revert") == 0, "C2 unloading reverts nothing when nothing was applied");

	/* power controllers the loader already enabled are left alone */
	tree_reset();
	n_ps[0]->avail = true;
	n_ps[4]->avail = true;
	ret = ane_h16_register(&drv, "t6040", true);
	chk(ret == 0 && ev_index("update /power-management@502280000/power-controller@3c0 status") < 0 &&
	    ev_index("update /power-management@50228c000/power-controller@18 status") < 0 &&
	    ev_index("update /power-management@50228c000/power-controller@0 status") >= 0,
	    "P1 only the power controllers that are disabled get a status change");
	ane_h16_unregister(&drv);
	chk(n_ps[0]->avail && n_ps[4]->avail && !n_ps[1]->avail, "P2 unload restores only what was changed");

	/* iommus stays when the DART providers are available */
	tree_reset();
	n_dart[0]->avail = n_dart[1]->avail = n_dart[2]->avail = true;
	ret = ane_h16_register(&drv, "t6040", true);
	chk(ret == 0 && of_find_property(n_ane, "iommus", NULL) && ev_index("remove /ane@484000000 iommus") < 0,
	    "I1 iommus is kept when every DART it names is available");
	ane_h16_unregister(&drv);

	/* failures unwind completely */
	tree_reset();
	sim_cs_apply_rc = -EINVAL;
	ret = ane_h16_register(&drv, "t6040", true);
	chk(ret == -EINVAL && !drv_registered && ev_count("cs-destroy") == 1 && tree_untouched(),
	    "F1 a refused apply destroys the changeset, registers no driver and leaves the tree untouched");
	tree_reset();
	sim_drv_rc = -ENOMEM;
	ret = ane_h16_register(&drv, "t6040", true);
	chk(ret == -ENOMEM && !drv_registered && ev_index("revert") > ev_index("register-driver") && ev_count("cs-destroy") == 1 &&
	    tree_untouched(), "F2 a driver registration failure reverts and destroys the changeset");
	tree_reset();
	sim_update_fail_n = 3;
	ret = ane_h16_register(&drv, "t6040", true);
	chk(ret == -ENOMEM && !drv_registered && ev_count("cs-destroy") == 1 && heap_unheld_live() == 0 && sim_prop_misuse == 0 &&
	    tree_untouched(), "F3 a refused update frees the property it built, destroys the changeset and touches nothing");
	{
		int bad = 0, n;

		for (n = 1; n < 60 && !bad; n++) {
			tree_reset();
			sim_alloc_fail_n = n;
			ret = ane_h16_register(&drv, "t6040", true);
			if (!ret) {
				ane_h16_unregister(&drv);
				break;
			}
			if (ret != -ENOMEM || drv_registered || heap_unheld_live() || sim_prop_misuse || !tree_untouched() ||
			    ev_count("cs-destroy") != 1)
				bad = n;
		}
		chk(!bad && n > 20, "F4 an allocation failure at any point leaves no unowned block, frees nothing it gave away and changes nothing");
	}

	printf("OVERLAY-SIM: %s (%d failure%s)\n", fails ? "FAIL" : "all checks PASS", fails, fails == 1 ? "" : "s");
	return fails ? 1 : 0;
}
