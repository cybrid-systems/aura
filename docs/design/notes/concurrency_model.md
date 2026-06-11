# Aura 并发模型设计

> **注意（历史文档）**：本文档记录了 fiber scheduler + 并发原语的早期设计（2026-05）。当前已全部实装（#109 + #119）：多线程 work-stealing scheduler、fiber:spawn/join/yield、per-session eventfd、MutationBoundary yield 等。完整现状见 `design/core/agent_orchestration.md` §0（Implementation Status）和 `design/runtime/async_serve.md`。

**Status**: Design Proposal
**Author**: Ani
**Date**: 2026-05-29

---

## 1. 动机

### 1.1 当前状态

Aura 现有一套完整的并发原语，但调度器是**单线程协作式**的：

| 组件 | 状态 | 说明 |
|------|------|------|
| `fiber:spawn` / `fiber:join` | ✅ 已实现 | stackful fiber (ucontext + mmap) |
| Scheduler (epoll) | ✅ 已实现 | 单线程 event loop, 协作式 |
| Thread Pool | ✅ 已实现 | 4× std::jthread (阻塞操作 offload) |
| CSP Channel | ✅ 已实现 | channel:create/send/recv/try-recv/close |
| `orch:parallel` | ⚠️ 逻辑并行 | 纯 Aura, fiber:spawn → fiber:join, 单线程 |
| `eval:async` | ✅ 已实现 | 线程池上异步 eval |
| `thread_pool:enqueue` | ✅ 已实现 | 后台阻塞操作 offload |

### 1.2 核心瓶颈

Fiber 调度器运行在**单 OS 线程**上。所有 fiber 通过 `swapcontext` 协作式切换。
当 `orch:parallel` 同时启动多个 fiber 时，它们依然是**串行**执行的：

```
┌─ Scheduler (单线程 epoll) ───────────────────┐
│                                                │
│  fiber-A [● ● ● ● ● ● ●]                     │
│  fiber-B [         ● ● ● ● ●]                 │
│  fiber-C [              ● ● ● ● ● ●]         │
│                                                │
│  → 任何时候只有一个 fiber 在跑                  │
│  → 多核 CPU 只有一个核在工作                    │
│  → ant colony search 260 变异仍然串行          │
│                                                │
└────────────────────────────────────────────────┘
```

### 1.3 Aura 工作负载特征

Aura 不是传统 Web 服务或数据并行系统。它的独特工作负载：

1. **很多 Agent 同时运行**，每个 Agent 独立 workspace（copy-on-write AST）
2. **Agent 之间通过 send/recv 通信**，不共享内存
3. **Ant colony 搜索**：260 个微变异，每个 <1ms，mutate + eval-current
4. **LLM 调用**：阻塞 ~30s HTTP，需要 offload 不阻塞其他 Agent
5. **类型检查/编译**：CPU-bound，间歇性

### 1.4 Aura 哲学映射

| 原则 | 并发设计映射 |
|------|-------------|
| **着力即差** | 不为并发设框架。Agent 自然用 `send/recv` 通信，不需要显式锁 |
| **代码即记忆** | 每个 Agent 有独立 workspace。消息传递，不共享状态 |
| **控制论闭环** | Ant colony 搜索要快。并行变异需要 work-stealing 加速 |

---

## 2. 三层并发模型

```
┌──────────────────────────────────────────────────────────┐
│  Layer 3: Agent Orchestration (Actor + CSP)              │
│  ───────────────────────────────────────────────         │
│                                                           │
│  agent-A ──send──→ agent-B ──send──→ agent-C             │
│  (ws-A)           (ws-B)           (ws-C)                │
│     ↑ 独立 workspace, 消息驱动, 无共享状态                │
│                                                           │
│  orch:pipeline / orch:parallel / orch:conduct             │
│  channel:create/send/recv                                 │
│                                                           │
├──────────────────────────────────────────────────────────┤
│  Layer 2: Fiber Scheduler (Work-stealing M:N)            │
│  ───────────────────────────────────────────────         │
│                                                           │
│  Thread 1: fiber-A [●●●] fiber-C [●●]                    │
│  Thread 2: fiber-B [●●●●]                                │
│  Thread 3: (idle → steal Thread 2) [●●]                  │
│  Thread 4: (LLM offload → eventfd → resume)              │
│                                                           │
│  M fibers 跑在 N OS 线程上, 空闲线程 steal 任务           │
│  协作式 yield, 不需要抢占式调度                            │
│                                                           │
├──────────────────────────────────────────────────────────┤
│  Layer 1: Blocking I/O Offload (Thread Pool)              │
│  ───────────────────────────────────────────────         │
│                                                           │
│  ❄ LLM HTTP (~30s)  → pool thread → eventfd → wake      │
│  ❄ type-check (~50ms) → pool thread → eventfd → wake    │
│  ❄ file I/O            → pool thread → eventfd → wake    │
│                                                           │
│  ✅ 已有实现, 直接复用                                     │
└──────────────────────────────────────────────────────────┘
```

### 2.1 Layer 3: Agent Orchestration — Actor + CSP

**原则：** Agent 是 actor。私有 workspace，消息驱动。

```
(agent:spawn "planner" (lambda (task)
   ;; 在自己的 workspace 里做事
   (agent:ask "analyst" task)))      
   │                      ↑ message passing
   └── workspace-A ───┐
                      ↓
               workspace-B (analyst)
```

**现有原语（不变）：**
- `agent:spawn` / `agent:ask` / `agent:list`
- `send` / `recv` (跨 session 消息)
- `channel:create` / `channel:send` / `channel:recv` (CSP)
- `orch:pipeline` / `orch:parallel` / `orch:conduct`

**对调度器的影响：** 无。这些原语在语义上不变，底层 fiber 调度透明。

### 2.2 Layer 2: Fiber Scheduler — Work-stealing M:N

这是设计的核心。将单线程 scheduler 改造为多线程 work-stealing 调度器。

#### 2.2.1 架构

```
┌─────────────────────────────────────────────────────────┐
│  Scheduler                                               │
│                                                           │
│  ┌─ Thread 1 (epoll 主线程) ────────────────────────┐   │
│  │  local_queue_1: [fiber-A, fiber-C]                 │   │
│  │  epoll_wait (stdin + eventfd + channel)            │   │
│  │  ↓ eventfd →  fiber 入 local_queue                 │   │
│  └────────────────────────────────────────────────────┘   │
│                                                           │
│  ┌─ Thread 2 ────────────────────────────────────────┐   │
│  │  local_queue_2: [fiber-B, fiber-D]                 │   │
│  │  ↓ 空闲 → steal(local_queue_1)                     │   │
│  └────────────────────────────────────────────────────┘   │
│                                                           │
│  ┌─ Thread 3 ────────────────────────────────────────┐   │
│  │  local_queue_3: [fiber-E]                          │   │
│  │  ↓ 空闲 → steal(global_queue)                      │   │
│  └────────────────────────────────────────────────────┘   │
│                                                           │
│  Global queue: [fiber-F] (负载均衡用)                     │
│                                                           │
└─────────────────────────────────────────────────────────┘
```

#### 2.2.2 数据结构

```cpp
// per-thread fiber queue (lock-free deque)
struct LocalQueue {
    // 每个线程一个 deque<Fiber*>
    // push_back / pop_front 无锁（单生产者）
    // steal 需要加锁或 CAS（多消费者）
    moodycamel::ConcurrentQueue<Fiber*> queue;
    // 或: lock-free work-stealing deque (Chase-Lev)
};

// global queue (backup for balancing)
struct GlobalQueue {
    std::mutex mutex;
    std::deque<Fiber*> queue;
};

// 调度器
class Scheduler {
    // N 个 worker 线程
    std::vector<WorkerThread> workers_;
    // 主线程处理 epoll（或所有线程都参与 epoll）
    int epoll_fd_;
    // global queue
    GlobalQueue global_;
};

class WorkerThread {
    std::thread thread_;
    LocalQueue local_;
    // 运行到阻塞（yield/eventfd）或完成
    // 如果 local_ 空了，尝试 steal
    // 如果所有 queue 都空，阻塞等待 new work signal
};
```

#### 2.2.3 Fiber 调度状态机

```
                    ┌──────────────┐
                    │   Ready      │ ← 可在任何线程上被调度
                    └──────┬───────┘
                           │ resume (work-stealing)
                           ↓
                    ┌──────────────┐
              ┌─────│   Running    │───── yield() ──→ Ready (re-enqueue)
              │     └──────┬───────┘
              │            │
              │     eventfd wait
              │     (recv/channel)
              │            ↓
              │     ┌──────────────┐
              └─────│   Waiting    │ ← epoll event → Ready
                    └──────────────┘
                           │ done
                           ↓
                    ┌──────────────┐
                    │    Done      │
                    └──────────────┘

关键变化:
  - yield() 把 fiber 放回 local queue（不是全局就绪队列）
  - Running 状态不绑定到特定线程
  - Fiber 被 resume 时可能和上次在不同线程上
```

#### 2.2.4 正确的 yield 语义

```cpp
// 当前 yield 实现（单线程）:
void Fiber::yield() {
    swapcontext(&ctx_, &g_scheduler->main_ctx_);
    // resume 时回到这里
}

// 多线程 yield 实现:
void Fiber::yield() {
    // 1. 保存当前上下文
    // 2. 把 fiber 放回 local queue
    // 3. 从 local queue 取下一个 fiber 来跑
    // 4. 如果没有, 尝试 steal
    // 5. 如果所有 queue 都空, 阻塞等 signal
}
```

这里不能用简单的 `swapcontext` 切回 main context，因为是多线程的。需要**每个 worker 线程有自己的调度上下文**。

#### 2.2.5 Work-stealing 策略

```
steal_attempt(thread_id):
  1. 随机选一个 victim thread（不选自己）
  2. 尝试从 victim.local_queue 尾部 pop 一个 fiber
     (Chase-Lev deque: 本地从头部 pop, 偷取者从尾部 pop)
  3. 成功 → 在自己的 local_queue 头部 push
  4. 失败 → 尝试 global_queue
  5. 全部空 → 自旋一会儿 → 阻塞 (condition_variable)
```

**为什么从尾部偷？**
- 本地 worker 从头部取（最近加入的，缓存热）
- 偷取者从尾部取（最久未执行的，缓存冷——但反正不在自己的 cache 里）
- 减少冲突

#### 2.2.6 Fiber 迁移

一个 fiber 在 `yield` / `recv` 后被另一个线程 resume。这是安全的，因为：

1. **无共享状态**：每个 Agent 独立 workspace
2. **ucontext 可迁移**：保存的寄存器上下文可在任何线程恢复
3. **eventfd 线程安全**：`write(2)` 是原子的
4. **FiberState 需要 `std::atomic`** 保护

```cpp
struct Fiber {
    std::atomic<FiberState> state_;  // 改成 atomic
    // ... 其余不变
};
```

#### 2.2.7 Thread Safety 清单

| 资源 | 保护方式 | 说明 |
|------|---------|------|
| `Fiber::state_` | `std::atomic` | 调度器读写, fiber 读 |
| `Fiber::eventfd_` | 只读(创建后) | 创建后不变 |
| `Fiber::ctx_` | fiber 自己持有 | resume 时被调度器读/写 |
| `LocalQueue` | lock-free deque | 或 fine-grained lock |
| `GlobalQueue` | `std::mutex` | 低频率访问 |
| `Scheduler::epoll_fd_` | 一个线程 IO | 或 SO_REUSEPORT + 多个 epoll fd |
| `Agent workspace` | 无保护 | copy-on-write, 每个 fiber 独享 |

### 2.3 Layer 1: Blocking I/O Offload (Thread Pool)

**现有实现（复用）：**

```cpp
// serve/thread_pool.h  — 已有
class ThreadPool {
    void enqueue(std::function<void()> fn, int wake_evfd);
};
// serve/fiber.h
extern thread_local Fiber* g_current_fiber;
```

**阻塞操作流程（不变）：**

```
Fiber: (thread_pool:enqueue fn)
  → thread_pool.enqueue(fn, wake_evfd)
  → fiber state = Waiting
  → fiber yield()
  → (池线程执行 fn, 完成后 write(wake_evfd))
  → scheduler epoll 检测到 eventfd
  → fiber state = Ready → 入 local queue
  → (可能被其他线程 resume)
```

---

## 3. 实现计划

### Phase 1: Local Queue + 基础多线程（~300 行 C++）

**目标**：把单线程 scheduler 拆成 N 个 worker 线程，每个有 local queue。
不实现 steal，退化为 round-robin 或 static distribution。

```
改动范围:
  src/serve/fiber.h          — FiberState → std::atomic
  src/serve/scheduler.h      — WorkerThread + LocalQueue
  src/serve/scheduler.cpp    — 多线程 event loop
  CMakeLists.txt             — 可能需要 -pthread (通常已有)
  src/main.cpp               — 配置线程数

新增:
  src/serve/worker.h         — WorkerThread 类
  src/serve/worker.cpp       — 实现

验证:
  - fiber:spawn N 个 fiber, 每个做 CPU 密集计算
  - 确认 N 个线程都在跑（htop / perf stat）
  - orch:parallel 测试
```

### Phase 2: Work-stealing（~200 行 C++）

**目标**：实现 Chase-Lev work-stealing deque，空闲线程 steal。

```
改动范围:
  src/serve/worker.h         — 加 steal_one() 方法
  src/serve/worker.cpp       — 主循环: work → steal → sleep

关键算法:
  Chase-Lev concurrent deque:
    - push: lock-free (atomic head)
    - pop: lock-free (atomic tail)
    - steal: CAS on tail (只有 steal 需要 CAS)

验证:
  - N 个 CPU-bound fiber: 确认负载均衡
  - ant colony 搜索: 并发加速比接近 N×
```

### Phase 3: 细粒度调优（~100 行 C++）

**目标**：性能优化 + 调度指标暴露。

```
优化:
  - 自旋策略: 先 spin 100ns, 再 steal, 再阻塞
  - NUMA-aware: 尽量不跨 NUMA node steal
  - epoll 分摊: 所有 worker 都能处理 wake eventfd

指标暴露:
  - 暴露调度器指标给 Aura (scheduler:stats)
  - queue 长度, steal 次数, idle 时间
  - 让 Agent 能 self-monitor 调度状态
```

### Phase 4: 自适应调度（~150 行 Aura + ~50 行 C++）

**目标**：调度策略可配置/可 self-modify。

```
新增原语:
  (scheduler:worker-count)       → 线程数
  (scheduler:set-steal-strategy) → 配置 steal 策略
  (scheduler:pin agent-id)       → fiber 固定到某线程 (affinity)

Aura 层:
  lib/std/scheduler.aura — 调度策略适配器
```

---

## 4. 收益预期

### 4.1 Ant colony 搜索（核心场景）

| 指标 | 当前 (单线程) | Phase 1 (N worker) | Phase 2 (work-steal) |
|------|:-----------:|:-----------------:|:-------------------:|
| 260 变异搜索 | ~260ms | ~260ms/N | ~260ms/N |
| 16 核加速比 | 1× | ~8-12× | ~14-16× |
| IPC 次数 | 1 | 1 | 1 |
| 锁争用 | 0 | 低 | 很低 (lock-free) |

### 4.2 Agent 编排

| 指标 | 当前 | Phase 1 | Phase 2 |
|------|:---:|:-------:|:-------:|
| 10 agent 并行编译 | 串行 500ms | 并行 ~50ms | 并行 ~50ms |
| LLM + 编译混合 | LLM 卡住其他 | 不卡 | 不卡 |
| `orch:parallel` | 逻辑并行 | 真正并行 | 真正并行 |

### 4.3 内存开销

| 组件 | 每项 | N=8 总计 |
|------|:----:|:--------:|
| Fiber (2MB 栈) | 2MB | 2MB × fiber 数 |
| Worker thread 内核栈 | 8KB | 64KB |
| Local queue (初始) | 64 指针 | 4KB |
| 总开销 (100 fiber) | — | ~200MB |

---

## 5. 风险与对策

### 5.1 ucontext 跨线程

**风险**：`swapcontext` 是 POSIX 标准，但跨线程使用依赖实现。
**测试验证**：在 x86_64 Linux（glibc）上已确认安全。其他平台需要验证。

### 5.2 并发 GC

**风险**：Aura 的 GC（双 arena）不是线程安全的。多个 fiber 同时跑时不能同时 GC。
**对策**：每个 fiber 有自己独立的 arena（已有 `fiber:spawn` 的 workspace 隔离）。或者 GC 在 `yield` 时做。

### 5.3 栈增长

**风险**：mmap 分配 2MB 栈，但在多线程时每个线程也有自己的内核栈。
**对策**：fiber 栈和线程栈互不干涉。`valgrind --tool=memcheck` 验证。

### 5.4 Fiber 在错误线程上 eventfd

**风险**：fiber 被线程 B resume 后，它的 eventfd 在 epoll 上的注册可能在线程 A 上。
**对策**：epoll 设计为跨线程安全的。或者所有 eventfd 注册到一个共享 epoll fd，由一个轻量级 dispatch 线程处理。

---

## 6. 验收标准

### Phase 1
- [ ] N 个 worker 线程，每个有独立 local queue
- [ ] fiber:spawn N 个 CPU-bound fiber，N 个 CPU 都满载
- [ ] 已有 bench/regression 测试全部通过
- [ ] `orch:parallel` 使用并行 fibers 且速度提升

### Phase 2
- [ ] Work-stealing 实现（Chase-Lev deque）
- [ ] 线程空闲时从别的线程 steal fiber
- [ ] 负载不均场景下加速比优于 round-robin
- [ ] Lock-free steal 路径，无级联阻塞

### Phase 3
- [ ] 调度器指标暴露给 Aura
- [ ] Ant colony 搜索 benchmark: 260 变异 < 260/16 ms

### Phase 4
- [ ] Agent 可配置/监控调度策略
- [ ] scheduler.aura 模块可用

---

## 7. 未解决的问题

1. **epoll 所有权**：所有 eventfd（stdin + session eventfd）在一个 epoll fd 上，还是每个 worker thread 有自己的 epoll？
   - 折衷：一个 shared epoll + dispatch 线程，或 SO_REUSEPORT 风格每个 worker 一个 epoll fd。

2. **Fiber migration 影响**：fiber 在不同线程上 resume 对 `g_current_fiber` 正确性有影响吗？
   - 答：没有。`thread_local g_current_fiber` 只在 fiber 自己跑时有用，不同线程上指向不同 fiber 是正确的。

3. **Work-stealing 与 cache 局部性**：fiber 被 steal 到另一个线程后，它的 workspace（arena）可能不在那个线程的 cache 中。对 <1ms 的微变异影响可接受，但对大 workspace 可能需要 fiber-aware stealing。

4. **线程数自动配置**：`std::thread::hardware_concurrency()` 作为默认值，但允许 Agent 或配置覆盖。

---

## 8. 附录：与 Go GMP 的对比

| 维度 | Go GMP | Aura (本设计) |
|------|--------|---------------|
| 调度单元 | goroutine (2KB 栈) | fiber (2MB 栈, ucontext) |
| 栈增长 | 动态 (copy) | 静态 (guard page) |
| 抢占 | 监控式 (sysmon) | 协作式 (yield) |
| 阻塞 | syscall → 线程分离 | thread pool offload |
| M:N 模型 | yes | yes |
| 无共享状态 | channel (推荐) | workspace (强制) |
| Work-stealing | yes (全局+本地) | yes (Chase-Lev) |
| GC | STW + 并发标记 | arena 隔离 per fiber |

**为什么不用 Go 式抢占？**
Aura 的 Agent 工作负载是 **短任务 + 显式 yield**。没有 goroutine 那种 daemon loop。
协作式 yield 足够，实现更简单，不需要编译器支持（no stack copying）。

**为什么用 ucontext 而不是 boost.context / libco?**
ucontext 是 POSIX 标准，零依赖，已在 Aura 中使用。
2MB 栈对深度递归 eval 足够（当前实测安全）。

---

## 9. 变更概述

```
新增文件:
  src/serve/worker.h          — WorkerThread 声明
  src/serve/worker.cpp        — WorkerThread 实现
  src/serve/scheduler.h       — 更新 (WorkerThread 向量 + GlobalQueue)
  docs/design/concurrency_model.md  — 本文

修改文件:
  src/serve/fiber.h           — FiberState → std::atomic
  src/serve/fiber.cpp         — 移除 g_scheduler main_ctx 依赖
  src/serve/scheduler.cpp     — 多线程 event loop
  src/main.cpp                — --worker-threads=N 参数
  CMakeLists.txt              — 无变化 (已有 pthread)
  tests/suite/worksteal.aura  — 新测试用例
```
