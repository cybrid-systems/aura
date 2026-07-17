# ResourceQuota Manager production enforcement (#1618)

**Issue:** [#1618](https://github.com/cybrid-systems/aura/issues/1618)  
**Builds on:** #1013 · #1547 · #1554 · #1579 · #1590 · #1594 · #1600  
**Status:** Production closed-loop.

## Problem

Long-running multi-fiber self-mutating workloads need a single explicit
manager that enforces memory / fibers / time / mutation budgets and
returns **typed** `AuraErrorKind::ResourceQuotaExceeded` (with provenance),
never an opaque PanicCheckpoint rollback.

## Solution

| Piece | Behavior |
|-------|----------|
| `ResourceQuotaManager` | Facade over `ResourceQuota` + provenance stamp |
| `process_resource_quota_manager()` | Scheduler / Fiber spawn / orch |
| `check_mutation_quota` / `try_acquire` | Typed reject + `mutation_budget_rejected` |
| `check_arena_quota` / `allocate_checked` | Memory dim typed reject |
| `note_quota_reject_typed` | Bumps violation counters; **not** panic path |
| Scheduler `spawn` | Uses manager `check_and_consume_fiber` |

## Metrics (`query:resource-quota-stats`, schema **1618**)

No new `query:*-stats` (#1448 freeze).

| Key | Meaning |
|-----|---------|
| `quota_violation_total` | All typed dim rejects |
| `mutation_budget_rejected` / `_total` | Mutation-dim rejects |
| `quota_reject_typed_total` | AuraResult path (not panic) |
| `panic_quota_distinguished_total` | Classified quota ≠ panic |
| `manager_enforce_total` | try_acquire / manager reject path |
| `manager-wired` / `typed-reject-not-panic` | 1 |
| `schema` | **1618** (lineage 1600\|1590\|…) |

## Tests

| File | Role |
|------|------|
| `tests/test_resource_quota_manager_1618.cpp` | **#1618** AC |
| Prior quota suite | Lineage accepts 1618 |

## Related

- `docs/design/orch-resource-quota-1600.md`
- `docs/design/resource-quota-wired-1594.md`
- `src/core/resource_quota.hh`
