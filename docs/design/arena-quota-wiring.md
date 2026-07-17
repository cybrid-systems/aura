# Arena allocate_raw ↔ Resource Quota Wiring

**Issues:** #1498 (production parent), #1487, #1546, #1481, #1547, #1548  
**Related:** `Evaluator::check_arena_quota` / `allocate_checked`, `try_acquire`, `query:resource-quota-stats`

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
| `ASTArena::try_allocate` | **[quota-bound]** | Forwards to `allocate_raw` |
| `ASTArena::create<T>` | **[quota-bound]** | Via `allocate_raw`; returns `nullptr` on reject |
| `SmallObjectPool::try_allocate` | **[non-quota]** | Internal tier; main path still gates at `allocate_raw` entry |
| Orphan `ASTArena` (no `set_arena`) | **[unbound-alloc]** | `arena_owner_ == nullptr` → unlimited |
| Pre-`set_arena` service bootstrap | **[unbound-alloc]** | No owner until Evaluator binds |
| Hot-path create after owner set | **[quota-bound]** | Same as create |
| JIT / AOT prewarm buffers | **[unbound-alloc]** | Separate allocators, not ASTArena |
| Module `ArenaGroup` child arenas | **[unbound-alloc]** until group/owner wired | Future: bind owner per module arena |

Callers outside `arena.ixx` never invoke `allocate_raw` directly (it is **private**). Production paths enter via `create` / `try_allocate` / `Evaluator::allocate_checked`.

## Contract

```
set_arena(ASTArena*)
  └─ set_arena_owner(this, allow_fn)
       allow_fn → !check_arena_quota(size).has_value()

allocate_raw(size, align)
  if owner && !allow_fn(owner, size) → return nullptr   // no stats.used bump
  else → small-pool / pmr allocate (unchanged)

allocate_checked(size, align)   // typed surface
  check_arena_quota → AuraError{ResourceQuotaExceeded} on reject
  try_allocate → nullptr only if OOM / other failure after quota pass
```

- `resource_quota_memory_ == 0` → unlimited (default).
- Limit is **per-request size** (not cumulative heap), matching #1481 helpers.

## Tests

| File | Role |
|------|------|
| `tests/test_resource_quota.cpp` | Helper isolation (#1481) |
| `tests/test_arena_quota_wired.cpp` | Owner wiring + reject path (#1546) |
