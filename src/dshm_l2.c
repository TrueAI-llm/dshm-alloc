#include "dshm_l2.h"
#include "dshm_l1.h"
#include "dshm_atomic.h"

#include <jemalloc/jemalloc.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>

static void *
dshm_extent_alloc(extent_hooks_t *hooks, void *new_addr,
		  size_t size, size_t alignment,
		  bool *zero, bool *commit, unsigned arena_ind)
{
	if (new_addr != NULL)
		return NULL;

	struct dshm_chunk_alloc_result r = L1_alloc();
	if (r.err)
		return NULL;

	void *addr = (void *)(uintptr_t)(g_state->pool_base_va +
					(uint64_t)r.chunk_id * DSHM_CHUNK_SIZE);
	*zero = false;
	*commit = true;
	return addr;
}

static bool
dshm_extent_dalloc(extent_hooks_t *hooks, void *addr, size_t size,
		   bool committed, unsigned arena_ind)
{
	if (size < DSHM_CHUNK_SIZE)
		return false;

	uint64_t chunk_id = ((uint64_t)(uintptr_t)addr - g_state->pool_base_va)
			    / DSHM_CHUNK_SIZE;
	L1_free((__u32)chunk_id);
	return true;
}

static bool
dshm_extent_commit(extent_hooks_t *hooks, void *addr,
		   size_t size, size_t offset,
		   size_t length, unsigned arena_ind)
{
	return true;
}

static bool
dshm_extent_decommit(extent_hooks_t *hooks, void *addr,
		     size_t size, size_t offset,
		     size_t length, unsigned arena_ind)
{
	return false;
}

static bool
dshm_extent_purge_lazy(extent_hooks_t *hooks, void *addr,
		       size_t size, size_t offset,
		       size_t length, unsigned arena_ind)
{
	return false;
}

static bool
dshm_extent_purge_forced(extent_hooks_t *hooks,
			 void *addr, size_t size,
			 size_t offset, size_t length,
			 unsigned arena_ind)
{
	return false;
}

static bool
dshm_extent_split(extent_hooks_t *hooks, void *addr,
		  size_t size, size_t size_a,
		  size_t size_b, bool committed,
		  unsigned arena_ind)
{
	return false;
}

static bool
dshm_extent_merge(extent_hooks_t *hooks, void *addr_a,
		  size_t size_a, void *addr_b,
		  size_t size_b, bool committed,
		  unsigned arena_ind)
{
	return false;
}

static const extent_hooks_t dshm_hooks = {
	.alloc         = dshm_extent_alloc,
	.dalloc        = dshm_extent_dalloc,
	.destroy       = NULL,
	.commit        = dshm_extent_commit,
	.decommit      = dshm_extent_decommit,
	.purge_lazy    = dshm_extent_purge_lazy,
	.purge_forced  = dshm_extent_purge_forced,
	.split         = dshm_extent_split,
	.merge         = dshm_extent_merge,
};

int dshm_init(uint64_t pool_base_va, uint64_t pool_size,
	      uint32_t my_node_id, int local_cache_size)
{
	if (pool_size == 0 || local_cache_size <= 0)
		return -EINVAL;
	if (my_node_id == 0 || my_node_id > DSHM_MAX_NODES)
		return -EINVAL;
	if (pool_size != (uint64_t)DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE)
		return -EINVAL;

	struct dshm_superblock *sb = (struct dshm_superblock *)(void *)pool_base_va;

	if (sb->magic == DSHM_MAGIC) {
		/* Already initialized */
	} else if (dshm_cas_u64(&sb->magic, 0ULL, DSHM_INITIALIZING)) {
		sb->pool_base_va = pool_base_va;
		sb->pool_size    = pool_size;
		sb->chunk_size   = DSHM_CHUNK_SIZE;
		sb->num_chunks   = DSHM_NUM_CHUNKS;
		sb->max_nodes    = DSHM_MAX_NODES;
		memset(sb->entries, 0, sizeof(sb->entries));
		memset(sb->alloc_timestamps, 0, sizeof(sb->alloc_timestamps));
		dshm_smp_mb();
		__atomic_store_n(&sb->magic, DSHM_MAGIC, __ATOMIC_RELEASE);
	} else {
		while (__atomic_load_n(&sb->magic, __ATOMIC_ACQUIRE)
		       != DSHM_MAGIC)
			dshm_cpu_relax();
	}

	int err = L1_init_state(pool_base_va, pool_size, my_node_id,
				local_cache_size);
	if (err)
		return err;

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	int jerr = mallctl("arenas.create", &arena_ind, &sz, NULL, 0);
	if (jerr) {
		L1_cleanup_state();
		return -jerr;
	}

	char cmd[64];
	snprintf(cmd, sizeof(cmd), "arena.%u.extent_hooks", arena_ind);
	const extent_hooks_t *hooks_ptr = &dshm_hooks;
	mallctl(cmd, NULL, NULL, (void *)&hooks_ptr, sizeof(hooks_ptr));

	g_state->arena_ind = arena_ind;
	return 0;
}

void *shared_malloc(size_t size)
{
	return mallocx(size, MALLOCX_ARENA(g_state->arena_ind));
}

void shared_free(void *ptr)
{
	dallocx(ptr, 0);
}
