# `src/orch/` — unified agent orchestration

**Issue:** #1588  
**Headers:** `orch.h`, `agent_spawn.h`  
**Module inventory:** `orch.ixx` (`export module aura.orch`)

## Why this module

Orchestration lived across `serve/` (fiber, mailbox, parallel_orch) and
`compiler/` (agent / parallel-intend primitives). `src/orch/` is the
**composition layer**: one include surface and `agent_spawn` abstraction for
multi-agent work (spawn + mailbox + join + parallel batch).

## Layout

| File | Role |
|------|------|
| `orch.h` | Umbrella include + `aura::orch` aliases |
| `agent_spawn.h` | `spawn_agent_with_mailbox`, `join_agent(s)`, `agent_send`/`recv`, `conduct_parallel`, registry |
| `orch.ixx` | Phase / component inventory for docs |

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
(void)agent_send(agent, {.payload = "hello"});
(void)join_agent(agent, 5000);

// Parallel batch (alias of parallel_orch::parallel_intend)
auto batch = conduct_parallel(sched, tasks, {.max_concurrency = 4});
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

## Related

- #1584 Fiber::join  
- #1585 MultiFiberMailbox  
- #1586 parallel_orch  
- #1587 `(parallel-intend)`  
- #1603 docs/tutorial follow-up  
