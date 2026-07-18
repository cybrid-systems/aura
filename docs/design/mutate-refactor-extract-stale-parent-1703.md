# mutate:refactor/extract post-parse parent re-validate (#1703)

**Issue:** [#1703](https://github.com/cybrid-systems/aura/issues/1703)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685)–[#1702](https://github.com/cybrid-systems/aura/issues/1702),
especially [#1701](https://github.com/cybrid-systems/aura/issues/1701)  
**Files:** `evaluator_primitives_mutate.cpp`  
**Status:** P1 correctness — 8th capture-before-`parse_to_flat` instance
(P0 minimal extract path).

## Problem

`mutate:refactor/extract` captured `parent_of_target` then called
`parse_to_flat` for a stub define, then `set_child(parent_of_target, …)`.

## Fix

Already applied as a #1701 sibling harden; #1703 locks AC coverage:

```cpp
// parent_of + parent_child_index_if_attached (not O(N×C))
const auto size_before_parse = flat.size();
auto pr = parse_to_flat(...);
// is_live_node(node) + parent_slot_ok; else re-derive
set_child(parent_of_target, child_idx, pr.root);
```

No lockless batch path for this primitive.

## Tests

`tests/test_mutate_refactor_extract_stale_parent_1703.cpp`
