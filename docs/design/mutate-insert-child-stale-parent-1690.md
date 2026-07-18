# mutate:insert-child post-parse parent re-validate (#1690)

**Issue:** [#1690](https://github.com/cybrid-systems/aura/issues/1690)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685) (rebind), [#1687](https://github.com/cybrid-systems/aura/issues/1687) (set-body)  
**Files:** `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — 3rd capture-before-`parse_to_flat` instance.

## Problem

`mutate:insert-child` captures `parent` then parses into the same FlatAST.
Without re-validation, free-list recycle / bulk topology restore could
redirect `InsertChildMutator` to the wrong node (and log the wrong parent).

NodeId indices are stable under pure SoA growth; the guard is still required
for free-list / restore safety and documents the post-parse contract.

## Fix

```cpp
const auto size_before_parse = flat.size();
// pre-check: parent live in [0, size_before)
auto pr = parse_to_flat(...);
// post-check: parent still live && parent < size_before_parse
```

Uses `is_live_node` (not generation-tagged `is_valid`). Wired on:

- public `mutate:insert-child` + lockless batch variant
- public `mutate:splice` + lockless batch (each parse iteration)

## Tests

`tests/test_mutate_insert_child_stale_parent_1690.cpp`
