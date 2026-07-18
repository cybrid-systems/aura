# mutate:replace-subtree post-parse parent re-validate (#1697)

**Issue:** [#1697](https://github.com/cybrid-systems/aura/issues/1697)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685),
[#1687](https://github.com/cybrid-systems/aura/issues/1687),
[#1690](https://github.com/cybrid-systems/aura/issues/1690),
[#1694](https://github.com/cybrid-systems/aura/issues/1694)  
**Files:** `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — 5th capture-before-`parse_to_flat` instance.

## Problem

`mutate:replace-subtree` captures `parent_id` / `child_idx` from
`flat.parent_of(target)`, then `parse_to_flat` appends into the same
FlatAST, then uses those ids for:

1. `set_child(parent_id, child_idx, pr.root)`
2. `mark_dirty_upward(parent_id)`
3. `add_mutation_subtree(..., parent_id, child_idx, ...)` (rollback path)

#1685 added fail-hard checks. Pure SoA growth keeps NodeId indices
stable; free-list recycle / bulk topology restore can still break the
parent↔target edge. Fail-hard alone is insufficient when the edge can
be re-derived from a still-live target.

## Fix

Same re-derive pattern as #1694:

```cpp
const auto size_before_parse = flat.size();
const auto target_ref = flat.make_ref(target);
auto pr = parse_to_flat(...);

auto parent_slot_ok = [&] {
  return live pre-parse parent && children[child_idx] == target;
};
if (!parent_slot_ok()) {
  // re-derive from target StableNodeRef + parent_child_index_if_attached
  // else stale-ref error
}
// set_child / dirty / log with post-parse-valid parent_id, child_idx
```

Wired on public `mutate:replace-subtree` and lockless batch variant.
Uses `is_live_node` (not generation-tagged `is_valid` alone for multi-edge
safety; target also checked via `StableNodeRef::is_valid_in`).

## Tests

`tests/test_mutate_replace_subtree_stale_parent_1697.cpp`
