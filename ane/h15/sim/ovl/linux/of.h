/* Fake <linux/of.h> for sim/sim_overlay.c: a flat fake live tree with the real kernel signatures. */
#ifndef SIM_OVL_OF_H
#define SIM_OVL_OF_H
#include <linux/types.h>

struct device_node {
	const char *path;	/* full path */
	const char *compat;	/* one compatible, or NULL */
	phandle phandle;
	int refs;
	bool attached;		/* part of the live tree */
	char keys[16][32];	/* string properties (only /__symbols__ gets any) */
	char vals[16][96];
	int nprops;
};

struct of_changeset {
	int n;
	struct {
		struct device_node *np;
		bool create;		/* true: attach np; false: add the property below */
		char key[32];
		char val[96];
	} ent[32];
	bool applied;
};

struct device_node *of_find_node_by_path(const char *path);
struct device_node *of_find_compatible_node(struct device_node *from, const char *type, const char *compat);
struct device_node *of_find_all_nodes(struct device_node *prev);
bool of_machine_is_compatible(const char *compat);
void of_node_put(struct device_node *np);
int of_property_read_string(const struct device_node *np, const char *name, const char **out);

void of_changeset_init(struct of_changeset *ocs);
void of_changeset_destroy(struct of_changeset *ocs);
struct device_node *of_changeset_create_node(struct of_changeset *ocs, struct device_node *parent, const char *full_name);
int of_changeset_add_prop_string(struct of_changeset *ocs, struct device_node *np, const char *prop_name, const char *str);
int of_changeset_apply(struct of_changeset *ocs);
int of_changeset_revert(struct of_changeset *ocs);

int of_overlay_fdt_apply(const void *overlay_fdt, u32 overlay_fdt_size, int *ret_ovcs_id, const struct device_node *target_base);
int of_overlay_remove(int *ovcs_id);
#endif
