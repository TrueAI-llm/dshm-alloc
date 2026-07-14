#include "dshm_l1.h"
#include "dshm_atomic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

int main(void)
{
	/* Allocate a real superblock in local memory for unit test. */
	struct dshm_superblock *sb = calloc(1, sizeof(*sb));
	assert(sb);

	/* Set up the superblock fields (simulating bootstrap). */
	sb->magic = DSHM_MAGIC;
	sb->pool_base_va = (__u64)(void *)sb;
	sb->pool_size = DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE;
	sb->chunk_size = DSHM_CHUNK_SIZE;
	sb->num_chunks = DSHM_NUM_CHUNKS;
	sb->max_nodes = DSHM_MAX_NODES;

	/* Init L1 state: pool_base_va points to the superblock. */
	int err = L1_init_state(sb->pool_base_va, sb->pool_size,
				/*my_node_id=*/1, /*cache_size=*/2);
	assert(err == 0);

	/* Test 1: Allocate 1 chunk via slow path.
	 * With cache_size=2, the slow path allocates 1 chunk (returned to caller)
	 * AND refills the cache to max=2. After this:
	 * - r1.chunk_id is owned by us
	 * - cache.cnt == 2 (refilled by slow path) */
	struct dshm_chunk_alloc_result r1 = L1_alloc();
	assert(r1.err == 0);
	assert(r1.chunk_id < DSHM_NUM_CHUNKS);
	assert(sb->entries[r1.chunk_id].owner_node == 1);
	assert(sb->entries[r1.chunk_id].owner_pid == g_state->my_pid);
	assert(g_state->cache.cnt == 2);

	/* Test 2: Drain the cache by popping both refilled chunks.
	 * This gives us 3 total allocated chunks (r1 + 2 from cache). */
	struct dshm_chunk_alloc_result r2 = L1_alloc();
	assert(r2.err == 0);
	assert(r2.chunk_id != r1.chunk_id);
	struct dshm_chunk_alloc_result r3 = L1_alloc();
	assert(r3.err == 0);
	assert(r3.chunk_id != r1.chunk_id);
	assert(r3.chunk_id != r2.chunk_id);
	assert(g_state->cache.cnt == 0);

	/* Test 3: Free r1 — cache is empty, so it goes to local cache. */
	L1_free(r1.chunk_id);
	assert(g_state->cache.cnt == 1);

	/* Test 4: Allocate again — should come from cache (fast path), returning r1. */
	struct dshm_chunk_alloc_result r4 = L1_alloc();
	assert(r4.err == 0);
	assert(r4.chunk_id == r1.chunk_id);
	assert(g_state->cache.cnt == 0);

	/* Test 5: Allocate many chunks to exhaust the cache and exercise slow path
	 * refill. */
	for (int i = 0; i < 10; i++) {
		struct dshm_chunk_alloc_result r = L1_alloc();
		assert(r.err == 0);
	}
	assert(g_state->cache.cnt > 0);

	/* Test 6: Verify no two chunk_ids are the same (no double-alloc).
	 * Reinit g_state only (sb still has entries from Tests 1-5, which is fine —
	 * the slow path will skip occupied entries). cache_size=0 means no refill,
	 * so every alloc goes through CAS. */
	int *alloced = calloc(DSHM_NUM_CHUNKS, sizeof(int));
	assert(alloced);
	L1_cleanup_state();
	err = L1_init_state(sb->pool_base_va, sb->pool_size, 1, 0);
	assert(err == 0);
	for (int i = 0; i < 100; i++) {
		struct dshm_chunk_alloc_result r = L1_alloc();
		assert(r.err == 0);
		assert(r.chunk_id < DSHM_NUM_CHUNKS);
		assert(alloced[r.chunk_id] == 0);
		alloced[r.chunk_id] = 1;
	}

	/* Cleanup */
	free(alloced);
	free(sb);
	L1_cleanup_state();

	printf("test_l1_cas: PASS\n");
	return 0;
}
