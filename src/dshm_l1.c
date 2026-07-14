#include "dshm_l1.h"
#include "dshm_atomic.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

struct dshm_proc_state *g_state = NULL;

int L1_init_state(__u64 pool_base_va, __u64 pool_size,
		  __u32 my_node_id, int local_cache_size)
{
	if (g_state != NULL)
		return -EBUSY;

	g_state = malloc(sizeof(*g_state));
	if (!g_state)
		return -ENOMEM;

	g_state->pool_base_va = pool_base_va;
	g_state->pool_size    = pool_size;
	g_state->my_node_id   = my_node_id;
	g_state->my_pid       = (uint32_t)getpid();
	g_state->cache.cnt    = 0;
	g_state->cache.max    = local_cache_size;
	g_state->cache.chunk_ids = malloc((size_t)local_cache_size * sizeof(__u32));
	if (!g_state->cache.chunk_ids) {
		free(g_state);
		g_state = NULL;
		return -ENOMEM;
	}
	pthread_spin_init(&g_state->cache.lock, 0);

	return 0;
}

void L1_cleanup_state(void)
{
	if (!g_state)
		return;
	free(g_state->cache.chunk_ids);
	pthread_spin_destroy(&g_state->cache.lock);
	free(g_state);
	g_state = NULL;
}

/* Forward declaration for slow path (implemented in Task 3). */
static struct dshm_chunk_alloc_result L1_slow_path(void);

struct dshm_chunk_alloc_result L1_alloc(void)
{
	struct dshm_chunk_alloc_result r;

	/* Fast path: pop from local cache. */
	pthread_spin_lock(&g_state->cache.lock);
	if (g_state->cache.cnt > 0) {
		r.chunk_id = g_state->cache.chunk_ids[--g_state->cache.cnt];
		r.err = 0;
		pthread_spin_unlock(&g_state->cache.lock);
		return r;
	}
	pthread_spin_unlock(&g_state->cache.lock);

	/* Slow path: global CAS bitmap. */
	return L1_slow_path();
}

void L1_free(__u32 chunk_id)
{
	/* Try to return to local cache first. */
	pthread_spin_lock(&g_state->cache.lock);
	if (g_state->cache.cnt < g_state->cache.max) {
		g_state->cache.chunk_ids[g_state->cache.cnt++] = chunk_id;
		pthread_spin_unlock(&g_state->cache.lock);
		return;
	}
	pthread_spin_unlock(&g_state->cache.lock);

	/* Cache full → release to global bitmap.
	 * Plain atomic store: we are the sole owner. */
	struct dshm_superblock *sb =
		(struct dshm_superblock *)(void *)g_state->pool_base_va;
	struct dshm_chunk_entry free_entry = { .owner_node = 0, .owner_pid = 0 };
	dshm_store_entry(&sb->entries[chunk_id], free_entry);
}

/* Placeholder slow path — replaced in Task 3. */
static struct dshm_chunk_alloc_result L1_slow_path(void)
{
	struct dshm_chunk_alloc_result r = { .err = -ENOMEM, .chunk_id = 0 };
	return r;
}
