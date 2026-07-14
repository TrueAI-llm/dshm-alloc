#ifndef DSHM_L1_H
#define DSHM_L1_H

#include "dshm_types.h"
#include <pthread.h>

struct dshm_proc_state {
	__u64 pool_base_va;
	__u64 pool_size;
	__u32 my_node_id;
	__u32 my_pid;
	unsigned arena_ind;  /* jemalloc arena index (set by L2) */

	/* Local cache */
	struct {
		pthread_spinlock_t lock;
		int cnt;
		int max;
		__u32 *chunk_ids;  /* dynamically allocated, max elements */
	} cache;
};

/* Singleton, set by dshm_init(). */
extern struct dshm_proc_state *g_state;

/* L1 API */
struct dshm_chunk_alloc_result L1_alloc(void);
void L1_free(__u32 chunk_id);

/* Internal: init per-process state (called by dshm_init). */
int L1_init_state(__u64 pool_base_va, __u64 pool_size,
		  __u32 my_node_id, int local_cache_size);

/* Internal: free per-process state. */
void L1_cleanup_state(void);

#endif /* DSHM_L1_H */
