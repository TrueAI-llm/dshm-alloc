#ifndef DSHM_L2_H
#define DSHM_L2_H

#include <stdint.h>
#include <stddef.h>

int dshm_init(uint64_t pool_base_va, uint64_t pool_size,
	      uint32_t my_node_id, int local_cache_size);

void *shared_malloc(size_t size);

void shared_free(void *ptr);

#endif /* DSHM_L2_H */
