# ResourceQuota wired refine (#1594)

**Issue:** [#1594](https://github.com/cybrid-systems/aura/issues/1594)  
**Refines:** #1481, #1487, #1498, #1546, #1547, #1554, #1556, #1579, #1590  
**Status:** Closed-loop shipped (implementation landed in sister issues; this doc is the AC inventory + verification map).

## AC closed-loop map

| AC | Requirement | Surface | Shipped |
|----|-------------|---------|---------|
| 1 | Inventory `allocate_raw` / pool call-sites | This doc + `arena-quota-wiring.md` | **PASS** |
| 2 | `allocate_raw` quota gate via owner | `ASTArena::allocate_raw` → `quota_allow_fn_` | **#1546 / #1554** |
| 3 | `try_acquire` → typed `ResourceQuotaExceeded` | `MutationBoundaryGuard::try_acquire` | **#1547** |
| 4 | Legacy ctor deprecated; migrate 2 typed hosts | `[[deprecated]]` + `service.ixx` / rebind+set-body | **#1556** |
| 5 | Tests: exceed → typed + rejects + 1000-iter + legacy | `test_resource_quota_wired` + siblings | **#1594 / #1546 / #1547 / #1590** |
| 6 | Gate + metrics primitive | `query:resource-quota-stats` schema **1590** | **#1481 / #1590** |

## Inventory: `allocate_raw` family

| Site | Classification | Notes |
|------|----------------|-------|
| `ASTArena::allocate_raw` | **[quota-bound]** | Owner + `quota_allow_fn_` first; reject → `nullptr`, no `used` bump |
| `ASTArena::allocate_raw_impl` | **[non-quota]** | Post-gate body (small pool / pmr / safepoint) |
| `ASTArena::try_allocate` | **[quota-bound]** | Forwards to `allocate_raw` |
| `ASTArena::allocate_checked` | **[quota-bound]** | Typed `AuraResult`; single allow_fn (#1554) |
| `ASTArena::create<T>` | **[quota-bound]** | Via `allocate_raw`; `nullptr` on reject |
| `Evaluator::allocate_checked` | **[quota-bound]** | Delegates to arena when owner wired |
| `SmallObjectPool::try_allocate` | **[non-quota]** | Internal tier; outer path still gates at `allocate_raw` |
| Orphan `ASTArena` (no owner) | **[orphan-arena]** | Unlimited until `set_arena` / `set_temp_arena` |
| `set_temp_arena` / `ArenaGroup` children | **[quota-bound]** when default owner set | #1554 |
| JIT / AOT host buffers | **[non-quota]** | Separate allocators, not ASTArena |

Callers outside `arena.ixx` never call `allocate_raw` directly (private). Production entry is `create` / `try_allocate` / `allocate_checked`.

## Inventory: `MutationBoundaryGuard` production paths

| Site | Classification | Notes |
|------|----------------|-------|
| `MutationBoundaryGuard::try_acquire` | **[typed-quota]** | Preferred; `AuraResult` + bump on pass |
| Legacy ctor | **[legacy-soft-fail]** | `[[deprecated]]`; over quota → `is_inert()` + flag false (#1590) |
| `service.ixx` typed mutate / eval_with_mutation | **[typed-quota]** | Migrated #1547 / #1556 (AC4 hosts) |
| `mutate:rebind` / `mutate:set-body` | **[typed-quota]** | #1556 |
| `AURA_MUTATION_BOUNDARY_PROTECT` | **[typed-quota]** | Macro uses `try_acquire` |
| Other `evaluator_primitives_mutate.cpp` guards | **[legacy-soft-fail]** | Soft-fail under budget; gradual migrate OK |
| `verify_tool.cpp`, `eda`, diagnostic | **[legacy-soft-fail]** | Tooling / lower-frequency paths |

## Contract (unified)

```
Arena (memory dimension)
  set_arena / set_temp_arena → set_arena_owner(ev, allow_fn)
  allocate_raw(size)
    if owner && !allow_fn(owner, size) → nullptr   // no used bump
    else → allocate_raw_impl
  allocate_checked → ResourceQuotaExceeded (typed)

Guard (mutations dimension)
  try_acquire(ev, pending)
    check_mutation_quota → ResourceQuotaExceeded | unique_ptr
  legacy ctor
    same check; reject → inert guard + success_flag=false  // no throw

Observability
  resource_quota_checks_total / resource_quota_rejects_total
  (engine:metrics "query:resource-quota-stats")  schema 1590
```

## Tests

| File | Role |
|------|------|
| `tests/test_resource_quota_wired.cpp` | **#1594** AC consolidation (arena + Guard + 1000-iter + metrics) |
| `tests/test_arena_quota_wired.cpp` | #1546 / #1554 allocate_raw family |
| `tests/test_mutation_guard_typed_error.cpp` | #1547 try_acquire + stress + legacy |
| `tests/test_resource_quota_hotpath.cpp` | #1590 closed-loop + schema 1590 |
| `tests/test_resource_quota.cpp` / `*_module` / `*_edge` | Helpers, process quota, edges |
| `tests/test_issue_1487.cpp` / `test_issue_1498.cpp` | Prior closed-loops |

## Out of scope (issue body)

- AOT/LLVM re-emit quota (#1480 Phase 3)
- `walk_active_closures` quota-taint (linear ownership track)

## Related docs

- `docs/design/arena-quota-wiring.md`
- `docs/design/resource-quota-hotpath.md`
- `docs/contributing.md` (ResourceQuota section)
