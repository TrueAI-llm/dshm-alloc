#include "dshm_l2.h"
#include "dshm_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#define TEST_POOL_SIZE (DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE)

int main(void)
{
	void *pool = mmap(NULL, TEST_POOL_SIZE,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
			 -1, 0);
	if (pool == MAP_FAILED) {
		fprintf(stderr, "test_l2_malloc: 128T mmap failed, trying 4GB\n");
		assert(dshm_init(0, 0x100000000ULL, 1, 4) == -EINVAL);
		printf("test_l2_malloc: SKIP (128T mmap unavailable)\n");
		return 0;
	}

	int err = dshm_init((uint64_t)pool, TEST_POOL_SIZE, 1, 4);
	assert(err == 0);

	void *ptr1 = shared_malloc(4096);
	assert(ptr1 != NULL);
	assert((uint64_t)ptr1 >= (uint64_t)pool);
	assert((uint64_t)ptr1 < (uint64_t)pool + TEST_POOL_SIZE);

	memset(ptr1, 0xAB, 4096);
	unsigned char *bytes = ptr1;
	assert(bytes[0] == 0xAB);
	assert(bytes[4095] == 0xAB);

	void *ptrs[100];
	for (int i = 0; i < 100; i++) {
		ptrs[i] = shared_malloc(64 * 1024);
		assert(ptrs[i] != NULL);
		memset(ptrs[i], i & 0xFF, 64 * 1024);
	}
	for (int i = 0; i < 100; i++) {
		unsigned char *b = ptrs[i];
		assert(b[0] == (i & 0xFF));
		assert(b[64 * 1024 - 1] == (i & 0xFF));
	}

	for (int i = 0; i < 100; i++)
		shared_free(ptrs[i]);
	shared_free(ptr1);

	/* Don't munmap the pool — it may overlap with libc mappings and
	 * cause a segfault on stdout flush. The process exits immediately
	 * after, so the OS reclaims everything. */

	printf("test_l2_malloc: PASS\n");
	return 0;
}
