# Unify invalidate + MutationBoundaryGuard::try_acquire (#1634)

**Issue:** [#1634](https://github.com/cybrid-systems/aura/issues/1634)  
**Builds on:** #1476 · #1547 · #1556 · #1524 · #1628  
**Status:** Production mandate — dual-epoch invalidate + typed quota.

## Contract

```
typed_mutate / typed_mutate_atomic / mark_define_dirty / invalidate_function:
  → atomic_bump_epochs_and_stamp_bridge  (unified dual-epoch)
  typed_mutate_atomic also bumps typed_mutate_atomic_invalidations_total

MutationBoundaryGuard::try_acquire(ev, pending):
  if !check_mutation_quota → AuraError{ResourceQuotaExceeded}
  else unique_ptr<Guard>

legacy MutationBoundaryGuard ctor:
  [[deprecated("use try_acquire")]]; soft-fail inert on quota (#1590)

Guard::~ (outermost):
  if !success:
    linear_post_mutate_enforce_all
    enforce_linear_boundary_consistency(mark_all)
    walk_active_closures (probe)
    guard_failure_linear_enforce_total++
  else:
    enforce_linear_boundary_consistency(only_if_moved)
```

## Metrics (`query:resource-quota-stats`, schema **1634**)

| Key | Meaning |
|-----|---------|
| `mutation_guard_try_acquire_total` / `_reject_total` | try_acquire path |
| `mutation_boundary_try_acquire_fail_total` | alias of reject (#1634 body) |
| `resource_quota_rejects_total` | alias of rejects_total |
| `typed_mutate_atomic_invalidations_total` | atomic batch invalidations |
| `guard_failure_linear_enforce_total` | failure-path linear probe |
| `typed_mutate_atomic_unified_invalidate` / `atomic_bump_epochs_unified` | 1 |
| `schema` | **1634** (lineage 1628…1547) |

## Tests

| File | Role |
|------|------|
| `tests/test_unify_invalidate_try_acquire_1634.cpp` | **#1634** AC |
| `tests/test_mutation_guard_try_acquire_1628.cpp` | Prior try_acquire |
| `tests/test_issue_1524.cpp` | atomic_bump on typed_mutate |

## Related

- `docs/design/resource-quota-hotpath.md`
- `docs/design/unified-invalidation-epoch-protocol.md`
- `docs/design/invalidate-consistency.md`
