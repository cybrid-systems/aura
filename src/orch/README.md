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

MVP scope is single-agent only (`scripts/check_orch_mvp_scope.py --strict`). C++ entry points: `spawn_agent_with_mailbox`, `join_agent`, `agent_send`/`agent_recv`, `parallel_intend`.

Regression: `tests/orch/agent_primitives_2011.aura` · `tests/serve/test_fiber_orch_core_batch` AC4/AC8.

See [`docs/architecture.md`](../../docs/architecture.md) · [`docs/wire-formats.md`](../../docs/wire-formats.md) §10.