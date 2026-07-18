# mutate:remove-node removes ALL parents (#1688)

**Issue:** [#1688](https://github.com/cybrid-systems/aura/issues/1688)  
**Files:** `mutators.ixx`, `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — FlatAST is a DAG; first-parent-only was wrong.

## Problem

`mutate:remove-node` scanned for the first parent edge and returned `#t`
after a single `RemoveChildMutator` apply. Shared children (multi-parent
DAG) left residual edges, incomplete mutation logs, and misleading success.

## Fix (Option A)

```cpp
// mutators.ixx
collect_incoming_child_edges(flat, target); // higher child_index first per parent
remove_node_from_all_parents(flat, target, on_edge);
```

- Public + lockless batch paths share the helper
- Each edge gets a structural mutation log entry
- Same-parent multi-slot edges remove high→low index to avoid shift bugs
- Structural mutators use `is_live_node` (not `is_valid`): each
  `remove_child` bumps `generation_`, so `is_valid` would reject still-live
  parents on the 2nd+ edge

## Semantics

Success means **zero remaining incoming child edges**. The node slot may
remain in the flat; use a future `mutate:remove-edge` for single-edge
removal if needed.

## Tests

`tests/test_mutate_remove_node_all_parents_1688.cpp`
