/* Host shim for <linux/seq_file.h> — enough to drive the real
 * ane_timeline_show through the real fops open path in the unit test. */
#ifndef ANE_STATS_SHIM_LINUX_SEQ_FILE_H
#define ANE_STATS_SHIM_LINUX_SEQ_FILE_H

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <linux/fs.h>

struct seq_file {
	char *buf;
	size_t size;
	size_t len;
	int (*show)(struct seq_file *, void *);
	void *private;
};

static inline void seq_printf(struct seq_file *m, const char *fmt, ...)
{
	va_list ap;
	int room = (m->size > m->len) ? (int)(m->size - m->len) : 0;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(m->buf + m->len, room, fmt, ap);
	va_end(ap);
	m->len += (n > 0) ? (size_t)n : 0;
}

static inline int single_open(struct file *file,
			      int (*show)(struct seq_file *, void *),
			      void *priv)
{
	struct seq_file *m = calloc(1, sizeof(*m));

	if (!m)
		return -ENOMEM;
	m->show = show;
	m->private = priv;
	file->private_data = m;
	return 0;
}

static inline int single_release(struct inode *inode, struct file *file)
{
	(void)inode;
	free(file->private_data);
	file->private_data = NULL;
	return 0;
}

#endif /* ANE_STATS_SHIM_LINUX_SEQ_FILE_H */
