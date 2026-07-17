# StableNodeRef Auto-Restamp Across GC / Steal / Safepoint

**Issues:** #1497 (parent closed-loop), #1473 (MVP hooks), #1500 (batch restamp), #1490 (post-steal EnvFrame)

## Problem

`validate_or_refresh` / `refresh_if_stale` existed, but GC-safepoint, fiber-steal,
and compact/re-pin paths each walked **only** `cow_boundary_pinned_refs_` (or
none), skipping `atomic_batch_pinned_refs_`. Long AI sessions under GC + steal
could leave pinned handles stale.

## Unified protocol (#1497)

All production sites call:

```
auto_restamp_pinned_stable_refs_at(site)
  → restamp_pinned_stable_refs()   // atomic-batch + cow-boundary
  → site counters (steal / gc_safepoint / compact / yield-resume)
```

| Site | Enum | Trigger |
|------|------|---------|
| Fiber steal | `Steal` | `probe_linear_ownership_on_fiber_steal` |
| GC safepoint | `GcSafepoint` | `probe_linear_ownership_at_gc_safepoint` |
| Compact / re-pin | `CompactOrRepin` | `re_pin_cow_children_from_snapshot` / arena compact |
| Yield resume | `YieldResume` | `restore_post_yield_or_rollback` on desync |

`refresh_if_stale` restamps full provenance (gen/wrap/cow/mutation_id/subtree_gen)
while preserving `fiber_id`, `workspace_id`, and `boundary_pinned`.

## Metrics (AC3)

| Metric | Meaning |
|--------|---------|
| `stable_ref_steal_auto_refresh_total` | Refs successfully refreshed (any registry) |
| `boundary_pinned_refresh_count` | COW-boundary pins that were **stale** and restamped |
| `stable_ref_validations_at_steal` | Site counter (steal / compact / yield) |
| `stable_ref_validations_at_gc_safepoint` | Site counter (GC) |

## Tests

- `tests/test_issue_1497.cpp` — real pinned refs + 1000-iter GC/steal/mutate
- `tests/test_issue_1473.cpp` — empty-hook stress (still green)
- `tests/test_issue_1500.cpp` — batch restamp unit coverage
