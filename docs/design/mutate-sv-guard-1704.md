# mutate:sv-* MutationBoundaryGuard (#1704 / #1705)

**Issues:** [#1704](https://github.com/cybrid-systems/aura/issues/1704),
[#1705](https://github.com/cybrid-systems/aura/issues/1705)  
**Siblings:** [#469](https://github.com/cybrid-systems/aura/issues/469),
[#1683](https://github.com/cybrid-systems/aura/issues/1683),
[#694](https://github.com/cybrid-systems/aura/issues/694)  
**Files:** `evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — naked SV mutates without Guard / live check.

## Problem

`mutate:sv-add-coverpoint` (#1704) and `mutate:sv-weaken-property`
(#1705) were P0 stubs that:

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

Both primitives shipped together in `307d7117`; #1705 locks the
weaken-property sibling AC (same template).

`maybe_sv_hardware_closedloop` already skips nested shared lock when
`mutation_boundary_held()` (#1683).

API still returns `#t` / `#f` for P0 callers.

## Tests

`tests/test_mutate_sv_guard_1704.cpp` (covers both prims; AC2 is weaken)
