# Distributed Chunk Allocator Design

**Date**: 2026-07-14
**Status**: Design
**Author**: Sisyphus (OhMyOpenCode)

## 1. Problem Statement

A 128TB shared GVA (Global Virtual Address) pool is visible to all processes on all nodes
in a multi-node, multi-process system. Each node's process can independently mmap the same
GVA range and see identical data (the underlying memory layer guarantees physical page
coherence across nodes — this is assumed, not implemented here).

We need a **malloc-like allocator** that:

1. Allocates **1GB-granularity chunks** from the shared 128TB pool, coordinated across all
   processes on all nodes (distributed, decentralized, no daemon).
2. Feeds those 1GB chunks to **jemalloc** (via its native `extent_hooks_t` interface),
   which performs fine-grained sub-allocation internally.
3. Distinguishes "shared pool allocations" from "local heap allocations" at the API level.

The allocator does **not** manage physical pages, NUMA affinity, cache coherence, or
cross-node crash recovery. Those are delegated to lower layers or the application.

## 2. Design Decisions

| # | Decision Point | Choice | Rationale |
|---|---|---|---|
| 1 | GVA segment acquisition | Setter injection | Application provides `dshm_init(pool_va, size, node_id, cache_size)` early at startup |
| 2 | Instance granularity | Per-process, no daemon | Each process runs its own allocator instance; no daemon process |
| 3 | Chunk owner identity | `node_id + pid` | Identifies who allocated the chunk; for labeling/debugging only |
| 4 | Process exit hook | None | No automatic reclamation on process exit; jemalloc does not call `extent_dalloc` on exit |
| 5 | Metadata location | Pool head (shared VA) | Superblock at chunk 0, ~2MB, visible to all processes |
| 6 | Slow-path scan strategy | Random-start linear scan | Avoid CAS contention at pool head; 131072 entries, random offset |
| 7 | jemalloc integration | Native `extent_hooks_t` | Non-invasive; jemalloc's extent metadata already lives in process-local heap (rtree + `edata_t`), not in chunk |
| 8 | Extent metadata location | Process-local heap | jemalloc default behavior — confirmed by source analysis (see Appendix A) |
| 9 | Physical page management | Not handled | Delegated to lower layer (OBMM or similar) |
| 10 | Reclamation strategy | Explicit free only | `extent_dalloc` callback → `L1_free`; process exit does NOT reclaim; leaks accepted |
| 11 | Cross-node stale detection | None | No automatic detection; no heartbeat |
| 12 | OS-restart leaks | Not handled | Accepted; manual cleanup or application-level recovery |
| 13 | Intra-chunk fine-grained slicing | jemalloc-managed | `extent_alloc` returns 1GB; jemalloc splits internally |
| 14 | Local cache size | Configurable | Default 4 chunks (4GB); tunable via setter parameter |
| 15 | API style | jemalloc native + distinct arena | `shared_malloc`/`shared_free` route to a dedicated arena |
| 16 | Timestamp in metadata | Debug only | `alloc_timestamp` recorded at allocation; not used in reclamation logic |
| 17 | L1 data structure | Single-layer CAS bitmap | 131072 entries of `{owner_node, owner_pid}`; upgradable to two-level if needed |

## 3. Architecture

```
┌────────────────────────────────────────────────────────────────┐
│  User Process                                                  │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Application Code                                        │  │
│  │    shared_malloc(size)  ↔  shared_free(ptr)             │  │
│  │    (routes to dshm arena, distinct from local malloc)    │  │
│  └────────────────────┬─────────────────────────────────────┘  │
│                       │                                          │
│  ┌────────────────────▼─────────────────────────────────────┐  │
│  │  libdshm_malloc.so  (user-space library)                 │  │
│  │  ┌────────────────────────────────────────────────────┐   │  │
│  │  │  L2: jemalloc extent_hooks adapter                  │   │  │
│  │  │    extent_alloc  → L1_alloc(1G) → return VA        │   │  │
│  │  │    extent_dalloc → L1_free(chunk_id)               │   │  │
│  │  │    (extent metadata in jemalloc rtree, process-local)│   │  │
│  │  └────────────────────────┬───────────────────────────┘   │  │
│  │  ┌────────────────────────▼───────────────────────────┐   │  │
│  │  │  L1: Distributed 1G chunk allocator (user-space)    │   │  │
│  │  │  ┌─────────────────────────────────────────────┐    │   │  │
│  │  │  │  fast path: per-process local cache          │    │   │  │
│  │  │  │    (configurable, default 4 × 1G chunks)     │    │   │  │
│  │  │  └──────────────────┬──────────────────────────┘    │   │  │
│  │  │                     │ miss                          │   │  │
│  │  │  ┌──────────────────▼──────────────────────────┐    │   │  │
│  │  │  │  slow path: global CAS bitmap                 │    │   │  │
│  │  │  │    random-start scan of 131072 entries        │    │   │  │
│  │  │  └─────────────────────────────────────────────┘    │   │  │
│  │  └─────────────────────────────────────────────────────┘   │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  Shared GVA Pool 128TB                                   │  │
│  │  ┌───────────────────────────────────────────────────┐   │  │
│  │  │  chunk 0: metadata (superblock + entries[131072])│   │  │
│  │  │           each entry: {owner_node, owner_pid,     │   │  │
│  │  │                      alloc_timestamp (debug)}     │   │  │
│  │  ├───────────────────────────────────────────────────┤   │  │
│  │  │  chunk 1..131071: allocatable 1G chunks           │   │  │
│  └──────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────┘

Not in architecture:
  ✗ Kernel device (/dev/dshm) — removed, no fd-release hook needed
  ✗ Heartbeat mechanism — removed, no automatic reclamation
  ✗ Daemon — per-process, no daemon
  ✗ Cross-node stale detection — no automatic reclamation
```

### Layer Responsibilities

- **L0 (Shared GVA Pool)**: 128TB shared VA + pool-head metadata. Pure data area; does not
  coordinate allocation.
- **L1 (Distributed 1G Chunk Allocator)**: Per-process instance. Fast path uses local cache;
  slow path uses global CAS on shared bitmap. No daemon.
- **L2 (jemalloc extent_hooks Adapter)**: `extent_alloc` calls L1 to get a 1GB chunk and
  returns it to jemalloc. jemalloc manages fine-grained slicing internally. Extent metadata
  stays in jemalloc's internal rtree (process-local).

## 4. Data Structures

### 4.1 Superblock (at chunk 0)

```c
#define DSHM_NUM_CHUNKS    131072       /* 128T / 1G */
#define DSHM_MAX_NODES     256
#define DSHM_CHUNK_SIZE    0x40000000ULL  /* 1G = 1 << 30 */
#define DSHM_MAGIC         0x4453484d4d43424cULL  /* "DSHMMCBL" */
#define DSHM_INITIALIZING  0x494e4954494e4954ULL  /* "INITINIT" */

struct dshm_superblock {
    /* --- Read-only fields, initialized once at bootstrap --- */
    __u64 magic;                  /* DSHM_MAGIC when ready, DSHM_INITIALIZING during init */
    __u64 pool_base_va;           /* 128T base VA */
    __u64 pool_size;               /* 128T = 0x800000000000 */
    __u64 chunk_size;              /* 1G = 0x40000000 */
    __u32 num_chunks;              /* 131072 */
    __u32 max_nodes;               /* 256 */
    __u8  pad[216];                /* pad to 256 bytes (40 + 216 = 256) */

    /* --- Chunk entries: 8 bytes each, CAS unit --- */
    struct dshm_chunk_entry {
        __u32 owner_node;           /* 0 = FREE, 1..255 = allocator's node_id */
        __u32 owner_pid;            /* allocator's OS pid */
    } entries[DSHM_NUM_CHUNKS];     /* 1MB total */

    /* --- Alloc timestamps (debug only, not in CAS path) --- */
    __u64 alloc_timestamps[DSHM_NUM_CHUNKS];  /* 1MB, CLOCK_MONOTONIC at alloc time */

    /* Total metadata: ~2MB (256B header + 1MB entries + 1MB timestamps),
       fits well within chunk 0 (1GB) */
};
```

### Design Notes

1. **CAS unit is 8 bytes** (`dshm_chunk_entry`): `{owner_node, owner_pid}`. `owner_node == 0`
   means FREE. Allocation CAS: `{0, 0}` → `{my_node, my_pid}`.
2. **Timestamp is NOT in the CAS unit**: It lives in a separate array. After successful CAS,
   a plain store records the timestamp. A racing reader may see a stale timestamp, which is
   acceptable for debug purposes.
 3. **Chunk 0 uses ~2MB** of its 1GB capacity. The remaining ~1022MB is idle, reserved for
   future metadata expansion.
4. **`DSHM_INITIALIZING` magic**: Intermediate state during bootstrap. Readers spin-wait
   until magic becomes `DSHM_MAGIC`.

### 4.2 Per-Process State

```c
#define DSHM_DEFAULT_CACHE_SIZE  4

struct dshm_proc_state {
    __u64 pool_base_va;
    __u64 pool_size;
    __u32 my_node_id;
    __u32 my_pid;
    unsigned arena_ind;               /* jemalloc arena index */

    /* Local cache */
    struct {
        pthread_spinlock_t lock;     /* multi-thread protection within process */
        int cnt;
        int max;                      /* configurable, default 4 */
        __u32 chunk_ids[];            /* max elements */
    } cache;
};

static struct dshm_proc_state *g_state;  /* singleton, set by dshm_init() */

/* L1_alloc return type */
struct dshm_chunk_alloc_result {
    int  err;       /* 0 on success, -ENOMEM if pool exhausted */
    __u32 chunk_id; /* valid when err == 0 */
};
```

## 5. L1 Allocation/Free Protocol

### 5.1 Allocation (Two-Level)

```
L1_alloc():
  ┌─ Fast path: per-process local cache ──────────────────┐
  │                                                        │
  │  lock(cache.lock)                                      │
  │  if (cache.cnt > 0):                                  │
  │      chunk_id = cache.chunk_ids[--cache.cnt]          │
  │      unlock(cache.lock)                                │
  │      VA = pool_base_va + chunk_id * 1G                │
  │      return {chunk_id, VA}                            │
  │  unlock(cache.lock)                                    │
  └────────────────────────────────────────────────────────┘
                    │ cache empty
                    ▼
  ┌─ Slow path: global CAS bitmap ─────────────────────────┐
  │                                                        │
  │  start = random() % DSHM_NUM_CHUNKS                   │
  │  for i in 0..DSHM_NUM_CHUNKS:                          │
  │      idx = (start + i) % DSHM_NUM_CHUNKS              │
  │      retry:                                            │
  │          old = read entries[idx]      // expect {0, 0}│
  │          if (old.owner_node != 0):                    │
  │              continue     // occupied, skip            │
  │          new = {my_node, my_pid}                      │
  │          if (CAS(&entries[idx], old, new)):           │
  │              alloc_timestamps[idx] = clock()          │
  │              // Return this chunk to caller            │
  │              // Then refill local cache:               │
  │              //   keep CAS-allocating until cache full │
  │              //   or 8 consecutive CAS failures       │
  │              while (cache.cnt < cache.max              │
  │                     && consec_fail < 8):               │
  │                  find next FREE slot via scan          │
  │                  if CAS success:                      │
  │                      push to cache                     │
  │                      consec_fail = 0                   │
  │                  else:                                 │
  │                      consec_fail++                     │
  │              return {idx, VA}                          │
  │          else:                                         │
  │              goto retry   // CAS failed, retry idx     │
  │                                                        │
  │  return -ENOMEM  // all 131072 chunks occupied         │
  └────────────────────────────────────────────────────────┘
```

**Cache refill strategy**: When slow path succeeds, it continues CAS-allocating chunks until
the local cache is full or N (default 8) consecutive CAS failures occur. This amortizes the
CAS cost: one slow-path entry preheats the cache.

### 5.2 Free

```
L1_free(chunk_id):
    // Try to return to local cache first
    lock(cache.lock)
    if (cache.cnt < cache.max):
        cache.chunk_ids[cache.cnt++] = chunk_id
        unlock(cache.lock)
        return

    // Cache full → release to global bitmap
    unlock(cache.lock)
    entries[chunk_id] = {0, 0}   // owner_node=0 = FREE
    // Note: plain store, NOT CAS.
    // We are the sole owner of this chunk; no concurrent freeer.
    // Safe on ARM64: dshm_chunk_entry is 8-byte aligned (array of
    // __u32+__u32 in a struct, naturally 8-byte aligned within the
    // superblock). ARM64 guarantees atomicity of aligned 8-byte stores.
    // The store is visible to other processes via cache coherence;
    // their CAS will observe the updated value.
```

**Why free uses plain store, not CAS**: The freeer is the chunk's sole owner (the process
that won the allocation CAS). No other process can simultaneously free a chunk it doesn't
own. If the application erroneously frees another process's chunk, that's an application bug,
not an allocator concern.

### 5.3 Thread Safety

- **Local cache**: Protected by `pthread_spinlock_t`. Within a process, multiple threads
  share one cache.
- **Global CAS**: Naturally thread-safe across processes and nodes. ARM64 LSE atomics
  provide 8-byte CAS natively.
- **No cross-process lock**: All coordination is via CAS on the shared bitmap. No mutexes,
  no futexes, no daemon.

## 6. L2: jemalloc Integration

### 6.1 extent_hooks Implementation

```c
static void *
dshm_extent_alloc(extent_hooks_t *hooks, void *new_addr, size_t size,
                  size_t alignment, bool *zero, bool *commit,
                  unsigned arena_ind)
{
    if (new_addr != NULL)
        return NULL;   /* We don't support fixed-address allocation */

    /* size is typically 4MB (jemalloc default lg_chunk=22).
       We always allocate 1G and let jemalloc split internally. */

    struct dshm_chunk_alloc_result r = L1_alloc();
    if (r.err)
        return NULL;

    void *addr = (void *)(g_state->pool_base_va + r.chunk_id * DSHM_CHUNK_SIZE);

    /* 1G is naturally 1G-aligned, satisfies jemalloc's alignment requirement. */
    *zero = false;   /* Let jemalloc handle zeroing (pages may be reused) */
    *commit = true;  /* Always committed */

    return addr;
}

static bool
dshm_extent_dalloc(extent_hooks_t *hooks, void *addr, size_t size,
                   bool committed, unsigned arena_ind)
{
    /* jemalloc may split the 1G chunk into smaller extents.
       When dalloc is called with size < 1G, we retain (don't free).
       Only when size == 1G (the original alloc size) do we return to L1. */

    if (size < DSHM_CHUNK_SIZE)
        return false;  /* "Not handled" — jemalloc will call destroy, which is NULL,
                           so jemalloc retains the extent internally. This is the desired
                           behavior: the sub-1G extent stays tracked by jemalloc's
                           split/merge mechanism and will be coalesced back to 1G
                           before the next dalloc call. */

    __u64 chunk_id = ((__u64)addr - g_state->pool_base_va) / DSHM_CHUNK_SIZE;
    L1_free(chunk_id);
    return true;   /* Handled, jemalloc won't call destroy */
}

/* commit: true = success, false = failure.
   Pages are always committed (physical pages managed by lower layer).
   Return true to indicate "commit handled — pages are committed." */
static bool
dshm_extent_commit(extent_hooks_t *hooks, void *addr, size_t size,
                   size_t offset, size_t length, unsigned arena_ind)
{
    return true;   /* Commit always succeeds; physical pages not our concern */
}

/* decommit: true = success (pages decommitted), false = failure (pages still committed).
   We don't support decommit. Return false to tell jemalloc "decommit failed,
   pages remain committed." This is the desired behavior. */
static bool
dshm_extent_decommit(extent_hooks_t *hooks, void *addr, size_t size,
                     size_t offset, size_t length, unsigned arena_ind)
{
    return false;  /* Decommit not supported; pages stay committed */
}

/* purge_lazy: true = purged, false = not purged.
   We don't purge. Return false so jemalloc knows pages are not purged
   and will zero-fill on next use if needed. */
static bool
dshm_extent_purge_lazy(extent_hooks_t *hooks, void *addr, size_t size,
                       size_t offset, size_t length, unsigned arena_ind)
{
    return false;  /* Not purged; jemalloc will handle zeroing on reuse */
}

static bool
dshm_extent_purge_forced(extent_hooks_t *hooks, void *addr, size_t size,
                         size_t offset, size_t length, unsigned arena_ind)
{
    return false;  /* Not purged */
}

/* split: true = split failed (refused), false = split succeeded (allowed).
   We allow jemalloc to split the 1G chunk internally. */
static bool
dshm_extent_split(extent_hooks_t *hooks, void *addr, size_t size,
                  size_t size_a, size_t size_b, bool committed,
                  unsigned arena_ind)
{
    return false;  /* Allow split */
}

/* merge: true = merge failed (refused), false = merge succeeded (allowed).
   We allow jemalloc to merge adjacent sub-extents back. */
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
    .destroy       = NULL,   /* Safe: dalloc always returns true (handled) for 1G,
                                and returns false (not handled) for sub-1G which means
                                jemalloc retains internally without calling destroy.
                                destroy is never invoked. */
    .commit        = dshm_extent_commit,
    .decommit      = dshm_extent_decommit,
    .purge_lazy    = dshm_extent_purge_lazy,
    .purge_forced  = dshm_extent_purge_forced,
    .split         = dshm_extent_split,
    .merge         = dshm_extent_merge,
};
```

### 6.2 Initialization (Setter API)

```c
int dshm_init(__u64 pool_base_va, __u64 pool_size,
              __u32 my_node_id, int local_cache_size)
{
    /* Validate inputs */
    if (pool_size == 0 || local_cache_size <= 0)
        return -EINVAL;

    /* node_id=0 is reserved as the FREE sentinel in chunk entries.
       Valid node IDs are 1..DSHM_MAX_NODES (1..255). */
    if (my_node_id == 0 || my_node_id > DSHM_MAX_NODES)
        return -EINVAL;

    /* The superblock's entries array is statically sized to DSHM_NUM_CHUNKS
       (131072 = 128T / 1G). The caller must provide a pool_size consistent
       with this. */
    if (pool_size != DSHM_NUM_CHUNKS * DSHM_CHUNK_SIZE)
        return -EINVAL;

    struct dshm_superblock *sb = (void *)pool_base_va;

    /* --- Superblock bootstrap (optimistic init) --- */
    if (sb->magic == DSHM_MAGIC) {
        /* Already initialized */
    } else if (CAS(&sb->magic, 0ULL, DSHM_INITIALIZING)) {
        /* Won the init race — I am the bootstrap process */
        sb->pool_base_va = pool_base_va;
        sb->pool_size    = pool_size;
        sb->chunk_size   = DSHM_CHUNK_SIZE;   /* 1G */
        sb->num_chunks   = DSHM_NUM_CHUNKS;
        sb->max_nodes    = DSHM_MAX_NODES;
        memset(sb->entries, 0, sizeof(sb->entries));
        memset(sb->alloc_timestamps, 0, sizeof(sb->alloc_timestamps));
        smp_mb();   /* Release barrier: ensure all writes are visible before magic */
        sb->magic = DSHM_MAGIC;
    } else {
        /* Lost the init race — wait for completion */
        while (sb->magic != DSHM_MAGIC)
            cpu_relax();
        smp_rmb();  /* Acquire barrier: ensure we see all fields after magic */
    }

    /* --- Initialize per-process state ---
       dshm_init must be called exactly once per process. A second call
       would leak g_state and create a duplicate jemalloc arena. */
    if (g_state != NULL)
        return -EBUSY;  /* Already initialized */

    g_state = malloc(sizeof(*g_state) + local_cache_size * sizeof(__u32));
    g_state->pool_base_va = pool_base_va;
    g_state->pool_size    = pool_size;
    g_state->my_node_id   = my_node_id;
    g_state->my_pid       = getpid();
    g_state->cache.cnt    = 0;
    g_state->cache.max    = local_cache_size;
    pthread_spin_init(&g_state->cache.lock, 0);

    /* --- Create jemalloc arena with custom hooks --- */
    unsigned arena_ind;
    size_t sz = sizeof(arena_ind);
    int err = je_mallctl("arenas.create", &arena_ind, &sz, NULL, 0);
    if (err)
        return -err;

    /* Install extent_hooks on the new arena */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "arena.%u.extent_hooks", arena_ind);
    extent_hooks_t *hooks = (extent_hooks_t *)&dshm_hooks;
    je_mallctl(cmd, NULL, NULL, (void *)&hooks, sizeof(hooks));

    g_state->arena_ind = arena_ind;
    return 0;
}
```

### 6.3 User-Facing API

```c
void *shared_malloc(size_t size)
{
    return je_mallocx(size, MALLOCX_ARENA(g_state->arena_ind));
}

void shared_free(void *ptr)
{
    je_dallocx(ptr, 0);   /* jemalloc routes to correct arena automatically */
}
```

**API separation**: `shared_malloc`/`shared_free` route to a dedicated jemalloc arena
with custom `extent_hooks`. Standard `malloc`/`free` continue to use the default arena
with system mmap. This satisfies the requirement that shared-pool allocations be
distinguishable from local-heap allocations.

## 7. Complete Data Flow

### 7.1 malloc → free Lifecycle

```
1. Process startup:
   dshm_init(pool_va, 128T, node_id=2, cache_size=4)
     → Bootstrap/check superblock
     → Create jemalloc arena, bind extent_hooks
     → Initialize local cache (empty)

2. User calls shared_malloc(64KB):
   → mallocx(64KB, MALLOCX_ARENA(dshm_arena))
   → jemalloc looks for free extent in arena
   → No available extent → calls extent_alloc(size=4MB, align=4MB)
   → dshm_extent_alloc:
       → L1_alloc()
       → Fast path: local cache has chunks → pop one
       → VA = pool_base + chunk_id * 1G
       → return VA (1G!)
   → jemalloc receives 1G, splits into 4MB extents internally
   → jemalloc slices 64KB from a 4MB extent
   → Returns ptr to user

3. User reads/writes via ptr (within the 1G chunk)

4. User calls shared_free(ptr):
   → dallocx(ptr, 0)
   → jemalloc marks 64KB as free
   → (extent not immediately released)

5. Over time, all 64KB blocks within a 4MB extent become free
   → jemalloc coalesces into a free extent

6. All 4MB extents within the 1G chunk become free
   → jemalloc calls extent_dalloc(addr, size=1G)
   → dshm_extent_dalloc:
       → size == 1G → L1_free(chunk_id)
       → chunk returned to local cache or global bitmap

7. Process exits:
   → jemalloc's atexit handler flushes stats, releases TSD
   → jemalloc does NOT call extent_dalloc on exit
   → Chunk entries still show owner = {node2, pid}
   → OS munmaps the process's VA mapping
   → Chunk VA slots remain occupied (leaked) — by design
```

### 7.2 Boundary Conditions

**1. jemalloc extent size vs 1G**: jemalloc's default `lg_chunk=22` (4MB). Our
`extent_alloc` returns 1G, jemalloc splits internally. jemalloc's max alignment is 4MB;
1G natural alignment satisfies this.

**2. `extent_alloc` with `new_addr != NULL`**: jemalloc may pass a specific address
(retain scenario). We return NULL (unsupported). jemalloc falls back to `new_addr=NULL`.

**3. Multi-threading within a process**: Local cache protected by spinlock. Global CAS
is naturally thread-safe. No cross-process locks.

**4. `extent_dalloc` with size < 1G**: jemalloc split the chunk; dalloc is for a sub-extent.
We return `false` (not handled) without calling `L1_free`. Since `destroy` is NULL, jemalloc
retains the sub-extent internally. The sub-extent stays tracked by jemalloc's split/merge
mechanism and will be coalesced back to 1G before the next full-chunk dalloc call.

**5. Process death (normal or abnormal)**: No reclamation. Chunk entries retain the dead
process's `{node_id, pid}`. VA slots are permanently occupied. Application must call
`L1_free` explicitly or accept the leak.

**6. Node OS restart**: Same as process death — no reclamation. All chunks owned by
processes on that node are leaked.

## 8. Out of Scope

The following are explicitly NOT handled by this design:

- **Physical page management**: How physical pages back the GVA range, NUMA placement,
  page fault handling — delegated to the lower layer (OBMM or similar).
- **NUMA affinity**: Chunk allocation does not consider which node's physical memory backs
  the VA. Future enhancement.
- **Cross-node crash detection/recovery**: No heartbeat, no stale detection, no automatic
  reclamation. Dead processes' chunks leak.
- **Cache coherence**: Assumed that the lower layer guarantees cross-node cache coherence
  for the shared GVA pool.
- **GVA segment mmap mechanism**: The setter `dshm_init(pool_base_va, ...)` assumes the
  caller has already obtained the shared GVA range. How it was mmap'd is the caller's concern.
- **jemalloc internal fine-grained management**: jemalloc handles all sub-1G slicing, splitting,
  merging, and purging. We only provide 1G chunks.
- **Persistence**: Allocation state is ephemeral. On full pool reset (all processes exit,
  superblock is reinitialized), all allocation state is lost.

## Appendix A: jemalloc Extent Metadata Storage

Source-level analysis confirms that jemalloc's extent metadata **already lives outside the
chunk by default** — no configuration is needed.

### A.1 Where extent metadata lives

Two locations, both process-local:

1. **Radix tree (`rtree_t` inside `emap_t`)**: A global address→metadata index. Maps
   `vaddr → (edata_t*, metadata)`. Allocated via `base_alloc_rtree()` from the `base`
   allocator. NOT in the chunk.

   Source: `include/jemalloc/internal/rtree.h` (jemalloc dev branch, commit `81034ce1`):
   ```c
   /* This radix tree ... is for associating metadata with extents
      that are currently owned by jemalloc. */
   struct rtree_leaf_elm_s {
       atomic_p_t le_edata;       /* (edata_t *) */
       atomic_u_t le_metadata;
   };
   ```

2. **`edata_t` extent descriptors**: 128-byte structs containing `e_bits`, `e_addr`,
   `e_size_esn`, `e_sn`, etc. Allocated via `base_alloc_edata()` from the `base` allocator.
   NOT in the chunk.

   Source: `include/jemalloc/internal/edata.h`:
   ```c
   /* sizeof(edata_t) is 128 bytes on 64-bit architectures. */
   struct edata_s {
       uint64_t e_bits;
       void *e_addr;         /* Pointer to the extent (our 1G chunk VA) */
       union { size_t e_size_esn; size_t e_bsize; };
       hpdata_t *e_ps;
       uint64_t e_sn;
       /* ... */
   };
   ```

### A.2 `base` allocator: two hook sets

The `base` allocator has **separate hook sets** for user extents and metadata:

```c
struct base_s {
    ehooks_t ehooks;        /* User-configurable extent hooks (our 1G chunks) */
    ehooks_t ehooks_base;   /* Hooks for metadata allocations (rtree, edata_t) */
    size_t edata_allocated;
    size_t rtree_allocated;
    /* ... */
};
```

By default, `ehooks_base` uses plain mmap. This means **metadata allocations stay in
process-local heap**, completely decoupled from the shared 1G chunks. When the owner
process dies, metadata vanishes with the process heap — the shared chunk contains no
metadata to clean up.

### A.3 Implication for this design

- **Custom `extent_hooks_t->alloc`**: Returns a VA from the 128T shared pool. jemalloc
  wraps it in a fresh `edata_t` (process-local) and inserts into the rtree (process-local).
- **Process death**: Process-local `edata_t` and rtree entries vanish with the process heap.
  The 1G chunk in the shared pool is untouched (no metadata was written into it).
- **Chunk reuse**: A new process can allocate the same 1G VA (after `L1_free`). jemalloc
  builds a *fresh* `edata_t` and *fresh* rtree entries. No cross-process metadata corruption.

### A.4 Version info

- Analysis based on jemalloc `dev` branch (post-5.3.0).
- `emap` module introduced in PR #1761 (Feb 2020).
- Extent-state tracking in rtree leaves added in PR #2037.
- The metadata-stays-external design has been true since at least jemalloc 4.x.
