/* Fake <linux/of.h> for sim/sim_overlay.c: a flat fake live tree with the real kernel signatures. A changeset records what it is told and
 * applies and reverts it in order. */
#ifndef SIM_OVL_OF_H
#define SIM_OVL_OF_H
#include <linux/types.h>

struct property {
	char *name;
	int length;
	void *value;
};

#define SIM_MAX_PROPS 12
#define SIM_PROP_BYTES 48

struct sim_prop {
	char name[24];
	int length;
	unsigned char value[SIM_PROP_BYTES];
	bool present;
	struct property pub;	/* what of_find_property hands out */
};

struct device_node {
	const char *path;
	const char *compat;
	bool avail;
	phandle phandle;
	int refs;
	struct sim_prop props[SIM_MAX_PROPS];
};

enum sim_cs_kind { CS_UPDATE, CS_ADD, CS_REMOVE };

struct of_changeset {
	int n;
	struct {
		enum sim_cs_kind kind;
		struct device_node *np;
		struct sim_prop before, after;
	} ent[40];
	bool applied;
};

struct device_node *of_find_node_by_path(const char *path);
struct device_node *of_find_all_nodes(struct device_node *prev);
bool of_machine_is_compatible(const char *compat);
bool of_device_is_available(const struct device_node *np);
int of_device_is_compatible(const struct device_node *np, const char *compat);
struct property *of_find_property(const struct device_node *np, const char *name, int *lenp);
void of_node_put(struct device_node *np);

void of_changeset_init(struct of_changeset *ocs);
void of_changeset_destroy(struct of_changeset *ocs);
int of_changeset_apply(struct of_changeset *ocs);
int of_changeset_revert(struct of_changeset *ocs);
int of_changeset_update_property(struct of_changeset *ocs, struct device_node *np, const struct property *prop);
int of_changeset_add_prop_u32(struct of_changeset *ocs, struct device_node *np, const char *name, u32 val);
int of_changeset_remove_property(struct of_changeset *ocs, struct device_node *np, struct property *prop);
#endif
