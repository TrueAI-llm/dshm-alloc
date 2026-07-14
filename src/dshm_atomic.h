#ifndef DSHM_ATOMIC_H
#define DSHM_ATOMIC_H

#include <stdint.h>
#include <string.h>
#include <time.h>

/*
 * User-space wrappers for the kernel-style primitives used in the spec.
 * Maps smp_mb/CAS/cpu_relax to GCC __atomic builtins and sched_yield.
 */

/* 8-byte CAS. Returns 1 on success (old matched, value updated to `new_val`).
 * Returns 0 on failure (old did not match). */
static inline int dshm_cas_u64(volatile uint64_t *ptr,
			       uint64_t old_val, uint64_t new_val)
{
	return __atomic_compare_exchange_n(ptr, &old_val, new_val, 0,
					  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* Convenience: CAS on a chunk_entry (8 bytes, treated as u64).
 * Uses memcpy to avoid strict-aliasing UB (safe under -Wpedantic -Werror). */
static inline int dshm_cas_entry(volatile struct dshm_chunk_entry *entry,
				struct dshm_chunk_entry old_e,
				struct dshm_chunk_entry new_e)
{
	uint64_t old_u, new_u;
	memcpy(&old_u, &old_e, sizeof(old_u));
	memcpy(&new_u, &new_e, sizeof(new_u));
	volatile uint64_t *p = (volatile uint64_t *)entry;
	return dshm_cas_u64(p, old_u, new_u);
}

/* Plain atomic store on an 8-byte entry (for L1_free).
 * Uses memcpy to avoid strict-aliasing UB. */
static inline void dshm_store_entry(volatile struct dshm_chunk_entry *entry,
				   struct dshm_chunk_entry val)
{
	uint64_t val_u;
	memcpy(&val_u, &val, sizeof(val_u));
	volatile uint64_t *p = (volatile uint64_t *)entry;
	__atomic_store_n(p, val_u, __ATOMIC_RELEASE);
}

/* Release barrier (writer side, before publishing). */
static inline void dshm_smp_mb(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Acquire barrier (reader side, after observing a published value). */
static inline void dshm_smp_rmb(void)
{
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
}

/* Spin-wait hint. */
static inline void dshm_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ volatile("yield");
#else
	/* Fallback: no-op or sched_yield for non-x86/ARM. */
#endif
}

/* Monotonic clock in nanoseconds. */
static inline uint64_t dshm_clock_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif /* DSHM_ATOMIC_H */
