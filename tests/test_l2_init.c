#include "dshm_l2.h"
#include "dshm_types.h"
#include "dshm_l1.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>

int main(void)
{
	/* Test 1: Reject pool_size == 0 */
	int err = dshm_init(0x1000, 0, 1, 4);
	assert(err == -EINVAL);

	/* Test 2: Reject local_cache_size <= 0 */
	err = dshm_init(0x1000, DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE, 1, 0);
	assert(err == -EINVAL);

	/* Test 3: Reject my_node_id == 0 (FREE sentinel) */
	err = dshm_init(0x1000, DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE, 0, 4);
	assert(err == -EINVAL);

	/* Test 4: Reject my_node_id > DSHM_MAX_NODES */
	err = dshm_init(0x1000, DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE, 257, 4);
	assert(err == -EINVAL);

	/* Test 5: Reject pool_size != 128T */
	err = dshm_init(0x1000, 0x100000000ULL, 1, 4);
	assert(err == -EINVAL);

	/* Test 6: Double-init rejection. We pre-set g_state to simulate
	 * an already-initialized state. We use a calloc'd superblock so the
	 * bootstrap code can safely read sb->magic (it will be 0, triggering
	 * the CAS init path, which writes DSHM_INITIALIZING then DSHM_MAGIC).
	 * After bootstrap, L1_init_state is called, which checks g_state != NULL
	 * and returns -EBUSY. */
	struct dshm_superblock *sb6 = calloc(1, sizeof(*sb6));
	assert(sb6);
	struct dshm_proc_state fake = {0};
	g_state = &fake;
	err = dshm_init((uint64_t)(void *)sb6,
			DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE, 1, 4);
	assert(err == -EBUSY);
	g_state = NULL;
	free(sb6);

	printf("test_l2_init: PASS\n");
	return 0;
}
