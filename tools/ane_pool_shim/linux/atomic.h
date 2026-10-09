/* Host shim for <linux/atomic.h> — plain 64-bit arithmetic. */
#ifndef ANE_POOL_SHIM_LINUX_ATOMIC_H
#define ANE_POOL_SHIM_LINUX_ATOMIC_H

typedef struct {
	long long counter;
} atomic64_t;

static inline void atomic64_set(atomic64_t *v, long long i)
{
	v->counter = i;
}

static inline void atomic64_add(long long i, atomic64_t *v)
{
	v->counter += i;
}

static inline void atomic64_sub(long long i, atomic64_t *v)
{
	v->counter -= i;
}

static inline long long atomic64_read(const atomic64_t *v)
{
	return v->counter;
}

#endif /* ANE_POOL_SHIM_LINUX_ATOMIC_H */
