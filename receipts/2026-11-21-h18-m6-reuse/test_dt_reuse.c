// SPDX-License-Identifier: GPL-2.0-only OR MIT
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
#define smp_load_acquire(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define smp_store_release(p, value) __atomic_store_n((p), (value), __ATOMIC_RELEASE)
static int apply_rc, remove_rc, apply_id, remove_id, apply_calls, remove_calls, registrations, notifications;
static bool existing, board, chip, soc_present;
static const char *key = "ane-t8152-boot";
static struct device_node aic;
static int node_refs;
static const struct file_operations *control;
static struct notifier_block *notifier;
static void __module_get(struct module *m) { ++m->refs; }
static void module_put(struct module *m) { assert(m->refs > 1); --m->refs; }
static bool of_machine_is_compatible(const char *s) { return !strcmp(s, "apple,j873g") ? board : chip; }
static struct device_node *of_find_node_by_path(const char *s) {
    struct device_node *node = !strcmp(s, "/soc") ? (soc_present ? &soc : NULL) :
                               !strcmp(s, "/soc/interrupt-controller@301000000") ? &aic :
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
static int of_property_read_string(struct device_node *p, const char *name, const char **out) { (void)p; (void)name; *out = key; return 0; }
static bool of_device_is_compatible(const struct device_node *n, const char *s) { (void)n; (void)s; return false; }
static struct device_node *of_find_node_by_phandle(u32 ph) { (void)ph; return NULL; }
bool ane_h18_boot_overlay_owned;
static int of_property_read_u32(struct device_node *p, const char *name, u32 *v) { if (p != &aic) return -EINVAL; *v = !strcmp(name, "phandle") ? 1 : 3; return 0; }
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
static void load(struct platform_driver *d) {
    memset(&owner, 0, sizeof(owner)); dt_pinned = false; dt_id = 0; control = NULL;
    board = chip = soc_present = true;
    fake_root.parent = NULL; fragment.parent = &fake_root; overlay.parent = &fragment;
    live_root.full_name = ""; live_root.parent = NULL;
    soc.full_name = "soc"; soc.parent = &live_root;
    notification.overlay = &overlay; notification.target = &soc;
    assert(ane_h18_dt_register(d, "t8152", "boot") == 0);
}
static int bad;
static void chk(int ok, const char *what) { printf("%s: %s\n", ok ? "PASS" : "FAIL", what); bad |= !ok; }
int main(void) {
    struct platform_driver d = { 0 };
    apply_id = 7;
    /* first load: no ANE node yet, the boot overlay is applied through the control */
    load(&d);
    chk(control != NULL && registrations == 1, "RU1 first load with no ANE node creates the overlay control");
    { ssize_t ar = operation("apply"); printf("apply ret=%zd calls=%d id=%d\n", ar, apply_calls, dt_id); chk(ar == (ssize_t)strlen("apply") && apply_calls == 1 && dt_id == 7, "RU2 apply adds the boot overlay"); }
    chk(owner.refs == 0, "RU3 the applied overlay takes no module reference, so rmmod is possible");
    chk(operation("remove") == -EPERM && remove_calls == 0 && dt_id == 7, "RU4 overlay remove is refused: the PSD nodes are never removed (their driver has no .remove)");
    ane_h18_dt_unregister(&d, "t8152", "boot");
    chk(registrations == 0 && notifications == 0 && control == NULL && owner.refs == 0, "RU5 unload leaves the overlay applied and holds nothing");
    /* second load: the overlay nodes are still live */
    existing = true;
    load(&d);
    chk(registrations == 1 && control == NULL && apply_calls == 1 && remove_calls == 0 && !node_refs,
        "RU6 the next load binds the existing ANE node: no control, no second apply");
    ane_h18_dt_unregister(&d, "t8152", "boot");
    chk(registrations == 0 && owner.refs == 0, "RU7 the second unload holds nothing either");
    printf(bad ? "SIM-DT-REUSE: FAILED\n" : "SIM-DT-REUSE: all PASS\n");
    return bad;
}
