#include "dshm_l1.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

#define NTHREADS 8
#define ALLOCS_PER_THREAD 50

struct thread_arg {
	int tid;
	__u32 *alloced_ids;
};

static void *worker(void *arg)
{
	struct thread_arg *ta = arg;
	for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
		struct dshm_chunk_alloc_result r = L1_alloc();
		assert(r.err == 0);
		ta->alloced_ids[ta->tid * ALLOCS_PER_THREAD + i] = r.chunk_id;
	}
	return NULL;
}

int main(void)
{
	struct dshm_superblock *sb = calloc(1, sizeof(*sb));
	assert(sb);
	sb->magic = DSHM_MAGIC;
	sb->pool_base_va = (__u64)(void *)sb;
	sb->pool_size = DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE;
	sb->chunk_size = DSHM_CHUNK_SIZE;
	sb->num_chunks = DSHM_NUM_CHUNKS;
	sb->max_nodes = DSHM_MAX_NODES;

	int err = L1_init_state(sb->pool_base_va, sb->pool_size, 1, 2);
	assert(err == 0);

	__u32 *alloced = calloc(NTHREADS * ALLOCS_PER_THREAD, sizeof(__u32));
	assert(alloced);

	pthread_t threads[NTHREADS];
	struct thread_arg args[NTHREADS];
	for (int i = 0; i < NTHREADS; i++) {
		args[i].tid = i;
		args[i].alloced_ids = alloced;
		pthread_create(&threads[i], NULL, worker, &args[i]);
	}
	for (int i = 0; i < NTHREADS; i++)
		pthread_join(threads[i], NULL);

	int *seen = calloc(DSHM_NUM_CHUNKS, sizeof(int));
	assert(seen);
	int total = NTHREADS * ALLOCS_PER_THREAD;
	for (int i = 0; i < total; i++) {
		__u32 id = alloced[i];
		assert(id < DSHM_NUM_CHUNKS);
		assert(seen[id] == 0);
		seen[id] = 1;
	}

	free(seen);
	free(alloced);
	free(sb);
	L1_cleanup_state();

	printf("test_l1_concurrent: PASS (%d chunks, no duplicates)\n", total);
	return 0;
}
