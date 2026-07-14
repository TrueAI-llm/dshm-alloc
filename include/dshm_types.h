#ifndef DSHM_TYPES_H
#define DSHM_TYPES_H

#include <stdint.h>

#define DSHM_NUM_CHUNKS    131072       /* 128T / 1G */
#define DSHM_MAX_NODES    256
#define DSHM_CHUNK_SIZE    0x40000000ULL  /* 1G = 1 << 30 */
#define DSHM_MAGIC         0x4453484d4d43424cULL  /* "DSHMMCBL" */
#define DSHM_INITIALIZING  0x494e4954494e4954ULL  /* "INITINIT" */
#define DSHM_DEFAULT_CACHE_SIZE  4

typedef uint64_t __u64;
typedef uint32_t __u32;
typedef uint8_t  __u8;

/* CAS unit: 8 bytes */
struct dshm_chunk_entry {
	__u32 owner_node;  /* 0 = FREE, 1..255 = allocator's node_id */
	__u32 owner_pid;   /* allocator's OS pid */
} __attribute__((packed, aligned(8)));

/* L1_alloc return type */
struct dshm_chunk_alloc_result {
	int  err;       /* 0 on success, -ENOMEM if pool exhausted */
	__u32 chunk_id; /* valid when err == 0 */
};

/* Superblock at chunk 0 of the shared pool */
struct dshm_superblock {
	/* Read-only fields, initialized once at bootstrap */
	__u64 magic;
	__u64 pool_base_va;
	__u64 pool_size;
	__u64 chunk_size;
	__u32 num_chunks;
	__u32 max_nodes;
	__u8  pad[216];  /* pad to 256 bytes (40 + 216 = 256) */

	/* Chunk entries: 8 bytes each, CAS unit */
	struct dshm_chunk_entry entries[DSHM_NUM_CHUNKS];  /* 1MB */

	/* Alloc timestamps (debug only, not in CAS path) */
	__u64 alloc_timestamps[DSHM_NUM_CHUNKS];  /* 1MB */

	/* Total metadata: ~2MB (256B header + 1MB entries + 1MB timestamps),
	   fits well within chunk 0 (1GB) */
};

#endif /* DSHM_TYPES_H */
