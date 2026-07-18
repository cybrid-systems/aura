# mutate:wrap post-parse parent re-validate (#1700)

**Issue:** [#1700](https://github.com/cybrid-systems/aura/issues/1700)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685)–[#1699](https://github.com/cybrid-systems/aura/issues/1699),
[#1689](https://github.com/cybrid-systems/aura/issues/1689)  
**Files:** `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — 7th capture-before-`parse_to_flat` instance
(two parent edges: `parent_of_target` + `sentinel_parent`).

## Problem

`mutate:wrap` captures `parent_of_target` (and previously used O(N×C)
scans) before `parse_to_flat`, then:

1. `set_child(sentinel_parent, …, node)` — attach target into wrapper
2. `set_child(parent_of_target, …, pr.root)` — install wrapper in place
3. `add_mutation(node, "wrap", …)`

`parent_of_target` / `node` predate parse; free-list / topology stress
can break the edge. `sentinel_parent` is found post-parse (safe) but
should use `parent_of` not O(N×C).

## Fix

```cpp
// pre-parse: parent_of + parent_child_index_if_attached
const auto size_before_parse = flat.size();
auto pr = parse_to_flat(...);  // restamps gens (#273)

// post-parse: is_live_node(node) + parent_slot_ok; else re-derive
// sentinel: scan only [size_before_parse, size) + parent_of
set_child(sentinel_parent, …, node);
set_child(parent_of_target, …, pr.root);
```

Do **not** use `StableNodeRef::is_valid_in` after parse (#1699 pitfall).

Public + lockless paths both updated.

## Tests

`tests/test_mutate_wrap_stale_parent_1700.cpp`
