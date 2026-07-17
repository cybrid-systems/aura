# AI Closed-Loop Readiness Observability

**Issues:** #1499 (production expand), #1470 (MVP), #1483 / #1493 (per-fiber)

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
| `health-score` | 0–100 (higher better); penalties for wraps/invalidations/rollbacks/cascade/quota/relower |
| `action` | 0=ok, 1=investigate-refs, 2=throttle-mutate, 3=raise-quota, 4=check-cascade |
| `linear-enforcements` | `linear_post_mutate_enforcements_total` |
| `cascade-depth-max` / `cascade-depth-avg-x100` | Invalidate BFS depth |
| `invalidation-protocol` | Dual-epoch protocol entries |
| `bridge-epoch-bumps` | Epoch write-side progress |
| `steal-auto-refresh` / `boundary-pinned-refresh` | StableNodeRef restamp |
| `live-closure-stale-prevented` | Apply-path stale prevention |
| `yield-rollbacks` | Mutation boundary yield rollbacks |
| `quota-rejects` | ResourceQuota rejects |
| `relower-blocks` / `partial-relower` / `full-relower` / `relower-partial-bp` | Incremental re-lower mix |
| `fiber-depth-max` / `live-mutation-depth` | Per-fiber / live Guard depth |
| `schema` | **1499** |

## Health score (sketch)

```
health = 100
  − wraps? 20
  − min(20, invalidations/5) if ≥10
  − min(20, rollbacks/5) if ≥5
  − min(15, cascade_max) if ≥8
  − min(15, quota_rejects/5) if ≥5
  − 10 if mostly full re-lower under load
  − hygiene / dirty-prune soft penalties
clamp [0, 100]
```

## Tests

- `tests/test_issue_1470.cpp` — MVP shape + recommendation
- `tests/test_issue_1499.cpp` — health-score, breakdown, schema, stress
