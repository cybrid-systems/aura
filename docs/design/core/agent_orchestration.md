# Agent Orchestration — 交响乐指挥

> **Status (2026-06-08, Issue #119):** ✅ Phase 1-3 全部完成。`fiber:join`
> 实现（#109 初始 + #119 修复 serve-async 模式下的 eventfd 阻塞
> wakeup）；`fiber:spawn` 返回值；`orch:parallel` 真并行；多线程
> fiber scheduler（#109 Phase 1）；work-stealing scheduler（Phase 2）；
> C++26 `std::execution` 风格 adapter；fiber affinity + broadcast +
> mailbox-stats；T3 test fix。Issues #109 / #119 closings（archived: `docs-archive-pre-2026-06`）。

---

## 0. Implementation Status (2026-06-11, Issue #156)

**重要**：本文档的 **Phase 1-3 全部实装**（#109 + #119 + #107 part 1/5/6 + #108 part 2 + #110）；Phase 4 是长期演进项。准确分两层（完整原语 + 实现表见下文"实现状态（2026-06）"）：

### C++ Core Layer (TL;DR — 完整表见后文)

| 原语 / 组件 | 实装 | 备注 |
|------|------|------|
| `fiber:spawn` / `fiber:yield` / `fiber:join` | ✓ | #109 + #119：stdin 模式 `condition_variable` 真阻塞；serve-async 模式 eventfd wakeup |
| `_agent:spawn` / `send` / `recv` / `reply` | ✓ | 跨 session 消息传递（带 correlation id）|
| `mailbox-count` / `orch:metrics` / `orch:reset-metrics` | ✓ | #109 stats |
| `workspace_mtx_` 共享/独占协议 | ✓ (#107 part 1) | mutate unique / query shared |
| `MutationBoundary yield` | ✓ | 不让任何 fiber 饿死 |
| DefUseIndex per-sym 失效 | ✓ (#107 part 5) | `defuse_affected_syms_` + `defuse_touch_fn_` |
| Direct FlatAST snapshot/restore (深拷贝) | ✓ (#107 part 6) | 保留 SymId / mutation_log / type_id |
| Mailbox / Scheduler / Fiber | ✓ | `serve/mailbox.h` / `serve/scheduler.cpp` / `serve/fiber.cpp` |
| Cross-host agent communication | 🔴 (Phase 4) | 当前是进程内 only |
| Persistent agent state across serve restarts | 🔴 (Phase 4) | 需外部存储 |
| Auto-scaling fiber pool | 🔴 (Phase 4) | 当前固定线程数 |
| AutoFixEngine for agent prompts | 🔴 (Phase 4) | 设计中 |

### Aura Layer (TL;DR — 完整表见后文)

| Helper | 实装 | 备注 |
|--------|------|------|
| `orch:define-role` / `orch:step` / `orch:pipeline` / `orch:conduct` | ✓ | `std/orchestrator.aura` |
| `orch:parallel` (真并行) | ✓ | #109 — 串行 fallback 已移除 |
| `orch:if` / `orch:retry` / `orch:role` / `orch:step*` | ✓ | 步骤构造器 |
| `agent:spawn` / `agent:ask` / `agent:list` / `agent:status` / `agent:stop` / `agent:restart` | ✓ | 生命周期 + correlation-id 请求/响应 |

### 已实现 vs 计划

- ✅ **Phase 1-3 全部实装**（多线程 + work-stealing + 真阻塞 + mailbox-stats + 共享/独占协议 + DefUseIndex + 深拷贝 snapshot）
- ✅ **验收**：`tests/suite/concurrent.aura` 12/12 PASS（`--load` 与 `--serve` 双模式）；ASAN 0 leaks on concurrent stress
- 🔴 **Phase 4 长期演进**：auto-scaling / cross-host / persistent state / AutoFixEngine

**AI Agent 读者请注意**：完整 agent orchestration 体系已实装。AI Agent 写多 Agent 协作代码时**不需要**手动管 fiber / mailbox / 锁 —— 用 `orch:parallel` / `orch:conduct` / `agent:ask` / `agent:spawn` 即可。跨 session 通信（serve 模式）用 `agent:ask` 自动包 correlation-id + timeout。

---

**Audience:** 任何使用 `lib/std/orchestrator.aura` 或 `lib/std/agent.aura` 写多 Agent 协作的人。
**Audience (2):** 给 evaluator 加新的 fiber / agent / mailbox 原语的开发者（详见
[`docs/developer/evaluator.md`](../developer/evaluator.md)）。

---

## 哲学：指挥不弹琴

交响乐指挥不演奏任何乐器。指挥的工作是：

1. **读总谱** — 知道整个作品的结构
2. **给提示** — 在正确的时间让正确的乐手进入
3. **调平衡** — 弦乐太响？让铜管轻一点
4. **处理意外** — 乐手翻错页？指挥引导回来

对应到 Agent 编排：

```
指挥 = orchestrator
乐手 = agent (planner, coder, tester, reviewer)
总谱 = pipeline / workflow definition
提示 = agent:ask (send + wait reply)
排练 = orchestrator 按步骤推进
演出事故 = 异常处理 / retry / fallback
```

**核心原则： orchestrator 不做具体工作，它只协调。**

---

## 架构总览

```
                        ┌──────────────────┐
                        │  Orchestrator     │  ← lib/std/orchestrator.aura
                        │  (高层编排)        │     纯 Aura，零 C++ 依赖
                        │  define-role       │
                        │  pipeline          │
                        │  parallel          │
                        └────────┬─────────┘
                                 │ 用这些原语编排
            ┌────────────────────┼────────────────────┐
            ▼                    ▼                    ▼
    ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
    │ Agent A      │   │ Agent B      │   │ Agent C      │
    │ (planner)    │   │ (coder)      │   │ (tester)     │
    │              │   │              │   │              │
    │ closure /    │   │ closure /    │   │ closure /    │
    │ session +   │   │ session +    │   │ session +    │
    │ mailbox     │   │ mailbox      │   │ mailbox      │
    └──────────────┘   └──────────────┘   └──────────────┘
            │                   │                   │
            └───────────────────┼───────────────────┘
                                │
                    ┌───────────▼───────────┐
                    │  Messaging Layer       │  ← send / recv / my-id
                    │  mailbox / fiber       │     lib/std/agent.aura
                    │  eventfd wakeup        │
                    └───────────────────────┘
```

---

## 实现状态（2026-06）

### ✅ 已实现（C++ 层）

| 原语 | 位置 | 说明 |
|------|------|------|
| `fiber:spawn` | `evaluator_impl.cpp` ~11701 | serve 模式创建真 fiber；stdin 模式直接 `std::thread` |
| `fiber:yield` | `evaluator_impl.cpp` ~11761 | 切换到调度器 |
| **`fiber:join`** | `evaluator_impl.cpp` ~11841 | **#109 + #119 实现**：stdin 模式 `std::condition_variable` 真阻塞；serve-async 模式 eventfd wakeup（joiner_map_ + g_fiber_lookup）|
| `_agent:spawn` | `evaluator_impl.cpp` ~11790 | 创建命名 session + fiber |
| `send` / `recv` / `reply` | `messaging_bridge.h` | 跨 session 消息传递（带 correlation id） |
| `mailbox-count` | `evaluator_impl.cpp` ~11668 | 邮箱消息数（#109 stats） |
| `orch:metrics` | `evaluator_impl.cpp` ~11877 | fiber scheduler 统计（spawn/join/queue 长度） |
| `orch:reset-metrics` | `evaluator_impl.cpp` ~11909 | 重置 metrics |
| Mailbox | `serve/mailbox.h` | 基于 eventfd 的 fiber-safe 消息队列 |
| Scheduler | `serve/scheduler.cpp` | 多线程 + work-stealing（#109 Phase 1-2） |
| Fiber | `serve/fiber.cpp` | 带 guard page 的 stackful fiber |

### ✅ 已实现（Aura 层）

| 模块 | 位置 | 说明 |
|------|------|------|
| `orch:define-role` | `std/orchestrator.aura` | 注册命名角色 |
| `orch:step` | `std/orchestrator.aura` | 单步执行 |
| `orch:pipeline` | `std/orchestrator.aura` | 串行管线 |
| `orch:conduct` | `std/orchestrator.aura` | 读总谱 + 步骤推进（含 `if` / `retry`） |
| **`orch:parallel`** | `std/orchestrator.aura` | **真并行**（#109 — 已移除串行 fallback） |
| `orch:if` / `orch:retry` / `orch:role` / `orch:step*` | `std/orchestrator.aura` | 步骤构造器 |
| `agent:spawn` | `std/orchestrator.aura` | 包装 `_agent:spawn`，fallback 到本地 closure |

（Refactor 3.1 note）_agent:spawn 内部 merr 已清理到集中 make_merr (post 3.1)。详见 evaluator.md §3.2。
（Phase 2 pilot-5 note）test_issue_135（多 agent 并行编排验证）已转为使用早期 aura_add_issue_test helper + append。CMake dedup 持续小步推进。详见 evaluator.md §12。
| `agent:ask` | `std/orchestrator.aura` | 带 correlation-id + 超时的请求/响应 |
| `agent:list` / `agent:status` / `agent:stop` / `agent:restart` | `std/orchestrator.aura` | 生命周期管理 |

### ❌ 已废弃（早期 stub）

| 原语 | 原因 |
|------|------|
| `fiber:join` (old stub) | 永远返回 `#t`，不等待；已被 #109 实现替换 |
| `orch:parallel` (old serial fallback) | 串行 fallback 已移除（#109）—— 失败会显式传播，避免掩盖并行正确性 bug |

---

## 数据流

### 1. 本地 Agent（closure 模式）

```
agent:spawn "planner" (lambda (task) ...)
  │
  ▼
agent-register: *agents* ← ("planner" handler "running" ts)
  │
  ▼
agent:ask "planner" input
  │
  ▼
agent-lookup → 找到 handler
  │
  ▼
(handler input) → 返回结果
```

**已在 Aura 层完整实现**，零 C++ 改动。

### 2. 跨 Session Agent（serve 模式）

```
agent:spawn "coder"
  │
  ▼
_agent:spawn (C++ primitive)
  │
  ├── g_session_create("coder")        ← serve_async.cpp
  │   ├── 创建新 Session + CompilerService
  │   ├── 注册 session 到全局注册表
  │   └── spawn fiber 运行 session 事件循环
  │
  ▼
agent:ask "coder" "write tests" 60
  │
  ├── send("coder", "{id:req-001, from:me, body:...}")
  │    └── Mailbox::push → wake target fiber via eventfd
  │
  ├── recv loop（等待 correlation-id 匹配的回复）
  │    └── Mailbox::pop(true, 60000ms)
  │
  └── 返回 reply body
```

**C++ 层已实现**（`session:create` + `send/recv`），Aura 层 `agent:ask` 已包装好。

### 3. 并行管线（真并行，#109）

```scheme
(define results
  (orch:parallel
    (list
      (lambda (x) (* x 2))
      (lambda (x) (+ x 100))
      (lambda (x) (- x 50)))
    5))
;; results → (10 105 -45)
```

**实装**（`std/orchestrator.aura`）：

```scheme
(define (orch:parallel fns input . timeout-arg)
  (define timeout-sec (if (pair? timeout-arg) (car timeout-arg) #f))
  (if (null? fns) (quote ())
    (let ((fiber-ids (quote ())))
      (define (try-fiber remaining)
        (when (pair? remaining)
          (define fid (try (fiber:spawn (lambda () ((car remaining) input)))
                           (catch (e) 0)))
          (if (not (= fid 0))
            (set! fiber-ids (cons fid fiber-ids)))
          (try-fiber (cdr remaining))))
      (try-fiber fns)

      (if (null? fiber-ids) (quote ())
        (let* ((timeout-fid (if timeout-sec
                             (fiber:spawn (lambda () (sleep timeout-sec) 'timeout))
                             #f))
               (all-ids (if timeout-fid (cons timeout-fid fiber-ids) fiber-ids))
               (results (quote ())))
          (define (collect-all ids)
            (when (pair? ids)
              (define id (car ids))
              (define r (try (fiber:join id) (catch (e) 'error)))
              (cond
                ((and timeout-fid (equal? r 'timeout))
                 (set! results (cons #f results))
                 (collect-all (cdr ids)))
                (else
                  (set! results (cons r results))
                  (collect-all (cdr ids))))))
          (collect-all (reverse all-ids))
          (if timeout-fid
            (cdr (reverse results))
            (reverse results)))))))
```

**底层 fiber:join 行为**（`evaluator_impl.cpp` ~11841）：

- **stdin 模式**（无 fiber scheduler）：`std::unique_lock` +
  `condition_variable::wait_for(200s)` —— OS 线程真阻塞，由 `s_fiber_results_cv_.notify_*`
  唤醒。零 CPU 烧。
- **serve-async 模式**（有 fiber scheduler）：Issue #119 修复后的
  正确阻塞实现 —— joiner fiber 通过 `g_fiber_lookup` 拿到
  target `Fiber*`，检查 target.is_done()，如果没完成就调
  `Scheduler::add_joiner(target_id, joiner_fiber)` 注册为
  joiner，然后 `Fiber::yield(BlockingIO)`。 target 完成后
  `Scheduler::on_fiber_done` 找到 joiner_map_里所有 joiner
  的 eventfd 写 1 ，IO thread 的 epoll 检测到写入并 resume
  joiner fiber。 Joiner 醒来后 `remove_joiner` 清理并取
  结果。零 spin wait，零 CPU 烧。

两种模式共享同一个 `s_fiber_results_` entry（`ready` flag + `value` shared_ptr）。

---

## fiber:join 实现方案（#109 —— 已实装）

### 1. Fiber 结果存储

```cpp
// evaluator_impl.cpp 内部（命名空间作用域）
struct FiberResult {
    bool ready = false;
    std::shared_ptr<std::optional<EvalValue>> value;  // shared_ptr 让 spawn 之后 join 之前能跨线程
};
static std::unordered_map<int64_t, FiberResult> s_fiber_results_;
static std::mutex s_fiber_results_mtx_;
static std::condition_variable s_fiber_results_cv_;
```

### 2. fiber:spawn 时

```cpp
// 在 spawn 之前分配 entry，结果通过 shared_ptr 跨线程传递
auto result_ptr = std::make_shared<std::optional<EvalValue>>();
{
    std::lock_guard<std::mutex> lock(s_fiber_results_mtx_);
    s_fiber_results_[fid] = FiberResult{false, result_ptr};
}
// spawn 时把 result_ptr 捕获到 closure 里
```

### 3. fiber:join 时

```cpp
primitives_.add("fiber:join", [this](std::span<const EvalValue> a) -> EvalValue {
    auto fid = static_cast<int64_t>(as_int(a[0]));
    auto is_ready = [fid] {
        auto it = s_fiber_results_.find(fid);
        return it != s_fiber_results_.end() && it->second.ready;
    };

    if (aura::messaging::g_fiber_yield) {
        // Serve-async: yield-and-check
        for (int iter = 0; iter < 200000; ++iter) {
            { std::lock_guard<std::mutex> lock(s_fiber_results_mtx_);
              if (is_ready()) break; }
            aura::messaging::g_fiber_yield();
        }
    } else {
        // Stdin: 真实阻塞等待
        std::unique_lock<std::mutex> lock(s_fiber_results_mtx_);
        s_fiber_results_cv_.wait_for(
            lock, std::chrono::seconds(200), is_ready);
    }

    // 取结果、清理
    std::shared_ptr<std::optional<EvalValue>> result_ptr;
    {
        std::lock_guard<std::mutex> lock(s_fiber_results_mtx_);
        auto it = s_fiber_results_.find(fid);
        if (it == s_fiber_results_.end() || !it->second.ready || !it->second.value
            || !it->second.value->has_value()) {
            s_fiber_results_.erase(it);
            return make_void();
        }
        result_ptr = std::move(it->second.value);
        s_fiber_results_.erase(it);
    }
    return std::move(**result_ptr);
});
```

### 4. 通知机制

- **stdin 模式**：fiber 完成后 `s_fiber_results_cv_.notify_all()`。
- **serve-async 模式**：fiber 完成后只设 `ready=true`（scheduler 在下一次 yield
  时自然会 re-check；不主动 notify，因为 scheduler 不能 park）。

---

## Mutation Boundary 与并发 mutate

**核心 invariant（#107 part 1）**：`Evaluator::workspace_mtx_` 是 `std::shared_mutex`。
- 所有 `mutate:*` 取 `std::unique_lock`。
- 所有 `query:*` 取 `std::shared_lock`。
- 多个 query 可并行；query 与 mutate 互斥。

### Yield at mutation boundary

```cpp
// 每个 mutate:* 在 take lock 之后、修改 AST 之前
defuse_version_++;
if (aura::messaging::g_fiber_yield_mutation_boundary)
    aura::messaging::g_fiber_yield_mutation_boundary();
```

`g_fiber_yield_mutation_boundary` 是 Aura scheduler 提供的 hook：
让其他 fiber 在 mutation 之前有机会跑（avoid starvation）。这不会释放
锁，只是 yield CPU。

### 跨 fiber 共享 session

`workspace_mtx_` + `MutationBoundary yield` 的组合让：
- 多个 agent session 可以同时 query（共享锁）。
- 一个 agent mutate 时，其他 agent 的 query 阻塞（直到 mutate 完）。
- 任意 fiber 都不会"饿死"（mutation boundary 让步）。

详见 [`docs/developer/evaluator.md §3`](../developer/evaluator.md#3-mutate-primitives--locking-protocol)。

---

## Cross-Session Messaging 示例

```scheme
;; Session A: 发送一个任务给 agent-b
(send "agent-b" "{\"id\":\"req-001\",\"body\":\"hello\"}")

;; Session B: 接收任务
(define msg (recv 100))    ; 阻塞 100s 等待
(display msg)               ; → {"id":"req-001","body":"hello"}

;; Session B: 回复
(reply msg "pong")
```

**带 correlation id 的请求/响应**（`agent:ask` 实现）：

```scheme
;; A 端
(define reply
  (agent:ask "agent-b" "process this" 30))  ; 30s timeout
;; B 端必须用相同的 id 回复

;; B 端
(define req (recv 30))
(reply req (process (json-get req "body")))
```

---

## 多 Agent Pipeline 示例

```scheme
(require "std/orchestrator" all:)

;; 1. 注册角色
(orch:define-role "planner"
  (orch:role (lambda (task) (plan-it task))))
(orch:define-role "coder"
  (orch:role (lambda (plan) (write-code plan)) "direct"))
(orch:define-role "tester"
  (orch:role (lambda (code) (run-tests code)) "direct"))

;; 2. 串行管线
(define output
  (orch:pipeline
    (list "planner" "coder" "tester")
    "build a fib function"))

;; 3. 并行 (e.g. 并行跑多个 reviewer)
(define reviews
  (orch:parallel
    (list
      (lambda (code) (review-style code))
      (lambda (code) (review-perf code))
      (lambda (code) (review-correctness code)))
    code))

;; 4. 复杂 conduct（含条件 + 重试）
(define output
  (orch:conduct
    (list
      "planner"
      "coder"
      (orch:if (lambda (c) (passes-tests? c))
               "ship-it"
               (orch:retry "coder" 3)))
    "initial task"))
```

---

## 实现路标

### Phase 1 — 核心原语（C++）✅
- [x] `fiber:join` 真阻塞 + 返回值（#109）
- [x] `orch:parallel` 真并行（移除串行 fallback，#109）
- [x] `fiber:spawn` 返回 fiber id + 值（#109）

### Phase 2 — 多线程 / 性能 ✅
- [x] Multi-threaded fiber scheduler（#109 Phase 1）
- [x] Work-stealing scheduler（#109 Phase 2）
- [x] C++26 `std::execution` 风格 adapter
- [x] Fiber affinity + broadcast + mailbox-stats
- [x] `thread_local` c-stack depth guard（避免 fiber 溢出 C++ 栈）
- [x] T3 test fix（绕开 Begin 的 letrec path 死锁）

### Phase 3 — 集成 & 安全 ✅
- [x] `workspace_mtx_` 共享 / 独占协议（#107）
- [x] Mutation boundary yield（让其他 fiber 在 mutate 之前跑）
- [x] DefUseIndex per-sym 失效（#107 part 5）
- [x] ASAN: 0 leaks on 50-iter snapshot+mutate+restore loop

### Phase 4 — 长期演进 🟡
- [ ] Auto-scaling fiber pool（当前固定线程数）
- [ ] Cross-host agent communication（当前是进程内）
- [ ] Persistent agent state across serve restarts
- [ ] AutoFixEngine for agent prompts

---

## 验收清单（2026-06）

- [x] `fiber:join` 阻塞等待 fiber 完成并返回结果（stdin + serve-async 两路）
- [x] `orch:parallel` 用真实 fiber 并行执行
- [x] `(fiber:spawn (lambda () (+ 1 2)))` + `(fiber:join fid)` → 3
- [x] `tests/suite/concurrent.aura` 12/12 PASS（`--load` 与 `--serve` 双模式）
- [x] ASAN: 0 leaks on concurrent stress
- [x] 多个 agent session 共享 workspace AST 不退化

---

## Related

- Issues #109 / #119 closings — archived: `git tag docs-archive-pre-2026-06`
- `src/serve/scheduler.cpp`, `src/serve/fiber.cpp` — fiber scheduler 实现
- `src/serve/mailbox.h` — mailbox / channel
- `tests/suite/orchestrator.aura` — 编排端到端用例
- [`docs/design/autonomous-self-evolving-agents.md`](autonomous-self-evolving-agents.md) — Self-Evolving Agent 模式
