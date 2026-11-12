// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* test_dt_pmp: the overlay phandle identity gate in ane_h18_dt.c. The overlay phandles are
 * raw numbers from one base-dtb build and a rebuild can shift them, and the driver core powers
 * a provider-carrying node at bind time, BEFORE probe can check anything - so the identity
 * of every power-domains claim (pmgr pwrstate, ANE register offset) must be verified while
 * the change is still bytes, before of_overlay_fdt_apply. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>

typedef uint8_t u8;
typedef unsigned int u32;
#define __user
#define pr_err(...) printf("ERR: " __VA_ARGS__)
#define pr_info(...) do { } while (0)
#define pr_crit(...) printf("CRIT: " __VA_ARGS__)
#define MODULE_STATE_LIVE 0
#define MODULE_STATE_GOING 1
struct module { int state, refs; } owner;
#define THIS_MODULE (&owner)
#define DEFINE_MUTEX(n) int n
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
struct device_node { const char *compat; u32 reg_val; int refs; const char *full_name; struct device_node *parent; } fake_root;
struct of_overlay_notify_data { struct device_node *overlay, *target; } notification;
struct notifier_block { int (*notifier_call)(struct notifier_block *, unsigned long, void *); };
struct platform_driver { int unused; };
struct dentry { int unused; } entry;
struct file { int unused; };
struct file_operations { struct module *owner; ssize_t (*write)(struct file *, const char *, size_t, loff_t *); };
#define OF_OVERLAY_PRE_APPLY 1
#define NOTIFY_DONE 0
#define notifier_from_errno(r) (r)
#define IS_ERR_OR_NULL(p) (!(p))
#define PTR_ERR(p) (-1)
#define ENOMEM_UNUSED 0
static int apply_calls, apply_id, registrations;
static bool board, chip;

static const struct file_operations *control;
static struct notifier_block *notifier;
static void __module_get(struct module *m) { ++m->refs; }
static void module_put(struct module *m) { assert(m->refs > 1); --m->refs; }
static bool of_machine_is_compatible(const char *s)
{
	if (!strcmp(s, "apple,j873g"))
		return board;
	if (!strcmp(s, "apple,k114c"))
		return true;
	return chip;	/* apple,t8152 / apple,t6050 both true */
}
static struct device_node live_cpu, live_mpm, live_tdbase;
static struct device_node *of_find_node_by_path(const char *s) { (void)s; return NULL; }
static struct device_node *of_find_compatible_node(void *a, void *b, const char *c) { (void)a; (void)b; (void)c; return NULL; }
static void of_node_put(struct device_node *p) { if (p && p->refs > 0) p->refs--; }
static int of_property_read_string(struct device_node *p, const char *name, const char **out)
{ (void)p; (void)name; *out = "ane-t8132-diagnostic"; return 0; }
static int of_property_read_u32(struct device_node *p, const char *name, u32 *v)
{
	if (p && !strcmp(name, "reg")) {
		*v = p->reg_val;
		return 0;
	}
	return -EINVAL;
}

/* ---- fake libfdt over the embedded blobs ---- */
typedef unsigned int fdt32_t;
#define fdt32_to_cpu(x) (x)
static u32 live_cpu_phandle, live_mpm_phandle, live_tdbase_phandle;
static struct device_node *of_find_node_by_phandle(u32 ph)
{
	if (ph && ph == live_cpu_phandle)
		return &live_cpu;
	if (ph && ph == live_mpm_phandle)
		return &live_mpm;
	if (ph && ph == live_tdbase_phandle)
		return &live_tdbase;
	return NULL;
}
static bool of_device_is_compatible(const struct device_node *n, const char *s)
{
	return n->compat && !strcmp(n->compat, s);
}

/* ---- fake overlay apply ---- */
static int of_overlay_fdt_apply(const void *blob, u32 size, int *id, const struct device_node *base)
{
	(void)blob; (void)size; (void)base;
	*id = ++apply_calls;
	apply_id = *id;
	return 0;
}
static int of_overlay_remove(int *id) { (void)id; apply_calls--; return 0; }
static int of_overlay_notifier_register(struct notifier_block *n) { notifier = n; return 0; }
static int of_overlay_notifier_unregister(struct notifier_block *n) { assert(notifier == n); notifier = NULL; return 0; }
static int platform_driver_register(struct platform_driver *d) { (void)d; ++registrations; return 0; }
static void platform_driver_unregister(struct platform_driver *d) { (void)d; --registrations; }
static struct dentry *debugfs_create_file(const char *n, int mode, void *parent, void *data, const struct file_operations *ops)
{ (void)n; (void)mode; (void)parent; (void)data; control = ops; return &entry; }
static void debugfs_remove(struct dentry *p) { assert(p == &entry); control = NULL; }
static int copy_from_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }

/* the dt stage params, driven directly by the tests (the same file-local statics dt.c uses) */
static const char *dt_optin;
static const char *dt_stage;

#define ANE_H18_DT_TEST
#include "../ane_h18_dt.c"

static int fails;

static void chk(int ok, const char *name)
{
	printf("  %-70s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok)
		fails++;
}

static int apply_with(const char *optin, const char *stage)
{
	dt_optin = optin;
	dt_stage = stage;
	apply_calls = 0;
	owner.state = MODULE_STATE_LIVE;
	owner.refs = 2;
	dt_pinned = false;
	dt_id = 0;
	dt_applying = false;
	return ane_h18_dt_command("apply");
}

int main(void)
{
	int ret;

	board = chip = true;	/* board: the j873g machine compat the t8152 flow would need */
	live_cpu_phandle = 0xa4;
	live_cpu.compat = "apple,t6050-pmgr-pwrstate";
	live_cpu.reg_val = 0x3c8;
	live_cpu.refs = 4;	/* the pre-apply check takes and puts one reference per apply */
	live_mpm_phandle = 0xa2;
	live_mpm.compat = "apple,t6050-pmgr-pwrstate";
	live_mpm.reg_val = 0x3c0;
	live_mpm.refs = 4;
	live_tdbase_phandle = 0xa3;
	live_tdbase.compat = "apple,t6050-pmgr-pwrstate";
	live_tdbase.reg_val = 0x3d0;
	live_tdbase.refs = 4;

	/* T1: the t8132 blob, the live tree resolves all three claims to the right ANE domains */
	ret = apply_with("t8132", "dt");
	chk(ret == 0 && apply_calls == 1, "T1 good phandle targets: apply proceeds");

	/* T2: shifted base dtb - the ps_ane_cpu phandle resolves to a foreign node: refused pre-apply */
	live_cpu.compat = "apple,t6050-pmgr-OTHER";
	ret = apply_with("t8132", "dt");
	chk(ret == -EINVAL && apply_calls == 0, "T2 foreign target: refused before apply");

	/* T3: the final dtb predates the ps_ane_cpu pin - dangling phandle: refused pre-apply */
	live_cpu_phandle = 0;
	live_cpu.compat = "apple,t6050-pmgr-pwrstate";
	ret = apply_with("t8132", "dt");
	chk(ret == -EINVAL && apply_calls == 0, "T3 dangling phandle: refused before apply");

	/* T4: a pwrstate node of the wrong domain (reg is another pmgr offset): refused pre-apply */
	live_cpu_phandle = 0xa4;
	live_cpu.reg_val = 0x3a8;
	ret = apply_with("t8132", "dt");
	chk(ret == -EINVAL && apply_calls == 0, "T4 wrong pmgr offset: refused before apply");
	live_cpu.reg_val = 0x3c8;

	/* T5: the t8103 fallback compatible also passes the identity */
	live_cpu.compat = "apple,t8103-pmgr-pwrstate";
	ret = apply_with("t8132", "dt");
	chk(ret == 0 && apply_calls == 1, "T5 t8103 fallback compatible accepted");

	/* T7: the stage gate - the t8132 opt-in admits dt and reads only */
	dt_optin = "t8132";
	chk(ane_h18_dt_stage_allowed("dt"), "T7 t8132 allows stage=dt");
	chk(ane_h18_dt_stage_allowed("reads"), "T7 t8132 allows stage=reads");
	chk(!ane_h18_dt_stage_allowed("boot"), "T7 t8132 refuses stage=boot");
	chk(!ane_h18_dt_stage_allowed("psd"), "T7 t8132 refuses stage=psd");
	chk(!ane_h18_dt_stage_allowed("power"), "T7 t8132 refuses stage=power");

	/* T9: a claimed ps phandle dangling in the live tree - refused pre-apply */
	live_mpm_phandle = 0;
	ret = apply_with("t8132", "dt");
	chk(ret == -EINVAL && apply_calls == 0, "T9 dangling ps_ane_mpm claim: refused before apply");
	live_mpm_phandle = 0xa2;

	/* T10: a claimed ps phandle resolving to a foreign node - refused pre-apply */
	live_tdbase.compat = "pci-bridge-node";
	ret = apply_with("t8132", "dt");
	chk(ret == -EINVAL && apply_calls == 0, "T10 foreign ps_ane_tdbase claim: refused before apply");
	live_tdbase.compat = "apple,t6050-pmgr-pwrstate";

	/* T8: the embedded t8132 blob's power-domains cells equal the pinned phandles the
	 * kernel resolves at apply - the two halves of the identity gate must agree */
	{
		const u8 *b = ane_h18_dt_t8132_blob;
		size_t n = sizeof(ane_h18_dt_t8132_blob), i;
		u32 struct_off, strings_off;
		u32 cells[3];
		int ncells = 0, found = 0;

		/* FDT header (big-endian): magic, totalsize, off_dt_struct, off_dt_strings */
		struct_off = ((u32)b[8] << 24) | ((u32)b[9] << 16) | ((u32)b[10] << 8) | b[11];
		strings_off = ((u32)b[12] << 24) | ((u32)b[13] << 16) | ((u32)b[14] << 8) | b[15];
		for (i = struct_off; i + 12 < n; i++) {
			u32 tok = ((u32)b[i] << 24) | ((u32)b[i+1] << 16) | ((u32)b[i+2] << 8) | b[i+3];
			u32 len, nameoff;

			if (tok != 3)	/* FDT_PROP */
				continue;
			len = ((u32)b[i+4] << 24) | ((u32)b[i+5] << 16) | ((u32)b[i+6] << 8) | b[i+7];
			nameoff = ((u32)b[i+8] << 24) | ((u32)b[i+9] << 16) | ((u32)b[i+10] << 8) | b[i+11];
			if (len % 4 == 0 && len > 0 && len <= 16 &&
			    !strcmp((const char *)b + strings_off + nameoff, "power-domains")) {
				u32 c;

				for (c = 0; c < len / 4 && ncells < 3; c++)
					cells[ncells++] = ((u32)b[i+12+c*4] << 24) | ((u32)b[i+13+c*4] << 16) |
							  ((u32)b[i+14+c*4] << 8) | b[i+15+c*4];
				found = 1;
				break;
			}
		}
		chk(found, "T8 t8132 blob carries a power-domains cell");
		chk(ncells == 3, "T8 blob claims exactly three power domains");
		chk(cells[0] == ANE_H18_DT_T8132_PS_MPM_PHANDLE, "T8 blob cell 0 equals the pinned ps_ane_mpm phandle the apply resolves");
		chk(cells[1] == ANE_H18_DT_T8132_PS_TDBASE_PHANDLE, "T8 blob cell 1 equals the pinned ps_ane_tdbase phandle the apply resolves");
		chk(cells[2] == ANE_H18_DT_T8132_PS_CPU_PHANDLE, "T8 blob cell 2 equals the pinned ps_ane_cpu phandle the apply resolves");
	}

	printf("%s\n", fails ? "test_dt_pmp: FAILED" : "test_dt_pmp: all checks PASS");
	return fails ? 1 : 0;
}
