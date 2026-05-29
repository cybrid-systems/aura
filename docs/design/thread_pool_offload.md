# 线程池与阻塞操作 Offload 方案

**2026-05-29**
**Issue：** [#22 P0] Scheduler 单线程瓶颈 + 阻塞操作 offload

---

## 问题

Aura 的 fiber 调度器是**单线程合作式**的。所有 fiber 共享一个 OS 线程：

```
┌─ Event Loop (epoll) ─────────────────────┐
│                                           │
│  Fiber A: recv → yield → recv → yield    │   ← LLM 调用时
│  Fiber B: recv → yield → recv → yield    │     白白空转
│  Fiber C: type-check → 阻塞全部纤维       │   ← 编译卡死一切
│                                           │
└───────────────────────────────────────────┘
```

当任一 fiber 做**阻塞操作**（编译、类型检查、文件读取、`eval-current` 全量重编译），**所有其他 fiber 都得等**。

这不是多核并行的问题，是**阻塞操作没有 offload 到后台线程**的问题。

---

## 现有模式（启动线程 + eventfd）

`serve_async.cpp` 里 `g_http_post_async` 已经实现了这套模式：

```
Fiber A 需要 LLM 调用:
  │
  ├── std::thread t([...]() {        ← 新线程
  │     fork+exec curl (阻塞 30s)
  │     write(eventfd, 1)             ← 完成后通知
  │   })
  │   t.detach()
  │
  ├── Fiber::yield()                  ← Fiber 让出 CPU
  │
  ... (其他 fiber 继续工作) ...
  │
  └── eventfd 可读 → scheduler 唤醒 Fiber A
```

**问题**：每次 LLM 调用都 `fork+exec curl` + `pthread_create`。开销大。

---

## 方案：Thread Pool

```
┌─ Event Loop (主线程) ────────────────────┐
│                                           │
│  Fiber A: recv → yield → recv → yield    │
│  Fiber B: compile → yield → resume       │
│  Fiber C: file-read → yield → resume     │
│                                           │
│  需要阻塞操作时：                          │
│    task = [fn, eventfd]                   │
│    thread_pool.enqueue(task)              │
│    Fiber::yield()                         │
│                                           │
├───────────────────────────────────────────┤
│                                           │
│  ┌─ Thread Pool ──────────────────────┐   │
│  │  Thread 1: 编译 type-check         │   │
│  │  Thread 2: 文件 I/O                │   │
│  │  Thread 3: LLM HTTP (curl)         │   │
│  │  Thread 4: 类型检查 / AOT          │   │
│  │                                     │   │
│  │  完成后: write(task->eventfd, 1)    │   │
│  └─────────────────────────────────────┘   │
│                                           │
└───────────────────────────────────────────┘
```

### ThreadPool 类

```cpp
class ThreadPool {
public:
    ThreadPool(size_t num_threads = 4);
    ~ThreadPool();

    // 入队任务。返回一个 eventfd，完成时触发
    // fn 是阻塞操作，在池线程上执行
    // 返回的 eventfd 会注册到 scheduler 的 epoll
    int enqueue(std::function<void()> fn);

    // 任务数
    size_t pending() const;

private:
    std::vector<std::thread> workers_;
    std::deque<std::pair<int, std::function<void()>>> queue_;  // (eventfd, fn)
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};
```

### 工作流程

```
1. Fiber 调用 (thread_pool:enqueue fn)
2. C++ primitive:
   a. 创建 eventfd
   b. thread_pool.enqueue(fn, eventfd)
   c. 将 eventfd 注册到 scheduler 的 epoll
   d. fiber 状态设为 Waiting + yield
3. 池线程取出 task，执行 fn
4. fn 完成后：write(eventfd, 1)
5. Scheduler 的 epoll_wait 检测到 eventfd 可读
6. Scheduler 将 fiber 放回 ready queue
7. Fiber 恢复执行，读取结果
```

### 结果传递

```
┌─────────────┐         ┌─────────────┐
│  Fiber      │         │  Thread Pool │
│             │         │             │
│  auto p =   │         │             │
│    make_shared‹        │             │
│     optional‹         │             │
│       EvalValue››    │             │
│             │         │             │
│  thread_pool│         │             │
│   .enqueue( │         │             │
│    [p](){   │─────→   │  *p = fn()  │
│     ... })  │         │  write(fd)  │
│             │         │             │
│  yield()    │         │             │
│             │         │             │
│  ← epoll ──│─────────│  eventfd!   │
│             │         │             │
│  return *p  │         │             │
└─────────────┘         └─────────────┘
```

使用 `shared_ptr<optional<EvalValue>>` 模式（和 fiber:join 一样），天然无锁：
- Fiber 持有 shared_ptr 的引用
- 线程写入指定地址
- 完成后 eventfd 通知

---

## Aura 原语

```scheme
;; 入队阻塞任务
(thread_pool:enqueue fn) → void

;; 自动 offload 编译（已有 eval-current 的增强）
(eval-current)  →  内部：自动 offload 到线程池
```

所有现有调用 `eval-current` 的代码自动受益。

---

## 实现路标

### Phase 1 — ThreadPool 核心（~200 行 C++）

| 文件 | 内容 |
|------|------|
| `src/serve/thread_pool.h` | ThreadPool class 声明 |
| `src/serve/thread_pool.cpp` | ThreadPool 实现 + eventfd 管理 |
| `src/messaging_bridge.h` | 新增 `g_thread_pool_enqueue` 回调 |
| `src/compiler/evaluator_impl.cpp` | `thread_pool:enqueue` 原语 |

### Phase 2 — 编译 Offload（~50 行）

| 文件 | 内容 |
|------|------|
| `src/serve/serve_async.cpp` | `eval-current` 自动 offload 到线程池 |
| `src/messaging_bridge.h` | 新增 `g_compile_async` 回调 |

### Phase 3 — 测试

| 内容 |
|------|
| 线程池基本入队/处理/完成 |
| fiber 通过线程池执行阻塞计算 |
| `orch:parallel` + 线程池 offload 集成 |
| benchmark：编译不卡 fiber |

---

## 验收清单

- [ ] `ThreadPool` 支持 N 个工作线程 + eventfd 完成通知
- [ ] `thread_pool:enqueue` Aura 原语
- [ ] fiber 在 enqueue 后正确 yield + 被 eventfd 唤醒
- [ ] `eval-current` 自动通过线程池编译
- [ ] 10 个 fiber 同时请求编译，主循环不被阻塞
