# Execution Adapter — C++26 std::execution for Layer 3 Orchestration

> Issue #33: Integrate C++26 senders/receivers as the backbone of Aura's
> `orch:*` pipeline, preserving the existing M:N fiber scheduler as the
> execution backend.

---

## 1. Motivation

Aura's concurrency model has three layers:

| Layer | Scope | Current Status |
|-------|-------|---------------|
| **L1** | Thread pool + `std::jthread` | `src/serve/thread_pool.h` |
| **L2** | M:N fiber scheduler, Chase-Lev work-stealing | `src/serve/scheduler.h` |
| **L3** | Orchestration: `orch:parallel`, CSP channels | Ad-hoc `serve_async.cpp` |

Layer 3 (`orch:parallel`, `orch:pipeline`) is currently a thin wrapper around
`g_fiber_spawn`. There is no structured concurrency — no composition,
no cancellation propagation, no automatic error handling across pipelines.

C++26 introduces `std::execution` (P2300R10), a framework for asynchronous,
composable work using **senders** and **receivers**. This document proposes
wrapping Aura's M:N scheduler as a `std::execution::scheduler` and exposing
combinators as Aura primitives.

---

## 2. Architecture

```
Aura primitives (orch:parallel, orch:pipeline, orch:when_all)
          ↓
aura::exec::fiber_scheduler          ← std::execution::scheduler
          ↓
aura::serve::Scheduler               ← existing work-stealing M:N scheduler
          ↓
WorkerThread [...]                   ← existing Chase-Lev deques
```

### 2.1 Key Abstraction — `fiber_scheduler`

A `std::execution::scheduler` has:
- A `schedule()` method returning a **sender** that represents "run work here"
- The sender/receiver model handles: value, error, stopped (cancelled) signals

Our `fiber_scheduler` wraps `Scheduler`:

```cpp
namespace aura::exec {

class fiber_scheduler {
public:
    explicit fiber_scheduler(serve::Scheduler& sched);

    // std::execution::scheduler interface
    auto schedule() -> /* a sender that spawns a fiber */;
};

} // namespace aura::exec
```

### 2.2 Sender Types

| Sender | Behaviour |
|--------|-----------|
| `fiber_sender` | Spawns a fiber on the M:N scheduler. Completes when fiber yields/done. |
| `parallel_sender` | Spawns N fibers, signals completion when all finish. |
| `pipeline_sender` | Chains N senders sequentially, passing values through. |

---

## 3. Implementation Strategy

### 3.1 Phase 1: `fiber_scheduler` + `fiber_sender`

The fundamental building block. Wraps `Scheduler::spawn` into a sender.

```cpp
// Sender concept requirements:
// 1. connect(sender, receiver) → operation_state
// 2. start(operation_state) → begins async work
// 3. receiver has set_value/set_error/set_stopped

class fiber_sender {
    serve::Scheduler* sched_;
    std::function<void()> func_;
public:
    using completion_signatures = std::execution::completion_signatures<
        std::execution::set_value_t(),
        std::execution::set_error_t(std::exception_ptr),
        std::execution::set_stopped_t()
    >;

    struct operation_state {
        serve::Fiber* fiber_ = nullptr;

        friend void tag_invoke(std::execution::start_t, operation_state& os) noexcept {
            // fiber resumes → calls set_value on the receiver
        }
    };

    friend operation_state tag_invoke(
        std::execution::connect_t, fiber_sender&& snd, auto rcvr);
};
```

### 3.2 Phase 2: Just-in-time scheduling adapter

Instead of implementing full P2300 (which requires `std::execution::schedule`,
`sender_t`, `receiver_t`, etc.), we can use Aura's own tags that map to the
senders/receivers semantic but are simpler:

```cpp
// Lightweight sender — doesn't need full P2300 infrastructure
template<typename Fn>
struct fiber_sender {
    serve::Scheduler* sched;
    Fn fn;
    // set_value() called when fiber completes
};
```

### 3.3 Phase 3: Combinators

```cpp
// when_all: spawn N fibers, wait for all to complete
auto snd = exec::when_all(sched, {fn1, fn2, fn3});

// let_value: chain senders (pipeline)
auto snd = exec::let_value(sched, fn1, fn2);

// with_timeout: cancellation after deadline
auto snd = exec::with_timeout(sched, fn, 5s);
```

---

## 4. New Files

- `src/exec/execution_adapter.h` — fiber_scheduler + sender/operation_state
- `src/exec/execution_adapter.cpp` — implementation
- `src/exec/combinators.h` — when_all, let_value, with_timeout
- `src/exec/combinators.cpp` — implementation

---

## 5. Aura Primitives Map

| Aura primitive | Execution adapter |
|---------------|-------------------|
| `orch:parallel` | `exec::when_all` |
| `orch:pipeline` | `exec::let_value` chain |
| `orch:timeout` | `exec::with_timeout` |
| `orch:metrics` | Adapter queries `Scheduler::metrics()` |
| `orch:schedule_on` | Explicit scheduler selection |

---

## 6. Integration with Existing Fibers

The existing fiber mechanism remains unchanged:

- **Scheduling**: Same Chase-Lev deque, same WorkerThread loop
- **Yield points**: `Fiber::yield()` is transparent — fibers are preemptible
  in the old way. The sender/receiver model adds a new way to *compose* them,
  not a new way to run them.
- **Cancellation**: When a `when_all` cancels, it writes to the fiber's
  eventfd with a "cancelled" flag. The fiber's next yield checks this flag.

---

## 7. Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| P2300 experimental support | GCC 16 has `<execution>` but P2300 is still TS. We use custom lightweight sender types that follow the semantic. |
| Overhead of sender/receiver objects | Allocation is per-spawn; fibers already allocate stacks. Sender overhead is negligible. |
| Backward compatibility | Existing `g_fiber_spawn` / `orch:parallel` keep working. New primitives are additive. |

---

## 8. Roadmap

| Step | Description |
|------|-------------|
| 1 | Create `src/exec/` directory + `execution_adapter.h/.cpp` |
| 2 | Define `fiber_scheduler`, `fiber_sender`, `fiber_receiver` |
| 3 | Implement `schedule()` + `connect()` + `start()` |
| 4 | Add `when_all` combinator |
| 5 | Add `let_value` combinator (pipeline) |
| 6 | Add `with_timeout` combinator |
| 7 | Expose as Aura primitives |
| 8 | Tests: unit + integration with existing serve_async |

---

> **Status**: Design — ready for implementation.
