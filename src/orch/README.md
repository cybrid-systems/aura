# src/orch/

Agent orchestration facade — `orch.h` · `agent_spawn.h` · `orch.ixx` (#1588).

## Aura language primitives (Issue #1588 / #2011)

| Primitive | Calling convention | Result |
|-----------|-------------------|--------|
| `(orch:spawn-agent name [thunk] [:attach-mailbox bool] [:high-water n] [:keepalive-interval-ms n])` | `name` string; optional 0-arg thunk; optional keywords | hash `{ok, id, name, schema=1588, schema-2011, quota-exceeded[, error]}`; **quota reject → typed Aura error** |
| `(orch:agent-join name [:timeout-ms n])` | name as registered at spawn | hash `{ok, status, wait-us, schema}` (`status` = ok/timeout/cancelled/invalid) |
| `(orch:agent-send name payload)` | payload string/int/bool | hash `{ok, status, schema}` (`status` = ok/backpressure/closed); unknown agent → error |
| `(orch:agent-recv name [:wait bool] [:timeout-ms n])` | default wait `#t` | hash `{ok, empty, payload, schema}` |
| `(orch:parallel-intend tasks …)` | alias of `(parallel-intend …)` | same as parallel-intend batch hash |
| `(engine:metrics "query:orch-module-stats")` | stats facade | live `OrchModuleStats` (+ mailbox/parallel mirrors) |

### `parallel-intend` semantics (Issue #2081)

`(parallel-intend tasks ...)` runs tasks on a real fiber pool under `parallel_orch::parallel_intend`,
but **Evaluator `apply_closure` is serialized** by a shared `std::mutex eval_mu` so AST/mutate
safety is preserved across fibers. The batch hash therefore carries `eval-serialized=#t` (and
`schema-2081`) so Agents can introspect the contract:

| Aspect | Behavior |
|--------|----------|
| Fibers | concurrent under `:max-concurrency` / FailurePolicy (`#1587`/`#2007`) |
| `Evaluator::apply_closure` | **serialized** via shared `eval_mu` (mutation safety) |
| Throughput | ~ sequential `apply_closure` cost + scheduling overhead |
| Mutating thunks | safe (always go through the lock) |
| Pure thunks | still serialized today; future `:pure #t` may skip the lock |

For CPU-only pure work that does not need Evaluator mutation, prefer direct
`serve::parallel_orch::parallel_intend` from C++ with custom `TaskSpec` bodies that don't
call back into the Evaluator. The Aura-level `parallel-intend` is the orchestration contract;
the eval-serialization is the safety floor.

MVP scope is single-agent only (`scripts/check_orch_mvp_scope.py --strict`). C++ entry points: `spawn_agent_with_mailbox`, `join_agent`, `agent_send`/`agent_recv`, `parallel_intend`.

### `AgentScope` (Issue #2083, opt-in feature flag)

`src/orch/agent_scope.h` provides an opt-in scoped multi-agent supervision root,
gated by `#define AURA_ENABLE_AGENT_SCOPE`. Default builds keep the MVP linter
green; commercial multi-agent builds define the flag per TU to opt in.

```cpp
#define AURA_ENABLE_AGENT_SCOPE
#include "orch/agent_scope.h"

aura::serve::Scheduler sched(2);
// ... run scheduler ...

aura::orch::AgentScope scope(sched);
auto& h1 = scope.spawn({.name = "a", .body = [] { /* ... */ }});
auto& h2 = scope.spawn({.name = "b", .body = [] { /* ... */ }});

scope.cancel_all();                       // best-effort request_cancel
auto jr = scope.join_all(/*timeout_ms=*/5000); // mirror #2082 cancel+drain (default 2s)
// Issue #2153: JoinPolicy{.primary_ms, .drain_ms} for SLA-tuned cancel→release.
// ~AgentScope: cancel + best-effort drain + reservation release.
```

Rules (per Issue #2083 AC4):
1. **No** process-global registry (linter still forbids `AgentRegistry` /
   `global_agent_registry` / `conduct_parallel`).
2. Scope destructor is the supervision root (cancel + best-effort drain +
   reservation release, mirroring `join_agents` #2082 contract).
3. Default (flag off) tree still passes `--strict` MVP scope linter:
   - `scripts/check_orch_mvp_scope.py` allows `AgentScope` only in TUs
     that `#define AURA_ENABLE_AGENT_SCOPE` (new `FEATURE_FLAG_PATTERNS`
     mechanism; see linter source).
4. Distinct from `OrchAgentNameTable` (#2078) and `parallel_intend` (#1587):
   - `OrchAgentNameTable`: per-Evaluator name bookkeeping for Aura
     primitives (`orch:spawn-agent` / `orch:agent-join`).
   - `parallel_intend`: short-lived batch thunks (no long-lived names).
   - `AgentScope`: long-lived named agents, parent-cancel + `join_all`
     semantics, bound to an explicit owner (Scheduler reference).

Regression: `tests/orch/test_agent_scope_2083` (AC1-AC6).

See [`docs/architecture.md`](../../docs/architecture.md) · [`docs/wire-formats.md`](../../docs/wire-formats.md) §10.