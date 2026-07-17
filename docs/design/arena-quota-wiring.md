# Arena allocate_raw ↔ Resource Quota Wiring

**Issue:** #1546 (parent #1481)  
**Related:** `Evaluator::check_arena_quota` / `allocate_checked`, `query:resource-quota-stats`

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
