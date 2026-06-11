# Thread-Safe GC — 双 Arena 模型下的并发内存管理

**Status**: Design Proposal
**Author**: Ani
**Date**: 2026-05-30
**Priority**: P2
**Dependencies**: Multi-threaded work-stealing scheduler (Phase 1-4 ✅), Double Arena model ✅

---

## 1. 动机

### 1.1 当前内存模型

Aura 的内存管理基于**双 Arena** + **手动 `gc-temp`**：

```
┌────────────────────────────────────────────┐
│  CompilerService                           │
│                                            │
│  persistent_arena_  (原 arena_)            │
│  ├── 模块 FlatAST（永久）                  │
│  ├── workspace FlatAST（set-code 复用）    │
│  ├── while 循环闭包体                       │
│  └── 模块 Env                              │
│                                            │
│  temp_arena_  (新增, 双 arena)             │
│  ├── task 闭包体 (cl_flat/cl_pool)         │
│  ├── task copy_env 结果                    │
│  ├── eval-expr 临时 AST                    │
│  └── 模式匹配 AST                          │
│                                            │
│  回收方式: ASTArena::reset() O(1) 释放     │
│  标记: Closure.owner_arena + in_task_context_│
└────────────────────────────────────────────┘
```

**当前 GC 操作**：
- `gc-temp` 原语：重置 `temp_arena_` 并清理相关的 `closures_` / `cells_` vector
- 依赖**任务边界清晰**的模型（task 结束即可确保 temp_arena 无活跃引用）
- Workspace 使用 **Copy-on-Write 分层**，每层独立 FlatAST/StringPool

### 1.2 为什么需要线程安全

多线程 work-stealing 调度器（Phase 1-4）让 fiber 可以在不同 worker 上**并行执行**。当前线程不安全的点：

| 场景 | 问题 |
|------|------|
| Fiber-A alloc 在 temp_arena，Fiber-B 同时 reset | 双写 UB |
| Fiber-A 读 persistent_arena 的 FlatAST，Fiber-B 写 workspace | 读-写竞争 |
| Fiber-A 创建 closure 在 temp_arena，Fiber-B 引用 | use-after-free |
| `(*agents*)` 全局变量多线程并发访问 | 数据竞争 |

### 1.3 设计目标

| 维度 | 目标 | 非目标 |
|------|------|--------|
| Safety | 无数据竞争，无 use-after-free | 无锁 GC |
| Throughput | GC pause < 5ms | 实时 GC（soft real-time 而非 hard） |
| Allocation | 单 fiber 内分配无锁 | 跨 fiber 引用计数自动管理 |
| Backward compat | 保留双 Arena + `gc-temp` 语义 | 重写整个内存系统 |
| Incremental | Phase 1-4 逐步推进 | 一次性大重构 |

---

## 2. 方案对比

参考现有双 Arena 设计文档的分析：

| 方案 | 复杂度 | 与双 Arena 兼容性 | 性能 | 推荐？ |
|------|--------|-------------------|------|--------|
| **A: Per-fiber arena 隔离** | 最简 | **❌ 破坏现有 AST 模型** | 无 STW，但跨 fiber copy-on-write 成本高 | **强烈反对** |
| **B: STW + 并行标记** | 中 | **✅ 最兼容** | GC pause 随 worker 扩展 | **✅ 推荐** |
| **C: 分代 + 写屏障** | 最高 | ❌ 过度设计 | 吞吐量最高 | ⏳ v2.0+ 远期 |

### 为什么 B 最适合

1. **保留双 Arena 模型** — `persistent_arena_` 作为 from-space，`temp_arena_` 回收由 GC 接管
2. **当前 `gc-temp` 已是 STW 雏形** — 重置 temp_arena 时保证无活跃引用，本质就是 STW
3. **Workspace COW 层可与 GC 结合** — 每层独立 flat，GC 可以按层粒度处理
4. **实现复杂度可控** — 并行标记直接复用 work-stealing queue
5. **解决 benchmark "内存线性增长"** — benchmark 中每个 task 产生 ~3 临时闭包，405 轮后 ~500KB 线性增长

---

## 3. 架构设计

### 3.1 分区域策略

不是对所有内存区域一刀切。Aura 的内存分为三类，每类需要不同的 GC 策略：

```
┌─────────────────────────────────────────────────────┐
│  Aura Memory Regions                                │
│                                                     │
│  Region 1:  evaluator vector heaps                  │
│  ┌──────────────────────────────────────────────┐   │
│  │ string_heap_  pairs_  hash_heap_  cells_     │   │
│  │ closure_arena_  keyword_table_               │   │
│  │                                              │   │
│  │ GC 策略: 现有的 gc-heap + 线程安全锁           │   │
│  │ 这些是 std::vector，双 arena 不影响             │   │
│  └──────────────────────────────────────────────┘   │
│                                                     │
│  Region 2: temp_arena_ (临时数据)                   │
│  ┌──────────────────────────────────────────────┐   │
│  │ task 闭包体  copy_env  eval-expr AST         │   │
│  │ 模式匹配 AST                                 │   │
│  │                                              │   │
│  │ GC 策略: gc-temp + fiber 活性检测              │   │
│  │ 当前 gc-temp 假设无活跃引用。多线程下需要       │   │
│  │ 确认所有 fiber 都释放了引用才能 reset。         │   │
│  └──────────────────────────────────────────────┘   │
│                                                     │
│  Region 3: persistent_arena_ + workspace COW        │
│  ┌──────────────────────────────────────────────┐   │
│  │ 模块 FlatAST  while 闭包体  模块 Env          │   │
│  │ workspace 各层的 FlatAST/StringPool           │   │
│  │                                              │   │
│  │ GC 策略: 引用计数 + 延迟回收                   │   │
│  │ 模块代码几乎从不释放。workspace 层在层被       │   │
│  │ 替换时释放（已有 Drop 模式）。持久区主要风险     │   │
│  │ 是 closure 逃逸到模块外。                      │   │
│  └──────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
```

### 3.2 安全点 (Safepoint)

利用已有 `YieldReason` + `is_stealable()` 机制，与 fiber 调度器集成：

```cpp
// scheduler.h — 新增
enum class GCPhase : uint8_t {
    None,        // 正常执行，不做 GC
    Requested,   // GC 请求已广播，等待 fiber 到达安全点
    Sweeping,    // 同步 sweep 进行中
    Complete,    // GC 完成，恢复执行
};

struct WorkerGCState {
    std::atomic<GCPhase> phase{GCPhase::None};
    std::atomic<int32_t> fibers_at_safepoint{0};
    std::atomic<int64_t> gc_epoch{0};  // 单调递增 GC 世代
};

// fiber.cpp — 在 yield() 和 alloc() 中检查安全点
void Fiber::yield(YieldReason reason) {
    check_gc_safepoint();
    last_yield_reason_.store(reason, std::memory_order_release);
    swapcontext(&ctx_, &worker_ctx->uctx);
}

// 每次分配时检查（轻量级 atomic load）
inline void check_gc_safepoint() {
    auto& gc = g_worker_ctx->gc_state;
    if (gc.phase.load(std::memory_order_acquire) == GCPhase::Requested) {
        enter_gc_safepoint();
    }
}
```

**为什么可以在 alloc 时检查而不是只在 yield 时**：
- 长时间计算的 fiber 可能 N 亿指令不 yield
- 在 `arena_->alloc()` 路径中插入一次 `check_gc_safepoint()` 的开销 < 1ns（hot cache atomic load）
- 当前 `ASTArena::alloc()` 已经是一层包装，hook 点清晰

### 3.3 协调器 (GC Coordinator)

运行在 IO thread（Scheduler 主线程），与 epoll loop 集成：

```cpp
// gc_coordinator.h
class GCCollector {
    // 全局 GC 状态
    std::atomic<GCPhase> global_phase_{GCPhase::None};
    int64_t gc_epoch_{0};

    // 每个 evaluator 注册自己的根集回调
    struct RootSource {
        int worker_id;
        std::function<void(RootSet&)> flush_fn;
    };
    std::vector<RootSource> root_sources_;
    std::mutex root_sources_mutex_;

public:
    // 请求 GC — 由 alloc 阈值或显式 gc() 触发
    // 如果 gc_epoch 与上一次相同，不重复 GC（去重）
    bool request(int64_t caller_epoch);

    // 执行 GC 循环（在 IO thread 同步调用）
    void collect();

private:
    void broadcast_safepoint();
    void wait_for_all_workers(int timeout_ms = 100);
    void parallel_mark(RootSet& roots);
    void sweep_temp_arenas();
    void reclaim_heaps();
    void resume_fibers();
};
```

### 3.4 GC 流程（分阶段详细）

```
Phase 1 — Safepoint
┌──────────────────────────────────────────────────┐
│ Coordinator          Worker-1          Worker-2   │
│                                                     │
│ broadcast(Requested)                                │
│                  ├──── saepoint ──► yield → wait    │
│                  ├──── saepoint ──► alloc → wait    │
│                  └──── saepoint ──► explicit gc → wait│
│                                                     │
│ wait all workers:                                   │
│   100μs spin / 1ms epoll timeout / 10ms yield      │
└──────────────────────────────────────────────────┘

Phase 2 — Root Collection (串行, 停止世界)
┌──────────────────────────────────────────────────┐
│ 1. 冻结每个 evaluator 的 string_heap_ / pairs_   │
│ 2. 冻结每个 evaluator 的 closure_arena_          │
│ 3. 冻结 s_fiber_results (fiber:join 共享结果)    │
│ 4. 枚举全局 *agents* 注册表                       │
│ 5. 枚举正在执行的 fiber 栈（保守扫描）             │
└──────────────────────────────────────────────────┘

Phase 3 — Parallel Mark
┌──────────────────────────────────────────────────┐
│ Worker-1     Worker-2     Worker-3                │
│  ┌──────┐    ┌──────┐    ┌──────┐                 │
│  │gray  │    │gray  │    │gray  │                 │
│  │queue │    │queue │    │queue │                 │
│  └──┬───┘    └──┬───┘    └──┬───┘                 │
│     │ steal ────│─── steal  │                      │
│     ▼           ▼           ▼                      │
│  Chunk分割: temp_arena 按 object 划分              │
│  每个 worker 标记自己的 chunk                       │
└──────────────────────────────────────────────────┘

Phase 4 — Sweep
┌──────────────────────────────────────────────────┐
│ temp_arena:                                       │
│   未标记的闭包体 → 从 closures_ 移除               │
│   ASTArena::reset()  → 释放所有                    │
│   重新分配（惰性）                                  │
│                                                    │
│ evaluator heaps:                                   │
│   string_heap_  → 标记+压缩                        │
│   pairs_        → 标记+压缩                        │
│   hash_heap_    → 标记+压缩                        │
│   cells_        → 标记+压缩                        │
└──────────────────────────────────────────────────┘

Phase 5 — Resume
┌──────────────────────────────────────────────────┐
│ broadcast(Complete)                                │
│ fiber 从 safepoint 恢复执行                        │
│ global_phase_ = None                               │
│ gc_epoch_++                                       │
└──────────────────────────────────────────────────┘
```

### 3.5 根集 (Root Set)

```
每个 CompilerService (Evaluator) 的根：
├── string_heap_ 中每个 slot        → 指向 ast::StringPool 中 intern 的字符串
├── pairs_ 中每个 Pair              → car + cdr 可能是 int/closure/pair ref
├── hash_heap_ 中每个 slot          → key-value 对，值可能是 closure ref
├── cells_ 中每个 cell              → 值可能是 closure ref
├── closures_ 中每个 Closure        → cl_flat, cl_pool (arena 指针)
├── keyword_table_                  → 纯字符串，无外部引用
├── s_fiber_results                 → fiber:spawn 返回值缓存
├── g_current_compiler_service      → 当前正在执行的 evaluator
├── g_current_fiber                 → fiber 的栈 + 寄存器
├── *agents* 全局注册表             → agent:spawn 注册的所有 handler closure
└── workspace_tree_->nodes_[i].flat → 每层的 FlatAST (COW)
```

**根集收集策略**：

```cpp
// 每个 worker 在 GC 时调用
void Evaluator::flush_gc_roots(RootSet& out) {
    // 1. vector heaps — 每个元素遍历
    for (auto& s : string_heap_)
        if (is_allocated(s)) out.push_string_root(s);
    for (auto& p : pairs_)
        if (is_allocated(p)) out.push_pair_root(p);
    // 2. closures — 所有活动闭包
    for (auto& [id, c] : closures_)
        out.push_closure_root(c);
    // 3. fiber results
    // 4. 当前 eval stack
}
```

---

## 4. 增量实现路径

### Phase 1: 安全点（1-2 天）

| 任务 | 文件 | 说明 |
|------|------|------|
| `GCPhase` / `WorkerGCState` | `scheduler.h` | 每个 worker 的 GC 状态 + 世代计数器 |
| `check_gc_safepoint()` | `fiber.cpp` / `arena.ixx` | yield + alloc 路径的安全点检查 |
| GC coordinator 骨架 | `gc_coordinator.h/cpp` | request → safepoint → resume 原型 |
| Safepoint broadcast | `scheduler.cpp` | IO thread 向所有 worker 广播 |

**验证**：`test_gc_safepoint_all_stop()` — 4 个 worker，每个执行 500ms 计算型 fiber，触发 GC 后在 100ms 内全部到达安全点。

### Phase 2: 根集收集（2-3 天）

| 任务 | 文件 | 说明 |
|------|------|------|
| `flush_gc_roots()` | `evaluator_impl.cpp` | evaluator 各 heap 的根集遍历 |
| RootSet 类型 | `gc_coordinator.h` | 根集容器 + 去重 |
| fiber stack 保守扫描 | `fiber.cpp` | 扫描栈 + 寄存器 |
| s_fiber_results 注册 | `evaluator_impl.cpp` | `fiber:spawn` 结果缓存 |
| *agents* 全局表 | — | 纯 Aura，GC coordinator 需枚举 |

**验证**：`test_gc_root_set_complete()` — 创建复杂对象图（100+ closure 链），验证 GC 后所有 reachable 对象存活。

### Phase 3: temp_arena sweep（2-3 天）

| 任务 | 文件 | 说明 |
|------|------|------|
| temp_arena 标记 pass | `gc_coordinator.cpp` | 标记 temp_arena 中的活动闭包 |
| 未标记 closure 回收 | `evaluator_impl.cpp` | 从 closures_ 移除 |
| temp_arena reset | `evaluator_impl.cpp` | 回收所有未标记内存 |
| string_heap_/pairs_ 压缩 | `evaluator_impl.cpp` | 标记-压缩 vector heaps |

**验证**：`test_gc_temp_arena_collect()` — 创建 task 闭包链，GC 后确认 temp_arena 中 unreachable 闭包被回收。

### Phase 4: 持久区 + 调优（2-3 天）

| 任务 | 文件 | 说明 |
|------|------|------|
| Workspace COW 层 GC 集成 | `evaluator_impl.cpp` | 未使用的 workspace 层可回收 |
| Metrics | `gc_metrics.h/cpp` | GC pause、freed bytes、mark time |
| 自适应触发 | `gc_coordinator.cpp` | 基于 alloc 频率或内存阈值 |
| Fiber 分配计数 | `arena.ixx` | 每个 worker 的 alloc counter |

**验证**：`test_gc_stress()` — 高频 alloc + 多 fiber，验证无内存泄漏。`test_gc_metrics()` — 确认 GC 指标合理性。

---

## 5. 与现有基础设施的集成

| 已有设施 | 用途 | 改动量 |
|----------|------|--------|
| `YieldReason` + `is_stealable()` | 安全点检查 | 小 — 新增 GCPhase 枚举 |
| `FiberState::Waiting/Done` | GC 期间 fiber 状态管理 | 无 — 复用已有 |
| `Scheduler::run()` epoll loop | GC coordinator 事件驱动 | 中 — 插入 GC collect() 调用点 |
| Chase-Lev deque | 并行标记工作队列 | 小 — 复用 deque 算法 |
| `alignas(64)` + cache line padding | GC 关键字段防 false sharing | 小 — 已有模式 |
| `WorkerThread` metrics | GC metrics 集成 | 中 — 新增 GC 相关字段 |
| `g_current_fiber` (thread_local) | 安全点期间快速定位 fiber | 无 — 直接使用 |
| `Closure.owner_arena` | 判断闭包属于 temp 还是 persistent | 无 — 已有字段 |
| `in_task_context_` | 区分 task 代码 vs 模块代码 | 无 — 已有字段 |

---

## 6. 内存管理演进路线图

```
当前 (v0.x)             下一阶段 (v1.0)           远期 (v2.0+)
┌─────────────┐       ┌──────────────────┐      ┌───────────────────┐
│ 双 Arena     │ ──→  │ STW + 并行标记    │ ──→ │ 分代 + 写屏障      │
│ 手动 gc-temp │       │ 保留双 Arena     │      │ 年轻代 bump alloc   │
│ 依赖任务边界   │       │ 自动检测根集      │      │ 老年代 mark-compact │
│              │       │ 并发安全         │      │ 卡表 + remembered set│
└─────────────┘       └──────────────────┘      └───────────────────┘
```

### 为什么不分步跳过 Phase 直接做 v2.0

1. **当前 workload profile**：Agent task 边界清晰，大部分对象生命周期短。分代替不上优势
2. **写屏障代价**：需要编译器代码生成支持 → 整个 JIT 改一遍
3. **benchmark 瓶颈**：405 轮产生 ~500KB，要解决的不是 pause 时间而是"泄漏"累积

---

## 7. 风险 & 缓解

| 风险 | 缓解 | 优先级 |
|------|------|--------|
| 安全点饥饿 — 计算型 fiber 长时间不 yield 不 alloc | 编译器插入定期 yield point（每 ~1000 条指令） | P0 |
| 根集遗漏 → dangling pointer | Debug 模式：GC 后校验所有 mark 过的对象 | P1 |
| temp_arena reset 后仍有 fiber 持有引用 | 引用计数 + gc_epoch 检查 | P1 |
| STW pause > 5ms | Phase 1 测量基线，Phase 4 增量标记 | P1 |
| Workspace COW 层的版本管理冲突 | GC 期间冻结 workspace 切换 | P2 |
| `*agents*` 全局表在 GC 期间变更 | GC 期间暂停 agent:spawn 和 agent:stop | P2 |

---

## 8. 测试计划

| # | 测试 | 场景 | 预期 |
|---|------|------|------|
| 1 | `test_gc_safepoint_all_stop()` | 4 worker × 5 计算型 fiber，触发 GC | ≤100ms 全到达 |
| 2 | `test_gc_root_set_complete()` | 100+ closure 链 | 所有 reachable 存活 |
| 3 | `test_gc_temp_arena_collect()` | temp_arena task 闭包 | 未标记的被回收 |
| 4 | `test_gc_concurrent_alloc()` | GC 期间 fiber 继续 alloc | 无数据竞争 |
| 5 | `test_gc_stress()` | 高频 alloc + spawn | 无内存泄漏 |
| 6 | `test_gc_no_deadlock()` | GC 期间 fiber:join/spawn | 无死锁 |
| 7 | `test_gc_metrics()` | GC pause、freed bytes | 指标合理 |
| 8 | `test_gc_gc_temp_compat()` | `gc-temp` 仍可调用 | 功能不变 |
| 9 | `test_gc_benchmark_memory()` | 405 轮 benchmark 后检查 | 内存稳定不增长 |

---

## 9. 里程碑

```
Week 1: Phase 1 — 安全点 + GC coordinator 骨架
        [所有 fiber 能在 GC 请求后 100ms 内到达安全点]

Week 2: Phase 2 — 根集收集
        [GC 知道哪些对象是活的，debug 模式通过校验]

Week 3: Phase 3 — temp_arena sweep
        [完整 GC 循环运行，benchmark 内存不增长]

Week 4: Phase 4 — 持久区 + 调优
        [GC pause < 5ms，性能可接受]
```
