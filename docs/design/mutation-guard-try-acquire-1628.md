# MutationBoundaryGuard::try_acquire typed quota (#1628)

**Issue:** [#1628](https://github.com/cybrid-systems/aura/issues/1628)  
**Builds on:** #1547 · #1556 · #1590 · #1618 · #1498  
**Status:** Production closed-loop.

## Problem

Guard construction historically used panic-checkpoint / soft-inert paths
for mutation quota. Agents need **typed** `ResourceQuotaExceeded` for
backoff/retry without process-level panic.

## Solution

```
MutationBoundaryGuard::try_acquire(ev, pending_count)
  mutation_guard_try_acquire_total++
  check_mutation_quota(pending)
    reject → note_quota_reject_typed
             mutation_guard_try_acquire_reject_total++
             return unexpected(ResourceQuotaExceeded)
  mutation_quota_used_ += pending
  construct Guard (quota_prechecked)
```

Legacy ctor `[[deprecated]]` soft-fails inert (no throw).

### Call sites (production)

| Site | Path |
|------|------|
| `typed_mutate` | try_acquire |
| `eval_on_current` | try_acquire |
| `MUTATION_BOUNDARY_PROTECT` | try_acquire |

## Metrics (`query:resource-quota-stats`, schema **1628**)

| Key | Meaning |
|-----|---------|
| `mutation_guard_try_acquire_total` | Factory entries |
| `mutation_guard_try_acquire_reject_total` | Typed rejects |
| `try_acquire_wired` / `panic_checkpoint_quota_replaced` | 1 |
| `eval_on_current_try_acquire` / `typed_mutate_try_acquire` | 1 |
| `schema` | **1628** (lineage 1618\|1600\|1547) |

## Tests

| File | Role |
|------|------|
| `tests/test_mutation_guard_try_acquire_1628.cpp` | **#1628** AC |
| `tests/test_mutation_guard_typed_error.cpp` | #1547 lineage |

## Docs

- `docs/design/core/mutate_api.md` — try_acquire section
- `docs/design/error-handling-policy.md` — quota typed path
