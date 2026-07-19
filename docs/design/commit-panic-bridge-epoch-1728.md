# Bump bridge_epoch on commit_panic_checkpoint (#1728)

**Issue:** [#1728](https://github.com/cybrid-systems/aura/issues/1728)  
**Sibling:** [#1510](https://github.com/cybrid-systems/aura/issues/1510), [#1485](https://github.com/cybrid-systems/aura/issues/1485)  
**Files:** `evaluator.ixx`  
**Status:** P1 — commit cleared checkpoint but left bridge_epoch stale.

## Problem

After `commit_panic_checkpoint()`, closures could still pass
`is_bridge_stale` / `aura_closure_call` freshness checks because
`current_bridge_epoch()` was not advanced, so cross-COW reads
could see pre-commit provenance.

## Fix

Call the same `bridge_epoch_bump_fn_` service hook used by
`compact_env_frames` (#1510) after clearing checkpoint fields.
Wired by `CompilerService` to `bump_bridge_epoch()` (mutation_epoch_
+ AOT `aura_set_current_bridge_epoch` lockstep).

`clear_panic_checkpoint()` (discriminator skip / #1727) does **not**
bump — only successful commit does.

## Tests

`tests/test_commit_panic_bridge_epoch_1728.cpp`
