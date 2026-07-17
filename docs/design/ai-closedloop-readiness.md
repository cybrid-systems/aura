# AI Closed-Loop Readiness Observability

**Issues:** #1593 (SLO + trend + adaptive linkage), #1499 (production expand),
#1470 (MVP), #1483 / #1493 / #1591 / #1592 (sibling metrics)

## Primitive

```
(engine:metrics "query:ai-closedloop-readiness-stats")
```

Single high-frequency query for AI agents before/after self-mutation batches.

## Fields

### #1470 contract (stable)

| Key | Meaning |
|-----|---------|
| `wraps` | FlatAST generation wrap count |
| `invalidations` | StableNodeRef invalidations |
| `batch-commits` | Atomic batch commits |
| `hygiene-skips` | Macro hygiene skipped |
| `dirty-prunes` | Mark-dirty boundary prunes |
| `recommendation` | 0=ok … 4=dirty-prune pressure |

### #1499 production expand

| Key | Meaning |
|-----|---------|
| `health-score` | 0–100 (higher better) |
| `action` | 0=ok … 4=check-cascade |
| `linear-enforcements` | post-mutate linear checks |
| `cascade-depth-max` / `cascade-depth-avg-x100` | Invalidate BFS |
| `steal-auto-refresh` / `boundary-pinned-refresh` | StableNodeRef restamp |
| `yield-rollbacks` / `quota-rejects` | Boundary / ResourceQuota |
| `relower-*` / `fiber-depth-max` / `live-mutation-depth` | Relower + Guard depth |

### #1593 SLO + trend + adaptive + siblings

| Key | Meaning |
|-----|---------|
| `slo-breach` | 1 if health &lt; 70 or action ≥ 3 |
| `slo-threshold` | 70 |
| `slo-breach-total` | Process lifetime breach samples |
| `health-trend` | Δ health vs previous sample |
| `health-prev` | Previous sample health |
| `samples-total` | Sample count |
| `avg-hold-time-us` | MutationBoundary hold average |
| `safepoint-wait-while-mutation-held-us` | GC wait under Guard (#1591) |
| `safe-yield-skipped-held` | Safe-yield skips |
| `post-steal-refresh-count` | Resume refresh passes (#1592) |
| `steal-inner-deferred-starvation-mitigated-count` | Steal fairness (#1492) |
| `adaptive-safepoint-recommended` | 1 when orchestrators should back off |
| `adaptive-soft-triggers` | Times soft adapt ran (health &lt; 50) |
| `adaptive-safepoint-threshold` | Current adaptive threshold |
| `schema` | **1593** (Agents may still accept 1499) |

## Health score (sketch)

```
health = 100
  − wraps? 20
  − min(20, invalidations/5) if ≥10
  − min(20, rollbacks/5) if ≥5
  − min(15, cascade_max) if ≥8
  − min(15, quota_rejects/5) if ≥5
  − 10 if mostly full re-lower under load
  − hygiene / dirty-prune / long-hold / safepoint-wait soft penalties
clamp [0, 100]
```

## Adaptive linkage

When `slo-breach` and `health-score < 50`, one soft
`bump_safepoint_adaptive_threshold()` runs so GC/orchestrators back off
under severe pressure. Orchestrators should also read
`adaptive-safepoint-recommended` and `action`.

## Tests

- `tests/test_issue_1470.cpp` — MVP shape
- `tests/test_issue_1499.cpp` — health-score (schema 1499|1593)
- `tests/test_ai_closedloop_readiness_1593.cpp` — SLO / trend / adaptive
