# Inter-Agent Messaging — Design & Implementation

**Status**: Design (2026-05-25)
**Design Author**: Ani
**Driver**: 多 Agent 协作（EDSL Roadmap W7-8）

## 1. 真实场景分析

### 场景一：分层编译管线

```
Agent Lint（只读扫描器）
  └─ 持续扫描 workspace AST → 发现 anti-pattern
  └─ (send 'dev-agent {:type :lint-warning :node 42 :msg "let inlinable"})

Agent Dev（主开发者）
  └─ 收到 lint message → 决定是否修复
  └─ 修复后 (send 'linter :ack)
```

**价值**：事件驱动的代码审查闭环，Lint Agent 持续在后台运行。

### 场景二：多人协作式调试

```
Agent User（你）
  └─ (send 'debugger {:task "修复 fib OOM"})
  └─ (send 'reviewer {:policy "不允许递归改迭代"})

Agent Debugger（助手）
  └─ workspace:create-child 'debug
  └─ 反复 mutate → (send 'reviewer {:proposed-mutation id :impact effects})

Agent Reviewer（审查者）
  └─ 收到 proposal → query:effects → (send 'debugger {:verdict :approve})
```

**关键洞察**：三个 Agent **不在同一个 LLM context window 里**。共享的是 AST diff + 消息队列。

### 场景三：代码进化车间

```
Generator Agent          Verifier Agent           Selector Agent
  └─ mutate workspace      └─ typecheck + eval      └─ 收集结果
  └─ send verifier sid     └─ send result            └─ 选择最优
  └─ ...并行产生5个变异      └─ ...并行验证            └─ send merger
```

**对比串行**：单 Agent 卡在同一个 LLM；多 Agent 真正并行利用 serve session。

### 场景四：安全沙箱

```
Outer Agent（网关）        Inner Agent（沙箱）        Janitor Agent（清理者）
  └─ create-child sandbox   └─ 只读读取             └─ 定时检查
  └─ lock                   └─ send mutation-req    └─ 超时清理
  └─ 审核回复               └─ 等待审批             └─ send event
```

### 交互模式总结

| 模式 | 原语量 | 需求 |
|:----|:------:|:-----|
| 通知 (fire-and-forget) | 1 | `send` |
| 请求-响应 | 3 | `send` + `recv` + coroutine |
| 广播 | 2 | `(send :broadcast msg)` |
| 协商 (多轮) | 5 | `send-await` + timeout |
| 事件流 | 5 | mailbox filter + pattern |

## 2. 架构

### 2.1 总体设计

消息系统不依赖外部基础设施。在一个 `--serve` 进程内，所有 session 共享：

```
serve process
  ├── sessions = {
  │   "default": { evaluator, mailbox: [...] },
  │   "agent-a": { evaluator, mailbox: [...] },
  │   "agent-b": { evaluator, mailbox: [...] },
  │   ...
  │ }
  └── 全局 mailbox registry（线程安全 map）
```

### 2.2 CompilerService 扩展

```cpp
struct Mailbox {
    std::vector<std::string> messages;  // FIFO
    std::mutex mtx;
    std::condition_variable cv;         // for blocking recv
};

class CompilerService {
    // 新增
    Mailbox mailbox_;
    std::string session_id_;  // "default", "agent-a", etc.
    
    // 静态共享注册表
    static std::unordered_map<std::string, CompilerService*> registry_;
    static std::mutex registry_mtx_;
};
```

### 2.3 消息格式

消息是简单的 Aura 值（字符串）。发 JSON 字符串以便结构化：

```lisp
(send 'agent-b "{\"type\":\"query\",\"sym\":\"fib\"}")
```

更高级的结构化（P1+）：
```lisp
(send 'agent-b (json-encode {:type :query :sym "fib"}))
```

### 2.4 EDSL 原语

```
(send target-id message)     → #t / #f
  将 message（字符串）推入 target session 的 mailbox
  
(recv [timeout-ms])          → message 或 #f
  从当前 session 的 mailbox 取出一条消息
  无参数时阻塞等待
  有 timeout-ms 时超时返回 #f

(my-id)                      → session-id 字符串
  返回当前 session 的 ID
```

### 2.5 注册流程

`--serve` 启动时：

```cpp
// main.cpp 中创建 session 时
cs.session_id_ = session_name;
CompilerService::registry_[session_name] = &cs;

// 注册 EDSL 原语
primitives_.add("send", ...);
primitives_.add("recv", ...);
primitives_.add("my-id", ...);

// session 销毁时清理
CompilerService::registry_.erase(session_name);
```

## 3. 实现

### 3.1 Evaluator 端原语

```cpp
// (send target-id-string message-string)
primitives_.add("send", [this](const auto& a) -> EvalValue {
    if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
        return make_bool(false);
    
    auto target = string_heap_[as_string_idx(a[0])];
    auto msg = string_heap_[as_string_idx(a[1])];
    
    auto* svc = find_my_service();  // 找当前 session 所属的 CompilerService
    if (!svc) return make_bool(false);
    
    auto* target_svc = CompilerService::lookup(target);
    if (!target_svc) return make_bool(false);
    
    target_svc->mailbox_.push(msg);
    return make_bool(true);
});

// (recv [timeout-ms])
primitives_.add("recv", [this](const auto& a) -> EvalValue {
    auto* svc = find_my_service();
    if (!svc) return make_void();
    
    int timeout_ms = -1;  // -1 = block
    if (a.size() >= 1 && is_int(a[0]))
        timeout_ms = static_cast<int>(as_int(a[0]));
    
    auto msg = svc->mailbox_.pop(timeout_ms);
    if (!msg) return make_void();  // timeout → void
    
    auto idx = string_heap_.size();
    string_heap_.push_back(*msg);
    return make_string(idx);
});

// (my-id)
primitives_.add("my-id", [this](const auto&) -> EvalValue {
    auto* svc = find_my_service();
    if (!svc) return make_string(0);  // empty string
    auto idx = string_heap_.size();
    string_heap_.push_back(svc->session_id());
    return make_string(idx);
});
```

### 3.2 Mailbox 实现

```cpp
struct Mailbox {
    std::vector<std::string> msgs_;
    std::mutex mtx_;
    std::condition_variable cv_;
    
    void push(const std::string& msg) {
        std::lock_guard lk(mtx_);
        msgs_.push_back(msg);
        cv_.notify_one();
    }
    
    std::optional<std::string> pop(int timeout_ms) {
        std::unique_lock lk(mtx_);
        if (timeout_ms < 0) {
            cv_.wait(lk, [this]{ return !msgs_.empty(); });
        } else if (timeout_ms == 0) {
            if (msgs_.empty()) return std::nullopt;
        } else {
            if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                              [this]{ return !msgs_.empty(); }))
                return std::nullopt;
        }
        auto msg = std::move(msgs_.front());
        msgs_.erase(msgs_.begin());
        return msg;
    }
};

class CompilerService {
    Mailbox mailbox_;
    std::string session_id_;
    
    static std::unordered_map<std::string, CompilerService*> registry_;
    static std::mutex registry_mtx_;
    
public:
    static void register_session(const std::string& id, CompilerService* svc) {
        std::lock_guard lk(registry_mtx_);
        registry_[id] = svc;
    }
    
    static void unregister_session(const std::string& id) {
        std::lock_guard lk(registry_mtx_);
        registry_.erase(id);
    }
    
    static CompilerService* lookup(const std::string& id) {
        std::lock_guard lk(registry_mtx_);
        auto it = registry_.find(id);
        return it != registry_.end() ? it->second : nullptr;
    }
};
```

### 3.3 CompilerService 改造

在 `service.ixx` / `service_impl.cpp` 中添加：

```cpp
// service.ixx
export class CompilerService {
public:
    // 新增
    void set_session_id(const std::string& id);
    std::string session_id() const;
    void push_message(const std::string& msg);
    std::optional<std::string> pop_message(int timeout_ms = -1);
    
private:
    struct MailboxImpl {
        std::vector<std::string> msgs;
        std::mutex mtx;
        std::condition_variable cv;
    };
    std::unique_ptr<MailboxImpl> mailbox_;
    std::string session_id_;
};
```

### 3.4 main.cpp 集成

```cpp
// 创建 session 时
cs.set_session_id(session_name);
CompilerService::register_session(session_name, &cs);

// 销毁 session 时（用户退出时）
CompilerService::unregister_session(session_name);
```

### 3.5 Find-my-service 问题

Evaluator 需要知道它属于哪个 CompilerService。当前 Evaluator 通过 `service.ixx` 的全局注册表与 CompilerService 关联。我加一个反向指针：

```cpp
// service.ixx — CompilerService 创建时
// 在注册 primitive 之前，将自身指针传给 Evaluator
class CompilerService {
    // ...
    void init_agent_primitives() {
        evaluator_.set_compiler_service(this);
        evaluator_.primitives().add("send", ...);
        evaluator_.primitives().add("recv", ...);
        evaluator_.primitives().add("my-id", ...);
    }
};
```

Evaluator 新增：
```cpp
// evaluator.ixx
void* compiler_service_ = nullptr;  // CompilerService*
```

## 4. 实现路标

### P0 — 基本 send/recv/my-id（3-5 天）

| 任务 | 说明 |
|:----|:------|
| CompilerService mailbox | 线程安全 FIFO + 条件变量 |
| CompilerService 注册表 | static map + mutex |
| `send` 原语 | 查找目标 session 的 mailbox 并 push |
| `recv` 原语 | 从当前 session 的 mailbox pop（阻塞） |
| `my-id` 原语 | 返回当前 session ID |
| main.cpp 集成 | session 创建/销毁时注册/注销 |
| Evaluator 反向指针 | `compiler_service_` |
| 测试 | 双 session send/recv 验证 |

### P1 — 非阻塞 + 超时（+2 天）

| 任务 | 说明 |
|:----|:------|
| `(recv timeout)` | 带超时参数的非阻塞读取 |
| `(reply msg)` | 回复最后一条消息的发送者 |
| `(session-active? id)` | 检查 session 存活 |

### P2 — 高级模式（+3 天）

| 任务 | 说明 |
|:----|:------|
| `send-and-wait` | 发送并阻塞等待回复 |
| `broadcast` | 发送给所有已注册 session |
| `mailbox-stats` | 查询 mailbox 大小 + 积压 |
| session death detection | 断开时通知订阅者 |

## 5. 安全注意事项

1. **消息大小限制**：当前无限制。建议 P1 加 1MB 上限。
2. **无认证**：所有 session 在同一个进程内可信。未来跨进程需要认证。
3. **死锁风险**：`recv` 阻塞等待时不会阻塞其它 session。每个 session 有独立线程。
4. **消息积压**：如果一个 session 发送远多于接收，mailbox 会无限增长。P1 加容量限制 (max 10K)。

## 6. 测试策略

```
双 session 测试：

Session A:                               Session B:
  set-code "(define x 1)"                   (recv)  ← 阻塞等待
  (send 'B "hello")                         → "hello"
  (recv)  ← 阻塞                            (send 'A "world")
  → "world"

超时测试:
  (recv 100)  ← 100ms 超时 → #f

广播测试:
  (send :broadcast "announce")  → 所有 session 收到
```
