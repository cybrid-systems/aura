# WorkspaceSwapGuard for synthesize:optimize (#1717)

**Issue:** [#1717](https://github.com/cybrid-systems/aura/issues/1717)  
**Files:** `evaluator_primitives_agent.cpp`  
**Status:** P1 UAF — bare child workspace swap without exception safety.

## Problem

Manual `create_child` → assign flat/pool → work → restore → `delete_child`
could leak the child or leave the evaluator on the child workspace if
any path threw or early-returned.

## Fix (Option A)

RAII `WorkspaceSwapGuard`:

- ctor: `create_child` + `ensure_local_flat` + swap via public setters
- dtor/`release()`: restore saved flat/pool + `delete_child`

Used for both `"xover"` and `"evolve-variant"` paths in
`synthesize:optimize`.

## Tests

`tests/test_workspace_swap_guard_1717.cpp`
