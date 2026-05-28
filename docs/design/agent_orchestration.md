# Agent Orchestration — 交响乐指挥

**更新：2026-05-28**
**状态：Design**
**Issue：** [#15 P1] 缺少 Agent 编排原语

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

## 现有基础设施

```
                        ┌──────────────────┐
                        │  Orchestrator     │
                        │  (高层编排)        │
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
    │ fiber +      │   │ fiber +      │   │ fiber +      │
    │ session +    │   │ session +    │   │ session +    │
    │ mailbox      │   │ mailbox      │   │ mailbox      │
    └──────────────┘   └──────────────┘   └──────────────┘
            │                   │                   │
            └───────────────────┼───────────────────┘
                                │
                    ┌───────────▼───────────┐
                    │  Messaging Layer       │
                    │  send / recv / my-id   │
                    │  mailbox registry      │
                    └───────────────────────┘
```

**已实现的（绿）：** `send`/`recv`/`my-id`/`session-active?`/`mailbox-count`/`fiber:spawn`/`fiber:yield`

**缺失的（红）：**
- `agent:spawn` — 创建带名字 + 代码的 agent 会话
- `agent:ask` — 发送消息 + 等待回复（带超时）
- 真正的 fiber 并行调度
- 管线控制流（条件、重试、fan-out/fan-in）

---

## 设计

### 1. `agent:spawn` — 聘用乐手

```scheme
;; 语法
(agent:spawn name code) → agent-id | error

;; 示例
(define planner (agent:spawn "planner"
  '(begin
     (define (plan task) ...)
     (let loop ()
       (define msg (recv))
       (cond ((string=? msg "work")
              (send "default" (plan task)))
             ((string=? msg "stop") "stopped")
             (else (loop)))
       (loop)))))
```

**实现：**

```cpp
// evaluator_impl.cpp
primitives_.add("agent:spawn", [this, mev](const auto& a) -> EvalValue {
    if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
        return mev("bad-arg", "usage: (agent:spawn name code)");

    auto name = string_heap_[as_string_idx(a[0])];
    auto code = string_heap_[as_string_idx(a[1])];

    // 1. 创建新 session（复用 existing serve session infrastructure）
    // 2. 在 session 中 set-code + eval (agent 的主循环)
    // 3. 启动 fiber 运行 agent 的主循环
    // 4. 返回 agent-id

    auto* svc = find_my_service();
    if (!svc) return mev("no-service", "not in serve mode");

    auto agent_id = svc->spawn_agent(name, code);
    if (agent_id.empty())
        return mev("spawn-failed", "could not spawn agent");

    auto sidx = string_heap_.size();
    string_heap_.push_back(agent_id);
    return make_string(sidx);
});
```

**行为：**
1. 创建一个新的命名 session（`CompilerService` 实例）
2. 在该 session 中 `set-code` 加载 agent 代码
3. 启动 fiber 运行 `eval-current`（agent 的 event loop 开始运行）
4. 返回 agent 的 session ID 字符串
5. agent 的 mailbox 自动工作（`recv` 在自己的 mailbox 上阻塞）

### 2. `agent:ask` — 指挥给提示

```scheme
;; 语法
(agent:ask target msg [timeout-sec]) → reply | timeout-error

;; 示例
(define plan (agent:ask planner
  "请为 fib(100) 设计优化方案"
  30))  ;; 30秒超时
```

**实现：**

```cpp
primitives_.add("agent:ask", [this, mev](const auto& a) -> EvalValue {
    // 1. send message to target agent's mailbox
    // 2. 生成一个唯一的 correlation-id
    // 3. 在消息中包含 correlation-id + 当前 session 的 reply-to
    // 4. 调用 recv-timeout 等待带该 correlation-id 的回复
    // 5. 超时返回错误
});
```

**消息格式（结构化）：**

```
send → {:from "conductor" :id "req-001" :type :request :body "..."}
recv ← {:from "planner"  :id "req-001" :type :reply   :body "..."}
```

`correlation-id` 让 `agent:ask` 能识别哪个回复是自己的。不在 busy waiting 里混入其他消息。

### 3. `orch:parallel` — 真并行

**现状：** 串行 cons 递归，注释写着"fiber:spawn 可用时改为并行"

```scheme
;; 想要：
(orch:parallel (list fn1 fn2 fn3) input)
;; → 同时启动 3 个 fiber，各处理同一 input
;; → 等待全部完成，收集结果列表
```

**实现：**

```scheme
(define (orch:parallel fns input)
  (if (null? fns) (quote ())
    (let* ((results (quote ()))
           (spawned '()))
      ;; 为每个函数 spawn fiber
      (define (spawn-all remaining)
        (when (pair? remaining)
          (define id (fiber:spawn (lambda () ((car remaining) input))))
          (set! spawned (cons id spawned))
          (spawn-all (cdr remaining))))
      (spawn-all fns)
      ;; 收集结果（等待所有 fiber 完成）
      (define (collect-all pending)
        (when (pair? pending)
          (set! results (cons (fiber:join (car pending)) results))
          (collect-all (cdr pending))))
      (collect-all (reverse spawned))
      (reverse results))))
```

需要 `fiber:join` 原语——等待 fiber 完成并获取返回值。

### 4. 工作流 DSL — 总谱

```scheme
;; 总谱定义
(define workflow
  (orch:workflow "planner-coder-tester"
    :steps (list
      (orch:step "planner"    :input task :output plan)
      (orch:step "coder"      :input plan  :output code)
      (orch:parallel
        (list
          (orch:step "tester"  :input code :output test-result)
          (orch:step "reviewer" :input code :output review)))
      (orch:if (lambda (ctx) (string=? (ctx 'tester) "pass"))
        (orch:step "deployer" :input code :output "deployed")
        (orch:step "coder"    :input (ctx 'review) :output fixed-code)))))

;; 执行
(orch:conduct workflow :input "写一个 fib 函数")
```

### 5. Agent 生命周期

```
状态图：

SPAWNED → RUNNING → STOPPED
           ↓ ↖️       ↑
         ERROR ───────┘
           ↓
       RESTARTING → RUNNING

API:
  (agent:status id)       → "running" | "stopped" | "error"
  (agent:stop id)         → 发送 stop 信号
  (agent:restart id)      → 重新 spawn
  (agent:list)            → 所有活跃 agent 列表
```

---

## 实现路标

### Phase 1 — 核心原语（C++）

| 优先级 | 原语 | 说明 |
|--------|------|------|
| P0 | `agent:spawn` | 创建命名 agent session + fiber |
| P0 | `agent:ask` | send + correlation-id + wait reply |
| P0 | `fiber:join` | 等待 fiber 完成并拿返回值 |
| P1 | `agent:stop` | 发送 stop 信号给 agent |
| P1 | `agent:list` | 列出所有活跃 agent |
| P2 | `agent:broadcast` | 发送给所有 agent |

### Phase 2 — 高层编排（Aura）

| 优先级 | 文件 | 说明 |
|--------|------|------|
| P0 | `std/orchestrator.aura` | 更新 `orch:parallel` 用真实 fiber |
| P1 | `std/orchestrator.aura` | 加 `orch:conduct` 工作流 DSL |
| P1 | `std/orchestrator.aura` | 加 `orch:if` / `orch:retry` 控制流 |
| P2 | `std/orchestrator.aura` | 加 `orch:fan-out` / `orch:fan-in` |

### Phase 3 — 示例与测试

| 优先级 | 内容 |
|--------|------|
| P0 | planner-coder-tester 三角色示例 |
| P1 | orchestrator suite 测试（20+ cases） |
| P2 | lint-agent 后台扫描示例 |
| P2 | 安全沙箱编排示例 |

---

## 增量交付

不一次做全部。第一步只加两个 C++ 原语：

```
agent:spawn + agent:ask
  ↓
然后用 Aura 写一个 planner-coder-tester 示例
  ↓
验证多 agent 协作 chain 走通后
  ↓
再迭代加 fiber:join / orch:parallel / 控制流
```

---

## 验收清单

- [ ] `agent:spawn` — 创建命名 agent session 并执行代码
- [ ] `agent:ask` — 向 agent 发送消息并等待回复（30s 超时）
- [ ] `orch:parallel` — 真实 fiber 并行（非串行 fallback）
- [ ] 3 角色示例 — planner 规划 → coder 编码 → tester 测试，通过 messaging 协作
- [ ] 所有新原语有 suite 测试
