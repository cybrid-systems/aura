# maybe_sv_hardware_closedloop workspace lock window (#1683)

**Issue:** [#1683](https://github.com/cybrid-systems/aura/issues/1683)  
**File:** `src/compiler/evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — pin `workspace_flat_` + `workspace_pool_` across closed-loop calls.

## Problem

`maybe_sv_hardware_closedloop` read `workspace_flat()` once, then made multiple
hardware/SV-IR calls and re-read `workspace_pool()` later without a lock. A
concurrent COW swap could pair old flat with new pool (or free the old flat).

## Fix

1. If this fiber does **not** already hold `MutationBoundaryGuard` unique lock,
   take `Evaluator::WorkspaceSharedLock` for the whole body.  
2. If outer unique is already held (`mutation_boundary_held()` or depth slot > 0),
   **skip** shared acquire — `std::shared_mutex` is not recursive.  
3. Re-read `ws` / `pool` only while the lock (or outer unique) is held.

## Tests

`tests/test_sv_closedloop_workspace_lock_1683.cpp`
