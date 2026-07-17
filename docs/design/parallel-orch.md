# parallel_orch / parallel_intend — parallel agent orchestration

**Issues:** #1586 (production), #1202 (Phase 1 scaffold)  
**API:** `src/serve/parallel_orch.h`  
**Module inventory:** `src/serve/parallel_orch.ixx` (phase tag)

## Problem

`intend` and related agent primitives are sequential. Multi-agent workflows
need a declarative way to spawn N intents, cap concurrency, join with timeout,
aggregate results/errors, and optionally fail-fast — reusing Fiber::join
(#1584) and MultiFiberMailbox (#1585).

## API

```cpp
namespace aura::serve::parallel_orch {

struct ParallelPolicy {
    uint32_t max_concurrency = 8; // 1..1024
    uint32_t timeout_ms = 0;      // 0 = no deadline
    bool fail_fast = false;
    bool collect_errors = true;
};

struct TaskSpec { function<TaskResult()> body; string name; };
struct TaskResult { bool ok; string value, error; uint64_t task_index, fiber_id, elapsed_us; };

BatchResult parallel_run(Scheduler&, span<const TaskSpec>, ParallelPolicy = {},
                         MultiFiberMailbox* mb = nullptr);
BatchResult parallel_intend(...); // alias
BatchResult sequential_run(span<const TaskSpec>); // baseline
}
```

## Semantics

| Feature | Behavior |
|---------|----------|
| Concurrency | Atomic permit gate; excess fibers yield `Explicit` (steal-safe) |
| Join | `Fiber::join(span, timeout_ms)` |
| Fail-fast | Sets abort flag; unstarted tasks get `aborted:fail-fast` |
| Mailbox | Optional `push` per completion (High priority on error) |
| Exceptions | Caught → `TaskResult{ok=false}` |

## Stats

Process-wide `g_parallel_orch_stats` + `query:parallel-orch-stats` (schema **1586**):

`batches`, `spawned`, `joined`, `ok`, `err`, `fail-fast-aborts`, `timeouts`,
`mailbox-posts`, `phase`, `schema`.

## Tests

`tests/test_parallel_orch.cpp` — policy, parallel vs sequential, fail-fast,
timeout, mailbox fan-in, concurrency cap, stats surface.

## Aura surface (#1587)

```scheme
;; tasks: vector or list of 0-arg thunks
(parallel-intend
  (vector
    (lambda () (intend "goal-a" gen ver))
    (lambda () (intend "goal-b" gen ver)))
  :max-concurrency 8
  :timeout-ms 60000
  :fail-fast #f
  :collect-errors #t)
;; → hash: status ok-count err-count aborted-count wait-us results schema=1587
```

Closure bodies are mutex-serialized for Evaluator safety; spawn/join/policy
still use the C++ `parallel_orch` path (`Fiber::join`, concurrency gate).

## Stress (#1602)

E2E concurrent mutate + parallel agent + join + GC:

- Suite: `tests/suite/parallel_orchestration_stress.aura`
- C++: `tests/test_parallel_orchestration_stress_1602.cpp`
- Design: `docs/design/parallel-orchestration-stress-1602.md`

## Related

- #1584 Fiber::join  
- #1585 MultiFiberMailbox  
- #1587 Aura `(parallel-intend)` primitive (this surface)
- #1602 parallel orchestration stress suite
