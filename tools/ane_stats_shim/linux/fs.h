/* Host shim for <linux/fs.h> — only what ane/ane_stats_show.c uses. */
#ifndef ANE_STATS_SHIM_LINUX_FS_H
#define ANE_STATS_SHIM_LINUX_FS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define __user
#ifndef ENOMEM
#define ENOMEM 12
#endif

struct module;

struct file {
	void *private_data;
};

struct inode {
	void *i_private;
};

struct file_operations {
	struct module *owner;
	int (*open)(struct inode *, struct file *);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	loff_t (*llseek)(struct file *, loff_t, int);
	int (*release)(struct inode *, struct file *);
};

/* seq_read/seq_lseek live in fs.h in the kernel; the host test never
 * calls them, the fops table only needs the addresses. */
static inline ssize_t seq_read(struct file *f, char __user *b, size_t s,
			       loff_t *p)
{
	(void)f; (void)b; (void)s; (void)p;
	return -1;
}

static inline loff_t seq_lseek(struct file *f, loff_t o, int w)
{
	(void)f; (void)o; (void)w;
	return 0;
}

#endif /* ANE_STATS_SHIM_LINUX_FS_H */
