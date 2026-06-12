# Incremental Type Cache Safety After Typed Mutations (Issue #168)

**Status:** Design document + Phase 1 implementation
**Date:** 2026-06-12
**Phase:** 1 of 4

## Problem

`InferenceEngine::infer_flat` (`src/compiler/type_checker_impl.cpp`)
has a cache that can return stale results after complex typed
mutations:

```cpp
if (!flat.is_dirty(id)) {
    auto cached = flat.type_id(id);
    if (cached > 0 && cached < reg_.size()) {
        auto tid = TypeId{cached, 1};
        if (reg_.free_vars(tid).empty()) {   // Issue #72
            ++stats_.cache_hits;
            return tid;
        }
        ++stats_.stale_cache;
    }
    ...
}
```

The cache assumes the caller (mutation paths) correctly sets
`is_dirty(id) = true` for any node that became invalid. But
the issue body documents 3 classes of mutation that don't
reliably propagate dirty:

1. **Let-Polymorphism**: pre-solve TYPE_VAR cached types + later
   mutation may not invalidate correctly.
2. **Occurrence Typing**: narrowing after structural changes may
   not be re-evaluated.
3. **ADT match exhaustiveness**: depends on `__match_tmp` +
   `get_match_info`; structural mutations can break this.

## Mitigations

### Mitigation #1 (Phase 1, this commit) — Global epoch gate on infer_flat

- Add `std::uint64_t cache_epoch_` to `TypeChecker` (cached
  snapshot of `mutation_epoch_` at last inference).
- `CompilerService` sets `tc.set_cache_epoch(mutation_epoch_)`
  before every `infer_flat` call (mirrors how #166's
  `mutation_epoch_` is bumped).
- `infer_flat` checks `if (cache_epoch_ != last_inference_epoch_)
  invalidate_everything()`. Single per-call check, catches all
  mutations since the last inference.
- Coarse: invalidates the whole cache, not per-node. Trades
  precision for safety.

### Mitigation #2 (deferred) — Per-node epoch + free_vars recursive check

- Add `last_seen_epoch_` per node (mirrors `IRCacheEntry`
  pattern from #166). More precise than global epoch.
- Recursive `free_vars()` check: a type with nested free vars
  in its substructure is also stale (not just top-level
  TYPE_VAR). Slower per check but more accurate.

### Mitigation #3 (deferred) — get_match_info robustness + match exhaustiveness

- Audit all match call sites. Make `get_match_info` always
  re-evaluate exhaustiveness on the post-mutation match arms.
- Test: 50+ mutations in a row on the same match form, verify
  exhaustiveness diagnostic doesn't get lost.

### Mitigation #4 (deferred) — Comprehensive regression tests

- Test_issue_168 covers:
  - Let-poly + mutate:rebind → cache invalidates, new type correct
  - match + mutate:replace-children → exhaustiveness re-evaluated
  - Occurrence typing + structural mutation → narrowing correct
  - High-frequency mutation loop (50+ iters) — cache stats
    should show stale_cache counter goes up appropriately

## Phase 1 tradeoffs

- **Coarse invalidation**: whole-cache on any epoch bump. More
  re-computation than strictly necessary, but provably correct.
- **CompilerService coupling**: `set_cache_epoch` is a new
  public method on TypeChecker. Tightens the binding between
  the service and the type checker, but that's the nature of
  the bug — the cache assumes the caller is well-behaved.
- **No new types / fields**: just `cache_epoch_` + `last_inference_epoch_`
  on TypeChecker. The actual cached types are in `FlatAST::type_id(id)`
  as before.

## What this fix prevents

**Before**: a complex mutation that doesn't set `is_dirty` on the
right node → `infer_flat` returns the OLD cached type, downstream
type checks use the wrong type, type errors get lost.

**After**: any mutation bumps `mutation_epoch_` (already done
by #166's `invalidate_function`). `infer_flat` checks the epoch
FIRST, treats all cached types as stale if the epoch changed.
Re-computes everything. Slower but provably correct.

## Commits

- TBD: this design doc + `cache_epoch_` + `set_cache_epoch` API + tests

## Phase 2-4 (deferred to fresh session)

- Mitigation #2: per-node epoch + recursive free_vars
- Mitigation #3: match exhaustiveness robustness
- Mitigation #4: comprehensive regression tests
