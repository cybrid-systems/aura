# Arena smart auto-compact + Shape/dirty closed-loop (#1621)

**Issue:** [#1621](https://github.com/cybrid-systems/aura/issues/1621)  
**Builds on:** #743 · #722 · #685 · #1518 · #1521 · #1467  
**Status:** Production closed-loop.

## Problem

Arena had auto-compact on alloc (`maybe_auto_compact_on_alloc`), boundary
probes, live_compact skeleton, and Shape on_compact hooks (#743), but the
decision policy was fragmented (hard-coded frag≥0.30 only) and Shape churn
did not feed Arena policy under multi-round AI mutation.

## Solution

| Piece | Behavior |
|-------|----------|
| `evaluate_auto_compact_policy` | Unified decision: frag / small-pool / dirty / shape_churn / defrag_req |
| Soft thresholds | frag≥0.30 hard; frag≥0.15 with defrag_req or churn |
| Render soft-gate | still skips compact; counts evaluations |
| Fiber path | safepoint + `defrag-fiber-safe-hits` |
| `ShapeProfiler::invalidate` | `signal_shape_churn` + `signal_dirty_cascade` when was_stable |
| Boundary / fiber probes | use smart policy; record boundary-exit / fiber-transition counters |
| Live defrag preference | when defrag_req or high-frag+churn |

## Metrics (`query:arena-auto-policy-stats`, schema **1621**)

| Key | Meaning |
|-----|---------|
| `auto-compact-triggers` | lineage 743 |
| `smart-policy-evaluations` | policy function calls |
| `smart-policy-triggers` | decisions that requested compact |
| `shape-churn-triggers` | triggers with shape churn bit |
| `boundary-exit-compacts` | MutationBoundary exit path |
| `fiber-transition-compacts` | steal/resume path |
| `live-defrag-policy-hits` | prefer_live_defrag decisions |
| `smart-policy-wired` / `closed-loop-wired` | 1 |
| `schema` | **1621** (lineage 743) |

## Tests

| File | Role |
|------|------|
| `tests/test_arena_auto_compact_policy_1621.cpp` | **#1621** AC |
| `tests/test_arena_auto_compact_fiber_defag_shape_dirty_closedloop.cpp` | Lineage accepts 1621 |

## Related

- `src/core/arena_auto_policy_stats.h`
- `ASTArena::maybe_auto_compact_on_alloc`
- `Evaluator::probe_arena_auto_policy_on_boundary_exit`
