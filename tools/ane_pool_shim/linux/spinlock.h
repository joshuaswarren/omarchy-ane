/* Host shim for <linux/spinlock.h> — the unit test is single-threaded. */
#ifndef ANE_POOL_SHIM_LINUX_SPINLOCK_H
#define ANE_POOL_SHIM_LINUX_SPINLOCK_H

typedef struct {
	int unused;
} spinlock_t;

static inline void spin_lock_init(spinlock_t *lock)
{
	lock->unused = 0;
}

static inline void spin_lock(spinlock_t *lock)
{
	lock->unused++;
}

static inline void spin_unlock(spinlock_t *lock)
{
	lock->unused--;
}

#endif /* ANE_POOL_SHIM_LINUX_SPINLOCK_H */
