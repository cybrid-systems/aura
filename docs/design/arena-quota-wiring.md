# Arena allocate_raw ↔ Resource Quota Wiring

**Issues:** #1498 (production parent), #1487, #1546, **#1554**, #1481, #1547, #1548, **#1594** (refine inventory)  
**Related:** `Evaluator::check_arena_quota` / `allocate_checked`, `try_acquire`, `query:resource-quota-stats`  
**Refine map:** `docs/design/resource-quota-wired-1594.md`

## #1498 production closed-loop

| AC | Surface | Shipped |
|----|---------|---------|
| 1 allocate + Guard | `allocate_raw` / `allocate_checked` + `try_acquire` typed `ResourceQuotaExceeded` | #1546/#1547/#1498 |
| 2 stats fields | `current_usage`, `memory_quota`, `memory_quota_total`, `exceeded_count`, schema **1498** | #1498 |
| 3 cumulative total | `set_resource_quota_memory_total` → `used + request > total` | #1498 |
| 4 exhaustion tests | `tests/test_issue_1498.cpp` + existing suite | #1498 |
| 5 registry primitives | eval under unlimited quota; stats via `register_stats_impl` | #1498 |

**Dual memory limits (both 0 = unlimited):**

| API | Semantics |
|-----|-----------|
| `set_resource_quota_memory(N)` | Per-request: reject if `request > N` |
| `set_resource_quota_memory_total(N)` | Cumulative: reject if `arena.used + request > N` |

## #1487 closed-loop map

| AC | Surface | Shipped |
|----|---------|---------|
| 1 `allocate_raw` quota | Owner callback → `check_arena_quota`; reject → `nullptr` (no used bump). Typed: `allocate_checked` → `AuraError{ResourceQuotaExceeded}` | #1546 |
| 2 MutationBoundary | `try_acquire` → `check_mutation_quota`; reject → typed error. Flush samples `resource_quota_checks_total` (no mid-boundary abort) | #1547, #1487 |
| 3 Primitive + metrics | `query:resource-quota-stats` (schema 1481), `resource_quota_checks/rejects_total` | #1481, #1548 |
| 4 Exhaustion tests | `test_resource_quota` + `test_arena_quota_wired` + `test_mutation_guard_typed_error` + `test_issue_1487` | #1548, #1487 |
| 5 Integration with #1481 | Helpers + counters + wiring end-to-end | all above |

## Inventory: `allocate_raw` family

| Site | Classification | Notes |
|------|----------------|-------|
| `ASTArena::allocate_raw` (definition) | **[quota-bound]** | Owner callback consulted first (#1546) |
| `ASTArena::allocate_raw_impl` | **[non-quota]** | Post-gate body; used by typed `allocate_checked` (#1554) |
| `ASTArena::try_allocate` | **[quota-bound]** | Forwards to `allocate_raw` |
| `ASTArena::allocate_checked` | **[quota-bound]** | Typed `AuraResult`; single allow_fn call (#1554) |
| `ASTArena::create<T>` | **[quota-bound]** | Via `allocate_raw`; returns `nullptr` on reject |
| `SmallObjectPool::try_allocate` | **[non-quota]** | Internal tier; main path still gates at `allocate_raw` entry |
| Orphan `ASTArena` (no `set_arena`) | **[unbound-alloc]** | `arena_owner_ == nullptr` → unlimited |
| Pre-`set_arena` service bootstrap | **[unbound-alloc]** | No owner until Evaluator binds |
| Hot-path create after owner set | **[quota-bound]** | Same as create |
| `set_temp_arena` | **[quota-bound]** | Same owner callback as primary (#1554) |
| JIT / AOT prewarm buffers | **[unbound-alloc]** | Separate allocators, not ASTArena |
| Module `ArenaGroup` child arenas | **[quota-bound]** when group default owner set | `set_default_arena_owner` on `set_arena` (#1554) |

Callers outside `arena.ixx` never invoke `allocate_raw` directly (it is **private**). Production paths enter via `create` / `try_allocate` / `ASTArena::allocate_checked` / `Evaluator::allocate_checked`.

## Contract

```
set_arena(ASTArena*)
  └─ set_arena_owner(this, allow_fn)
  └─ arena_group_->set_default_arena_owner(this, allow_fn)   // #1554

set_temp_arena(ASTArena*)
  └─ set_arena_owner(this, allow_fn)                         // #1554

allocate_raw(size, align)
  if owner && !allow_fn(owner, size) → return nullptr   // no stats.used bump
  else → allocate_raw_impl (small-pool / pmr)

ASTArena::allocate_checked(size, align)   // typed surface #1554
  allow_fn once → ResourceQuotaExceeded
  allocate_raw_impl → ArenaOutOfMemory only on true OOM

Evaluator::allocate_checked
  delegates to ASTArena::allocate_checked when owner wired (no double-count)
```

- `resource_quota_memory_ == 0` → unlimited (default) per-request.
- `resource_quota_memory_total_ != 0` → cumulative `used + request` (#1498).
- `query:resource-quota-stats` schema **1554**: `current_usage`, `exceeded_total`/`exceeded_count`, `primary_arena_wired`, `temp_arena_wired`, `group_owner_wired`.

## Tests

| File | Role |
|------|------|
| `tests/test_resource_quota.cpp` | Helper isolation (#1481) |
| `tests/test_arena_quota_wired.cpp` | Owner wiring + reject + temp/group/boundary (#1546/#1554) |
