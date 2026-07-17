# Unified `src/orch/` module

**Issue:** #1588  
**Priority:** P1 (architecture cleanup)

## Problem

Orchestration was fragmented:

- `src/serve/fiber.h` / `scheduler.h` — low-level M:N fibers  
- `src/serve/multi_fiber_mailbox.h` — multi-attach mailbox (#1585)  
- `src/serve/parallel_orch.h` — batch parallel_intend (#1586)  
- `evaluator_primitives_agent.cpp` — Aura intend / parallel-intend (#1587)  
- `lib/std/orchestrator.aura` — pure-Aura agent registry  

No single namespace for “spawn agent with mailbox + join + parallel batch”.

## Solution

Create **`src/orch/`** as a composition facade:

```
src/orch/
  orch.h           # umbrella
  agent_spawn.h    # spawn_agent_with_mailbox, join, send/recv, conduct_parallel
  orch.ixx         # aura.orch inventory
  README.md
```

Serve keeps implementations; orch **composes** them under `aura::orch`.

## agent_spawn

```cpp
AgentHandle spawn_agent_with_mailbox(Scheduler&, AgentSpec{
  .name, .body, .attach_mailbox = true});
JoinResult join_agent(AgentHandle&, optional timeout_ms);
PushStatus agent_send(AgentHandle&, MailMessage);
optional<MailMessage> agent_recv(...);
BatchResult conduct_parallel(...); // → parallel_intend
```

## Aura primitives

- `orch:spawn-agent` / `orch:agent-join` / `orch:parallel-intend`  
- `query:orch-module-stats` (schema 1588)

## Non-goals (this issue)

- Full migration of evaluator bodies into `src/orch/` (would couple EvalValue)  
- Supervisor trees / circuit-breakers (future extension points on this module)  
- Replacing `std/orchestrator.aura` (stdlib stays pure-Aura for stdin)

## Tests

`tests/test_orch_agent_spawn.cpp` — C++ agent_spawn + Aura orch:* + stats.
