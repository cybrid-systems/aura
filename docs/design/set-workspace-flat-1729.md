# Exception-safe set_workspace_flat under workspace_mtx_ (#1729)

**Issue:** [#1729](https://github.com/cybrid-systems/aura/issues/1729)  
**Sibling:** [#1663](https://github.com/cybrid-systems/aura/issues/1663) (set_arena serialize)  
**Files:** `evaluator.ixx`  
**Status:** P1 — raw pointer swap + throwing index rebuild.

## Problem

`set_workspace_flat` assigned `workspace_flat_` then rebuilt the
tag-arity index under EagerAfterCow. Concurrent readers could see
the new pointer mid-update, and `build_tag_arity_index` throw left
new flat + empty index.

## Fix

1. Hold `unique_lock` on `workspace_mtx_` for the whole swap.
2. On rebuild throw: restore prior `workspace_flat_`, re-invalidate
   index, rethrow.

## Tests

`tests/test_set_workspace_flat_1729.cpp`
