# Atomic arena owner vs concurrent allocate_raw (#1663)

**Issue:** [#1663](https://github.com/cybrid-systems/aura/issues/1663)  
**Builds on:** #1546 · #1554 · #1662  
**Status:** P0 concurrency fix — no torn owner/fn during set_arena.

## Problem

`set_arena_owner` wrote `arena_owner_` then `quota_allow_fn_` as two plain stores.
Concurrent `allocate_raw` could observe:

| Observation | Effect |
|-------------|--------|
| owner set, fn null | `if (fn && owner)` false → **quota bypass** |
| owner null, fn set | same skip |
| mid-clear | same |

## Fix (Option A)

### ASTArena (`owner_mtx_`)

| Op | Lock |
|----|------|
| `set_arena_owner` / `clear_arena_owner` | `unique_lock` |
| `has_arena_owner` / `arena_owner` / `owner_pair_consistent` | `shared_lock` |
| `allocate_raw` / `allocate_checked` | `shared_lock` **held across** `allow_fn` call |

Holding the lock across `allow_fn` prevents ~Evaluator from `clear`+destroy mid-callback
(unique_lock waits). `allow_fn` must not re-enter set/clear (would deadlock).

### Evaluator (`arena_set_mtx_`)

Serializes `set_arena` / `set_temp_arena` / dtor clear (rare path).

## Tests

| File | Role |
|------|------|
| `tests/test_set_arena_atomic_owner_1663.cpp` | **#1663** AC + stress |
| `tests/test_arena_owner_dtor_clear_1662.cpp` | dtor UAF lineage |

## Related

- #1662 — dtor must clear owner (UAF)
- #1546 — introduced arena_owner wire
