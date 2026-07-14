#include "dshm_l1.h"
#include "dshm_atomic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

int main(void)
{
	/* Allocate a real superblock in local memory (calloc'd = all entries FREE).
	 * pool_base_va points to the superblock itself, so the slow path can
	 * safely access sb->entries when the cache is empty. */
	struct dshm_superblock *sb = calloc(1, sizeof(*sb));
	assert(sb);
	sb->magic = DSHM_MAGIC;
	sb->pool_base_va = (__u64)(void *)sb;
	sb->pool_size = DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE;
	sb->chunk_size = DSHM_CHUNK_SIZE;
	sb->num_chunks = DSHM_NUM_CHUNKS;
	sb->max_nodes = DSHM_MAX_NODES;

	/* Init L1 state: pool_base_va points to the real superblock, cache_size = 4 */
	int err = L1_init_state((__u64)(void *)sb, DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE,
				/*my_node_id=*/1, /*cache_size=*/4);
	assert(err == 0);

	/* Pre-load local cache with 2 chunk IDs (simulating prior slow-path allocs). */
	pthread_spin_lock(&g_state->cache.lock);
	g_state->cache.chunk_ids[g_state->cache.cnt++] = 5;
	g_state->cache.chunk_ids[g_state->cache.cnt++] = 10;
	pthread_spin_unlock(&g_state->cache.lock);

	/* Test 1: Fast path — should return chunk_id=10 (LIFO). */
	struct dshm_chunk_alloc_result r1 = L1_alloc();
	assert(r1.err == 0);
	assert(r1.chunk_id == 10);

	/* Test 2: Fast path — should return chunk_id=5. */
	struct dshm_chunk_alloc_result r2 = L1_alloc();
	assert(r2.err == 0);
	assert(r2.chunk_id == 5);

	/* Test 3: Cache empty — L1_alloc should fall through to slow path.
	 * Since the superblock is calloc'd, entries are all zero (FREE),
	 * so it should find a chunk via CAS. This verifies fast→slow transition.
	 * NOTE: In Task 2, the placeholder slow path returns -ENOMEM, so this
	 * test fails. It passes after Task 3 implements the real slow path. */
	struct dshm_chunk_alloc_result r3 = L1_alloc();
	assert(r3.err == 0);
	/* chunk_id should be in range [0, DSHM_NUM_CHUNKS-1] */
	assert(r3.chunk_id < DSHM_NUM_CHUNKS);

	/* Cleanup */
	free(sb);
	L1_cleanup_state();

	printf("test_l1_cache: PASS\n");
	return 0;
}
