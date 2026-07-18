# mutate:set-body exception safety (#1686)

**Issue:** [#1686](https://github.com/cybrid-systems/aura/issues/1686)  
**Sibling:** [#1684](https://github.com/cybrid-systems/aura/issues/1684) (`run_or_rollback` helper)  
**Files:** `evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — set-body / siblings must not commit on throw.

## Problem

Same contract as #1684: `MutationBoundaryGuard` commits when `ok=true` at dtor.
`mutate:set-body` called post-mutate typecheck + ownership without catch; a throw
left `ok` true and committed a half-validated body.

## Fix

- **set-body** (landed with #1684, cited here): typecheck + ownership via
  `guard->run_or_rollback`
- **Sibling migration (#1686):**
  - `mutate:remove-node` — `apply_mutation` under `run_or_rollback`
  - `mutate:insert-child` — apply + log under `run_or_rollback`
  - `mutate:replace-subtree` — set_child / dirty / log under `run_or_rollback`
  - `mutate:atomic-batch` — each lockless sub-op dispatch under outer Guard
    `run_or_rollback` (throw → batch rollback + `batch-threw`)

## Tests

`tests/test_mutate_set_body_exception_safety_1686.cpp`
