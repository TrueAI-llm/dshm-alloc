# Distributed Chunk Allocator Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a user-space C library (`libdshm_malloc`) that allocates 1GB chunks from a 128TB shared GVA pool via CAS-coordinated bitmap, integrated with jemalloc via `extent_hooks_t`.

**Architecture:** Two layers — L1 (distributed 1G chunk CAS allocator with per-process local cache) and L2 (jemalloc extent_hooks adapter that routes `extent_alloc` to L1 and `extent_dalloc` to `L1_free`). Metadata lives in the pool head (~2MB superblock). No daemon, no kernel device, no automatic reclamation on process exit.

**Tech Stack:** C11, pthreads, GCC `__atomic` builtins, jemalloc 5.3+ (dev branch `extent_hooks_t` API), CMake build system, GoogleTest C++ test harness.

**Spec:** `docs/superpowers/specs/2026-07-14-distributed-chunk-allocator-design.md`

---

## File Structure

| File | Responsibility | LOC estimate |
|------|---------------|-------------|
| `include/dshm_types.h` | Constants, superblock struct, per-process state, alloc result struct | ~80 |
| `src/dshm_atomic.h` | User-space wrappers: CAS, smp_mb, smp_rmb, cpu_relax, clock_ns | ~60 |
| `src/dshm_l1.c` | L1 allocator: local cache pop/push, slow-path CAS scan, cache refill, L1_free | ~180 |
| `src/dshm_l1.h` | L1 public API: `L1_alloc`, `L1_free`, `dshm_proc_state` extern | ~30 |
| `src/dshm_l2.c` | extent_hooks impl, `dshm_init`, `shared_malloc`, `shared_free` | ~200 |
| `src/dshm_l2.h` | L2 public API: `dshm_init`, `shared_malloc`, `shared_free` | ~25 |
| `tests/test_l1_cache.c` | Unit tests: local cache fast path | ~80 |
| `tests/test_l1_cas.c` | Unit tests: slow-path CAS allocation + free | ~120 |
| `tests/test_l1_concurrent.c` | Multi-threaded allocation stress test | ~100 |
| `tests/test_l2_init.c` | Unit tests: dshm_init bootstrap + validation | ~100 |
| `tests/test_l2_malloc.c` | Integration tests: shared_malloc/free through jemalloc | ~120 |
| `CMakeLists.txt` | Build system: library + tests + jemalloc linkage | ~60 |
| `tests/CMakeLists.txt` | Test target definitions | ~30 |

**Size check:** Largest file `src/dshm_l2.c` at ~200 LOC — well within the 250 LOC ceiling.

---

## Chunk 1: L1 — Distributed 1G Chunk Allocator

### Task 1: Project Skeleton & Build System

**Files:**
- Create: `CMakeLists.txt`
- Create: `include/dshm_types.h`
- Create: `src/dshm_atomic.h`

- [ ] **Step 1: Create CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
project(dshm_malloc C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings
add_compile_options(-Wall -Wextra -Werror -Wpedantic)

# Library target — start with L1 only; L2 (dshm_l2.c) is added in Task 5
add_library(dshm_malloc STATIC
    src/dshm_l1.c
)
target_include_directories(dshm_malloc PUBLIC include src)

# Tests — add_subdirectory deferred to Task 2 (when tests/CMakeLists.txt is created)
```

- [ ] **Step 2: Create `include/dshm_types.h` with constants and structs**

```c
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
};

#endif /* DSHM_TYPES_H */
```

- [ ] **Step 3: Create `src/dshm_atomic.h` with user-space atomic primitives**

```c
#ifndef DSHM_ATOMIC_H
#define DSHM_ATOMIC_H

#include <stdint.h>
#include <string.h>
#include <time.h>

/*
 * User-space wrappers for the kernel-style primitives used in the spec.
 * Maps smp_mb/CAS/cpu_relax to GCC __atomic builtins and sched_yield.
 */

/* 8-byte CAS. Returns 1 on success (old matched, value updated to `new_val`).
   Returns 0 on failure (old did not match). */
static inline int dshm_cas_u64(volatile uint64_t *ptr,
                               uint64_t old_val, uint64_t new_val)
{
    return __atomic_compare_exchange_n(ptr, &old_val, new_val, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* Convenience: CAS on a chunk_entry (8 bytes, treated as u64).
   Uses memcpy to avoid strict-aliasing UB (safe under -Wpedantic -Werror). */
static inline int dshm_cas_entry(volatile struct dshm_chunk_entry *entry,
                                 struct dshm_chunk_entry old_e,
                                 struct dshm_chunk_entry new_e)
{
    uint64_t old_u, new_u;
    memcpy(&old_u, &old_e, sizeof(old_u));
    memcpy(&new_u, &new_e, sizeof(new_u));
    volatile uint64_t *p = (volatile uint64_t *)entry;
    return dshm_cas_u64(p, old_u, new_u);
}

/* Plain atomic store on an 8-byte entry (for L1_free).
   Uses memcpy to avoid strict-aliasing UB. */
static inline void dshm_store_entry(volatile struct dshm_chunk_entry *entry,
                                    struct dshm_chunk_entry val)
{
    uint64_t val_u;
    memcpy(&val_u, &val, sizeof(val_u));
    volatile uint64_t *p = (volatile uint64_t *)entry;
    __atomic_store_n(p, val_u, __ATOMIC_RELEASE);
}

/* Release barrier (writer side, before publishing). */
static inline void dshm_smp_mb(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Acquire barrier (reader side, after observing a published value). */
static inline void dshm_smp_rmb(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

/* Spin-wait hint. */
static inline void dshm_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#else
    /* Fallback: no-op or sched_yield for non-x86/ARM. */
#endif
}

/* Monotonic clock in nanoseconds. */
static inline uint64_t dshm_clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#endif /* DSHM_ATOMIC_H */
```

- [ ] **Step 4: Verify CMake configures**

Run: `mkdir -p build && cd build && cmake .. 2>&1 | tail -5`
Expected: CMake configures successfully (generates Makefiles). `make` will fail because `src/dshm_l1.c` doesn't exist yet — that's expected; it's created in Task 2.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt include/dshm_types.h src/dshm_atomic.h
git commit -m "dshm: add project skeleton, types, and atomic primitives"
```

---

### Task 2: L1 Local Cache (Fast Path)

**Files:**
- Create: `src/dshm_l1.h`
- Create: `src/dshm_l1.c`
- Create: `tests/CMakeLists.txt`
- Create: `tests/test_l1_cache.c`

- [ ] **Step 1: Create `src/dshm_l1.h` — L1 public API**

```c
#ifndef DSHM_L1_H
#define DSHM_L1_H

#include "dshm_types.h"
#include <pthread.h>

struct dshm_proc_state {
    __u64 pool_base_va;
    __u64 pool_size;
    __u32 my_node_id;
    __u32 my_pid;
    unsigned arena_ind;  /* jemalloc arena index (set by L2) */

    /* Local cache */
    struct {
        pthread_spinlock_t lock;
        int cnt;
        int max;
        __u32 chunk_ids[];  /* flexible array, max elements */
    } cache;
};

/* Singleton, set by dshm_init(). */
extern struct dshm_proc_state *g_state;

/* L1 API */
struct dshm_chunk_alloc_result L1_alloc(void);
void L1_free(__u32 chunk_id);

/* Internal: init per-process state (called by dshm_init). */
int L1_init_state(__u64 pool_base_va, __u64 pool_size,
                  __u32 my_node_id, int local_cache_size);

#endif /* DSHM_L1_H */
```

- [ ] **Step 2: Create `tests/CMakeLists.txt` and enable testing in root CMakeLists.txt**

First, add these two lines to the root `CMakeLists.txt` (replacing the deferred comment):
```cmake
enable_testing()
add_subdirectory(tests)
```

Then create `tests/CMakeLists.txt`:
```cmake
# Test helpers: we use a simple C test harness, not GoogleTest, to avoid
# external dependencies. Each test file has a main() and returns 0 on success.
#
# Test targets are added incrementally as each task creates the corresponding
# test source file. This avoids CMake errors about missing source files.

# Task 2: L1 cache fast path
add_executable(test_l1_cache test_l1_cache.c)
target_link_libraries(test_l1_cache dshm_malloc pthread)
add_test(NAME l1_cache COMMAND test_l1_cache)

# Task 3 will append test_l1_cas here.
# Task 4 will append test_l1_concurrent here.
# Task 6 will append test_l2_init here.
# Task 7 will append test_l2_malloc here.
```

- [ ] **Step 3: Write the failing test `tests/test_l1_cache.c`**

This test verifies the fast path: cache is pre-loaded with a chunk, L1_alloc pops it without touching the global bitmap.

```c
#include "dshm_l1.h"
#include "dshm_atomic.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

int main(void)
{
    /* Allocate a real superblock in local memory (calloc'd = all entries FREE).
       pool_base_va points to the superblock itself, so the slow path can
       safely access sb->entries when the cache is empty. */
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
       Since we didn't init the superblock, entries are all zero (FREE),
       so it should find a chunk via CAS. This verifies fast→slow transition. */
    struct dshm_chunk_alloc_result r3 = L1_alloc();
    assert(r3.err == 0);
    /* chunk_id should be in range [0, DSHM_NUM_CHUNKS-1] */
    assert(r3.chunk_id < DSHM_NUM_CHUNKS);

    /* Cleanup */
    free(sb);
    free(g_state);
    g_state = NULL;

    printf("test_l1_cache: PASS\n");
    return 0;
}
```

- [ ] **Step 4: Run the test to verify it fails (no L1_alloc yet)**

Run: `cd build && cmake .. && make test_l1_cache 2>&1 | tail -5`
Expected: FAIL — `dshm_l1.c` doesn't exist, link error.

- [ ] **Step 5: Create `src/dshm_l1.c` — L1_alloc fast path + L1_free + L1_init_state**

```c
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

    g_state = malloc(sizeof(*g_state) +
                     (size_t)local_cache_size * sizeof(__u32));
    if (!g_state)
        return -ENOMEM;

    g_state->pool_base_va = pool_base_va;
    g_state->pool_size    = pool_size;
    g_state->my_node_id   = my_node_id;
    g_state->my_pid       = (uint32_t)getpid();
    g_state->cache.cnt    = 0;
    g_state->cache.max    = local_cache_size;
    pthread_spin_init(&g_state->cache.lock, 0);

    return 0;
}

/* Forward declaration for slow path (implemented in Task 3). */
static struct dshm_chunk_alloc_result L1_slow_path(void);

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
       Plain atomic store: we are the sole owner. */
    struct dshm_superblock *sb =
        (struct dshm_superblock *)(void *)g_state->pool_base_va;
    struct dshm_chunk_entry free_entry = { .owner_node = 0, .owner_pid = 0 };
    dshm_store_entry(&sb->entries[chunk_id], free_entry);
}

/* Placeholder slow path — replaced in Task 3. */
static struct dshm_chunk_alloc_result L1_slow_path(void)
{
    struct dshm_chunk_alloc_result r = { .err = -ENOMEM, .chunk_id = 0 };
    return r;
}
```

- [ ] **Step 6: Run the test — Tests 1-2 pass (fast path), Test 3 fails (placeholder slow path)**

Run: `cd build && cmake .. && make test_l1_cache && ./tests/test_l1_cache`
Expected: FAIL at Test 3 (`r3.err` is -ENOMEM from placeholder slow path, not 0). Tests 1-2 pass (fast path cache pop). This is expected TDD scaffolding — the real slow path is implemented in Task 3.

- [ ] **Step 7: Commit**

```bash
git add src/dshm_l1.h src/dshm_l1.c tests/CMakeLists.txt tests/test_l1_cache.c
git commit -m "dshm: add L1 local cache fast path + L1_free + init_state"
```

---

### Task 3: L1 Slow Path (Global CAS Bitmap)

**Files:**
- Modify: `src/dshm_l1.c` — replace `L1_slow_path` placeholder
- Create: `tests/test_l1_cas.c`
- Modify: `tests/CMakeLists.txt` (add `test_l1_cas` target)

- [ ] **Step 1: Add `test_l1_cas` target to `tests/CMakeLists.txt`**

Append to `tests/CMakeLists.txt`:
```cmake
# Task 3: L1 CAS slow path
add_executable(test_l1_cas test_l1_cas.c)
target_link_libraries(test_l1_cas dshm_malloc pthread)
add_test(NAME l1_cas COMMAND test_l1_cas)
```

- [ ] **Step 2: Write the failing test `tests/test_l1_cas.c`**

This test uses a real superblock in local memory (calloc'd, all entries zero = FREE), allocates chunks via L1_alloc, and verifies the bitmap is updated correctly.

```c
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
    sb->pool_base_va = (__u64)(void *)sb;  /* point to itself for the test */
    sb->pool_size = DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE;
    sb->chunk_size = DSHM_CHUNK_SIZE;
    sb->num_chunks = DSHM_NUM_CHUNKS;
    sb->max_nodes = DSHM_MAX_NODES;

    /* Init L1 state: pool_base_va points to the superblock. */
    int err = L1_init_state(sb->pool_base_va, sb->pool_size,
                            /*my_node_id=*/1, /*cache_size=*/2);
    assert(err == 0);

    /* Test 1: Allocate 1 chunk via slow path.
       With cache_size=2, the slow path allocates 1 chunk (returned to caller)
       AND refills the cache to max=2. After this:
       - r1.chunk_id is owned by us
       - cache.cnt == 2 (refilled by slow path) */
    struct dshm_chunk_alloc_result r1 = L1_alloc();
    assert(r1.err == 0);
    assert(r1.chunk_id < DSHM_NUM_CHUNKS);
    /* The allocated chunk should now be owned by us. */
    assert(sb->entries[r1.chunk_id].owner_node == 1);
    assert(sb->entries[r1.chunk_id].owner_pid == g_state->my_pid);
    /* Cache was refilled to max=2 by the slow path. */
    assert(g_state->cache.cnt == 2);

    /* Test 2: Drain the cache by popping both refilled chunks.
       This gives us 3 total allocated chunks (r1 + 2 from cache). */
    struct dshm_chunk_alloc_result r2 = L1_alloc();
    assert(r2.err == 0);
    assert(r2.chunk_id != r1.chunk_id);
    struct dshm_chunk_alloc_result r3 = L1_alloc();
    assert(r3.err == 0);
    assert(r3.chunk_id != r1.chunk_id);
    assert(r3.chunk_id != r2.chunk_id);
    /* Cache is now empty. */
    assert(g_state->cache.cnt == 0);

    /* Test 3: Free r1 — cache is empty, so it goes to local cache. */
    L1_free(r1.chunk_id);
    assert(g_state->cache.cnt == 1);

    /* Test 4: Allocate again — should come from cache (fast path), returning r1. */
    struct dshm_chunk_alloc_result r4 = L1_alloc();
    assert(r4.err == 0);
    assert(r4.chunk_id == r1.chunk_id);  /* same chunk, was in cache */
    assert(g_state->cache.cnt == 0);

    /* Test 5: Allocate many chunks to exhaust the cache and exercise slow path
       refill. */
    for (int i = 0; i < 10; i++) {
        struct dshm_chunk_alloc_result r = L1_alloc();
        assert(r.err == 0);
    }
    /* After 10 allocs, cache should be partially full (refilled during slow path). */
    assert(g_state->cache.cnt > 0);

    /* Test 6: Verify no two chunk_ids are the same (no double-alloc).
       Reinit with cache_size=0 to avoid refill complications. Mark all
       previously allocated chunks as seen, then allocate 100 more and verify
       no duplicates among the new allocations. */
    int *alloced = calloc(DSHM_NUM_CHUNKS, sizeof(int));
    /* Reinit g_state only (sb still has entries from Tests 1-5, which is fine —
       the slow path will skip occupied entries). cache_size=0 means no refill,
       so every alloc goes through CAS. */
    free(g_state);
    g_state = NULL;
    err = L1_init_state(sb->pool_base_va, sb->pool_size, 1, 0);
    assert(err == 0);
    for (int i = 0; i < 100; i++) {
        struct dshm_chunk_alloc_result r = L1_alloc();
        assert(r.err == 0);
        assert(r.chunk_id < DSHM_NUM_CHUNKS);
        assert(alloced[r.chunk_id] == 0);  /* no double-alloc */
        alloced[r.chunk_id] = 1;
    }

    /* Cleanup */
    free(alloced);
    free(sb);
    free(g_state);
    g_state = NULL;

    printf("test_l1_cas: PASS\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails (slow path returns ENOMEM)**

Run: `cd build && cmake .. && make test_l1_cas && ./tests/test_l1_cas`
Expected: FAIL — slow path is still the placeholder returning -ENOMEM.

- [ ] **Step 4: Implement `L1_slow_path` in `src/dshm_l1.c`**

Replace the placeholder `L1_slow_path` function with the real implementation:

```c
#define DSHM_SLOWPATH_MAX_CONSEC_FAIL 8

static struct dshm_chunk_alloc_result L1_slow_path(void)
{
    struct dshm_superblock *sb =
        (struct dshm_superblock *)(void *)g_state->pool_base_va;

    struct dshm_chunk_alloc_result result = { .err = -ENOMEM, .chunk_id = 0 };

    /* Random start point. */
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

        /* Quick check: if owner_node != 0, skip (occupied). */
        __u32 on = __atomic_load_n((volatile uint32_t *)&e->owner_node,
                                   __ATOMIC_RELAXED);
        if (on != 0)
            continue;

        /* Attempt CAS: {0, 0} → {my_node, my_pid}. */
        struct dshm_chunk_entry old_entry = { .owner_node = 0, .owner_pid = 0 };
        if (dshm_cas_entry(e, old_entry, new_entry)) {
            /* Record timestamp (debug, plain store). */
            sb->alloc_timestamps[idx] = dshm_clock_ns();
            allocated_id = idx;
            found = 1;
            break;
        }
        /* CAS failed: someone else grabbed it. Continue scanning. */
    }

    if (!found)
        return result;  /* -ENOMEM, all chunks occupied */

    /* Refill local cache: keep CAS-allocating until cache full
       or DSHM_SLOWPATH_MAX_CONSEC_FAIL consecutive failures. */
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
            __u32 on = __atomic_load_n((volatile uint32_t *)&e->owner_node,
                                       __ATOMIC_RELAXED);
            if (on != 0)
                continue;

            struct dshm_chunk_entry old_e = { .owner_node = 0, .owner_pid = 0 };
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
```

Also add `#include <stdlib.h>` at the top of `dshm_l1.c` if not already present (for `rand_r`).

- [ ] **Step 5: Run both L1 tests to verify they pass**

Run: `cd build && cmake .. && make test_l1_cache test_l1_cas && ./tests/test_l1_cache && ./tests/test_l1_cas`
Expected: Both print PASS.

- [ ] **Step 6: Commit**

```bash
git add src/dshm_l1.c tests/test_l1_cas.c tests/CMakeLists.txt
git commit -m "dshm: implement L1 slow-path CAS bitmap scan with cache refill"
```

---

### Task 4: L1 Concurrent Multi-Thread Stress Test

**Files:**
- Create: `tests/test_l1_concurrent.c`
- Modify: `tests/CMakeLists.txt` (add `test_l1_concurrent` target)

- [ ] **Step 1: Add `test_l1_concurrent` target to `tests/CMakeLists.txt`**

Append to `tests/CMakeLists.txt`:
```cmake
# Task 4: L1 concurrent multi-thread stress test
add_executable(test_l1_concurrent test_l1_concurrent.c)
target_link_libraries(test_l1_concurrent dshm_malloc pthread)
add_test(NAME l1_concurrent COMMAND test_l1_concurrent)
```

- [ ] **Step 2: Write the concurrent stress test**

```c
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
    __u32 *alloced_ids;   /* shared array, indexed by thread*ALLOCS+slot */
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

    /* Verify no duplicates across all threads. */
    int *seen = calloc(DSHM_NUM_CHUNKS, sizeof(int));
    int total = NTHREADS * ALLOCS_PER_THREAD;
    for (int i = 0; i < total; i++) {
        __u32 id = alloced[i];
        assert(id < DSHM_NUM_CHUNKS);
        assert(seen[id] == 0);  /* no double-alloc */
        seen[id] = 1;
    }

    free(seen);
    free(alloced);
    free(sb);
    free(g_state);
    g_state = NULL;

    printf("test_l1_concurrent: PASS (%d chunks, no duplicates)\n", total);
    return 0;
}
```

- [ ] **Step 3: Run the test**

Run: `cd build && cmake .. && make test_l1_concurrent && ./tests/test_l1_concurrent`
Expected: PASS — 400 chunks allocated across 8 threads, no duplicates.

- [ ] **Step 4: Commit**

```bash
git add tests/test_l1_concurrent.c tests/CMakeLists.txt
git commit -m "dshm: add L1 concurrent multi-thread stress test"
```

- [ ] **Step 5: Run full L1 test suite**

Run: `cd build && cmake .. && make && ctest --output-on-failure`
Expected: All 3 L1 tests pass.

---

## Chunk 2: L2 — jemalloc Integration & End-to-End

### Task 5: L2 — extent_hooks Implementation

**Files:**
- Create: `src/dshm_l2.h`
- Create: `src/dshm_l2.c`

- [ ] **Step 1: Create `src/dshm_l2.h` — public API**

```c
#ifndef DSHM_L2_H
#define DSHM_L2_H

#include <stdint.h>
#include <stddef.h>

/* Initialize the distributed shared-memory allocator.
 *
 * pool_base_va:   VA of the 128T shared pool (obtained by caller).
 * pool_size:       Must be 128T (DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE).
 * my_node_id:      1..255 (0 is reserved as FREE sentinel).
 * local_cache_size: Per-process local cache depth (default 4).
 *
 * Returns 0 on success, -EINVAL on bad args, -EBUSY if already initialized.
 */
int dshm_init(uint64_t pool_base_va, uint64_t pool_size,
              uint32_t my_node_id, int local_cache_size);

/* Allocate from the shared pool (routes to a dedicated jemalloc arena). */
void *shared_malloc(size_t size);

/* Free memory allocated by shared_malloc. */
void shared_free(void *ptr);

#endif /* DSHM_L2_H */
```

- [ ] **Step 2: Create `src/dshm_l2.c` — extent_hooks + dshm_init + API**

```c
#include "dshm_l2.h"
#include "dshm_l1.h"
#include "dshm_atomic.h"

#include <jemalloc/jemalloc.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

/* --- extent_hooks implementation --- */

static void *
dshm_extent_alloc(extent_hooks_t *hooks, void *new_addr, size_t size,
                  size_t alignment, bool *zero, bool *commit,
                  unsigned arena_ind)
{
    if (new_addr != NULL)
        return NULL;  /* Don't support fixed-address allocation */

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
        return false;  /* Not handled; jemalloc retains internally */

    uint64_t chunk_id = ((uint64_t)(uintptr_t)addr - g_state->pool_base_va)
                        / DSHM_CHUNK_SIZE;
    L1_free((__u32)chunk_id);
    return true;  /* Handled */
}

static bool
dshm_extent_commit(extent_hooks_t *hooks, void *addr, size_t size,
                   size_t offset, size_t length, unsigned arena_ind)
{
    return true;  /* Commit always succeeds */
}

static bool
dshm_extent_decommit(extent_hooks_t *hooks, void *addr, size_t size,
                     size_t offset, size_t length, unsigned arena_ind)
{
    return false;  /* Decommit not supported */
}

static bool
dshm_extent_purge_lazy(extent_hooks_t *hooks, void *addr, size_t size,
                       size_t offset, size_t length, unsigned arena_ind)
{
    return false;  /* Not purged */
}

static bool
dshm_extent_purge_forced(extent_hooks_t *hooks, void *addr, size_t size,
                         size_t offset, size_t length, unsigned arena_ind)
{
    return false;  /* Not purged */
}

static bool
dshm_extent_split(extent_hooks_t *hooks, void *addr, size_t size,
                  size_t size_a, size_t size_b, bool committed,
                  unsigned arena_ind)
{
    return false;  /* Allow split */
}

static bool
dshm_extent_merge(extent_hooks_t *hooks, void *addr_a, size_t size_a,
                  void *addr_b, size_t size_b, bool committed,
                  unsigned arena_ind)
{
    return false;  /* Allow merge */
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

/* --- dshm_init --- */

int dshm_init(uint64_t pool_base_va, uint64_t pool_size,
              uint32_t my_node_id, int local_cache_size)
{
    /* Validate inputs */
    if (pool_size == 0 || local_cache_size <= 0)
        return -EINVAL;
    if (my_node_id == 0 || my_node_id > DSHM_MAX_NODES)
        return -EINVAL;
    if (pool_size != (uint64_t)DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE)
        return -EINVAL;

    struct dshm_superblock *sb = (struct dshm_superblock *)(void *)pool_base_va;

    /* Superblock bootstrap (optimistic init) */
    if (sb->magic == DSHM_MAGIC) {
        /* Already initialized */
    } else if (dshm_cas_u64(&sb->magic, 0ULL, DSHM_INITIALIZING)) {
        /* Won the init race */
        sb->pool_base_va = pool_base_va;
        sb->pool_size    = pool_size;
        sb->chunk_size   = DSHM_CHUNK_SIZE;
        sb->num_chunks   = DSHM_NUM_CHUNKS;
        sb->max_nodes    = DSHM_MAX_NODES;
        memset(sb->entries, 0, sizeof(sb->entries));
        memset(sb->alloc_timestamps, 0, sizeof(sb->alloc_timestamps));
        dshm_smp_mb();  /* Release barrier */
        __atomic_store_n(&sb->magic, DSHM_MAGIC, __ATOMIC_RELEASE);
    } else {
        /* Lost the init race — wait for completion */
        while (__atomic_load_n(&sb->magic, __ATOMIC_ACQUIRE) != DSHM_MAGIC)
            dshm_cpu_relax();
    }

    /* Initialize per-process L1 state */
    int err = L1_init_state(pool_base_va, pool_size, my_node_id,
                            local_cache_size);
    if (err)
        return err;

    /* Create jemalloc arena with custom hooks */
    unsigned arena_ind;
    size_t sz = sizeof(arena_ind);
    int jerr = je_mallctl("arenas.create", &arena_ind, &sz, NULL, 0);
    if (jerr) {
        free(g_state);
        g_state = NULL;
        return -jerr;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "arena.%u.extent_hooks", arena_ind);
    const extent_hooks_t *hooks_ptr = &dshm_hooks;
    je_mallctl(cmd, NULL, NULL, (void *)&hooks_ptr, sizeof(hooks_ptr));

    g_state->arena_ind = arena_ind;
    return 0;
}

/* --- User-facing API --- */

void *shared_malloc(size_t size)
{
    return je_mallocx(size, MALLOCX_ARENA(g_state->arena_ind));
}

void shared_free(void *ptr)
{
    je_dallocx(ptr, 0);
}
```

- [ ] **Step 3: Update root `CMakeLists.txt` to link jemalloc**

Modify the library target in `CMakeLists.txt` to add jemalloc linkage:

```cmake
# Find jemalloc
find_package(PkgConfig REQUIRED)
pkg_check_modules(JEMALLOC REQUIRED jemalloc)

add_library(dshm_malloc STATIC
    src/dshm_l1.c
    src/dshm_l2.c
)
target_include_directories(dshm_malloc PUBLIC include src)
target_include_directories(dshm_malloc PRIVATE ${JEMALLOC_INCLUDE_DIRS})
target_link_libraries(dshm_malloc PUBLIC ${JEMALLOC_LIBRARIES} pthread)
```

- [ ] **Step 4: Verify it compiles (link errors for jemalloc are OK if not installed yet)**

Run: `cd build && cmake .. && make 2>&1 | tail -10`
Expected: Either succeeds (if jemalloc installed) or fails with jemalloc not found.

- [ ] **Step 5: Commit**

```bash
git add src/dshm_l2.h src/dshm_l2.c CMakeLists.txt
git commit -m "dshm: add L2 jemalloc extent_hooks adapter and dshm_init API"
```

---

### Task 6: L2 — dshm_init Bootstrap Tests

**Files:**
- Create: `tests/test_l2_init.c`

This test verifies `dshm_init` input validation and superblock bootstrap, without jemalloc (we test jemalloc integration separately because it requires a real shared pool).

- [ ] **Step 1: Add test to `tests/CMakeLists.txt`**

Append:
```cmake
# Test for L2 dshm_init validation (no jemalloc linkage needed for validation tests)
add_executable(test_l2_init test_l2_init.c)
target_link_libraries(test_l2_init dshm_malloc pthread)
add_test(NAME l2_init COMMAND test_l2_init)
```

- [ ] **Step 2: Write `tests/test_l2_init.c`**

```c
#include "dshm_l2.h"
#include "dshm_types.h"
#include "dshm_l1.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* Test dshm_init input validation without jemalloc.
   We test the validation paths that fail BEFORE jemalloc arena creation. */

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
    err = dshm_init(0x1000, 0x100000000ULL, 1, 4);  /* 4GB, not 128T */
    assert(err == -EINVAL);

    /* Test 6: Double-init rejection. We pre-set g_state to simulate
       an already-initialized state. We use a calloc'd superblock so the
       bootstrap code can safely read sb->magic (it will be 0, triggering
       the CAS init path, which writes DSHM_INITIALIZING then DSHM_MAGIC).
       After bootstrap, L1_init_state is called, which checks g_state != NULL
       and returns -EBUSY. */
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
```

- [ ] **Step 3: Run the test**

Run: `cd build && cmake .. && make test_l2_init && ./tests/test_l2_init`
Expected: PASS — all 6 validation checks pass.

- [ ] **Step 4: Commit**

```bash
git add tests/test_l2_init.c tests/CMakeLists.txt
git commit -m "dshm: add dshm_init input validation tests"
```

---

### Task 7: End-to-End Integration Test (shared_malloc/shared_free)

**Files:**
- Create: `tests/test_l2_malloc.c`

This is the full integration test: it creates a shared pool (via `mmap` with `MAP_SHARED|MAP_ANONYMOUS` and a huge VM address space), calls `dshm_init`, then uses `shared_malloc`/`shared_free` through jemalloc.

- [ ] **Step 1: Add test to `tests/CMakeLists.txt`**

Append:
```cmake
# Integration test: shared_malloc/shared_free through jemalloc
add_executable(test_l2_malloc test_l2_malloc.c)
target_link_libraries(test_l2_malloc dshm_malloc pthread)
add_test(NAME l2_malloc COMMAND test_l2_malloc)
```

- [ ] **Step 2: Write `tests/test_l2_malloc.c`**

```c
#include "dshm_l2.h"
#include "dshm_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

/* For the integration test, we use a smaller pool to avoid requiring
   128T of VA. We override DSHM_NUM_CHUNKS for this test by using
   a small mmap'd region and lowering num_chunks.

   However, since the spec hardcodes 128T/1G = 131072 chunks, the
   integration test requires a 128T mmap. On 64-bit Linux, this is
   possible with MAP_NORESERVE (VA-only, no physical commit).

   We use MAP_ANONYMOUS|MAP_SHARED|MAP_NORESERVE to get a 128T
   VA range without allocating physical memory. */

#define TEST_POOL_SIZE (DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE)  /* 128T */

int main(void)
{
    /* Try to mmap 128T with MAP_NORESERVE. This should succeed on 64-bit. */
    void *pool = mmap(NULL, TEST_POOL_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE,
                      -1, 0);
    if (pool == MAP_FAILED) {
        /* If 128T fails (e.g., ulimit -v), try a smaller pool. */
        fprintf(stderr, "test_l2_malloc: 128T mmap failed, trying 4GB\n");
        /* Fall back to a minimal test that just verifies dshm_init
           rejects the wrong pool_size. */
        assert(dshm_init(0, 0x100000000ULL, 1, 4) == -EINVAL);
        printf("test_l2_malloc: SKIP (128T mmap unavailable)\n");
        return 0;
    }

    /* Test 1: dshm_init on the real pool. */
    int err = dshm_init((uint64_t)pool, TEST_POOL_SIZE, 1, 4);
    assert(err == 0);

    /* Test 2: shared_malloc returns a valid pointer within the pool. */
    void *ptr1 = shared_malloc(4096);
    assert(ptr1 != NULL);
    assert((uint64_t)ptr1 >= (uint64_t)pool);
    assert((uint64_t)ptr1 < (uint64_t)pool + TEST_POOL_SIZE);

    /* Test 3: Write and read back. */
    memset(ptr1, 0xAB, 4096);
    unsigned char *bytes = ptr1;
    assert(bytes[0] == 0xAB);
    assert(bytes[4095] == 0xAB);

    /* Test 4: Multiple allocations. */
    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = shared_malloc(64 * 1024);  /* 64KB each */
        assert(ptrs[i] != NULL);
        memset(ptrs[i], i & 0xFF, 64 * 1024);
    }
    /* Verify data integrity. */
    for (int i = 0; i < 100; i++) {
        unsigned char *b = ptrs[i];
        assert(b[0] == (i & 0xFF));
        assert(b[64 * 1024 - 1] == (i & 0xFF));
    }

    /* Test 5: Free all and verify no crash. */
    for (int i = 0; i < 100; i++)
        shared_free(ptrs[i]);
    shared_free(ptr1);

    /* Cleanup: munmap the pool. g_state remains (leaked by design). */
    munmap(pool, TEST_POOL_SIZE);

    printf("test_l2_malloc: PASS\n");
    return 0;
}
```

- [ ] **Step 3: Run the test**

Run: `cd build && cmake .. && make test_l2_malloc && ./tests/test_l2_malloc`
Expected: PASS — 101 allocations, data integrity verified.

- [ ] **Step 4: Commit**

```bash
git add tests/test_l2_malloc.c tests/CMakeLists.txt
git commit -m "dshm: add end-to-end integration test for shared_malloc/free"
```

---

### Task 8: Full Test Suite Run & Final Commit

- [ ] **Step 1: Run all tests**

Run: `cd build && cmake .. && make && ctest --output-on-failure`
Expected: All 5 tests pass:
- `l1_cache`
- `l1_cas`
- `l1_concurrent`
- `l2_init`
- `l2_malloc` (or SKIP if 128T mmap unavailable)

- [ ] **Step 2: Verify no compiler warnings**

Run: `cd build && make 2>&1 | grep -i warning`
Expected: No output (no warnings).

- [ ] **Step 3: Final commit (if any changes)**

```bash
git status  # Should show "nothing to commit, working tree clean"
# If there are uncommitted changes, stage only the intended files:
# git add <specific files>
```

- [ ] **Step 4: Tag the milestone**

```bash
git tag v0.1.0
```

---

## Summary

| Task | Component | Files | Tests | Steps |
|------|-----------|-------|-------|-------|
| 1 | Project skeleton | 3 | 0 | 5 |
| 2 | L1 fast path | 4 | 1 | 7 |
| 3 | L1 slow path (CAS) | 2 | 1 | 5 |
| 4 | L1 concurrent test | 1 | 1 | 4 |
| 5 | L2 extent_hooks | 3 | 0 | 5 |
| 6 | L2 init validation | 2 | 1 | 4 |
| 7 | L2 integration | 2 | 1 | 4 |
| 8 | Full suite | 0 | 0 | 4 |
| **Total** | | **~17** | **5** | **~38** |

## Execution Notes

- **TDD discipline**: Every task writes the failing test FIRST, verifies it fails, then implements.
- **Frequent commits**: Every task ends with a commit. No large uncommitted changes.
- **File size**: All files are under 250 LOC. `dshm_l1.c` is the largest at ~180 LOC.
- **jemalloc dependency**: Task 5+ requires jemalloc dev headers. Install via `apt install libjemalloc-dev` or equivalent.
- **128T mmap**: The integration test (Task 7) uses `MAP_NORESERVE` to get 128T VA without physical memory. If `ulimit -v` blocks this, the test gracefully skips.
