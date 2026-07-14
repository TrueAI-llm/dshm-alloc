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

static struct dshm_chunk_alloc_result L1_slow_path(void);

/* Allocate n consecutive chunks. Scans for n contiguous FREE entries,
 * CASes them one by one. Rolls back on any CAS failure. */
struct dshm_chunk_alloc_result L1_alloc_contiguous(__u32 n)
{
	struct dshm_chunk_alloc_result result = { .err = -ENOMEM, .chunk_id = 0 };

	if (n == 0)
		return result;
	if (n == 1)
		return L1_alloc();

	struct dshm_superblock *sb =
		(struct dshm_superblock *)(void *)g_state->pool_base_va;

	unsigned int seed = (unsigned int)dshm_clock_ns();
	__u32 start = (__u32)(rand_r(&seed) % DSHM_NUM_CHUNKS);

	struct dshm_chunk_entry new_entry = {
		.owner_node = g_state->my_node_id,
		.owner_pid  = g_state->my_pid,
	};
	struct dshm_chunk_entry free_entry = {
		.owner_node = 0, .owner_pid = 0,
	};

	for (__u32 attempt = 0; attempt < DSHM_NUM_CHUNKS; attempt++) {
		__u32 base = (start + attempt) % DSHM_NUM_CHUNKS;

		if (base + n > DSHM_NUM_CHUNKS)
			continue;

		__u32 acquired = 0;
		for (__u32 j = 0; j < n; j++) {
			__u32 idx = base + j;
			volatile struct dshm_chunk_entry *e = &sb->entries[idx];

			__u32 on = __atomic_load_n(
				(volatile uint32_t *)&e->owner_node,
				__ATOMIC_RELAXED);
			if (on != 0)
				goto rollback;

			struct dshm_chunk_entry old_entry = {
				.owner_node = 0, .owner_pid = 0 };
			if (!dshm_cas_entry(e, old_entry, new_entry))
				goto rollback;

			sb->alloc_timestamps[idx] = dshm_clock_ns();
			acquired++;
		}

		result.err = 0;
		result.chunk_id = base;
		return result;

	rollback:
		for (__u32 j = 0; j < acquired; j++) {
			dshm_store_entry(&sb->entries[base + j], free_entry);
		}
	}

	return result;
}

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
#define DSHM_SLOWPATH_MAX_CONSEC_FAIL 8

static struct dshm_chunk_alloc_result L1_slow_path(void)
{
	struct dshm_superblock *sb =
		(struct dshm_superblock *)(void *)g_state->pool_base_va;

	struct dshm_chunk_alloc_result result = { .err = -ENOMEM, .chunk_id = 0 };

	unsigned int seed = (unsigned int)dshm_clock_ns();
	__u32 start = (__u32)(rand_r(&seed) % DSHM_NUM_CHUNKS);

	struct dshm_chunk_entry new_entry = {
		.owner_node = g_state->my_node_id,
		.owner_pid  = g_state->my_pid,
	};

	__u32 allocated_id = 0;
	int found = 0;

	for (__u32 i = 0; i < DSHM_NUM_CHUNKS; i++) {
		__u32 idx = (start + i) % DSHM_NUM_CHUNKS;
		volatile struct dshm_chunk_entry *e = &sb->entries[idx];

		__u32 on = __atomic_load_n(
			(volatile uint32_t *)&e->owner_node,
			__ATOMIC_RELAXED);
		if (on != 0)
			continue;

		struct dshm_chunk_entry old_entry = {
			.owner_node = 0, .owner_pid = 0 };
		if (dshm_cas_entry(e, old_entry, new_entry)) {
			sb->alloc_timestamps[idx] = dshm_clock_ns();
			allocated_id = idx;
			found = 1;
			break;
		}
	}

	if (!found)
		return result;

	int consec_fail = 0;
	for (;;) {
		pthread_spin_lock(&g_state->cache.lock);
		if (g_state->cache.cnt >= g_state->cache.max) {
			pthread_spin_unlock(&g_state->cache.lock);
			break;
		}
		pthread_spin_unlock(&g_state->cache.lock);

		__u32 scan_start = (allocated_id + 1) % DSHM_NUM_CHUNKS;
		int refill_found = 0;

		for (__u32 j = 0; j < DSHM_NUM_CHUNKS; j++) {
			__u32 idx = (scan_start + j) % DSHM_NUM_CHUNKS;
			volatile struct dshm_chunk_entry *e = &sb->entries[idx];
			__u32 on = __atomic_load_n(
				(volatile uint32_t *)&e->owner_node,
				__ATOMIC_RELAXED);
			if (on != 0)
				continue;

			struct dshm_chunk_entry old_e = {
				.owner_node = 0, .owner_pid = 0 };
			if (dshm_cas_entry(e, old_e, new_entry)) {
				sb->alloc_timestamps[idx] = dshm_clock_ns();
				pthread_spin_lock(&g_state->cache.lock);
				g_state->cache.chunk_ids[g_state->cache.cnt++] = idx;
				pthread_spin_unlock(&g_state->cache.lock);
				refill_found = 1;
				consec_fail = 0;
				break;
			}
		}

		if (!refill_found) {
			consec_fail++;
			if (consec_fail >= DSHM_SLOWPATH_MAX_CONSEC_FAIL)
				break;
		}
	}

	result.err = 0;
	result.chunk_id = allocated_id;
	return result;
}
