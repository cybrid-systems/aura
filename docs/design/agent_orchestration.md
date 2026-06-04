# Agent Orchestration — 交响乐指挥

**更新：2026-05-29**
**状态：Phase 1 核心原语实现中**
**Issue：** [#21 P0] Missing core agent orchestration primitives

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

## 实现状态

### ✅ 已实现（C++ 层）

| 原语 | 位置 | 说明 |
|------|------|------|
| `fiber:spawn` | `evaluator_impl.cpp` | 在 serve 模式创建真实 fiber；stdin 模式直接运行 |
| `fiber:yield` | `evaluator_impl.cpp` | 从当前 fiber 切换到调度器 |
| `_agent:spawn` | `evaluator_impl.cpp` | 创建命名 session + fiber，用于跨 session 通信 |
| `_agent:list` | `evaluator_impl.cpp` | 列出所有活跃 agent session |
| `send` / `recv` | `messaging_bridge.h` | 跨 session 消息传递 |
| Mailbox | `serve/mailbox.h` | 基于 eventfd 的 fiber-safe 消息队列 |
| Scheduler | `serve/scheduler.cpp` | 单线程 epoll + ucontext 协程调度 |
| Fiber | `serve/fiber.cpp` | 带 guard page 的 stackful fiber |

### ✅ 已实现（Aura 层）

| 模块 | 位置 | 说明 |
|------|------|------|
| `orch:define-role` | `std/orchestrator.aura` | 注册命名角色 |
| `orch:step` | `std/orchestrator.aura` | 单步执行 |
| `orch:pipeline` | `std/orchestrator.aura` | 串行管线 |
| `orch:if` | `std/orchestrator.aura` | 条件分支 |
| `orch:retry` | `std/orchestrator.aura` | 重试机制 |
| `agent:spawn` | `std/orchestrator.aura` | 包装 `_agent:spawn`，fallback 到本地 closure |
| `agent:ask` | `std/orchestrator.aura` | 带 correlation-id + 超时的请求/响应 |
| `agent:list` | `std/orchestrator.aura` | 列出 agent |
| `agent:status` / `stop` / `restart` | `std/orchestrator.aura` | 生命周期管理 |

### ❌ 待实现

| 原语 | 问题 | 影响 |
|------|------|------|
| **`fiber:join`** | stub，永远返回 `#t`，不等待 | `orch:parallel` 无法工作 |
| **`orch:parallel`** | 注释写着"fiber:join 可用时改为并行" | 现在还是串行 |
| **`fiber:spawn` 传值返回** | closure `void()` 不返回值 | fiber 完成后的结果无法获取 |

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

### 3. 并行管线

```
orch:parallel (list fn1 fn2 fn3) input
  │
  ├── fiber:spawn (lambda () (fn1 input))  → fid1
  ├── fiber:spawn (lambda () (fn2 input))  → fid2
  ├── fiber:spawn (lambda () (fn3 input))  → fid3
  │
  ├── fiber:join fid1  → result1
  ├── fiber:join fid2  → result2
  ├── fiber:join fid3  → result3
  │
  └── (list result1 result2 result3)
```

**当前状态：** `fiber:spawn` 在 serve 模式创建真实 fiber，但 `fiber:join` 不等待、不返回值。

---

## fiber:join 实现方案

### 问题

`fiber:spawn` 的 closure 签名是 `void()`，无法返回值。
`fiber:join` 需要阻塞直到 fiber 完成，然后取出返回值。

### 方案

**Step 1: Fiber 结果存储**

在 `evaluator_impl.cpp` 中维护一个全局 map：

```cpp
// evaluator 内部
std::unordered_map<int64_t, std::optional<EvalValue>> fiber_results_;

// fiber:spawn 时：
auto fid = g_fiber_spawn([this, cid, &fiber_results_]() {
    auto result = apply_closure(cid, {});
    fiber_results_[current_fiber_id] = result;
});
```

但 `g_fiber_spawn` 不返回当前 fiber ID... 我需要查看当前的 `g_fiber_spawn` 实现。

**Step 2: fiber:join 实现**

```cpp
primitives_.add("fiber:join", [this](const auto& a) -> EvalValue {
    auto fid = as_int(a[0]);
    auto it = fiber_results_.find(fid);
    while (it == fiber_results_.end() || !it->second.has_value()) {
        // Fiber not done yet — yield
        if (g_fiber_yield) g_fiber_yield();
        else break;
        it = fiber_results_.find(fid);
    }
    if (it != fiber_results_.end() && it->second.has_value())
        return *it->second;
    return make_void();
});
```

但问题是：在 `fiber:spawn` 的 closure 里，我们如何知道当前 fiber 的 ID？

### 简化方案

不追踪具体 fiber ID，而是让 `fiber:spawn` 返回一个 token，`fiber:join` 用这个 token 等待。

最简单的实现：在 closure 外部包一层，捕获结果的存储位置。

```cpp
// fiber:spawn closure 里不直接存，而是用一个共享指针
auto result_ptr = std::make_shared<std::optional<EvalValue>>();

auto fid = g_fiber_spawn([this, cid, result_ptr]() {
    *result_ptr = apply_closure(cid, {});
});

// 保存映射 fid → result_ptr
fiber_results_[fid] = result_ptr;

// fiber:join fid:
// 1. 查找 fiber_results_[fid]
// 2. 如果 result_ptr 有值，返回值
// 3. 否则 yield 并重试
```

这个方案不需要改变 Fiber 类或调度器，只需改动 `evaluator_impl.cpp` 中 `fiber:spawn` 和 `fiber:join` 的实现。

---

## 实现路标

### Phase 1 — 核心原语（C++）

| 优先级 | 原语 | 文件 | 说明 |
|--------|------|------|------|
| P0 | `fiber:join` | `evaluator_impl.cpp` | 等待 fiber 完成并返回值 |
| P0 | `orch:parallel` | `std/orchestrator.aura` | 启用真并行（移除串行 fallback） |
| P1 | `fiber:spawn` 返回值 | `evaluator_impl.cpp` | closure 改为可返回值 |

### Phase 2 — 测试与示例

| 优先级 | 内容 |
|--------|------|
| P0 | planner-coder-tester 三角色示例 |
| P1 | orchestrator suite 测试（20+ cases） |
| P2 | lint-agent 后台扫描示例 |

---

## 增量交付

不一次做全部。第一步只改两个文件：

1. `evaluator_impl.cpp`：`fiber:join` 非 stub，真等待 + 返回值
2. `std/orchestrator.aura`：`orch:parallel` 移除串行 fallback

---

## 验收清单

- [ ] `fiber:join` 阻塞等待 fiber 完成并返回结果
- [ ] `orch:parallel` 用真实 fiber 并行执行
- [ ] `(fiber:spawn (lambda () (+ 1 2)))` + `(fiber:join fid)` → 3
- [ ] 并行管线测试覆盖串行/并行/空列表

---

## Related: Self-Evolving Agent Patterns

For patterns of building **long-running** AI agents that **evolve
their own logic** at the code level (beyond prompt-level evolution),
see
[`docs/design/autonomous-self-evolving-agents.md`](autonomous-self-evolving-agents.md).
Covers the capability ladder (prompts → tool calling → strategy
swap → strategy evolution), 4 patterns with Aura-level code
skeletons, and safety mechanisms.
