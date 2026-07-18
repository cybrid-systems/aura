# mutate:extract-function post-append parent re-validate (#1701)

**Issue:** [#1701](https://github.com/cybrid-systems/aura/issues/1701)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685)–[#1700](https://github.com/cybrid-systems/aura/issues/1700)  
**Files:** `evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — multi-`add_*` capture-before-use (worst of
the #1685 family; not a single `parse_to_flat`).

## Problem

`mutate:extract-function` captures `parent_of_target` / child index,
then performs 5+N `flat.add_*` (lambda, define, vars, call), then:

1. `set_child(parent_of_target, child_idx, call_id)`
2. `insert_child(ws_root, 0, define_id)` / `mark_dirty_upward(ws_root)`

Each `add_*` can stress SoA capacity / free-list topology. Pure vector
growth keeps NodeId indices stable; free-list recycle and bulk restore
still require post-append live + edge checks.

`mutate:refactor/extract` (sibling path with `parse_to_flat`) gets the
same re-derive pattern.

## Fix

```cpp
const auto size_before_appends = flat.size();
// … add_lambda / add_define / add_variable* / add_call …

// re-validate node + parent_slot_ok; else re-derive via parent_of
set_child(parent_of_target, child_idx, call_id);

auto ws_root = flat.root;
require is_live_node(ws_root);
insert_child(ws_root, 0, define_id);
```

No lockless batch path for this primitive.

## Tests

`tests/test_mutate_extract_stale_parent_1701.cpp`
