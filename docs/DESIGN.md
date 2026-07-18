# dshm-alloc 设计文档

> 本文档描述 `dshm-alloc` 代码仓当前实现的设计（截至 2026-07-18），
> 部分内容与 `docs/superpowers/specs/2026-07-14-distributed-chunk-allocator-design.md`
> 的原始设计稿存在差异（见 [§11 差异说明](#11-与原始设计稿的差异说明)）。

## 1. 问题背景与目标

在多节点、多进程系统中，存在一个对**所有节点所有进程**可见的 128TB 共享 GVA（Global Virtual Address）池。底层内存层保证跨节点物理页一致性（本文不实现，视为前提）。

目标：在共享池之上构建一个 **malloc 风格的分配器**，满足：

1. **粗粒度协调分配**：从 128TB 池中以 1GB 粒度分配 chunk，跨节点跨进程协调，**去中心化、无 daemon**。
2. **喂给 jemalloc**：将 1GB chunk 通过 jemalloc 原生 `extent_hooks_t` 接口供给，由 jemalloc 在内部完成细粒度子分配。
3. **API 区分**：在接口层区分"共享池分配"与"本地堆分配"。

**不在范围内**：物理页管理、NUMA 亲和、缓存一致性、跨节点崩溃恢复 —— 均下沉到更底层或由应用负责。

## 2. 分层架构

```
┌─────────────────────────────────────────────────────────────────┐
│  用户进程                                                        │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  应用代码                                                  │  │
│  │    shared_malloc(size)  ↔  shared_free(ptr)               │  │
│  │    (路由到 dshm 专用 arena，与本地 malloc 隔离)            │  │
│  └────────────────────────┬──────────────────────────────────┘  │
│                           │                                      │
│  ┌────────────────────────▼──────────────────────────────────┐  │
│  │  libdshm_malloc (静态库)                                  │  │
│  │  ┌──────────────────────────────────────────────────┐    │  │
│  │  │  L2: jemalloc extent_hooks 适配 + chunk feeder    │    │  │
│  │  │    extent_alloc  → feeder_alloc → L1_alloc        │    │  │
│  │  │    extent_dalloc → feeder_free  → L1_free         │    │  │
│  │  │    (extent 元数据存于 jemalloc rtree，进程本地)    │    │  │
│  │  └────────────────────────┬─────────────────────────┘    │  │
│  │  ┌────────────────────────▼─────────────────────────┐    │  │
│  │  │  L1: 分布式 1GB chunk 分配器 (进程内单例)           │    │  │
│  │  │  ┌────────────────────────────────────────────┐   │    │  │
│  │  │  │  快路径：进程本地 cache (默认 4 × 1GB)        │   │    │  │
│  │  │  └──────────────────┬─────────────────────────┘   │    │  │
│  │  │                     │ miss                        │    │  │
│  │  │  ┌──────────────────▼─────────────────────────┐   │    │  │
│  │  │  │  慢路径：全局 CAS bitmap (随机起点线性扫描) │   │    │  │
│  │  │  └────────────────────────────────────────────┘   │    │  │
│  │  └────────────────────────────────────────────────────┘   │  │
│  └──────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │  共享 GVA 池 128TB                                       │  │
│  │  ┌────────────────────────────────────────────────────┐ │  │
│  │  │  chunk 0: 元数据 (superblock + entries[] + ts[])   │ │  │
│  │  ├────────────────────────────────────────────────────┤ │  │
│  │  │  chunk 1..131071: 可分配 1GB chunk                │ │  │
│  │  └────────────────────────────────────────────────────┘ │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 各层职责

| 层 | 职责 | 实现位置 |
|----|------|----------|
| **L0 共享池** | 128TB 共享 VA + 池头元数据；纯数据区，不协调分配 | 外部 mmap（调用方提供 VA） |
| **L1 分布式 chunk 分配器** | 进程内单例。快路径用本地 cache；慢路径用共享 bitmap 上的 CAS。无 daemon、无跨进程锁 | `src/dshm_l1.{c,h}` |
| **L2 jemalloc 适配 + feeder** | `extent_alloc`→L1 取 chunk 并子切；`extent_dalloc`→归还 L1。extent 元数据留在 jemalloc 进程本地 rtree | `src/dshm_l2.{c,h}` |

## 3. 关键常量与数据结构

### 3.1 常量（`include/dshm_types.h`）

| 常量 | 值 | 含义 |
|------|----|------|
| `DSHM_NUM_CHUNKS` | 131072 | 128TB / 1GB |
| `DSHM_MAX_NODES` | 256 | 最大节点数（node_id 有效范围 1..255） |
| `DSHM_CHUNK_SIZE` | 0x40000000 (1GB) | 单 chunk 大小 |
| `DSHM_MAGIC` | "DSHMMCBL" | 超级块就绪标志 |
| `DSHM_INITIALIZING` | "INITINIT" | 引导中间态 |
| `DSHM_DEFAULT_CACHE_SIZE` | 4 | 本地 cache 默认容量 (4GB) |

### 3.2 超级块（位于 chunk 0，~2MB）

```c
struct dshm_chunk_entry {   /* 8 字节，CAS 操作原子单元 */
    __u32 owner_node;      /* 0 = FREE；1..255 = 分配者 node_id */
    __u32 owner_pid;        /* 分配者 OS pid */
} __attribute__((packed, aligned(8)));

struct dshm_superblock {
    /* 只读字段，引导时一次性写入 */
    __u64 magic;            /* DSHM_MAGIC / DSHM_INITIALIZING */
    __u64 pool_base_va;
    __u64 pool_size;        /* 128T */
    __u64 chunk_size;       /* 1G */
    __u32 num_chunks;       /* 131072 */
    __u32 max_nodes;        /* 256 */
    __u8  pad[216];         /* 头部补齐至 256 字节 */

    /* chunk 条目表：8 字节 × 131072 = 1MB，CAS 单元 */
    struct dshm_chunk_entry entries[DSHM_NUM_CHUNKS];

    /* 分配时间戳（仅调试，不参与 CAS） */
    __u64 alloc_timestamps[DSHM_NUM_CHUNKS];   /* 1MB */
};
```

**设计要点**：

- CAS 原子单元是 **8 字节**（`owner_node + owner_pid`）。`owner_node == 0` 即 FREE。
- 时间戳**不在 CAS 单元内**：CAS 成功后用普通 store 写入。并发读可能读到陈旧时间戳，对调试可接受。
- chunk 0 仅用 ~2MB，剩余 ~1022MB 空闲，预留给元数据扩展。

### 3.3 进程内状态（`src/dshm_l1.h`）

```c
struct dshm_proc_state {   /* 单例 g_state，由 dshm_init 设置 */
    __u64 pool_base_va;
    __u64 pool_size;
    __u32 my_node_id;
    __u32 my_pid;
    unsigned arena_ind;     /* jemalloc arena 索引，由 L2 设置 */

    struct {
        pthread_spinlock_t lock;  /* 进程内多线程保护 */
        int cnt;
        int max;                 /* 可配置，默认 4 */
        __u32 *chunk_ids;        /* 动态分配，max 个元素 */
    } cache;
};
extern struct dshm_proc_state *g_state;
```

### 3.4 Chunk Feeder（`src/dshm_l2.c`）

> **这是相对原始设计稿新增的子分配层**，用于解决 jemalloc 请求 extent 大小既不固定为 1GB、也可能超过 1GB 的问题。

```c
struct dshm_feeder {
    pthread_spinlock_t lock;
    __u32 active_chunk_id;          /* 当前正在子切的 chunk，0xFFFFFFFF=无 */
    __u64 active_offset;            /* 已从 active chunk 切出的字节数 */
    atomic_uint_least64_t *used_bytes; /* 每个 chunk 的已用字节计数，懒分配 */
};
```

feeder 把 jemalloc 的 ~2MB extent 请求从当前 active 1GB chunk 中**线性切割**，并用 `used_bytes[]` 引用计数追踪；当某 chunk 计数归零时整块归还 L1。

## 4. 原子原语（`src/dshm_atomic.h`）

将内核风格原语映射到 GCC `__atomic` 内建：

| 原语 | 实现 | 用途 |
|------|------|------|
| `dshm_cas_u64` | `__atomic_compare_exchange_n` (SEQ_CST) | 8 字节 CAS |
| `dshm_cas_entry` | memcpy 避免 strict-aliasing UB + `dshm_cas_u64` | chunk 条目 CAS |
| `dshm_store_entry` | `__atomic_store_n` (RELEASE) | L1_free 写 FREE |
| `dshm_smp_mb` / `dshm_smp_rmb` | SEQ_CST / ACQUIRE fence | 引导发布/获取 |
| `dshm_cpu_relax` | x86 `pause` / ARM64 `yield` | 自旋等待 |
| `dshm_clock_ns` | `CLOCK_MONOTONIC` | 分配时间戳 |

## 5. L1 分配/释放协议

### 5.1 单 chunk 分配 `L1_alloc()`

```
L1_alloc():
  ┌─ 快路径：进程本地 cache ────────────────────┐
  │  lock(cache.lock)                            │
  │  if (cache.cnt > 0):                        │
  │      chunk_id = cache.chunk_ids[--cnt]      │  /* LIFO */
  │      unlock; return {chunk_id, VA}          │
  │  unlock                                      │
  └──────────────────────────────────────────────┘
                    │ cache 空
                    ▼
  ┌─ 慢路径：全局 CAS bitmap ───────────────────┐
  │  start = rand() % DSHM_NUM_CHUNKS          │  /* 随机起点避免头部热点 */
  │  for i in 0..DSHM_NUM_CHUNKS:              │
  │      idx = (start + i) % NUM_CHUNKS        │
  │      if entries[idx].owner_node != 0:      │  /* RELAXED 读，占用则跳过 */
  │          continue                          │
  │      if CAS(entries[idx], {0,0}→{me}):     │
  │          timestamps[idx] = clock()         │
  │          /* 继续把 cache 装满，最多 8 次连续 CAS 失败为止 */ 
  │          while (cache.cnt < cache.max       │
  │                 && consec_fail < 8):        │
  │              ...CAS 下一个 FREE 槽位压入 cache
  │          return {idx, VA}                  │
  │  return -ENOMEM                            │  /* 全部 131072 已占用 */
  └──────────────────────────────────────────────┘
```

**cache 回填策略**：慢路径一旦 CAS 成功，会继续 CAS 分配直到 cache 装满或连续 8 次失败，从而**摊薄 CAS 成本**——一次慢路径入场即预热 cache。

### 5.2 连续 chunk 分配 `L1_alloc_contiguous(n)`

> 为支持超过单 chunk 大小的分配而新增。

```
L1_alloc_contiguous(n):
  if n == 1: return L1_alloc()
  start = rand() % DSHM_NUM_CHUNKS
  for attempt in 0..NUM_CHUNKS:
      base = (start + attempt) % NUM_CHUNKS
      if base + n > NUM_CHUNKS: continue
      acquired = 0
      for j in 0..n:
          idx = base + j
          if entries[idx].owner_node != 0: goto rollback   /* RELAXED 读预检 */
          if CAS(entries[idx], {0,0}→{me}): acquired++
          else: goto rollback
      return {base, VA}                       /* 全部 CAS 成功 */
  rollback:
      for j in 0..acquired:                   /* 回滚已获得的 */
          dshm_store_entry(entries[base+j], {0,0})
  return -ENOMEM
```

逐个 CAS + 失败回滚，保证 N 个连续槽位要么全部占有、要么全部释放。

### 5.3 释放 `L1_free(chunk_id)`

```
L1_free(chunk_id):
    lock(cache.lock)
    if (cache.cnt < cache.max):               /* 优先回填本地 cache */
        cache.chunk_ids[cnt++] = chunk_id
        unlock; return
    unlock
    /* cache 满 → 释放到全局 bitmap */
    dshm_store_entry(entries[chunk_id], {0,0})   /* 普通原子 store，非 CAS */
```

**为何用普通 store 而非 CAS**：释放者是该 chunk 的唯一 owner（赢得分配 CAS 的进程）。无其他进程能同时释放不属于自己的 chunk。`dshm_chunk_entry` 8 字节自然对齐，ARM64 保证对齐 8 字节 store 原子性；通过缓存一致性对其他进程可见。

### 5.4 线程安全

- 本地 cache：`pthread_spinlock_t` 保护，进程内多线程共享。
- 全局 CAS：天然跨进程/跨节点线程安全；ARM64 LSE 提供原生 8 字节 CAS。
- 无跨进程锁：所有协调只经共享 bitmap 上的 CAS，无 mutex/futex/daemon。

## 6. L2：jemalloc 集成与 Chunk Feeder

### 6.1 Chunk Feeder 子分配

feeder 在 1GB L1 chunk 与 jemalloc extent 请求之间做适配：

```
feeder_alloc(size, alignment):
    if size > CHUNK_SIZE:                       /* 超大分配 */
        n = ceil(size / CHUNK_SIZE)
        r = L1_alloc_contiguous(n)              /* 取连续 N 个 */
        used_bytes[r.chunk_id] += size
        return VA
    lock(feeder.lock)
    if active_offset + size > CHUNK_SIZE:        /* 当前 chunk 不够 */
        r = L1_alloc()                          /* 取新 1GB chunk */
        active_chunk_id = r.chunk_id
        active_offset = 0
    active_offset = align_up(active_offset, alignment)  /* 对齐 */
    chunk_id = active_chunk_id
    offset = active_offset
    active_offset += size
    used_bytes[chunk_id] += size
    unlock
    return pool_base + chunk_id*CHUNK + offset

feeder_free(addr, size):
    chunk_id = (addr - pool_base) / CHUNK_SIZE
    prev = used_bytes[chunk_id] -= size          /* 引用计数 */
    if prev == size:                             /* 该 chunk 全部归还 */
        if size > CHUNK_SIZE:
            free 连续 N 个 chunk
        else:
            if active_chunk_id == chunk_id:
                active_offset = CHUNK_SIZE; active = INVALID  /* 作废 active */
            L1_free(chunk_id)
```

### 6.2 extent_hooks 实现

`extent_alloc`→`feeder_alloc`，并设 `*zero=false`、`*commit=true`（物理页由底层管理，总是已 commit）。
`extent_dalloc`→`feeder_free` 并返回 `true`（已处理）。

其余 hook 语义：

| hook | 返回 | 语义 |
|------|------|------|
| `commit` | `true` | 始终已 commit |
| `decommit` | `false` | 不支持 decommit，页保持 commit |
| `purge_lazy` / `purge_forced` | `false` | 不 purge，jemalloc 复用时自行清零 |
| `split` / `merge` | `false` | 允许 jemalloc 内部切分/合并 1GB |
| `destroy` | `NULL` | dalloc 总返回 true，destroy 永不被调 |
| `alloc` 且 `new_addr != NULL` | `NULL` | 不支持固定地址分配 |

### 6.3 引导初始化 `dshm_init()`（Setter API）

```
dshm_init(pool_base_va, pool_size, my_node_id, local_cache_size):
    /* 输入校验：size>0、cache>0、node_id∈[1,255]、
       pool_size == NUM_CHUNKS * CHUNK_SIZE */
    sb = (superblock*)pool_base_va
    if sb->magic == DSHM_MAGIC:                  /* 已初始化 */
    elif CAS(&sb->magic, 0 → INITIALIZING):      /* 赢得引导竞争 */
        填写 pool_base_va/size/chunk_size/num_chunks/max_nodes
        memset entries 与 timestamps 为 0
        smp_mb(); store magic = DSHM_MAGIC       /* RELEASE 发布 */
    else:                                        /* 输竞争，自旋等待 */
        while (load magic != DSHM_MAGIC): cpu_relax
    L1_init_state(...)                           /* 进程内 g_state 单例 */
    feeder_init()                                /* used_bytes 懒分配 */
    mallctl("arenas.create") → arena_ind
    mallctl("arena.<ind>.extent_hooks" = &dshm_hooks)
    g_state->arena_ind = arena_ind
```

**引导是乐观 CAS**：首个把 magic 从 0 CAS 到 `INITIALIZING` 的进程负责初始化元数据，其余进程自旋等待 `DSHM_MAGIC`。

### 6.4 用户 API

```c
void *shared_malloc(size_t size) {
    return mallocx(size, MALLOCX_ARENA(g_state->arena_ind));
}
void shared_free(void *ptr) {
    dallocx(ptr, 0);   /* jemalloc 自动路由到正确 arena */
}
```

**API 隔离**：`shared_malloc/free` 走自定义 hook 的专用 arena；标准 `malloc/free` 仍走默认 arena + 系统 mmap。从而满足"共享池分配可区别于本地堆分配"的要求。

## 7. 完整数据流：malloc → free 生命周期

```
1. 进程启动
   dshm_init(pool_va, 128T, node_id=2, cache=4)
     → 引导/校验 superblock
     → 创建 jemalloc arena，绑定 extent_hooks
     → 初始化本地 cache（空）

2. 用户调 shared_malloc(64KB)
   → mallocx(64KB, MALLOCX_ARENA(dshm_arena))
   → jemalloc 找 extent → 无 → extent_alloc(size≈2MB)
   → feeder_alloc → L1_alloc() → 快路径 pop 1GB chunk
   → jemalloc 收到 VA，内部切分为 2MB extent，再切出 64KB
   → 返回 ptr

3. 用户经 ptr 读写（落在该 1GB chunk 内）

4. shared_free(ptr)
   → dallocx → jemalloc 标记 64KB 为空闲（extent 不立即释放）

5. 同 extent 内所有 64KB 释放 → jemalloc 合并为空闲 extent

6. 同 1GB chunk 内所有 extent 释放 → jemalloc 调 extent_dalloc(addr, size)
   → feeder_free → used_bytes 归零 → L1_free(chunk_id)
   → chunk 回到本地 cache 或全局 bitmap

7. 进程退出
   → jemalloc atexit flush 统计、释放 TSD
   → jemalloc 退出时**不**调 extent_dalloc
   → chunk 条目仍记 owner={node2,pid}，VA 槽位永久占用（按设计泄漏）
```

## 8. 边界条件与失败语义

| 条件 | 处理 |
|------|------|
| jemalloc extent 大小 vs 1GB | jemalloc 默认 `lg_chunk=22`(4MB)；feeder 从 1GB 中切，1GB 自然对齐满足 jemalloc 对齐要求 |
| `extent_alloc` 传 `new_addr != NULL` | 返回 NULL（不支持），jemalloc 回退到 `new_addr=NULL` |
| 单进程多线程 | cache 用 spinlock 保护；全局 CAS 天然线程安全 |
| 分配 > CHUNK_SIZE | `L1_alloc_contiguous(n)`，CAS N 个连续槽 + 失败回滚 |
| `extent_dalloc` size < 1GB | feeder_free 不立即归还，按 `used_bytes` 引用计数追踪 |
| 进程死亡（正常/异常） | 无回收；条目保留 `{node_id,pid}`，VA 永久占用；应用须显式 `L1_free` 或接受泄漏 |
| 节点 OS 重启 | 同进程死亡，无回收，该节点进程所占 chunk 全部泄漏 |
| 池耗尽 | 慢路径扫描完整 bitmap 后返回 `-ENOMEM` |

## 9. 构建与测试

### 9.1 构建（`CMakeLists.txt`）

- C11 / C++17，`-Wall -Wextra -Werror -Wpedantic`，`-Wno-unused-parameter`（jemalloc hook 签名含未用参数）。
- 经 `pkg-config` 查找 jemalloc，链接 `pthread`。
- 产物：`libdshm_malloc` 静态库（`dshm_l1.c` + `dshm_l2.c`）。

### 9.2 测试（`tests/`）

无外部测试框架（避免依赖），每个测试自带 `main()`，返回 0 即通过。**小池测试模式**：编译期覆盖 `DSHM_NUM_CHUNKS=512`、`DSHM_CHUNK_SIZE=16MB`，使测试可 mmap 可控规模（512×16MB=8GB），而非 128TB。测试直接编译库源码使宏覆盖同时作用于测试与库代码。

| 测试 | 覆盖 |
|------|------|
| `test_l1_cache` | 本地 cache 快路径 LIFO、cache 空→慢路径过渡 |
| `test_l1_cas` | 慢路径 CAS、cache 回填、free 回 cache、无重复分配 |
| `test_l1_concurrent` | 8 线程 × 50 次并发分配，验证无重复 chunk_id |
| `test_l2_init` | `dshm_init` 输入校验（size/cache/node_id/重复初始化） |
| `test_l2_malloc` | 端到端：128TB mmap + shared_malloc/free + 数据完整性 |

`test_l2_malloc` 在 128TB mmap 不可用时优雅降级为 SKIP。

## 10. 不在范围内（Out of Scope）

- **物理页管理**：GVA 如何被物理页 backing、NUMA 放置、缺页处理——下沉到底层（OBMM 等）。
- **NUMA 亲和**：chunk 分配不关心 VA 由哪个节点的物理内存 backing。
- **跨节点崩溃检测/恢复**：无心跳、无 stale 检测、无自动回收。
- **缓存一致性**：假定底层保证共享 GVA 池跨节点一致性。
- **GVA 段 mmap 机制**：`dshm_init` 假定调用方已获得共享 GVA 范围。
- **jemalloc 内部细粒度管理**：所有 sub-1GB 切分/合并/purge 由 jemalloc 负责。
- **持久化**：分配状态是易失的；全池重置（所有进程退出、superblock 重新初始化）后状态全部丢失。

## 11. 与原始设计稿的差异说明

原始设计稿（`specs/2026-07-14-...`）描述的是 `extent_alloc` 直接返回整块 1GB、`extent_dalloc` 仅在 `size==1G` 时归还 L1 的简化模型。**当前实现在此基础上演进**，主要差异：

1. **新增 chunk feeder 子分配层**（commit `b936aa7`）：jemalloc 实际以 ~2MB extent 请求，feeder 从 1GB chunk 中线性切割并用 `used_bytes[]` 引用计数追踪，避免每次 extent 请求都消耗一整块 1GB。
2. **支持超大分配**（commit `8295288`）：新增 `L1_alloc_contiguous(n)` + feeder 超大分支，处理 `size > DSHM_CHUNK_SIZE` 的请求（如 10GB malloc 触发单次 `extent_alloc` size≈10G）。
3. **`extent_dalloc` 不再按 `size==1G` 判定**：改为由 feeder 的 `used_bytes` 引用计数决定何时整块归还 L1。
4. **`g_state` 的 cache 用动态分配**（`malloc`）而非设计稿中的柔性数组 `chunk_ids[]`。
5. 引导发布用 `__atomic_store_n(..., __ATOMIC_RELEASE)` 而非设计稿中"先 `smp_mb` 再普通 store"的写法。

## 12. 文件清单

| 文件 | 职责 |
|------|------|
| `include/dshm_types.h` | 常量、superblock、alloc result 结构 |
| `src/dshm_atomic.h` | CAS/fence/relax/clock 用户空间包装 |
| `src/dshm_l1.h` / `dshm_l1.c` | L1 分配器：cache、CAS 慢路径、连续分配、free |
| `src/dshm_l2.h` / `dshm_l2.c` | L2：feeder、extent_hooks、dshm_init、shared_malloc/free |
| `tests/test_l1_*.c` | L1 单元/并发测试 |
| `tests/test_l2_*.c` | L2 初始化校验 + 端到端集成测试 |
| `CMakeLists.txt` / `tests/CMakeLists.txt` | 构建系统 |
| `docs/superpowers/specs/2026-07-14-...` | 原始设计稿（参考，见 §11 差异） |
| `docs/superpowers/plans/2026-07-14-...` | 实现计划 |
