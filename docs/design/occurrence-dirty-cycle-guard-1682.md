# auto_wire_k_occurrence_dirty_for_subtree cycle guard (#1682)

**Issue:** [#1682](https://github.com/cybrid-systems/aura/issues/1682)  
**File:** `src/compiler/evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — same bug class as #1679.

## Problem

Iterative DFS pushed children without a visited set. Cyclic FlatAST
children links (EDSL recovery / self-mutate) made the stack grow forever.

## Fix

Dense `seen[flat.size()]` bitvector (parity with #1679 reflect validate):

- Skip already-seen children  
- Live-node + bounds check  
- Defensive `visited > flat.size()` abort  

Exported as `aura::compiler::auto_wire_k_occurrence_dirty_for_subtree` for tests.

## Tests

`tests/test_occurrence_dirty_cycle_guard_1682.cpp`
