# Agent Decision Metrics Contract (Issue #1461)

**Status**: Enforced (stdlib + engine liveness)  
**Entry point**: `(agent:decision-metrics)` in `lib/std/agent.aura`  
**Consumers**: `auto-grow`, `edsl-fix`, `agent:closed-loop-once`, Self-Evolution suite (#1463)  
**Regression**: `tests/test_issue_1461.cpp`

## Goal

Give every self-evolving agent a **stable, always-callable, always-live** set of counters for the three safety decisions:

| Decision | When |
|----------|------|
| **commit** | Metrics healthy; keep the mutation |
| **back-off** | High rollback rate or many long mutation holds |
| **escalate** | Panic / recovery-failure signal — stop and ask a human |

This contract does **not** reintroduce demoted `query:*-stats` as public agent API. Agents use:

- `(agent:decision-metrics)` / `(agent:decide)` — preferred
- underlying live sources via `stats:get` / `atomic-batch:stats` / `mutation-log:summary` only

## Hash shape (schema `1461`)

Exactly **8** keys (Aura default hash capacity is 8):

| Key | Type | Source (live) |
|-----|------|----------------|
| `recommendation` | string `"commit"` \| `"back-off"` \| `"escalate"` | derived |
| `schema` | int `1461` | constant |
| `panic-count` | int | `stats:get "query:fiber-boundary-violation-stats"` → `recovery-failures` |
| `rollback-rate` | int 0–100 | **max**(mutation-log rate, atomic-batch rate) |
| `hold-time-us` | int | `stats:get "query:mutation-boundary-hold-stats"` → `hold-time-us-total` |
| `long-hold-count` | int | hold-stats → `holds-over-1ms` |
| `atomic-batch-commits` | int | `atomic-batch:stats` → `batch-count` |
| `atomic-batch-rollbacks` | int | `atomic-batch:stats` → `rollback-count` |

### rollback-rate formula

```
ml-rate    = rolled-back / total × 100          ; mutation-log:summary (0 if total=0)
batch-rate = rollback-count / (batch-count + rollback-count) × 100
rollback-rate = max(ml-rate, batch-rate)
```

Atomic-batch outcomes are **always live** after any batch attempt; mutation-log alone can under-report when rollbacks do not mark `RolledBack` status.

## Thresholds (stdlib defaults)

| Symbol | Default | Effect |
|--------|---------|--------|
| `*agent-panic-max*` | 1 | `panic-count ≥ 1` → **escalate** |
| `*agent-rollback-rate-max*` | 50 | `rollback-rate ≥ 50` → **back-off** |
| `*agent-long-hold-max*` | 50 | `long-hold-count > 50` → **back-off** (noise floor; counters are cumulative) |
| `*agent-yield-skip-max*` | 20 | `#1553` safe-yield `skipped-held` pressure → folds into long-hold |
| `*agent-quota-violation-max*` | 1 | `#1553` longrunning `quota-violations` → folded into panic-count |
| `*agent-fiber-rollback-max*` | 50 | `#1553` fiber Guard rollbacks → long-hold pressure |

### #1553 fold-ins (schema stays 1461 / 8 keys)

Live sources additionally read:

- `stats:get "query:mutation-boundary-safe-yield-stats"` (`skipped-held`, depth)
- `stats:get "query:longrunning-infra-stats"` (`quota-violations`)
- `(mutate:boundary-depth)` — nested Guard → **back-off** (never commit under Guard)
- fiber `rollbacks` from `query:fiber-boundary-violation-stats`

These are **folded** into existing `panic-count` / `long-hold-count` fields
so the public hash shape remains schema `1461`. For the full signal set
as an alist, call `(mutate:safety-snapshot)`.

Closed-loop path:

1. `(mutate:safe-yield)` before decide / mutate
2. `(mutate:atomic-batch-safe …)` instead of raw atomic-batch
3. `(mutate:safe-yield)` again after eval before post metrics

## Liveness guarantees (engine)

| Contract field | Bump path (must fire on event) | Verified by |
|----------------|--------------------------------|-------------|
| `atomic-batch-commits` | successful `mutate:atomic-batch` / `(mutate :atomic …)` | `test_issue_1461` AC2 |
| `atomic-batch-rollbacks` | failed batch (unsupported op / sub-op error → rollback) | AC2 + AC5 |
| `hold-time-us` / `long-hold-count` | outermost `MutationBoundaryGuard` dtor samples hold; `>1ms` → `holds-over-1ms` | AC2 Guard hold |
| `panic-count` (`recovery-failures`) | fiber resume mismatch recovery failure; also `Evaluator::bump_mutation_boundary_recovery_failure()` | AC5 inject + fiber path wire |
| fiber `rollbacks` (feeds safety) | outermost Guard with `success=false` → `bump_mutation_boundary_rollback()` | AC2 failed Guard |

**Production wire points (Issue #1461):**

1. `MutationBoundaryGuard::~MutationBoundaryGuard` — outermost `!success` → `bump_mutation_boundary_rollback()`
2. Fiber post-yield restore mismatch (`evaluator_fiber_mutation.cpp`) → `bump_mutation_boundary_recovery_failure()` + `bump_mutation_boundary_rollback()`
3. Existing atomic-batch domain counters (`atomic_batch_domain_.count` / `.rollbacks`)
4. Existing hold-time sampling in Guard dtor (`mutation_boundary_hold_*`)

## API

```scheme
(require "std/agent" all:)

(agent:decision-metrics)   ; → hash (schema 1461)
(agent:decide)             ; → 'commit | 'back-off | 'escalate

; Closed-loop MUST call metrics before treating a mutate as final:
(agent:closed-loop-once :source "..." :rebind "f" "(lambda (x) 0)")
(auto-grow "task" :source "..." :rebind "f" "...")   ; default EDSL path
(auto-grow "task" :prompt-only)                      ; LLM-only compat
```

## Rules for agents

1. Call `(agent:decision-metrics)` (or `(agent:decide)`) **before** accepting a commit.
2. On `"escalate"` / `'escalate`: do not blind-retry LLM; surface to human.
3. On `"back-off"` / `'back-off`: stop or slow down; do not thrash mutate.
4. Prefer `(mutate:atomic-batch-safe …)` / `(mutate:atomic-batch …)` / `(mutate :atomic …)` over free `eval` of LLM text. Multi-fiber loops should use the **safe** wrapper (#1553).
5. Yield only via `(mutate:safe-yield)` / `(ast:yield-at-boundary)` when boundary depth is 0 (#1504); never yield under Guard.
6. Do **not** invent new `*-stats` primitives — use this contract + `(stats:get)` / `(engine:metrics)` / `(mutate:safety-snapshot)`.

## Related

- #1460 — `std/agent` EDSL closed-loop rewrite (primary consumer; done)
- #1461 — this contract + engine liveness + injection tests
- #1463 — Self-Evolution reliability suite (reads this contract)
- #1553 — stdlib bridge of Guard / safe-yield / quota into mutate+agent+orchestrator
- #1504 / #1547 / #1546 — engine P0 surfaces
- `docs/agent-prompt-template.md` — agent red lines
- `docs/design/core/mutate_api.md` — atomic-batch + #1553 safety table
