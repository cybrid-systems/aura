# ResourceQuota hot-path enforcement (#1590)

**Issues:** #1590 (hot-path closed-loop), builds on #1481 / #1498 / #1546 / #1547 / #1554 / #1579; refined by **#1594** (`resource-quota-wired-1594.md`)

## AC surface

| Path | Behavior |
|------|----------|
| `ASTArena::allocate_raw` | Owner + `quota_allow_fn_` → nullptr on reject |
| `ASTArena::allocate_checked` | Typed `ResourceQuotaExceeded` |
| `Evaluator::allocate_checked` | Same via owner / explicit check |
| `MutationBoundaryGuard::try_acquire` | Typed `AuraResult` + `check_mutation_quota` |
| Legacy Guard ctor (#1590) | Soft-fail: `is_inert()`, `success_flag=false` |
| Scheduler spawn | Process `ResourceQuota` fibers dimension (#1579) |
| `query:resource-quota-stats` | schema **1590** (legacy 1579/… still accepted by agents) |

## Query keys (schema 1590)

`checks_total`, `rejects_total` / `exceeded_count`, `current_usage`, `quota`,
`memory_quota`, `memory_quota_total`, `max_fibers`, `max_mutations`,
`mutations_used`, process fiber fields, `hotpath_arena_gated`, `issue=1590`.

## Tests

- `tests/test_resource_quota.cpp` (#1481 helpers)
- `tests/test_arena_quota_wired.cpp` (#1546/#1554 allocate_raw)
- `tests/test_quota_edge_cases.cpp` (#1548)
- `tests/test_resource_quota_module.cpp` (#1579)
- `tests/test_resource_quota_hotpath.cpp` (#1590 closed-loop)
- `tests/test_resource_quota_wired.cpp` (#1594 AC consolidation)
