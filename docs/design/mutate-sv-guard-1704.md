# mutate:sv-* MutationBoundaryGuard (#1704)

**Issue:** [#1704](https://github.com/cybrid-systems/aura/issues/1704)  
**Siblings:** [#469](https://github.com/cybrid-systems/aura/issues/469),
[#1683](https://github.com/cybrid-systems/aura/issues/1683),
[#694](https://github.com/cybrid-systems/aura/issues/694)  
**Files:** `evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — naked SV mutates without Guard / live check.

## Problem

`mutate:sv-add-coverpoint` and `mutate:sv-weaken-property` were P0
stubs that:

1. Had no `MutationBoundaryGuard` (no workspace unique lock / rollback)
2. Used raw `workspace_flat()` without boundary
3. Did not require `is_live_node` on the target id

## Fix

```cpp
bool ok = true;
MutationBoundaryGuard guard(ev, &ok);
// read-only / arg / live-node checks with ok=false
// make_ref for provenance; apply under run_or_rollback
```

`maybe_sv_hardware_closedloop` already skips nested shared lock when
`mutation_boundary_held()` (#1683).

API still returns `#t` / `#f` for P0 callers.

## Tests

`tests/test_mutate_sv_guard_1704.cpp`
