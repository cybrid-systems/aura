# src/orch/

Agent orchestration facade — `orch.h` · `agent_spawn.h` · `orch.ixx` (#1588).

## Aura language primitives (Issue #1588 / #2011)

| Primitive | Calling convention | Result |
|-----------|-------------------|--------|
| `(orch:spawn-agent name [thunk] [:attach-mailbox bool] [:high-water n] [:keepalive-interval-ms n] [:max-no-yield-ms n])` | `name` string; optional 0-arg thunk; optional keywords | hash `{ok, id, name, schema=1588, schema-2011, quota-exceeded[, error]}`; **quota reject → typed Aura error** |
| `(orch:agent-poll name)` | Issue #2540 coop yield edge | hash `{ok, yielded, schema-2540}` — forces `Fiber::yield` when `max_no_yield_ms` window elapsed |
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

### Cooperative yield contract (Issue #2540 + production default #2585)

Long-running LLM-style agent bodies that never yield starve cancel/steal/GC
and force residual hard-reclaim (#2227 / #2533). Optional **`AgentSpec.max_no_yield_ms`**
documents a cooperative budget; bodies call `agent_poll()` / `orch:agent-poll`
(or `note_agent_progress` / `orch:agent-touch`) so a `Fiber::yield` runs
when the window elapses.

```cpp
AgentSpec spec{.name = "worker", .max_no_yield_ms = 10};
spec.body = [] {
    for (;;) {
        if (g_current_fiber && g_current_fiber->is_cancel_requested())
            break;
        // work...
        (void)aura::orch::agent_poll(); // forced yield if window elapsed
    }
};
```

**Production default (Issue #2585):** when production defaults are active
(`AURA_SANDBOX != off`) and `max_no_yield_ms == 0`, `spawn_agent_with_mailbox`
injects a **50ms** default window so the cancel/steal/GC surface stays
alive even when an agent body forgets `agent_poll`. Explicit `> 0` still
wins. Opt out via env `AURA_AGENT_MAX_NO_YIELD_MS=0` to keep the zero-cost
`#2540` path; under `AURA_SANDBOX=off` (unit Soft) the default is never
applied (preserves unit-test ergonomics).

| Setting | Behaviour |
|---------|-----------|
| `max_no_yield_ms == 0` + `AURA_AGENT_MAX_NO_YIELD_MS=0` opt-out | identical to pre-#2540 (no coop state, poll no-op) |
| `max_no_yield_ms == 0` + `AURA_SANDBOX=off` (dev_off) | identical to pre-#2540 (zero-cost, unit Soft) |
| `max_no_yield_ms == 0` + production | **50ms default injected** (Issue #2585); coop state installed; `agent_no_yield_default_applied_total` bumped once |
| `max_no_yield_ms > 0` | spawn installs `AgentCoopYield`; poll yields after window |

Metrics: `agent-forced-yield-total`, `schema-2540`, `agent-max-no-yield-wired`,
`agent-no-yield-default-applied-total` (#2585, bumped once per default injection).
Complements residual force-safepoint (#2533): cooperate first, hard-reclaim second.

Regression: `tests/orch/test_agent_max_no_yield_2540` (extends #2540 ACs with #2585 default
+ opt-out coverage; same binary, same `AURA_SANDBOX` env discipline).

### FailurePolicy ↔ AgentFailurePolicy bridge (Issue #2539)

Batch `serve::parallel_orch::FailurePolicy` (#2007) and long-lived
`AgentFailurePolicy` (#2229) stay separate surfaces. Issue #2539 adds a
**unidirectional** mapping so Agent frameworks can promote a batch policy
into a scope supervision policy without hand-rolled switch tables.

```cpp
#include "orch/orch.h"

// From FailurePolicy enum:
auto pol = aura::orch::to_agent_policy(
    aura::orch::FailurePolicy::RetryN, /*max_restarts=*/3);

// From ParallelPolicy (uses resolved_failure_policy + max_retries /
// consecutive_fail_limit / retry_backoff_ms):
aura::orch::ParallelPolicy pp;
pp.failure_policy = aura::orch::FailurePolicy::CircuitBreaker;
pp.consecutive_fail_limit = 5;
auto pol2 = aura::orch::to_agent_policy(pp);
// pol2.on_stall == Cancel, consecutive_stall_limit == 5

scope.watch_all(/*stall_ms=*/100, pol);
```

| Batch `FailurePolicy` | → `AgentFailurePolicy` |
|----------------------|-------------------------|
| `FailFast` | `on_stall = Cancel` |
| `CollectAll` | `on_stall = ReportOnly` |
| `RetryN` | `on_stall = RestartN`, `max_restarts` from arg / `pp.max_retries` |
| `CircuitBreaker` | `on_stall = Cancel`, `consecutive_stall_limit` from arg / `pp.consecutive_fail_limit` |

Semantic boundary:
1. **Batch** policies govern body-error admit/retry under `parallel_intend`.
2. **Agent** policies govern stall response under `AgentScope::watch_all`.
3. **RestartN** is only meaningful for long-lived agents with keepalive;
   `max_restarts = 0` disables re-spawn (cap at zero).
4. **Not calling the bridge leaves #2007 / #2229 defaults unchanged** (AC3).
5. Optional sugar (`orch:supervise-batch` / auto-apply after batch fail) is
   deferred; this issue ships the mapping API only.

Regression: `tests/orch/test_failure_policy_bridge_2539` (mapping table);
`tests/orch/test_agent_failure_policy_2229` (#2229 unchanged).

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

### `orch:scope-*` AgentScope supervision (Issue #2588)

Aura language surface for the C++ `AgentScope` (#2083 / #2161 / #2537) — multi-agent supervision root bound to Evaluator/session, **no process-global registry** (MVP linter still forbids `AgentRegistry` / `global_agent_registry` / `conduct_parallel`). Name bookkeeping remains `OrchAgentNameTable` only where `orch:spawn-agent` already registers; the scope is a handle container, not an agent map.

```aura
(orch:scope-spawn name [body] [:keepalive-ms n] [:max-no-yield-ms n] ...)
(orch:scope-watch [:stall-ms n]
                  [:policy 'cancel|'report-only|'restart-n]
                  [:max-restarts n]
                  [:consecutive-stall-limit n]
                  [:restart-backoff-ms n])
(orch:scope-join-all [:timeout-ms n] [:drain-ms n])
(orch:scope-cancel-all)
```

Semantics:
1. **Per-Evaluator scope** — `g_evaluator_agent_scopes()` keyed by `Evaluator*`
   (typed as `void*` in the header to avoid circular include with
   `compiler/evaluator.h`). Each Evaluator has its own `AgentScope`; the
   storage map is process-level but the SCOPE objects themselves are
   per-Evaluator and do **not** become a global agent map.
2. **Handles live in scope** — `orch:scope-spawn` pushes the new
   `AgentHandle` to the scope's `handles_` vector. Destructor /
   `orch:scope-join-all` = cancel + drain + reservation release (#2155 /
   #2009 no-leak contract). After `join_all` empties the scope the
   per-Evaluator slot is dropped (fresh `scope-spawn` re-creates).
3. **`orch:scope-watch`** — maps to `AgentScope::watch_all(stall_timeout_ms,
   AgentFailurePolicy)`. `RestartN` re-spawns under the same `AgentSpec`
   (#2229 sibling). Counts: alive / stalled / cancelled / done / closed /
   restart-count (incremental from RestartN bumps on the same watch call).
4. **Hierarchy optional later** — v1 is flat scope per Evaluator/session
   (`orch:scope-child` deferred). `AgentScope::spawn_child` exists in C++
   (#2537) but is not yet exposed to Aura.
5. **Not a global registry** — `scripts/check_orch_mvp_scope.py` still
   rejects `AgentRegistry` / `global_agent_registry` / `conduct_parallel`.
   The map `g_evaluator_agent_scopes()` is storage only; the `AgentScope`
   objects inside it are per-Evaluator. Name→handle bookkeeping stays
   in `Evaluator::agent_names_` (`orch:spawn-agent` / `orch:agent-join`
   only); the scope does not duplicate or replace it.

Hash results (AC3):
- `orch:scope-spawn` → `{ok, id, name, schema=2588, schema-2083, schema-2161, status}`
- `orch:scope-watch` → `{ok, alive, stalled, cancelled, done, closed, restart-count, policy, schema=2588, schema-2161, schema-2229}`
- `orch:scope-join-all` → `{ok, status, wait-us, drain-ms, schema=2588, schema-2083, schema-2153}`
- `orch:scope-cancel-all` → `{ok, cancelled-count, schema=2588, schema-2083, schema-2161}`

Metrics (`query:orch-module-stats`, schema-2588):
- `scope-spawn-total` — `orch:scope-spawn` invocations.
- `scope-watch-total` — `orch:scope-watch` invocations.
- `scope-watch-restart-count` — RestartN re-spawns (#2229 sibling, additive).
- `scope-join-all-total` — `orch:scope-join-all` invocations.
- `scope-cancel-all-total` — `orch:scope-cancel-all` invocations.
- `scope-dropped-total` — scopes dropped after join_all (empty scope released).
- `orch-scope-wired` — sentinel 1.

Regression: `tests/orch/test_orch_scope_2588` (Aura prims + scope lifecycle).
`scripts/check_orch_mvp_scope.py` (reintroduction guard) is unchanged
but the new surface introduces no removed-identifier symbols.

See [`docs/architecture.md`](../../docs/architecture.md) § multi-agent.

## Mailbox BP admission (default on, #2228 / #2535)

Production default: `kMailboxBpAdmitThresholdDefault = 32` — spawn with
`attach_mailbox` soft-rejects when `mailbox_bp_recent_total >= 32`.

Opt-out (legacy / diagnostic): `AURA_ORCH_BP_ADMIT_THRESHOLD=0`.
Quiet-period recovery: `#2398` / `AURA_ORCH_BP_WINDOW_MS` (default 30s).

### Per-spec override (Issue #2591, multi-tenant / multi-scope isolation)

Multi-tenant / multi-`AgentScope` hosts cannot rely on a single
process-wide threshold: one noisy producer scope can deny
`attach_mailbox` spawns for unrelated scopes/tenants. `#2591` adds a
**per-spec override** that isolates *policy* (not the gauge — the BP
recent gauge stays process-global for v1; cheaper, sufficient for
most production hosts).

`AgentSpec::bp_admit_threshold` is `std::optional<std::uint64_t>`:

| Value | Meaning |
|-------|---------|
| `nullopt` | Use process default (`#2535` default=32, or `AURA_ORCH_BP_ADMIT_THRESHOLD` env override) |
| `0` | **Admit off for THIS spawn** — always reject under `attach_mailbox` (cheap self-isolation for low-trust scopes) |
| `N > 0` | **Local threshold** — only deny if `mailbox_bp_recent_total >= N` (relax policy for trusted scopes) |

Aura surface: `:bp-admit-threshold n` kwarg on `(orch:spawn-agent …)`:

```text
(orch:spawn-agent my-agent body :attach-mailbox #t
                   :bp-admit-threshold 64)  ; trusted scope, tolerate more BP
(orch:spawn-agent untrusted-agent body :attach-mailbox #t
                   :bp-admit-threshold 0)   ; always reject mailbox attach
```

Metrics: `spawn_bp_admit_reject_total` (process-default storm) and
`spawn_bp_admit_reject_override_total` (per-spec override storm —
separate counter so dashboards can distinguish "global BP storm"
from "local noisy producer").

Structured reject stays `quota_dimension="mailbox-bp"` + `reserved=0`
(no-leak contract `#2155`); reject path unchanged otherwise. Agent
frameworks can branch on `quota_dimension` to surface BP vs fiber /
memory quota in different ways. Scope-local recent BP counter is the
natural follow-up (heavier; v1 keeps process-global gauge +
per-spawn threshold only).

Regression: `tests/orch/test_per_scope_bp_admit_2591`.

## Observability facade (Issue #2589)

Cancel-storm health for the **unified** hard-reclaim protocol
(`#2227` shared by `orch` + `parallel_orch`) is exposed through a
single `query:orch-module-stats` facade. No second primitive needed.

Source-of-truth split:

| Surface | Counters | Authoritative for |
|---------|----------|-------------------|
| `OrchModuleStats.join_drain_residual_total` / `_reclaim_total` / `_still_running` / `_body_retired_total` | `src/orch/agent_spawn.h` | `orch:agent-join` / `cancel_and_drain_*` paths (`#2397`) |
| `ParallelOrchStats.join_drain_residual_total` / `_reclaim_total` / `_join_drain_us_total` | `src/serve/parallel_orch.h` | `parallel_orch::parallel_run` Timeout path |

Facade keys (live read, **no double-bookkeeping**):

```text
query:orch-module-stats
├─ parallel-join-drain-residual-total            ← g_parallel_orch_stats.join_drain_residual_total
├─ parallel-join-drain-residual-reclaim-total    ← g_parallel_orch_stats.join_drain_residual_reclaim_total
├─ parallel-join-drain-us-total                  ← g_parallel_orch_stats.join_drain_us_total
├─ parallel-join-drain-source = 0                ← 0 = ParallelOrchStats (orch-side = 1; not wired to avoid mirror)
├─ orch-obs-facade-unified-2589 = 1              ← sentinel
├─ schema-2589 / issue-2589
├─ (orch-side) join-drain-residual-total / -reclaim-total / -still-running / -body-retired-total
│   ← OrchModuleStats (unchanged; orch paths bump directly)
```

**Dashboards / agents should query the facade**, not parallel_orch
internally. The facade is additive — `ParallelOrchStats` remains the
source of truth for parallel-only regression tests.

Regression: `tests/orch/test_orch_obs_facade_2589`.

## Security schedule gate (Issue #2590)

Pure gate that decides whether the orch / agent-body should **admit
new mutate**. Synthesizes:

- `commit_readiness().would_allow_commit` (Issue #2553, solve × linear
  × blame × truncate)
- Capability deny storm (#2534 short-window rate)
- `mid_fallback_rate_bp > SLO`
- Posture `wal_off` under Restricted (#2076)

Production default denies new mutate when `would_allow_new_mutate=false`.
Soft / `sandbox=off` is **observe-only** (counters bump, never denies).
Mailbox already enqueued is **not** killed — gate is additive over
existing admission.

`src/orch/security_schedule_gate.h` provides:

| Symbol | Purpose |
|--------|---------|
| `enum class SecurityScheduleForceReason` | `ok` / `commit_not_ready` / `deny_storm` / `mid_fallback_slo` / `posture_degraded` |
| `struct SecurityScheduleInput` | knobs (commit_readiness_would_allow, *_hard_reject, capability_deny_storm, mid_fallback_slo_breach, posture_wal_off_restricted, production_mode, soft_mode) |
| `struct SecurityScheduleDecision` | `would_allow_new_mutate` + `force_reason` |
| `decide_security_schedule(in)` | **pure** — same input → same output, no atomics (#2590 AC1) |
| `evaluate_security_schedule(in)` | pure + bumps process-wide counters atomically (#2590 AC2 + AC3) |
| `g_orch_security_schedule_counters` | per-reason deny totals + last decision (`last_force_reason_code`, `last_would_allow`) |
| `reset_orch_security_schedule_counters_for_test()` | test reset |

Decision priority (first match wins, only when `production_mode && !soft_mode`):

| Condition | `force_reason` | allow |
|-----------|---------------|-------|
| `!commit_readiness_would_allow && commit_readiness_hard_reject` | `commit_not_ready` | false |
| `capability_deny_storm` | `deny_storm` | false |
| `mid_fallback_slo_breach` | `mid_fallback_slo` | false |
| `posture_wal_off_restricted` | `posture_degraded` | false |
| other | `ok` | true |

Query surface (`query:security-schedule-gate`, schema-2590):

```text
would-allow-new-mutate           ← last_would_allow (1=allow, 0=deny)
force-reason-code                ← last_force_reason_code (enum int)
checks-total / deny-total / allow-total
deny-commit-not-ready-total / deny-deny-storm-total /
  deny-mid-fallback-slo-total / deny-posture-degraded-total
security-schedule-gate-wired = 1
schema-2590 / issue-2590
```

**Call sites** (caller wraps `evaluate_security_schedule()` BEFORE
admitting new mutate — additive):

- `orch:agent-body` mutate dispatch
- `orch:parallel-intend` mutate batch
- mutate-schedule entry points (typed_mutation_audit.h admission)

v1 ships the gate contract; orch admission wiring is per-call-site
(consumers wire via `evaluate_security_schedule` and surface the
deny via existing typed Aura errors). The #2543 AOT throttle pattern
(`decide_hot_update_throttle` in `aot_hot_update_health.hh`) is the
direct precedent.

Regression: `tests/orch/test_security_schedule_gate_2590` (pure
idempotency + production/soft mode matrix + counter bumps + query
surface + README source-cite). No docs/design/ per #1655.
