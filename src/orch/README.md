# `src/orch/` — unified agent orchestration

**Issue:** #1588 (module) · docs #1603 · quota #1600 · stress #1602  
**Headers:** `orch.h`, `agent_spawn.h`  
**Module inventory:** `orch.ixx` (`export module aura.orch`)

## Why this module

Orchestration lived across `serve/` (fiber, mailbox, parallel_orch) and
`compiler/` (agent / parallel-intend primitives). `src/orch/` is the
**composition layer**: one include surface and `agent_spawn` abstraction for
multi-agent work (spawn + mailbox + join + parallel batch).

```
Aura (parallel-intend / orch:*)
        │
        ▼
   src/orch/  ── compose ──►  serve/fiber · mailbox · parallel_orch
        │                     serve/scheduler · ResourceQuota
        ▼
   metrics: query:orch-module-stats · parallel-orch-stats · closedloop
```

## Layout

| File | Role |
|------|------|
| `orch.h` | Umbrella include + `aura::orch` aliases |
| `agent_spawn.h` | `spawn_agent_with_mailbox`, `join_agent(s)`, `agent_send`/`recv`, `conduct_parallel`, registry |
| `orch.ixx` | Phase / component inventory for docs |
| `README.md` | This file |

Building blocks remain in `src/serve/` (implementation). This module does not
duplicate fiber/scheduler code.

## C++ usage

```cpp
#include "orch/orch.h"

using namespace aura::orch;

Scheduler sched(4);
// ... run sched in background ...

auto agent = spawn_agent_with_mailbox(sched, {
    .name = "worker",
    .body = [] { /* work */ Fiber::yield(YieldReason::Explicit); },
    .attach_mailbox = true,
});
if (!agent.ok) {
    // agent.quota_exceeded / agent.error ("ResourceQuotaExceeded: …")
}
(void)agent_send(agent, {.payload = "hello"});
(void)join_agent(agent, 5000);

// Parallel batch (alias of parallel_orch::parallel_intend)
auto batch = conduct_parallel(sched, tasks, {.max_concurrency = 4, .timeout_ms = 60000});
```

## Aura surface (#1588)

| Primitive | Meaning |
|-----------|---------|
| `(orch:spawn-agent name [thunk])` | Spawn fiber agent + mailbox; returns hash |
| `(orch:agent-join name-or-id [:timeout-ms n])` | Join agent fiber |
| `(orch:parallel-intend …)` | Alias of `(parallel-intend …)` (#1587) |
| `(query:orch-module-stats)` | Module counters (schema **1588**) |

Stdlib `std/orchestrator` (`agent:spawn`, `orch:conduct`, …) remains the
high-level Aura framework; `src/orch/` is the C++/primitive foundation.

## Thread safety & production notes

- Aura `(parallel-intend)` **mutex-serializes** Evaluator apply; concurrency
  comes from fiber spawn/join and the permit gate, not parallel eval.
- Cross-fiber AST references need **StableNodeRef**; mutations need
  **MutationBoundaryGuard**.
- **ResourceQuota** Fibers dimension (#1600): spawn/batch may return
  `quota_exceeded` / `BatchStatus::QuotaExceeded`.
- Prefer timeouts on every join path used by Agents.

## Docs & tests

| Doc | Role |
|-----|------|
| [docs/orchestration-tutorial.md](../../docs/orchestration-tutorial.md) | Multi-fiber tutorial (#1603) |
| [docs/architecture.md](../../docs/architecture.md) | Module map + pipeline |
| [docs/wire-formats.md §10](../../docs/wire-formats.md#10-parallel-orchestration-contracts-1584--1600) | Hash / metrics contracts |
| [docs/design/src-orch-module.md](../../docs/design/src-orch-module.md) | Design |
| [docs/design/parallel-orch.md](../../docs/design/parallel-orch.md) | parallel_intend API |
| [docs/contributing.md §Orch](../../docs/contributing.md) | How to extend |

| Test | Role |
|------|------|
| `tests/test_orch_agent_spawn.cpp` | Facade + Aura orch:* |
| `tests/test_orch_resource_quota_1600.cpp` | Quota rejects |
| `tests/suite/parallel_orchestration_stress.aura` | E2E stress (#1602) |

## Related

- #1584 Fiber::join  
- #1585 MultiFiberMailbox  
- #1586 parallel_orch  
- #1587 `(parallel-intend)`  
- #1588 this module  
- #1600 ResourceQuota on orch paths  
- #1602 stress suite  
- #1603 docs / tutorial  
