# Multi-fiber / parallel agent orchestration tutorial

**Issue:** [#1603](https://github.com/cybrid-systems/aura/issues/1603)  
**Stack:** #1584 Fiber::join · #1585 MultiFiberMailbox · #1586 parallel_orch ·
#1587 `(parallel-intend)` · #1588 `src/orch/` · #1595 linear join · #1597/#1600 metrics/quota

This tutorial is for **developers and LLM Agents** shipping multi-agent
workloads on Aura. Architecture map: [architecture.md §Agent 编排](architecture.md#agent-编排).
Wire contracts: [wire-formats.md §10](wire-formats.md#10-parallel-orchestration-contracts-1584--1600).

---

## 1. Mental model

```
thunks / agent bodies
        │
        ▼
  parallel-intend / orch:spawn-agent
        │
        ▼
  Scheduler (M:N fibers) ── steal / yield ── GC safepoints
        │
        ├── Fiber::join / orch:agent-join   (wait + timeout)
        └── MultiFiberMailbox               (optional fan-in / backpressure)
        │
        ▼
  BatchResult hash  +  query:parallel-orch-stats / closedloop orch-health
```

| Do this | Avoid |
|---------|--------|
| Cap concurrency (`:max-concurrency`) | Unbounded spawn storms |
| Always set `:timeout-ms` in Agent loops | Infinite join |
| Handle `partial` / `timeout` / `quota-exceeded` | Assuming all tasks ok |
| Sample metrics **before** aggressive `gc-heap` | Holding stale hash table handles |
| Use StableNodeRef across fibers | Raw NodeId after steal/compact |

---

## 2. Quick start (Aura)

```bash
echo '(hash-ref (parallel-intend (vector (lambda () 1) (lambda () 2)) :timeout-ms 10000) "ok-count")' \
  | ./build/aura
# → 2
```

### 2.1 Parallel batch of pure thunks

```scheme
(define batch
  (parallel-intend
    (vector
      (lambda () (intend "analyze-coverage" gen ver))
      (lambda () (intend "mutate-fix" gen ver))
      (lambda () (+ 1 2)))
    :max-concurrency 8
    :timeout-ms 60000
    :fail-fast #f
    :collect-errors #t))

(hash-ref batch "status")     ; "ok" | "partial" | "timeout" | …
(hash-ref batch "ok-count")
(hash-ref batch "err-count")
(hash-ref batch "results")    ; vector of {ok, index, value|error}
```

`orch:parallel-intend` is the same primitive under the orch: namespace.

### 2.2 Nested fiber join

```scheme
(define fid (fiber:spawn (lambda () (* 6 7))))
(fiber:join fid)   ; → 42
```

Outer join storms are fine for short work; for production batches prefer
`parallel-intend` so concurrency caps and timeouts apply uniformly.

### 2.3 Concurrent mutate + GC (stress pattern)

Evaluator serializes closure bodies inside `(parallel-intend)`, but the
batch still exercises spawn/join/GC. Pattern from the stress suite:

```scheme
(define (f x) (+ x 1))
(set-code "(define (f x) (+ x 1))")
(eval-current)

(parallel-intend
  (vector
    (lambda ()
      (mutate:set-body "f" "(+ x 2)")
      (eval-current)
      (gc-heap)
      (f 0))
    (lambda () (begin (gc-heap) 1))
    (lambda () (fiber:join (fiber:spawn (lambda () 99)))))
  :max-concurrency 4
  :timeout-ms 30000
  :fail-fast #f)
```

Full suite: `tests/suite/parallel_orchestration_stress.aura`.

---

## 3. Stdlib orchestrator (roles / pipeline)

High-level pure-Aura framework (stdin-friendly):

```scheme
(require "std/orchestrator" all:)

(orch:define-role "coder"
  (cons (lambda (task) (string-append "[CODE] " task)) "direct"))

(orch:pipeline (list "coder") "build fib")

(orch:parallel
  (list
    (lambda (x) (string-append "style: " x))
    (lambda (x) (string-append "perf: " x)))
  "snippet")
```

Use stdlib when you need **role graphs** without C++ fibers; use
`parallel-intend` when you need **real multi-fiber** concurrency caps.

---

## 4. Backpressure and errors

### 4.1 Fail-fast vs collect-all

```scheme
;; Abort remaining work after first error
(parallel-intend tasks :fail-fast #t :timeout-ms 10000)

;; Run all admitted tasks; status may be "partial"
(parallel-intend tasks :fail-fast #f :collect-errors #t :timeout-ms 10000)
```

### 4.2 Mailbox backpressure (#1585)

C++ path (`MultiFiberMailbox::push`) returns `PushStatus::Backpressure`
when the high-water mark is hit. Agent policy options:

1. **Yield and retry** (preferred for soft real-time)
2. **Drop Low-priority** messages
3. **Fail-fast** the producer

`parallel_intend` optional mailbox posts use High priority on errors.

### 4.3 ResourceQuota (#1600)

If Fibers quota is exhausted:

- batch `status` → `"quota-exceeded"`
- C++ `AgentHandle.quota_exceeded` / error contains `ResourceQuotaExceeded`

```scheme
(engine:metrics "query:resource-quota-stats")
```

---

## 5. Observability

```scheme
;; Process-wide parallel_orch counters
(define po (engine:metrics "query:parallel-orch-stats"))
(hash-ref po "schema")    ; 1586
(hash-ref po "batches")
(hash-ref po "joined")
(hash-ref po "timeouts")

;; Unified AI closed-loop (includes orch health)
(define cl (engine:metrics "query:ai-closedloop-readiness-stats"))
(hash-ref cl "orch-health-score")
(hash-ref cl "avg-join-latency-us")
(hash-ref cl "parallel_task_throughput")
(hash-ref cl "adaptive-concurrency-recommended")

;; Module-level orch facade
(engine:metrics "query:orch-module-stats")  ; schema 1588
```

**GC tip:** FlatHashTable handles from `engine:metrics` can be invalidated
by aggressive `gc-heap`. Copy integers out immediately:

```scheme
(define batches (hash-ref (engine:metrics "query:parallel-orch-stats") "batches" 0))
(gc-heap)
;; use `batches`, do not re-use the old hash object
```

---

## 6. C++ composition (`src/orch/`)

```cpp
#include "orch/orch.h"

using namespace aura::orch;

Scheduler sched(4);
std::thread runner([&] { sched.run(); });

auto agent = spawn_agent_with_mailbox(sched, {
    .name = "worker",
    .body = [] {
        Fiber::yield(YieldReason::Explicit);
    },
    .attach_mailbox = true,
});
if (!agent.ok) {
    // check agent.quota_exceeded / agent.error
}
(void)join_agent(agent, /*timeout_ms=*/5000);

// Parallel batch (→ parallel_orch::parallel_intend)
std::vector<serve::parallel_orch::TaskSpec> tasks = { /* … */ };
auto batch = conduct_parallel(sched, tasks, {
    .max_concurrency = 8,
    .timeout_ms = 60000,
    .fail_fast = false,
});

sched.stop();
runner.join();
```

Module README: [`src/orch/README.md`](../src/orch/README.md).

---

## 7. Production checklist

- [ ] `:max-concurrency` set (default 8; lower under memory pressure)
- [ ] `:timeout-ms` set for every Agent-facing batch
- [ ] Branch on `status` ∈ {`ok`,`partial`,`timeout`,`quota-exceeded`,…}
- [ ] Inspect `err-count` / per-task `error` strings
- [ ] Watch `orch-health-score` / `adaptive-concurrency-recommended`
- [ ] Mutation only under Guard; no cross-fiber raw NodeId
- [ ] Stress locally: `./build/aura --load tests/suite/parallel_orchestration_stress.aura`

---

## 8. Related tests and design

| Artifact | Role |
|----------|------|
| `tests/suite/parallel_orchestration_stress.aura` | E2E mutate+join+GC |
| `tests/test_parallel_orchestration_stress_1602.cpp` | C++ metrics stress |
| `tests/test_parallel_orch.cpp` | parallel_orch unit |
| `tests/test_parallel_intend_primitive.cpp` | Aura primitive |
| `tests/test_orch_agent_spawn.cpp` | src/orch facade |
| `tests/test_orch_resource_quota_1600.cpp` | quota rejects |
| `docs/design/parallel-orch.md` | API design |
| `docs/design/src-orch-module.md` | Module layout |
| `docs/design/parallel-orchestration-stress-1602.md` | Stress how-to |
