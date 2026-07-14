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
#include <stdatomic.h>

/*
 * Chunk feeder: sub-allocates extents from a 1G L1 chunk.
 * jemalloc requests extents of ~2MB; we carve them out of the
 * current active chunk and track usage so the chunk can be
 * returned to L1 when fully freed.
 */
struct dshm_feeder {
	pthread_spinlock_t lock;
	__u32 active_chunk_id;    /* current chunk being carved, or -1 */
	__u64 active_offset;      /* bytes already carved from active chunk */
	/* Per-chunk used-bytes counter (indexed by chunk_id).
	 * Allocated lazily to DSHM_NUM_CHUNKS entries. */
	atomic_uint_least64_t *used_bytes;
};

static struct dshm_feeder feeder;

static int feeder_init(void)
{
	feeder.used_bytes = calloc(DSHM_NUM_CHUNKS,
				  sizeof(*feeder.used_bytes));
	if (!feeder.used_bytes)
		return -ENOMEM;
	feeder.active_chunk_id = 0xFFFFFFFF;
	feeder.active_offset = DSHM_CHUNK_SIZE; /* force alloc on first use */
	pthread_spin_init(&feeder.lock, 0);
	return 0;
}

static void feeder_cleanup(void)
{
	pthread_spin_destroy(&feeder.lock);
	free(feeder.used_bytes);
}

/* Carve `size` bytes from the pool. Returns VA, or NULL on failure.
 * Caller (jemalloc) must hold no dshm locks. */
static void *feeder_alloc(size_t size, size_t alignment)
{
	if (size > DSHM_CHUNK_SIZE) {
		__u32 n = (__u32)((size + DSHM_CHUNK_SIZE - 1) / DSHM_CHUNK_SIZE);
		struct dshm_chunk_alloc_result r = L1_alloc_contiguous(n);
		if (r.err)
			return NULL;
		__u64 va = g_state->pool_base_va +
			   (uint64_t)r.chunk_id * DSHM_CHUNK_SIZE;
		atomic_fetch_add_explicit(&feeder.used_bytes[r.chunk_id],
					  size, memory_order_relaxed);
		return (void *)(uintptr_t)va;
	}

	pthread_spin_lock(&feeder.lock);

	if (feeder.active_offset + size > DSHM_CHUNK_SIZE) {
		struct dshm_chunk_alloc_result r = L1_alloc();
		if (r.err) {
			pthread_spin_unlock(&feeder.lock);
			return NULL;
		}
		feeder.active_chunk_id = r.chunk_id;
		feeder.active_offset = 0;
	}

	__u64 misalign = feeder.active_offset % alignment;
	if (misalign)
		feeder.active_offset += alignment - misalign;

	__u32 chunk_id = feeder.active_chunk_id;
	__u64 offset = feeder.active_offset;
	feeder.active_offset += size;

	atomic_fetch_add_explicit(&feeder.used_bytes[chunk_id], size,
				  memory_order_relaxed);

	pthread_spin_unlock(&feeder.lock);

	return (void *)(uintptr_t)(g_state->pool_base_va +
				   (uint64_t)chunk_id * DSHM_CHUNK_SIZE +
				   offset);
}

/* Release `size` bytes at `addr` back. When a chunk's used_bytes
 * drops to 0, return it to L1. */
static void feeder_free(void *addr, size_t size)
{
	uint64_t va = (uint64_t)(uintptr_t)addr;
	__u32 chunk_id = (va - g_state->pool_base_va) / DSHM_CHUNK_SIZE;

	uint64_t prev = atomic_fetch_sub_explicit(
		&feeder.used_bytes[chunk_id], size, memory_order_relaxed);

	if (prev == size) {
		if (size > DSHM_CHUNK_SIZE) {
			__u32 n = (__u32)((size + DSHM_CHUNK_SIZE - 1)
					   / DSHM_CHUNK_SIZE);
			for (__u32 i = 0; i < n; i++)
				L1_free(chunk_id + i);
		} else {
			pthread_spin_lock(&feeder.lock);
			if (feeder.active_chunk_id == chunk_id) {
				feeder.active_offset = DSHM_CHUNK_SIZE;
				feeder.active_chunk_id = 0xFFFFFFFF;
			}
			pthread_spin_unlock(&feeder.lock);
			L1_free(chunk_id);
		}
	}
}

static void *
dshm_extent_alloc(extent_hooks_t *hooks, void *new_addr,
		  size_t size, size_t alignment,
		  bool *zero, bool *commit, unsigned arena_ind)
{
	if (new_addr != NULL)
		return NULL;

	void *addr = feeder_alloc(size, alignment);
	if (!addr)
		return NULL;

	*zero = false;
	*commit = true;
	return addr;
}

static bool
dshm_extent_dalloc(extent_hooks_t *hooks, void *addr, size_t size,
		   bool committed, unsigned arena_ind)
{
	feeder_free(addr, size);
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

	err = feeder_init();
	if (err) {
		L1_cleanup_state();
		return err;
	}

	unsigned arena_ind;
	size_t sz = sizeof(arena_ind);
	int jerr = mallctl("arenas.create", &arena_ind, &sz, NULL, 0);
	if (jerr) {
		feeder_cleanup();
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
