// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sim_overlay.c: drives the REAL ane_h15_overlay.c against a fake live device tree (the headers under sim/ovl/linux). Every call the code makes
 * into the tree, the changeset, the overlay API and the driver core is recorded in order, so the tests assert both what it does
 * and that it does nothing at all when a guard refuses. Exit 0 iff every check passes.
 *   make sim-overlay
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/types.h>

#include "../ane_h15_overlay.c"

static int fails;
static char ev[160][120];
static int nev;
static char msgs[80][200];
static int nmsg;
static struct device_node tree[24];
static int ntree;
static const char *machine;
static int sim_cs_apply_rc, sim_cs_prop_rc, sim_ovl_rc, sim_drv_rc;
static bool sim_ovl_partial, sim_ovl_active;
static const void *ovl_blob;
static u32 ovl_size;

static void ev_add(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (nev < 160)
		vsnprintf(ev[nev++], sizeof(ev[0]), fmt, ap);
	va_end(ap);
}

void sim_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (nmsg < 80)
		vsnprintf(msgs[nmsg++], sizeof(msgs[0]), fmt, ap);
	va_end(ap);
}

static void chk(int ok, const char *name)
{
	printf("  %-100s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok)
		fails++;
}

/* ---- the fake kernel ---- */
struct device_node *of_find_node_by_path(const char *path)
{
	for (int i = 0; i < ntree; i++)
		if (tree[i].attached && !strcmp(tree[i].path, path)) {
			tree[i].refs++;
			return &tree[i];
		}
	return NULL;
}

struct device_node *of_find_compatible_node(struct device_node *from, const char *type, const char *compat)
{
	(void)from;
	(void)type;
	for (int i = 0; i < ntree; i++)
		if (tree[i].attached && tree[i].compat && !strcmp(tree[i].compat, compat)) {
			tree[i].refs++;
			return &tree[i];
		}
	return NULL;
}

struct device_node *of_find_all_nodes(struct device_node *prev)
{
	int i = prev ? (int)(prev - tree) + 1 : 0;

	for (; i < ntree; i++)
		if (tree[i].attached)
			return &tree[i];
	return NULL;
}

bool of_machine_is_compatible(const char *compat)
{
	return machine && !strcmp(machine, compat);
}

void of_node_put(struct device_node *np)
{
	if (np)
		np->refs--;
}

int of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	for (int i = 0; np && i < np->nprops; i++)
		if (!strcmp(np->keys[i], name)) {
			*out = np->vals[i];
			return 0;
		}
	return -EINVAL;
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

struct device_node *of_changeset_create_node(struct of_changeset *ocs, struct device_node *parent, const char *full_name)
{
	static char path[24][40];
	struct device_node *np = &tree[ntree];

	(void)parent;
	snprintf(path[ntree], sizeof(path[0]), "/%s", full_name);
	np->path = path[ntree++];
	ocs->ent[ocs->n].np = np;
	ocs->ent[ocs->n++].create = true;
	ev_add("cs-create %s", np->path);
	return np;
}

int of_changeset_add_prop_string(struct of_changeset *ocs, struct device_node *np, const char *prop_name, const char *str)
{
	ev_add("cs-prop %s", prop_name);
	if (sim_cs_prop_rc)
		return sim_cs_prop_rc;
	ocs->ent[ocs->n].np = np;
	ocs->ent[ocs->n].create = false;
	snprintf(ocs->ent[ocs->n].key, sizeof(ocs->ent[0].key), "%s", prop_name);
	snprintf(ocs->ent[ocs->n++].val, sizeof(ocs->ent[0].val), "%s", str);
	return 0;
}

int of_changeset_apply(struct of_changeset *ocs)
{
	ev_add("cs-apply");
	if (sim_cs_apply_rc)
		return sim_cs_apply_rc;
	for (int i = 0; i < ocs->n; i++) {
		struct device_node *np = ocs->ent[i].np;

		if (ocs->ent[i].create) {
			np->attached = true;
		} else {
			snprintf(np->keys[np->nprops], sizeof(np->keys[0]), "%s", ocs->ent[i].key);
			snprintf(np->vals[np->nprops++], sizeof(np->vals[0]), "%s", ocs->ent[i].val);
		}
	}
	ocs->applied = true;
	return 0;
}

int of_changeset_revert(struct of_changeset *ocs)
{
	ev_add("cs-revert");
	for (int i = ocs->n - 1; i >= 0; i--) {
		struct device_node *np = ocs->ent[i].np;

		if (ocs->ent[i].create)
			np->attached = false;
		else
			np->nprops--;
	}
	return 0;
}

int of_overlay_fdt_apply(const void *blob, u32 size, int *id, const struct device_node *base)
{
	(void)base;
	ev_add("overlay-apply");
	ovl_blob = blob;
	ovl_size = size;
	*id = 0;
	if (!sim_ovl_rc || sim_ovl_partial) {
		*id = 7;
		sim_ovl_active = true;
	}
	return sim_ovl_rc;
}

int of_overlay_remove(int *id)
{
	ev_add("overlay-remove %d", *id);
	sim_ovl_active = false;
	*id = 0;
	return 0;
}

int platform_driver_register(struct platform_driver *drv)
{
	(void)drv;
	ev_add("driver-register");
	return sim_drv_rc;
}

void platform_driver_unregister(struct platform_driver *drv)
{
	(void)drv;
	ev_add("driver-unregister");
}

/* ---- the harness ---- */
static struct platform_driver drv = { "ane_h15" };

static struct device_node *add_node(const char *path, const char *compat, phandle ph)
{
	struct device_node *np = &tree[ntree++];

	np->path = path;
	np->compat = compat;
	np->phandle = ph;
	np->attached = true;
	return np;
}

static void reset(void)
{
	memset(tree, 0, sizeof(tree));
	memset(ev, 0, sizeof(ev));
	memset(msgs, 0, sizeof(msgs));
	memset(&ane_ovl, 0, sizeof(ane_ovl));
	ntree = nev = nmsg = 0;
	machine = NULL;
	sim_cs_apply_rc = sim_cs_prop_rc = sim_ovl_rc = sim_drv_rc = 0;
	sim_ovl_partial = sim_ovl_active = false;
	ovl_blob = NULL;
	ovl_size = 0;
	add_node("/", NULL, 1);
}

static struct device_node *t6031_tree(void)
{
	machine = "apple,t6031";
	add_node("/soc/interrupt-controller@292400000", NULL, 7);
	add_node("/soc/power-management@292280000/power-controller@520", NULL, 9);	/* has a phandle */
	add_node("/soc/power-management@292280000/power-controller@5a0", NULL, 0);	/* none */
	add_node("/soc/power-management@292280000/power-controller@5a8", NULL, 0);
	add_node("/soc/power-management@292280000/power-controller@5b8", NULL, 11);
	return add_node("/soc/power-management@292280000/power-controller@5c0", NULL, 0);
}

static void t6030_tree(void)
{
	machine = "apple,t6030";
	add_node("/soc/interrupt-controller@351000000", NULL, 7);
	add_node("/soc/power-management@350700000/power-controller@498", NULL, 0);
}

static int count_prefix(const char *prefix)
{
	int n = 0;

	for (int i = 0; i < nev; i++)
		if (!strncmp(ev[i], prefix, strlen(prefix)))
			n++;
	return n;
}

static int idx(const char *prefix)
{
	for (int i = 0; i < nev; i++)
		if (!strncmp(ev[i], prefix, strlen(prefix)))
			return i;
	return -1;
}

static int writes(void)
{
	return count_prefix("cs-") + count_prefix("overlay");
}

static bool said(const char *part)
{
	for (int i = 0; i < nmsg; i++)
		if (strstr(msgs[i], part))
			return true;
	return false;
}

static void test_off_by_default(void)
{
	reset();
	t6031_tree();
	chk(ane_h15_register(&drv, "t6031", false) == 0, "O1 apply_overlay off: register succeeds");
	chk(nev == 1 && !strcmp(ev[0], "driver-register"), "O2 apply_overlay off: nothing but the driver registration happens");
	ane_h15_unregister(&drv);
	chk(nev == 2 && !strcmp(ev[1], "driver-unregister"), "O3 off: unload only unregisters the driver");
}

static void test_t6031_happy(void)
{
	struct device_node *base;

	reset();
	base = t6031_tree();
	chk(ane_h15_register(&drv, "t6031", true) == 0, "H1 t6031: register with apply_overlay returns 0");
	chk(idx("cs-init") < idx("cs-create /__symbols__") && idx("cs-create /__symbols__") < idx("cs-prop aic") &&
	    idx("cs-prop ps_ane_base") < idx("cs-apply") && idx("cs-apply") < idx("overlay-apply") && idx("overlay-apply") < idx("driver-register"),
	    "H2 order: symbols changeset, then the overlay, then the driver");
	chk(count_prefix("cs-prop") == 6, "H3 six labels are published (aic and the five ANE power controllers)");
	chk(ovl_blob == ane_h15_ovl_t6031_blob && ovl_size == sizeof(ane_h15_ovl_t6031_blob), "H4 the t6031 blob is the one applied");
	chk(((const u8 *)ovl_blob)[0] == 0xd0 && ((const u8 *)ovl_blob)[1] == 0x0d && ((const u8 *)ovl_blob)[2] == 0xfe && ((const u8 *)ovl_blob)[3] == 0xed,
	    "H5 the blob is a flattened device tree (magic d00dfeed)");
	chk(base->phandle != 0 && !strcmp(base->path, "/soc/power-management@292280000/power-controller@5c0"), "H6 a target without a phandle was given one");
	chk(tree[1].phandle == 7 && tree[2].phandle == 9 && tree[5].phandle == 11, "H7 targets that already had a phandle keep it");
	{
		phandle a = tree[3].phandle, b = tree[4].phandle, c = tree[6].phandle;

		chk(a && b && c && a != b && b != c && a != c && a > 11 && b > 11 && c > 11, "H8 the new phandles are distinct and above every existing one");
	}
	chk(said("applied apple,t6031-ane"), "H9 the log says what was applied");
	chk(ane_ovl.overlay && ane_ovl.symbols, "H10 the module remembers what to undo");
	ane_h15_unregister(&drv);
	chk(idx("driver-unregister") < idx("overlay-remove 7") && idx("overlay-remove 7") < idx("cs-revert") && idx("cs-revert") < idx("cs-destroy"),
	    "H11 unload order: driver, then the overlay, then the symbols changeset");
	chk(!ane_ovl.overlay && !ane_ovl.symbols, "H12 after unload nothing is left applied");
	for (int i = 0; i < ntree; i++)
		if (tree[i].refs != 0 && tree[i].attached && strcmp(tree[i].path, "/"))
			fails++, printf("  leaked reference on %s: %d\n", tree[i].path, tree[i].refs);
}

static void test_t6030_happy(void)
{
	reset();
	t6030_tree();
	chk(ane_h15_register(&drv, "t6030", true) == 0, "T1 t6030: register with apply_overlay returns 0");
	chk(count_prefix("cs-prop") == 2 && ovl_blob == ane_h15_ovl_t6030_blob, "T2 t6030: two labels and the t6030 blob");
	chk(tree[2].phandle != 0 && tree[2].phandle != 7, "T3 t6030: ps_ane_sys had no phandle and got one");
}

static void test_t8122(void)
{
	reset();
	machine = "apple,t8122";
	add_node("/soc/interrupt-controller@2d1000000", NULL, 7);
	add_node("/soc/power-management@2d0700000/power-controller@438", NULL, 0);
	chk(ane_h15_register(&drv, "t8122", true) == 0,
	    "B1 t8122: own-chip overlay registers");
	chk(ovl_blob && ovl_blob != ane_h15_ovl_t6030_blob && ovl_blob != ane_h15_ovl_t6031_blob,
	    "B2 t8122: neither Pro nor Max payload is applied");
	chk(idx("cs-apply") < idx("overlay-apply") && idx("overlay-apply") < idx("driver-register"),
	    "B3 t8122: labels and overlay precede driver registration");
	ane_h15_unregister(&drv);
	chk(idx("driver-unregister") < idx("overlay-remove 7") && idx("overlay-remove 7") < idx("cs-revert"),
	    "B4 t8122: removal reverses registration");
	reset();
	machine = "apple,t6031";
	chk(ane_h15_register(&drv, "t8122", true) == -ENODEV && !writes() && !count_prefix("driver"),
	    "B5 t8122: Max machine refuses before any write");
	reset();
	machine = "apple,t8122";
	add_node("/soc/interrupt-controller@2d1000000", NULL, 7);
	chk(ane_h15_register(&drv, "t8122", true) == -ENOENT && !writes() && !count_prefix("driver"),
	    "B6 t8122: missing own-chip power target refuses before any write");
}

static void test_guards(void)
{
	reset();
	t6031_tree();
	chk(ane_h15_register(&drv, NULL, true) == -EINVAL && writes() == 0 && count_prefix("driver") == 0 && said("optin=(unset)"),
	    "G1 optin unset: refused, nothing written, driver not registered");
	reset();
	t6031_tree();
	chk(ane_h15_register(&drv, "t6034", true) == -EINVAL && writes() == 0 && count_prefix("driver") == 0, "G2 optin for a row without an overlay: refused, nothing written");
	reset();
	t6031_tree();
	machine = "apple,t6030";
	chk(ane_h15_register(&drv, "t6031", true) == -ENODEV && writes() == 0 && count_prefix("driver") == 0 && said("not an apple,t6031"),
	    "G3 wrong machine for the optin key: refused, nothing written");
	reset();
	t6031_tree();
	add_node("/soc/ane@3c8000000", "apple,t6031-ane", 0);
	chk(ane_h15_register(&drv, "t6031", true) == 0 && writes() == 0 && !strcmp(ev[0], "driver-register") && said("already has apple,t6031-ane"),
	    "G4 the tree already has the ANE node: nothing applied, the driver still registers");
	reset();
	t6031_tree();
	tree[3].attached = false;
	chk(ane_h15_register(&drv, "t6031", true) == -ENOENT && writes() == 0 && count_prefix("driver") == 0 && said("power-controller@5a0"),
	    "G5 a label's node is missing: refused, names the path, nothing written");
	reset();
	t6031_tree();
	{
		struct device_node *sym = add_node("/__symbols__", NULL, 0);

		snprintf(sym->keys[0], sizeof(sym->keys[0]), "aic");
		snprintf(sym->vals[0], sizeof(sym->vals[0]), "/soc/somewhere-else");
		sym->nprops = 1;
	}
	chk(ane_h15_register(&drv, "t6031", true) == -EEXIST && writes() == 0 && said("already names"),
	    "G6 a label already names another node: refused, nothing written");
	reset();
	t6031_tree();
	{
		struct device_node *sym = add_node("/__symbols__", NULL, 0);

		snprintf(sym->keys[0], sizeof(sym->keys[0]), "aic");
		snprintf(sym->vals[0], sizeof(sym->vals[0]), "/soc/interrupt-controller@292400000");
		sym->nprops = 1;
	}
	chk(ane_h15_register(&drv, "t6031", true) == 0 && count_prefix("cs-create") == 0 && count_prefix("cs-prop") == 5,
	    "G7 an existing /__symbols__ is reused and a label that already matches is not added again");
}

static void test_failures(void)
{
	reset();
	t6031_tree();
	sim_cs_apply_rc = -EIO;
	chk(ane_h15_register(&drv, "t6031", true) == -EIO && count_prefix("overlay-apply") == 0 && count_prefix("driver") == 0 && count_prefix("cs-destroy") == 1,
	    "F1 the changeset fails: the overlay is never applied, the driver never registered, the changeset destroyed");
	chk(!ane_ovl.symbols && !ane_ovl.overlay, "F2 nothing is recorded as applied");
	reset();
	t6031_tree();
	sim_cs_prop_rc = -ENOMEM;
	chk(ane_h15_register(&drv, "t6031", true) == -ENOMEM && count_prefix("cs-apply") == 0 && count_prefix("overlay-apply") == 0, "F3 a property add fails: no apply, no overlay");
	reset();
	t6031_tree();
	sim_ovl_rc = -EINVAL;
	chk(ane_h15_register(&drv, "t6031", true) == -EINVAL && count_prefix("driver") == 0 && idx("cs-revert") > idx("overlay-apply") && count_prefix("cs-destroy") == 1,
	    "F4 the kernel refuses the overlay: the symbols changeset is reverted, the driver not registered");
	chk(!ane_ovl.symbols && !ane_ovl.overlay && said("kernel refused the overlay"), "F5 nothing is recorded as applied and the log says why");
	chk(count_prefix("overlay-remove") == 0, "F8 invalid overlay without an ID needs no removal");
	reset();
	t6031_tree();
	sim_ovl_rc = -EIO;
	sim_ovl_partial = true;
	chk(ane_h15_register(&drv, "t6031", true) == -EIO && !sim_ovl_active && count_prefix("driver") == 0,
	    "F9 a post-apply error leaves no overlay devices or registered driver");
	chk(idx("overlay-remove 7") > idx("overlay-apply") && idx("cs-revert") > idx("overlay-remove 7"),
	    "F10 a partially applied overlay is removed before its symbol labels");
	reset();
	t6031_tree();
	sim_drv_rc = -EBUSY;
	chk(ane_h15_register(&drv, "t6031", true) == -EBUSY && idx("overlay-remove 7") > idx("driver-register") && idx("cs-revert") > idx("overlay-remove 7"),
	    "F6 the driver registration fails after the overlay: the overlay and the symbols are removed again");
	reset();
	t6031_tree();
	sim_drv_rc = -EBUSY;
	chk(ane_h15_register(&drv, "t6031", false) == -EBUSY && count_prefix("overlay") == 0 && count_prefix("cs-") == 0,
	    "F7 off by default and the driver fails: nothing to undo, nothing touched");
}

int main(void)
{
	printf("sim-overlay (the real ane_h15_overlay.c against a fake live tree):\n");
	test_off_by_default();
	test_t6031_happy();
	test_t6030_happy();
	test_t8122();
	test_guards();
	test_failures();
	printf(fails ? "SIM-OVERLAY: %d FAILED\n" : "SIM-OVERLAY: all checks PASS\n", fails);
	return fails ? 1 : 0;
}
