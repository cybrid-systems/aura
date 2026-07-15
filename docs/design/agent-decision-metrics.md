# Agent Decision Metrics Contract (Issue #1461 / used by #1460)

**Status**: Phase 1 — minimal live contract  
**Entry point**: `(agent:decision-metrics)` in `lib/std/agent.aura`  
**Consumers**: `auto-grow`, `edsl-fix`, `agent:closed-loop-once`, Self-Evolution suite (#1463)

## Goal

Give every self-evolving agent a **stable, always-callable** set of counters for the three safety decisions:

| Decision | When |
|----------|------|
| **commit** | Metrics healthy; keep the mutation |
| **back-off** | High rollback rate or long mutation holds |
| **escalate** | Panic / recovery-failure signal — stop and ask a human |

This contract does **not** reintroduce demoted `query:*-stats` as public agent API. It wraps already-live engine surfaces (`mutation-log:summary`, `atomic-batch:stats`, `stats:get` of hold/fiber stats).

## Hash shape (schema `1461`)

Exactly **8** keys (Aura default hash capacity is 8):

| Key | Type | Source |
|-----|------|--------|
| `recommendation` | string `"commit"` \| `"back-off"` \| `"escalate"` | derived |
| `schema` | int `1461` | constant |
| `panic-count` | int | `stats:get "query:fiber-boundary-violation-stats"` → `recovery-failures` |
| `rollback-rate` | int 0–100 | `mutation-log:summary` rolled-back / total × 100 |
| `hold-time-us` | int | `stats:get "query:mutation-boundary-hold-stats"` → `hold-time-us-total` |
| `long-hold-count` | int | hold-stats → `holds-over-1ms` |
| `atomic-batch-commits` | int | `atomic-batch:stats` → `batch-count` |
| `atomic-batch-rollbacks` | int | `atomic-batch:stats` → `rollback-count` |

## Thresholds (stdlib defaults)

| Symbol | Default | Effect |
|--------|---------|--------|
| `*agent-panic-max*` | 1 | `panic-count ≥ 1` → **escalate** |
| `*agent-rollback-rate-max*` | 50 | `rollback-rate ≥ 50` → **back-off** |
| `*agent-long-hold-max*` | 50 | `long-hold-count > 50` → **back-off** (noise floor; counters are cumulative) |

Tune by redefining these in agent code if needed. Engine-side liveness proofs (counters bump on every panic/rollback path) are tracked under #1461 follow-ups; the stdlib wrapper is the agent-facing freeze.

## API

```scheme
(require "std/agent" all:)

(agent:decision-metrics)   ; → hash (schema 1461)
(agent:decide)             ; → 'commit | 'back-off | 'escalate

; Closed-loop must call metrics before treating a mutate as final:
(agent:closed-loop-once :source "..." :rebind "f" "(lambda (x) 0)")
(auto-grow "task" :source "..." :rebind "f" "...")   ; default EDSL path
(auto-grow "task" :prompt-only)                      ; LLM-only compat
```

## Rules for agents

1. Call `(agent:decision-metrics)` (or `(agent:decide)`) **before** accepting a commit.
2. On `"escalate"` / `'escalate`: do not blind-retry LLM; surface to human.
3. On `"back-off"` / `'back-off`: stop or slow down; do not thrash mutate.
4. Prefer `(mutate:atomic-batch …)` / `(mutate :atomic …)` over free `eval` of LLM text.
5. Do **not** invent new `*-stats` primitives — use this contract + `(stats:get)` / `(engine:metrics)`.

## Related

- #1460 — `std/agent` EDSL closed-loop rewrite (primary consumer)
- #1461 — full engine liveness enforcement + regression injection tests
- #1463 — Self-Evolution reliability suite (reads this contract)
- `docs/agent-prompt-template.md` — agent red lines
