// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* test_dt_lifecycle: the t8132 diagnostic overlay control's host-side lifecycle:
 * registration, the apply/remove state machine, ownership and error paths. Kernel
 * overlay semantics are mocked; the refusal contract (wrong key, existing node,
 * foreign machine, unknown stage) is the real code under test. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

typedef uint8_t u8;
typedef unsigned int u32;
#define __user
#define MODULE_STATE_LIVE 0
#define MODULE_STATE_GOING 1
struct module { int state, refs; } owner;
#define THIS_MODULE (&owner)
#define DEFINE_MUTEX(n) int n
#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
struct device_node { const char *full_name; struct device_node *parent; } fake_root, fragment, overlay, soc, live_root;
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
#define PTR_ERR(p) (-ENOMEM)
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
static int apply_rc, remove_rc, apply_id, remove_id, apply_calls, remove_calls, registrations, notifications;
static bool existing, machine_ok, soc_present;
static int node_refs;
static const struct file_operations *control;
static struct notifier_block *notifier;
static void __module_get(struct module *m) { ++m->refs; }
static void module_put(struct module *m) { assert(m->refs > 1); --m->refs; }
static bool of_machine_is_compatible(const char *s)
{
	return !strcmp(s, "apple,k114c") ? machine_ok : !strcmp(s, "apple,t6050");
}
static struct device_node *of_find_node_by_path(const char *s) {
    struct device_node *node = !strcmp(s, "/soc") ? (soc_present ? &soc : NULL) :
                               (existing && strstr(s, "ane@") ? &soc : NULL);
    if (node) ++node_refs;
    return node;
}
static struct device_node *of_find_compatible_node(void *a, void *b, const char *c) {
    (void)a; (void)b; (void)c;
    if (existing) { ++node_refs; return &soc; }
    return NULL;
}
static void of_node_put(struct device_node *p) { if (p) { assert(node_refs > 0); --node_refs; } }
/* the three ps pwrstate nodes the identity gate resolves before every apply
 * (wrong-phandle refusal is dt_pmp's coverage) */
static struct device_node ps_mpm, ps_tdbase, ps_cpu;
static bool ps_cpu_present = true;
static struct device_node *of_find_node_by_phandle(u32 ph) {
    if (!ph || !ps_cpu_present)
        return NULL;
    ++node_refs;
    if (ph == 0xa2) return &ps_mpm;
    if (ph == 0xa3) return &ps_tdbase;
    return &ps_cpu;
}
static bool of_device_is_compatible(const struct device_node *n, const char *s) {
    return (!strcmp(s, "apple,t6050-pmgr-pwrstate")) &&
           (n == &ps_mpm || n == &ps_tdbase || n == &ps_cpu);
}
static int of_property_read_string(struct device_node *p, const char *name, const char **out) { (void)p; (void)name; *out = "ane-t8132-diagnostic"; return 0; }
static int of_property_read_u32(struct device_node *p, const char *name, u32 *v) {
    if (!strcmp(name, "reg")) {
        if (p == &ps_mpm) { *v = 0x3c0; return 0; }
        if (p == &ps_tdbase) { *v = 0x3d0; return 0; }
        if (p == &ps_cpu) { *v = 0x3c8; return 0; }
    }
    *v = 0; return -EINVAL;
}
static int of_overlay_fdt_apply(const void *blob, u32 size, int *id, const struct device_node *base) {
    (void)base; assert(size > 40 && ((const u8 *)blob)[0] == 0xd0); ++apply_calls;
    int r = notifier->notifier_call(notifier, OF_OVERLAY_PRE_APPLY, &notification);
    if (r) { *id = 7; return r; }
    *id = apply_id; return apply_rc;
}
static int of_overlay_remove(int *id) { ++remove_calls; *id = remove_id; return remove_rc; }
static int of_overlay_notifier_register(struct notifier_block *n) { notifier = n; ++notifications; return 0; }
static int of_overlay_notifier_unregister(struct notifier_block *n) { assert(notifier == n); --notifications; return 0; }
static int platform_driver_register(struct platform_driver *d) { (void)d; ++registrations; return 0; }
static void platform_driver_unregister(struct platform_driver *d) { (void)d; --registrations; }
static struct dentry *debugfs_create_file(const char *n, int mode, void *parent, void *data, const struct file_operations *ops) {
    (void)parent; (void)data; assert(!strcmp(n, "ane_h18_dt") && mode == 0200); control = ops; return &entry;
}
static void debugfs_remove(struct dentry *p) { assert(p == &entry); control = NULL; }
static int copy_from_user(void *d, const void *s, size_t n) { memcpy(d, s, n); return 0; }
#define ANE_H18_DT_TEST
#include "../ane_h18_dt.c"

static ssize_t operation(const char *s) {
    loff_t pos = 0; assert(control && control->owner == THIS_MODULE);
    ++owner.refs;
    ssize_t r = control->write(NULL, s, strlen(s), &pos);
    assert(owner.refs >= 1); --owner.refs;
    return r;
}
static void reset(struct platform_driver *d) {
    assert(!dt_pinned && !node_refs); memset(&owner, 0, sizeof(owner));
    apply_rc = remove_rc = 0; apply_id = 7; remove_id = 0;
    apply_calls = remove_calls = registrations = notifications = 0;
    existing = false; machine_ok = soc_present = true;
    fake_root.parent = NULL; fragment.parent = &fake_root; overlay.parent = &fragment;
    live_root.full_name = ""; live_root.parent = NULL;
    soc.full_name = "soc"; soc.parent = &live_root;
    notification.overlay = &overlay; notification.target = &soc;
    assert(ane_h18_dt_register(d, "t8132", "dt") == 0);
}
static int notifier_targets(struct platform_driver *d) {
    struct device_node other = { "gpu", &live_root };
    struct device_node bridge = { "bridge", &live_root };
    struct device_node nested = { "soc", &bridge };
    struct device_node lookalike = { "/soc", &bridge };
    struct { const char *name; struct device_node *target; int expected; } cases[] = {
        { "root-child leaf soc", &soc, 0 },
        { "other root child", &other, -EPERM },
        { "nested leaf soc", &nested, -EPERM },
        { "absolute-name lookalike", &lookalike, -EPERM },
    };
    int bad = 0;
    reset(d); dt_applying = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        notification.target = cases[i].target;
        int ret = ane_h18_dt_notify(&dt_notifier, OF_OVERLAY_PRE_APPLY, &notification);
        printf("%s: %s ret=%d expected=%d\n", ret == cases[i].expected ? "PASS" : "FAIL",
               cases[i].name, ret, cases[i].expected);
        bad |= ret != cases[i].expected;
        assert(!node_refs);
    }
    notification.target = &soc; soc_present = false;
    assert(ane_h18_dt_notify(&dt_notifier, OF_OVERLAY_PRE_APPLY, &notification) == -EPERM);
    assert(!node_refs);
    soc_present = true; dt_applying = false;
    ane_h18_dt_unregister(d, "t8132", "dt");
    return bad;
}
int main(void) {
    struct platform_driver d = {0};
    if (notifier_targets(&d)) return 1;
    reset(&d); assert(owner.refs == 0 && apply_calls == 0);
    assert(operation("psd") == -EINVAL && !strcmp(dt_stage, "dt") && !apply_calls);
    assert(operation("apply\n") == 6 && dt_id == 7 && owner.refs == 1);
    assert(operation("apply") == -EALREADY && apply_calls == 1);
    remove_rc = -EBUSY; remove_id = 7;
    assert(operation("remove") == -EBUSY && dt_id == 7 && owner.refs == 1);
    assert(operation("apply") == -EALREADY && apply_calls == 1);
    remove_rc = 0; remove_id = 0;
    assert(operation("remove\n") == 7 && !dt_pinned && !owner.refs);
    assert(operation("remove") == -ENOENT);
    assert(operation("bogus") == -EINVAL);
    assert(operation("") == -EINVAL);
    assert(operation("\n") == -EINVAL);
    assert(operation("apply\n\n") == -EINVAL);
    assert(operation("applyjunk") == -EINVAL);
    ane_h18_dt_unregister(&d, "t8132", "dt"); assert(!registrations && !notifications);
    reset(&d); apply_rc = -EIO;
    assert(operation("apply") == -EIO && remove_calls == 1 && !dt_pinned && !owner.refs);
    ane_h18_dt_unregister(&d, "t8132", "dt");
    reset(&d); apply_rc = -EIO; remove_rc = -EBUSY; remove_id = 7;
    assert(operation("apply") == -EIO && dt_pinned && dt_id == 7 && owner.refs == 1);
    remove_rc = 0; remove_id = 0; assert(operation("remove") == 6 && !owner.refs);
    ane_h18_dt_unregister(&d, "t8132", "dt");
    reset(&d); assert(operation("apply") == 5);
    remove_rc = -EIO; remove_id = 0;
    assert(operation("remove") == -EIO && dt_pinned && !dt_id && owner.refs == 1);
    assert(operation("apply") == -EALREADY);
    remove_rc = 0; assert(operation("remove") == 6 && !owner.refs);
    ane_h18_dt_unregister(&d, "t8132", "dt");
    reset(&d); assert(operation("apply") == 5);
    remove_id = 7;
    assert(operation("remove") == -EIO && dt_pinned && dt_id == 7 && owner.refs == 1);
    remove_id = 0; assert(operation("remove") == 6 && !owner.refs);
    ane_h18_dt_unregister(&d, "t8132", "dt");
    reset(&d); apply_rc = -ENOMEM; apply_id = 0;
    assert(operation("apply") == -ENOMEM && !dt_pinned && !owner.refs && !remove_calls);
    ane_h18_dt_unregister(&d, "t8132", "dt");
    reset(&d); existing = true;
    assert(operation("apply") == -EEXIST && !dt_pinned && !owner.refs);
    existing = false; owner.state = MODULE_STATE_GOING;
    assert(operation("apply") == -ENODEV && apply_calls == 1);
    owner.state = MODULE_STATE_LIVE; machine_ok = false;
    assert(operation("apply") == -ENODEV && apply_calls == 1);
    ane_h18_dt_unregister(&d, "t8132", "dt");
    machine_ok = true;
    /* no opt-in: plain registration, no control */
    assert(ane_h18_dt_register(&d, NULL, "dt") == 0 && !control && !notifications);
    ane_h18_dt_unregister(&d, NULL, "dt");
    /* the t8152 opt-in is not this module's opt-in: plain registration, no control */
    assert(ane_h18_dt_register(&d, "t8152", "dt") == 0 && !control && !notifications);
    ane_h18_dt_unregister(&d, "t8152", "dt");
    /* the incomplete t8132 row admits dt and reads only: psd/power/boot register plainly */
    assert(ane_h18_dt_register(&d, "t8132", "psd") == 0 && !control && !notifications);
    ane_h18_dt_unregister(&d, "t8132", "psd");
    assert(ane_h18_dt_register(&d, "t8132", "power") == 0 && !control && !notifications);
    ane_h18_dt_unregister(&d, "t8132", "power");
    assert(ane_h18_dt_register(&d, "t8132", "boot") == 0 && !control && !notifications);
    ane_h18_dt_unregister(&d, "t8132", "boot");
    /* a live ane node refuses the diagnostic registration outright */
    existing = true;
    assert(ane_h18_dt_register(&d, "t8132", "dt") == -EEXIST && !control && !notifications);
    existing = false;
    /* the reads stage arms the same control: the overlay carries the power providers */
    assert(ane_h18_dt_register(&d, "t8132", "reads") == 0 && control && notifications == 1);
    assert(!dt_pinned && !owner.refs);
    assert(operation("apply") == 5 && dt_pinned && owner.refs == 1);
    assert(operation("remove") == 6 && !dt_pinned && !owner.refs);
    ane_h18_dt_unregister(&d, "t8132", "reads");
    puts("PASS: t8132 diagnostic lifecycle; ownership and error model; not kernel lifecycle or hardware proof");
    return 0;
}
