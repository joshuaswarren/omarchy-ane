// SPDX-License-Identifier: MIT
/*
 * test_trace_debugfs_sim.c - userspace sim of the ane_t6021 trace_td
 * debugfs logic with a mocked debugfs:
 *
 *   ./test_trace_debugfs_sim new   must PASS: the trace blob lives in the
 *                                  trace's own root directory
 *                                  ("ane_t6021_trace"), at any load
 *                                  order relative to the probe
 *   ./test_trace_debugfs_sim old   must FAIL: the pre-fix code creates
 *                                  "ane_t6021" unconditionally and
 *                                  collides with the probe's stats
 *                                  directory in one order or the other
 *
 * Mock semantics mirror the kernel (fs/debugfs/inode.c):
 * - create_dir on a duplicate name returns the ERR pointer (-EEXIST) and
 *   logs; a child under an ERR parent is not created.
 * - remove_recursive(NULL or ERR) is a no-op (debugfs_remove_recursive
 *   checks IS_ERR_OR_NULL); it frees a real subtree exactly once, and a
 *   second touch of a freed dentry aborts (use-after-free).
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHILDREN 8

/* The one ERR_PTR-like error marker (lowercase: checkpatch reads an
 * all-caps sentinel as an errno).
 */
static struct node *const err_p = (struct node *)&err_p;

#define IS_ERR(p) ((struct node *)(p) == err_p)

struct node {
	const char *name;
	int is_dir;
	int freed;
	int free_events;
	struct node *parent;
	struct node *child[MAX_CHILDREN];
	int nchild;
};

static struct node root;
static struct node *probe_dir;
static struct node *trace_dir;
static struct node *last_removed;
static bool trace_buf;
static int dup_errors;
static int blob_creates;
static int checks, failures;

static void reset_world(void)
{
	memset(&root, 0, sizeof(root));
	root.name = "/";
	root.is_dir = 1;
	probe_dir = NULL;
	trace_dir = NULL;
	last_removed = NULL;
	trace_buf = false;
	dup_errors = 0;
	blob_creates = 0;
}

static struct node *child_of(struct node *dir, const char *name)
{
	int i;

	for (i = 0; i < dir->nchild; i++)
		if (!strcmp(dir->child[i]->name, name))
			return dir->child[i];
	return NULL;
}

static struct node *create_dir(const char *name, struct node *parent)
{
	struct node *n;

	if (IS_ERR(parent))
		return err_p;
	if (!parent)
		parent = &root;
	if (child_of(parent, name)) {
		dup_errors++;	/* the kernel's "already exists" log */
		return err_p;
	}
	assert(parent->nchild < MAX_CHILDREN);
	n = calloc(1, sizeof(*n));
	n->name = name;
	n->is_dir = 1;
	n->parent = parent;
	parent->child[parent->nchild++] = n;
	return n;
}

/* The parent is owned elsewhere; an ERR parent yields no file. */
static struct node *create_blob(const char *name, struct node *parent)
{
	if (IS_ERR(parent) || !parent)
		return err_p;
	/* Count the trace blob only: probe files use create_blob too. */
	if (!strcmp(name, "trace_td") && !child_of(parent, name))
		blob_creates++;
	return create_dir(name, parent);
}

static void remove_recursive(struct node *n)
{
	int i;

	if (IS_ERR(n) || !n)
		return;
	assert(!n->freed);
	/* The kernel unhashes the dentry: later lookups find nothing. */
	if (n->parent) {
		int j = 0;

		for (i = 0; i < n->parent->nchild; i++)
			if (n->parent->child[i] != n)
				n->parent->child[j++] = n->parent->child[i];
		n->parent->nchild = j;
	}
	for (i = 0; i < n->nchild; i++)
		remove_recursive(n->child[i]);
	n->freed = 1;
	n->free_events++;
	last_removed = n;
}

/* ane_rtclient_probe_inner, stats=1: create "ane_t6021" (die 0) with
 * ane_timeline + ane_pg_state; the IS_ERR guard skips the files when the
 * name is taken.
 */
static void probe_sim(void)
{
	probe_dir = create_dir("ane_t6021", NULL);
	if (!IS_ERR(probe_dir)) {
		create_blob("ane_timeline", probe_dir);
		create_blob("ane_pg_state", probe_dir);
	}
}

/* The pre-fix setter (old): unconditional create of the probe's name. */
static void trace_set_old(int on)
{
	if (on && !trace_buf) {
		trace_buf = true;
		trace_dir = create_dir("ane_t6021", NULL);
		create_blob("trace_td", trace_dir);
	}
}

/* The fixed setter (new): the trace owns "ane_t6021_trace" outright. */
static void trace_set_new(int on)
{
	if (on && !trace_buf) {
		trace_buf = true;
		trace_dir = create_dir("ane_t6021_trace", NULL);
		if (!IS_ERR(trace_dir))
			create_blob("trace_td", trace_dir);
	}
}

static void trace_free_sim(void)
{
	remove_recursive(trace_dir);
	trace_dir = NULL;
	trace_buf = false;
}

static struct node *file_at(const char *dir, const char *name)
{
	struct node *d = child_of(&root, dir);

	return d ? child_of(d, name) : NULL;
}

static void check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

int main(int argc, char **argv)
{
	int fixed;

	if (argc != 2 || (strcmp(argv[1], "old") && strcmp(argv[1], "new"))) {
		fprintf(stderr, "usage: %s old|new\n", argv[0]);
		return 2;
	}
	fixed = !strcmp(argv[1], "new");

	/* 1. Probe first (stats=1), then trace on: the measured M2 case. */
	reset_world();
	probe_sim();
	if (fixed)
		trace_set_new(1);
	else
		trace_set_old(1);
	check(!!file_at("ane_t6021_trace", "trace_td"),
	      "blob exists in the trace's own dir");
	check(!!file_at("ane_t6021", "ane_timeline") &&
	      !!file_at("ane_t6021", "ane_pg_state"),
	      "probe stats files intact");
	check(!!file_at("ane_t6021", "trace_td") == !fixed,
	      "old code loses the blob to the collision");
	trace_free_sim();
	check(probe_dir && !IS_ERR(probe_dir) && !probe_dir->freed,
	      "free does not touch the probe dir");

	/* 2. Trace on before probe (trace_td=1 at load), then probe. */
	reset_world();
	if (fixed)
		trace_set_new(1);
	else
		trace_set_old(1);
	probe_sim();
	check(!!file_at("ane_t6021_trace", "trace_td"),
	      "blob exists in the trace's own dir");
	check(!!file_at("ane_t6021", "ane_timeline") &&
	      !!file_at("ane_t6021", "ane_pg_state"),
	      "probe stats files intact");
	check(!file_at("ane_t6021", "ane_timeline") == !fixed,
	      "old code silently drops the stats files");
	trace_free_sim();

	/* 3. Toggle x3 with the probe present: one dir, one blob. */
	reset_world();
	probe_sim();
	{
		void (*set)(int) = fixed ? trace_set_new : trace_set_old;

		set(1);
		set(0);
		set(1);
		set(0);
		set(1);
	}
	check(blob_creates == 1, "blob created exactly once");
	check(dup_errors == 0, "no duplicate-name error over 3 toggles");
	check(!!file_at("ane_t6021_trace", "trace_td"), "blob still present");
	trace_free_sim();
	check(last_removed && last_removed->free_events == 1,
	      "trace dir freed exactly once");

	/* 4. Mock unit: duplicate create returns ERR; a child under an ERR
	 * parent is not created; remove_recursive(ERR) is a no-op.
	 */
	reset_world();
	check(!IS_ERR(create_dir("ane_t6021", NULL)), "first create wins");
	check(IS_ERR(create_dir("ane_t6021", NULL)),
	      "duplicate create returns ERR");
	remove_recursive(err_p);
	check(dup_errors == 1, "collision counted");

	printf("%s: %d checks, %d failures\n", argv[1], checks, failures);
	return failures ? 1 : 0;
}
