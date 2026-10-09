/* Host shim for <linux/list.h> — singly-threaded doubly-linked list. */
#ifndef ANE_POOL_SHIM_LINUX_LIST_H
#define ANE_POOL_SHIM_LINUX_LIST_H

#include <stddef.h>

struct list_head {
	struct list_head *prev;
	struct list_head *next;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

static inline void INIT_LIST_HEAD(struct list_head *head)
{
	head->prev = head;
	head->next = head;
}

static inline void list_add_tail(struct list_head *entry,
				 struct list_head *head)
{
	entry->next = head;
	entry->prev = head->prev;
	head->prev->next = entry;
	head->prev = entry;
}

static inline void list_del(struct list_head *entry)
{
	entry->prev->next = entry->next;
	entry->next->prev = entry->prev;
	entry->prev = NULL;
	entry->next = NULL;
}

#define list_for_each_entry(pos, head, member)			       \
	for (pos = container_of((head)->next, typeof(*pos), member);   \
	     &pos->member != (head);				       \
	     pos = container_of(pos->member.next, typeof(*pos), member))

#endif /* ANE_POOL_SHIM_LINUX_LIST_H */
