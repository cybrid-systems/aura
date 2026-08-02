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

### `parallel-intend` semantics (Issue #2081 / #2163)

`(parallel-intend tasks ...)` runs tasks on a real fiber pool under `parallel_orch::parallel_intend`.
By default **Evaluator `apply_closure` is serialized** by a shared `std::mutex eval_mu` so
AST/mutate safety is preserved across fibers. The batch hash carries `eval-serialized=#t`
(and `schema-2081`) so Agents can introspect the contract.

| Aspect | Default (`:pure #f`) | `:pure #t` (Issue #2163) |
|--------|----------------------|---------------------------|
| Fibers | concurrent under `:max-concurrency` / FailurePolicy (`#1587`/`#2007`) | same |
| `Evaluator::apply_closure` | **serialized** via shared `eval_mu` | **unlocked** when pure path taken |
| Throughput | ~ sequential apply cost + scheduling | concurrent apply for pure thunks |
| Mutating thunks | safe (always locked) | task error `pure-contract-violated` (+ metric) |
| Batch hash | `eval-serialized=#t` | `eval-serialized=#f` when any unlocked pure apply; `schema-pure-parallel=2163` |
| **isolation-level** (Issue #2400) | `serialized` | `best-effort-pure` (never transactional; even if all tasks fallback-locked) |
| Forced lock | n/a | if mutation boundary already held → lock + `pure_fallback_locked_total` |

**isolation-level enum** (`isolation-level` on every batch hash, schema-2400):
`serialized` | `best-effort-pure` | `none` (C++ TaskSpec-only path that never touches Evaluator).

**Pure contract (caller guarantees + best-effort probe):**

```text
(parallel-intend tasks :pure #t :max-concurrency 4 :timeout-ms 5000)
;; default pure=#f → eval-serialized=#t (unchanged AC1)
```

1. **Caller guarantees** each thunk is free of mutate/AST write. Prefer **shallow**
   pure arithmetic / pure reads — deep concurrent recursion on a shared
   `Evaluator` can still race internal heaps (pure is not a full reentrant VM).
2. **Best-effort probe**: after an unlocked apply, if `defuse_version` advanced or a
   mutation boundary is held, the task fails with error `pure-contract-violated`
   and `pure_contract_violated_total` advances (AC3). Does not roll back sibling
   concurrent pure applies — pure is not a transactional isolation level.
3. **Fallback lock**: if a mutation boundary is already held when a pure task starts,
   that task takes `eval_mu` and bumps `pure_fallback_locked_total`.
4. FailurePolicy / timeout / quota (`#2007` / `#1600`) are unchanged under pure (AC4).

Metrics (`query:orch-module-stats`, schema-2163):
`pure-parallel-batches-total`, `pure-parallel-tasks-total`,
`pure-contract-violated-total`, `pure-fallback-locked-total`.

For CPU-only pure work that must never touch the Evaluator, prefer direct
`serve::parallel_orch::parallel_intend` from C++ with custom `TaskSpec` bodies.

**Pitfalls (Issue #2230 — pure is best-effort, NOT isolation):**

The `:pure #t` contract is a *best-effort probe*, not a transactional isolation
level. Production Agents must NOT treat pure parallel as a full reentrant VM.
Documented footguns (locked by `tests/orch/test_parallel_intend_pure_contract_2230`):

1. **Sibling concurrent pure applies are NOT rolled back.** If a pure task
   fails with `pure-contract-violated` mid-batch, the other pure tasks may
   have already partially executed (no atomicity). The batch hash reports
   per-task errors via `CollectAll` / `FailFast` — Agents that need
   all-or-nothing must use the default `:pure #f` (serialized) or
   `serve::parallel_orch::parallel_intend` from C++ with manual barriers.
2. **Deep concurrent recursion on a shared `Evaluator` can race internal
   heaps.** The best-effort probe (defuse_version / mutation boundary
   check) catches *most* AST writes but does NOT cover indirect mutation
   paths (e.g. `engine:metrics` writers, side-channel caches). For deep
   recursion, prefer `serve::parallel_orch::parallel_intend` from C++ with
   `TaskSpec` bodies that never touch the Evaluator.
3. **Three metric paths are independent, not coupled.** `pure_unlocked_applies`
   counts unlocked fast-path thunks; `pure_fallback_locked` counts
   forced-lock thunks (boundary already held); `pure_contract_violated`
   counts thunks that wrote AST post-apply. A healthy batch shows
   `unlocked > 0`, `fallback = 0`, `violated = 0`; non-zero values in any
   slot indicate the corresponding class of risk. The test suite
   (`tests/orch/test_parallel_intend_pure_contract_2230`) locks all three
   paths with explicit ACs.
4. **Probe is NOT a transaction.** There is a small window between the
   `apply_closure` call and the post-apply `defuse_version` check where
   a runaway task could mutate state undetected. For Agent / host-thread
   orchestration that needs hard isolation, use the default `:pure #f`
   serialized path or the C++ `parallel_intend` with explicit locks.

Do not advertise `:pure #t` as a transactional isolation level in any
Agent-facing schema text. The issue's Phase C probe hardening (sampling
`total_mutations_` / workspace generation) is a follow-up if the probe
window proves too loose in production.

MVP scope is single-agent only (`scripts/check_orch_mvp_scope.py --strict`). C++ entry points: `spawn_agent_with_mailbox`, `join_agent`, `agent_send`/`agent_recv`, `parallel_intend`.

### `AgentScope` (Issue #2083, default multi-agent supervision root)

`src/orch/agent_scope.h` provides the default scoped multi-agent supervision
root. **Always available** under `aura::orch` via `#include "orch/orch.h"`
(no `#define` required) — Issue #2226 promoted it from the opt-in
`AURA_ENABLE_AGENT_SCOPE` feature flag to the default documented surface.

```cpp
#include "orch/orch.h"   // pulls in agent_scope.h automatically

aura::serve::Scheduler sched(2);
// ... run scheduler ...

aura::orch::AgentScope scope(sched);
auto& h1 = scope.spawn({.name = "a", .body = [] { /* ... */ }});
auto& h2 = scope.spawn({.name = "b", .body = [] { /* ... */ }});

scope.cancel_all();                       // best-effort request_cancel
auto jr = scope.join_all(/*timeout_ms=*/5000); // mirror #2082 cancel+drain (default 2s)
// Issue #2153: JoinPolicy{.primary_ms, .drain_ms} for SLA-tuned cancel→release.
// Issue #2161: batch liveness (no global registry)
//   auto wr = scope.watch_all(/*stall_ms=*/50, StallPolicy::Cancel);
//   // wr.alive / stalled / done / closed / cancelled
// ~AgentScope: cancel + best-effort drain + reservation release.
```

Rules (per Issue #2083 AC4 / #2161 AC5 / #2226 / #2537):
1. **No** process-global registry (linter still forbids `AgentRegistry` /
   `global_agent_registry` / `conduct_parallel`). AgentScope is bound to
   an explicit `serve::Scheduler&` owner passed at construction — no static
   name table, no process-static agent map. **Evaluator `orch:spawn-agent`
   name bookkeeping is separate** (`OrchAgentNameTable` #2078) and not
   touched by AgentScope.
2. Scope destructor is the supervision root (cancel + best-effort drain +
   reservation release, mirroring `join_agents` #2082 contract).
3. Distinct from `OrchAgentNameTable` (#2078) and `parallel_intend` (#1587):
   - `OrchAgentNameTable`: per-Evaluator name bookkeeping for Aura
     primitives (`orch:spawn-agent` / `orch:agent-join`).
   - `parallel_intend`: short-lived batch thunks (no long-lived names).
   - `AgentScope`: long-lived named agents, parent-cancel + `join_all`
     + `watch_all` (#2161) semantics, bound to an explicit owner
     (Scheduler reference). Hierarchy (#2537) is an explicit tree of
     scopes (`parent_` / `children_` / `spawn_child`), still no global map.

### Hierarchical AgentScope (Issue #2537)

Parent/child supervision tree without a global registry. Parent owns
children via `std::unique_ptr`; `parent_` is a non-owning back-pointer.

```cpp
aura::orch::AgentScope root(sched);
auto& worker_scope = root.spawn_child();   // parent owns child
worker_scope.spawn({.name = "w", .body = [] { /* ... */ }});
root.spawn({.name = "supervisor", .body = [] { /* ... */ }});

root.cancel_all();  // top-down: children first, then local handles
// ~AgentScope: cancel tree → destroy children (bottom-up drain) → join self
```

Cancel / destroy order:
1. **`cancel_all`**: recurse into children, then `request_cancel` on local handles.
2. **`~AgentScope`**: `cancel_all`, then `children_.clear()` (each child dtor
   drains its own handles), then join local handles.

Single-owner serial model (#2399) still applies per scope. `watch_all` /
RestartN remain scope-local (no cross-scope restart map).

Regression: `tests/orch/test_agent_scope_2083` (AC1-AC6 + #2161 watch_all);
`tests/orch/test_agent_scope_hierarchy_2537` (hierarchy AC1-AC6).

### `AgentFailurePolicy` (Issue #2229, supervision surface)

`src/orch/agent_spawn.h` lifts the `FailurePolicy` (#2007) family
into the long-lived agent supervision surface. `AgentScope::watch_all`
takes an `AgentFailurePolicy` instead of the binary `StallPolicy`,
turning "kill on stall" into recoverable multi-agent coordination.

```cpp
#include "orch/orch.h"   // pulls in agent_spawn.h + agent_scope.h

aura::serve::Scheduler sched(2);
aura::orch::AgentScope scope(sched);
auto& h = scope.spawn({.name = "worker", .body = [] { /* ... */ },
                       .keepalive_interval_ms = 50});

// RestartN: on stall, stop helper → cancel body → join drain
// (via #2227 hard-reclaim) → optional backoff → spawn replacement
// under the same AgentSpec. Capped at max_restarts; circuit-like
// consecutive_stall_limit forces Cancel after that.
aura::orch::AgentFailurePolicy pol;
pol.on_stall = aura::orch::AgentFailureAction::RestartN;
pol.max_restarts = 3;
pol.consecutive_stall_limit = 3;
pol.restart_backoff_ms = 0;
auto wr = scope.watch_all(/*stall_ms=*/100, pol);
// wr.alive / stalled / done / closed / cancelled

// Metrics (query:orch-module-stats):
//   agent-restart-total             (re-spawns performed)
//   agent-restart-exhausted-total    (max_restarts OR circuit-open)
//   agent-consecutive-stall-total    (per-stall observation)
//   schema-2229 / issue-2229 / agent-failure-policy-wired
```

Rules (per Issue #2229 AC2-AC3):
1. **No** process-global registry (linter still forbids
   `AgentRegistry` / `global_agent_registry` / `conduct_parallel`).
2. RestartN is scoped to `attach_mailbox` / `keepalive_interval_ms > 0`
   agents (long-lived). Short-lived batch work still uses
   `serve::parallel_orch::parallel_intend` with the #2007
   `FailurePolicy` family.
3. `on_join_fail` is `ReportOnly` by default — the #2227 hard-reclaim
   path already drives the fiber lifecycle after a non-Ok join; a
   separate restart hook on `join_fail` is out of scope for #2229
   (documented in `AgentFailurePolicy::on_join_fail`).
4. Optional Phase C (`CircuitBreaker` mirror of #2007) is deferred
   — the `consecutive_stall_limit` cap is the simpler version of
   the same idea and ships in #2229.

### `agent-ask` / `agent-reply` (Issue #2231 / #2401 / #2538, cross-agent request/response)

Standardized request/response channel between agents without a process-global
registry (per #1966). Builds only on existing `MultiFiberMailbox` + the
Evaluator name table (per #2078). Issue #2401 adds the standard **worker
reply** path. Issue #2538 upgrades correlation to **typed fields** on
`MailMessage` (`kind` + `correlation_id`) while dual-writing the legacy
text prefixes for compatibility.

```cpp
#include "orch/orch.h"   // pulls in agent_spawn.h

// Worker: prefer try_parse_ask (typed) or agent_reply (stamps kind+corr).
// agent_ask registers corr_id → per-ask reply mailbox (pending table, not
// AgentRegistry). agent_reply(corr, body) looks it up and pushes.
auto& b = scope.spawn({.name = "worker", .body = [&] {
    // auto m = mailbox->recv(...);
    // if (auto ask = aura::orch::try_parse_ask(*m))
    //     aura::orch::agent_reply(ask->correlation_id, response);
}});
aura::orch::AskResult r = aura::orch::agent_ask(b, "ping", /*timeout_ms=*/5000);
// r.ok / r.status ("ok" | "timeout" | "no-mailbox" | "malformed")
// r.payload / r.correlation_id

// Aura
// (orch:agent-ask name payload [:timeout-ms n])
//   → hash {ok, status, payload, correlation-id, schema-2231, schema-2538}
// (orch:agent-reply corr payload)
//   → hash {ok, status, schema-2401, schema-2538}
```

Rules (per Issue #2231 / #2401 / #2538):
1. **No** process-global agent registry (MVP linter still forbids
   `AgentRegistry` / `global_agent_registry` / `conduct_parallel`).
   **Typed correlation (#2538):** `MailMessage.kind` ∈ {Ask, Reply} +
   `correlation_id` — ask↔reply match does **not** require parsing payload
   text. Legacy dual-write: `"ask:<id>:<body>"` / `"reply:<id>:<body>"`.
   A short-lived **pending-ask table** (corr → reply mailbox) is not an
   agent map — entries exist only for the ask wait window.
2. **Worker must call `agent_reply`** (C++ or `orch:agent-reply`) so
   payloads are well-formed and dest lookup is correct. Hand-rolled
   legacy prefixes still work if pushed to the same pending mailbox
   (`kind=Normal`, `correlation_id=0` + `reply:<id>:` text).
3. **Per-ask temp reply mailbox** — unique per ask (AC5 interleave safety).
4. **Match order on asker** — typed (`kind=Reply` + matching corr) first,
   then legacy text prefix. Non-matching → `status="malformed"`.
5. **`agent_reply` structured fail** — unknown corr / closed mailbox /
   backpressure → `ok=#f` with status (`unknown-corr` | `closed` |
   `backpressure`); no hang.
6. **Backpressure on ask push** → `status="timeout"` + `agent_ask_timeout_total`.
7. **Process atomic correlation id** — monotonic within process lifetime.

Metrics (`query:orch-module-stats`, schema-2231 / schema-2401 / schema-2538):
- `agent-ask-total` — successful Ok returns.
- `agent-ask-timeout-total` — wait-window expirations (incl. BP at push).
- `agent-reply-total` — successful reply pushes (#2401).
- `agent-reply-fail-total` — unknown-corr / closed / backpressure (#2401).
- `agent-ask-typed-match-total` — Ok matches via typed fields (#2538).
- `agent-reply-typed-total` — replies that stamped kind+corr (#2538).
- `agent-ask-typed-corr-wired` — sentinel 1.

See [`docs/architecture.md`](../../docs/architecture.md) · [`docs/wire-formats.md`](../../docs/wire-formats.md) §10.
## Mailbox BP admission (default on, #2228 / #2535)

Production default: `kMailboxBpAdmitThresholdDefault = 32` — spawn with
`attach_mailbox` soft-rejects when `mailbox_bp_recent_total >= 32`.

Opt-out (legacy / diagnostic): `AURA_ORCH_BP_ADMIT_THRESHOLD=0`.
Quiet-period recovery: `#2398` / `AURA_ORCH_BP_WINDOW_MS` (default 30s).
