# Aura Async Serve 架构设计

**2026-05-25** — 基于 stackful fiber 的异步 serve 模式

---

## 0. Implementation Status (2026-06-11, Issue #156)

**重要**：本文档的 **Phase 1-3 (fiber + scheduler + 集成) 已实装**（#109 + #119 + `agent_orchestration.md` 描述的 fiber 体系）。文档原版以设计稿撰写，本节补充实装状态。

### C++ Core Layer (`src/serve/fiber.cpp` / `src/serve/scheduler.cpp` / `src/serve/mailbox.h` / `src/compiler/evaluator_impl.cpp`)

| 组件 | 实装 | 备注 |
|------|------|------|
| `Fiber` (ucontext + mmap stack + guard page) | ✓ | `src/serve/fiber.cpp`；默认 2MB/纤程；guard page 触发 SIGSEGV 早期发现栈溢出 |
| `Fiber::resume` / `Fiber::yield` (swapcontext) | ✓ | 切换成本 ~50-100ns |
| `Scheduler` (epoll 事件循环 + ready queue + wait map) | ✓ | `src/serve/scheduler.cpp`；`epoll_wait` 阻塞，per-session eventfd 唤醒 |
| 多线程 fiber scheduler | ✓ (#109 Phase 1) | 多 thread pool |
| Work-stealing scheduler | ✓ (#109 Phase 2) | 跨线程 work 偷取 |
| C++26 `std::execution` 风格 adapter | ✓ | 适配器层 |
| `Mailbox` (fiber-safe 队列) | ✓ | `src/serve/mailbox.h`；push + 唤醒目标 fiber |
| `Session` (per-agent 连接 + eventfd) | ✓ | session 注册表 + std::shared_ptr 跨线程 |
| `recv` 5 行 yield 改造 | ✓ | `evaluator_impl.cpp`；mailbox 空时 yield 让调度器切走 |
| `send` 触发 eventfd 唤醒 | ✓ | 目标 fiber 状态 `Waiting` 时 `write(f->eventfd, 1)` |
| `--serve-async` flag (`main.cpp`) | ✓ | 现有 `--serve` 不变；新增分支 |
| `AURA_SERVE_ASYNC` 条件编译 | ✓ | 编译宏开关 |
| `recv` 跨平台 (macOS kqueue) | 🟡 | 当前 epoll 是 Linux 路径；macOS / Windows path 通过 `g_fiber_yield` hook fallback |
| 异常传播 (fiber throw → scheduler catch) | △ (基础) | 当前在 `main.cpp` --serve-async 路径捕获 |
| 优雅退出 (`Scheduler::stop`) | ✓ | 设 `running_ = false`，fiber 自然 Done |

### Aura Layer (`lib/std/agent.aura` / `lib/std/orchestrator.aura`)

| Helper | 实装 | 备注 |
|--------|------|------|
| `agent:spawn` | ✓ | 包装 `_agent:spawn` (C++ primitive) |
| `send` / `recv` / `reply` | ✓ | `lib/std/agent.aura` 包装 C++ 桥 |
| 跨 session JSON 协议 (`{"cmd":"session-send",...}`) | ✓ | `--serve-async` 模式 |
| `agent:list` / `agent:status` / `agent:stop` / `agent:restart` | ✓ | 生命周期管理 |

### 文档与实装的差异

- ✅ **已实装**：`Fiber` / `Scheduler` / `Mailbox` / `Session` / `recv` yield / `send` 唤醒 / `--serve-async` 入口
- 🟡 **文档原版以设计稿撰写**（Phase 1-3 的"~0.5d/0.5d/0.5d" 估算）：实装实际是 #109 + #119 两条 issue 累计 work，包含多线程 + work-stealing + `fiber:join` 真阻塞 + 跨 platform fiber 适配
- 🔴 **未做**：cross-host agent communication（进程内 only）；persistent agent state across serve restarts（需外部存储）

**AI Agent 读者请注意**：C++ 层 fiber/scheduler/send/recv 已全部实装（#109 + #119）。高级 Agent 编排请使用 `std/orchestrator.aura` + `agent:ask` / `orch:*`（见 `design/core/agent_orchestration.md` §0）。Serve 协议是当前 LLM Agent 自演化的主要驱动方式（与 EDSL + typed mutation 结合）。

---


## 1. 为什么需要异步

当前 `--serve` 模式：

```
Agent-A → {"cmd":"exec","code":"(send \"agent-b\" ...)(recv)"}
                ↓
          while(getline)  ← 阻塞在 eval 内部
                ↓
          Agent-B 永远不会被调度        ← 🚫 死锁
```

Agent-A 的 eval 占用主线程，`recv` 等消息期间其他 session 无法被服务。

异步后：

```
fiber-A: eval → (send) → (recv mailbox_empty) → yield
                ↓                     → 调度 fiber-B
fiber-B: eval → (send "agent-a" ...)  → push mailbox_A → wake fiber-A
                ↓
fiber-A: resumed → recv returns → continue eval
```

核心约束：**eval 一行不改**。AST、parser、type checker、JIT、AOT 全不动。

---

## 2. 架构总览

```
┌─────────────────────────────────────────────────────────┐
│  Scheduler (epoll 事件循环)                               │
│                                                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
│  │ fiber-A  │  │ fiber-B  │  │ fiber-C  │  ...         │
│  │ session1 │  │ session2 │  │ session3 │              │
│  │ mailbox  │  │ mailbox  │  │ mailbox  │              │
│  │ eventfd  │  │ eventfd  │  │ eventfd  │              │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘              │
│       │              │              │                    │
│  ┌────┴─────┐  ┌────┴─────┐  ┌────┴─────┐              │
│  │ stdin    │  │ stdin    │  │ stdin    │              │
│  │ buffer   │  │ buffer   │  │ buffer   │              │
│  └──────────┘  └──────────┘  └──────────┘              │
│                                                           │
│  Ready queue: deque<Fiber*>                               │
│  Wait map:    unordered_map<int eventfd → Fiber*>         │
│  ───────────────────────────────────────────────────     │
│  epoll_wait(stdin_fd | session_eventfds...)              │
└─────────────────────────────────────────────────────────┘
```

---

## 3. 核心组件

### 3.1 Fiber（有栈协程）

```
struct Fiber {
    ucontext_t ctx;              // CPU 寄存器 + 栈指针
    void* stack;                 // mmap 分配
    size_t stack_size;           // 2MB-8MB
    int eventfd;                 // 每 session 独立唤醒 fd
    FiberState state;            // Ready | Waiting | Done
    int session_id;              // 所属 session ID
};
```

**栈管理：**
- 使用 `mmap(stack_size)` 分配，`mprotect(stack + guard_page, PROT_NONE)` 做 guard page
- 默认 2MB/纤程，递归深的 eval（closure chain + match + let* 展开）不会爆栈
- 栈底 guard page 触发 SIGSEGV → 早期发现栈溢出

**关键接口：**

```
// 从调度器切到此 fiber
void Fiber::resume() {
    swapcontext(&g_scheduler->main_ctx_, &this->ctx_);
}

// 从 fiber 切回调度器（recv 空时调用）
void Fiber::yield() {
    swapcontext(&this->ctx_, &g_scheduler->main_ctx_);
}
```

**生命周期：**
1. 创建：`state = Ready`，加入 ready queue
2. 调度：`resume()` → 继续执行 entry point
3. 等待：`recv` 空时 → `state = Waiting`，eventfd 注册到 epoll → `yield()`
4. 唤醒：消息到达 → `write(eventfd)` → epoll 醒 → `state = Ready` → 入 ready queue
5. 完成：`state = Done`

**上下文切换成本：** `swapcontext` 约 50-100ns。对比线程切换约 1-5μs（~50x 更轻量）。

### 3.2 Scheduler（调度器 + epoll 事件循环）

```
class Scheduler {
    deque<Fiber*> ready_queue_;         // Ready fiber 队列
    unordered_map<int, Fiber*> wait_map_; // eventfd → fiber 映射
    ucontext_t main_ctx_;                // 调度器上下文
    int stdin_fd_;                       // stdin
    int epoll_fd_;                       // epoll 实例
    bool running_ = true;
};
```

**主循环：**

```
void Scheduler::run() {
    while (running_) {
        // 1. 处理所有 Ready fiber
        while (!ready_queue_.empty()) {
            auto* fiber = ready_queue_.front();
            ready_queue_.pop_front();
            fiber->resume();
            // fiber 执行到 yield() 或完成
            if (fiber->state == Done) {
                epoll_ctl(epoll_fd_, DEL, fiber->eventfd);
                close(fiber->eventfd);
                // 回收 fiber
            }
        }

        // 2. 全部 Waiting → 等事件
        if (ready_queue_.empty()) {
            epoll_event events[64];
            int n = epoll_wait(epoll_fd_, events, 64, -1);  // 阻塞等待
            for (int i = 0; i < n; ++i) {
                auto* fiber = (Fiber*)events[i].data.ptr;
                if (fiber) {
                    // eventfd 可读 → drain + 入 ready queue
                    uint64_t val;
                    read(fiber->eventfd, &val, sizeof(val));
                    fiber->state = Ready;
                    ready_queue_.push_back(fiber);
                }
            }
            // 新 stdin 数据 → 读入 session buffer
        }
    }
}
```

**epoll 注册的内容：**
- `stdin_fd`：标准输入（新 agent 连接/命令）
- `session[N].eventfd`：每个 session 的唤醒 fd（消息到达时写）
- 共用 data.ptr 指向对应的 Fiber* 或 Session*

### 3.3 Mailbox + recv yield（关键 5 行）

```
// recv 原语（原改后对比）
// 改前：
auto result = g_mailbox_read(svc, timeout_ms);
if (!result) return make_void();

// 改后：
while (true) {
    auto result = g_mailbox_read(svc, 0);  // 非阻塞查一次
    if (result) {
        auto idx = string_heap_.size();
        string_heap_.push_back(std::move(*result));
        return make_string(idx);
    }
    // mailbox 空 → yield，让调度器切到其他 session
    g_current_fiber->state = Waiting;
    // eventfd 已在 epoll 中注册，消息到达时唤醒
    Fiber::yield();
    // 唤醒后继续循环（消息可能已被消费）
}
```

**唤醒机制：**
```
// send 原语内部：
void Session::send(target_id, msg) {
    auto* target = g_session_manager->get(target_id);
    target->mailbox.push(msg);
    
    // 如果目标 fiber 在等待 → eventfd 唤醒
    auto* f = target->fiber();
    if (f->state == Waiting) {
        uint64_t val = 1;
        write(f->eventfd, &val, sizeof(val));
        // epoll_wait 会醒 → 调度器把 fiber 入 ready queue
    }
}
```

### 3.4 Session（每 agent 连接）

```
class Session {
    int id_;
    string id_str_;
    Mailbox mailbox_;
    Fiber fiber_;
    string stdin_buffer_;       // 累积的 stdin 数据
};

// 每个 session 一个独立 stdin buffer
// 主循环读 stdin → 按 session 分发到对应 buffer
// session fiber 从自己的 buffer 读 JSON 命令
```

**Session 协议处理：**

```
void Session::run() {
    while (true) {
        auto cmd = read_json_from_stdin_buffer();
        if (!cmd) {
            // 等 stdin 数据 → yield
            g_current_fiber->state = Waiting;
            Fiber::yield();
            continue;
        }
        switch (cmd.type) {
            case EXEC:
                auto result = cs.eval(cmd.code);
                // eval 内可能 recv → yield → resume
                write_json_to_stdout(result);
                break;
            case SESSION_SEND:
                g_session_manager->send(cmd.target, cmd.msg);
                write_json_to_stdout({"status":"ok"});
                break;
        }
    }
}
```

---

## 4. 唤醒时序

```
fiber-A 执行 eval:
  (send "agent-b" ...)
    → mailbox_B.push(msg)
    → eventfd_B 写 1
    → epoll 下次 loop 会醒

  (recv)
    → mailbox_A 空
    → state = Waiting
    → yield()

──────────────────────────────────── epoll wait 分隔线

scheduler 处理 eventfd_B:
  → fiber-B state = Ready
  → ready_queue_.push_back(fiber-B)

scheduler resume fiber-B:
  → fiber-B eval (recv)
  → mailbox_B 有消息 → return

fiber-B eval:
  (send "agent-a" "pong")
    → mailbox_A.push("pong")
    → eventfd_A 写 1

fiber-B eval 完成 → state = Done

──────────────────────────────────── epoll wait 分隔线

scheduler 处理 eventfd_A:
  → fiber-A state = Ready
  → ready_queue_.push_back(fiber-A)

scheduler resume fiber-A:
  → recv yield 后继续
  → mailbox_A 有消息 → return "pong"
  → continue eval
```

---

## 5. 协议扩展

### 新增 JSON 命令

```
// 跨 session 发消息
→ {"cmd":"session-send","target":"agent-b","msg":"{\"type\":\"ping\"}"}
← {"status":"ok"}

// 异步 exec（不等待立即返回 pending）
→ {"cmd":"exec-async","id":"req-1","code":"(recv 5000)"}
← {"status":"pending","id":"req-1"}
// 稍后 server 主动推送结果
← {"status":"ok","id":"req-1","result":"pong"}
```

### stdin 多 session 复用

所有 agent 共享同一个 stdin/stdout。协议通过 `session_id` 字段区分：

```
→ {"session":"agent-a","cmd":"exec","code":"(+ 1 2)"}
← {"session":"agent-a","status":"ok","result":"3"}
→ {"session":"agent-b","cmd":"session-send","target":"agent-a","msg":"hi"}
← {"session":"agent-b","status":"ok"}
// agent-a 之前的 exec 被唤醒 → 主动推结果
← {"session":"agent-a","status":"ok","result":"hi"}
```

---

## 6. 对现有代码的侵入

| 模块 | 改动 | 风险 |
|:----|:-----|:----:|
| `evaluator_impl.cpp` | `recv` 原语加 `yield` 约 5 行 | 低，条件编译 `#ifdef AURA_SERVE_ASYNC` |
| `service.ixx` | 邮箱回调改接 fiber mailbox | 中 |
| `main.cpp` | 新增 `--serve-async` 分支（~150 行） | 低，不碰现有 serve |
| `CMakeLists.txt` | 加 `src/serve/*.cpp` | 无 |

**无侵入：** AST / parser / type checker / JIT / AOT / stdlib / arena

---

## 7. 安全与健壮性

| 问题 | 方案 |
|:----|:-----|
| 栈溢出 | `mmap` + `mprotect` guard page，超过 8MB 触发 SIGSEGV |
| 信号处理 | 主线程屏蔽大部分信号，fiber 内不处理信号 |
| 优雅退出 | `Scheduler::stop()` → 所有 fiber 恢复 → `Done` → exit |
| 异常传播 | fiber 内 throw → 调度器 catch → 标记 session 错误 |
| 内存泄漏 | fiber 析构时 unmap 栈、close eventfd |

---

## 8. 实现路径

### Phase 1（~0.5d）：Fiber 框架

```
src/serve/fiber.h/.cpp     — Fiber 类（ucontext + mmap stack + eventfd）
src/serve/scheduler.h/.cpp — Scheduler 类（epoll + ready queue + wait map）
src/serve/mailbox.h/.cpp   — Mailbox + Session
```

**最小 demo：** 1 个 main fiber + 2-3 session fiber，模拟 recv yield + eventfd 唤醒，验证 deep recursion 不会爆栈。

### Phase 2（~0.5d）：Serve 集成

```
main.cpp        — 新增 --serve-async，接 session manager + scheduler
evaluator_impl  — recv 加 yield（条件编译）
service.ixx     — 邮箱回调适配 fiber mailbox
```

### Phase 3（~0.5d）：测试

- 单 agent 同步 exec
- 双 agent send/recv 通信
- 多 agent 并发 eval
- 超时行为
- 优雅退出

---

## 9. 关键决策

| 决策 | 选择 | 理由 |
|:----|:-----|:------|
| Fiber 实现 | **ucontext** | 零依赖，POSIX，够用 |
| 栈大小 | **2MB/fiber** | eval 递归深度实测安全 |
| 栈分配 | **mmap** | 支持 guard page |
| 调度策略 | **round-robin ready queue** | 简单，session 无优先级 |
| 唤醒机制 | **per-session eventfd** | 精准唤醒，无轮询 |
| 事件引擎 | **epoll** | 成熟，跨平台 |
| 协议兼容 | **兼容 + 扩展** | 现有 agent 零改动 |
| 保护条件编译 | **AURA_SERVE_ASYNC 宏** | 不影响现有 serve |
