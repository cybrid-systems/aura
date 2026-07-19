# Bump bridge_epoch on truncate_env_frames_to_checkpoint (#1739)

**Issue:** [#1739](https://github.com/cybrid-systems/aura/issues/1739)  
**Sibling:** [#1728](https://github.com/cybrid-systems/aura/issues/1728) (`commit_panic_checkpoint`), [#1510](https://github.com/cybrid-systems/aura/issues/1510) (`compact_env_frames`)  
**Files:** `evaluator_env.cpp`, `evaluator.ixx`  
**Status:** P1 — truncate shrank env_frames_ but left bridge_epoch stale.

## Problem

After `truncate_env_frames_to_checkpoint()`, post-checkpoint
EnvIds become OOB. Closures captured against those EnvIds still
passed `is_bridge_stale` / `aura_closure_call` freshness checks
because `current_bridge_epoch()` was not advanced, so
`materialize_call_env` could walk a dangling or OOB env slot.

Same bug class as #1728 (commit cleared checkpoint without
bumping) and the dual-epoch contract of #1510/#1526.

## Fix

When frames are actually dropped, call the same
`bridge_epoch_bump_fn_` service hook used by `compact_env_frames`
and `commit_panic_checkpoint`. Wired by `CompilerService` to
`bump_bridge_epoch()` (mutation_epoch_ + AOT
`aura_set_current_bridge_epoch` lockstep).

No-op truncate (checkpoint size ≥ current size) does **not**
bump.

## Tests

`tests/test_truncate_env_bridge_epoch_1739.cpp`
