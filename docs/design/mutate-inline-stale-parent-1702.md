# mutate:inline-call post-clone parent re-validate (#1702)

**Issue:** [#1702](https://github.com/cybrid-systems/aura/issues/1702)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685)–[#1701](https://github.com/cybrid-systems/aura/issues/1701)  
**Files:** `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — DFS multi-`add_*` capture-before-use
(10th instance in the #1685 family).

## Problem

`mutate:inline-call` captures `call_parent` / child index, then DFS-clones
the callee body with many `flat.add_*`, then:

```cpp
flat.set_child(call_parent, call_idx, cloned_body);
mark_dirty_upward(call_parent);
```

Each clone append can stress SoA capacity / free-list topology.

## Fix

```cpp
const auto size_before_clone = flat.size();
// … DFS clone add_* …

// re-validate call_id + parent_slot_ok; else re-derive via parent_of
set_child(call_parent, call_idx, cloned_body);
```

Public + lockless. `is_live_node` only (prim restamps gens at the end).

## Tests

`tests/test_mutate_inline_stale_parent_1702.cpp`
