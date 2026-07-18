# runtime_reflect_validate_ast_subtree cycle guard (#1679)

**Issue:** [#1679](https://github.com/cybrid-systems/aura/issues/1679)  
**File:** `src/compiler/evaluator_primitives_query.cpp`  
**Status:** P1 correctness — terminate on cyclic FlatAST children links.

## Problem

Iterative DFS pushed every child without a visited set. A cycle
(self-loop or A↔B) from EDSL recovery / self-mutate made the stack grow
unbounded → hang. Metrics never bumped (`reflection_schema_*` silent).

## Fix

Dense `seen[flat.size()]` bitvector:

- Skip already-seen children (cycles **and** diamond DAGs).  
- Live-node + bounds check before push.  
- Hard `visited > flat.size()` abort → `stale_prevented` (defensive).

Non-cyclic trees/DAGs: same result as before (markers counted once on DAG).

## Call sites

- `(reflect:validate-macro-body id)`  
- `(reflect:validate-edsl id)`

## Tests

`tests/test_reflect_validate_cycle_guard_1679.cpp`
