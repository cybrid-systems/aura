# mutate:replace-pattern post-parse parent re-validate (#1694)

**Issue:** [#1694](https://github.com/cybrid-systems/aura/issues/1694)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685), [#1687](https://github.com/cybrid-systems/aura/issues/1687), [#1690](https://github.com/cybrid-systems/aura/issues/1690)  
**Files:** `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — 4th capture-before-`parse_to_flat` instance (inside match loop).

## Problem

Each match iteration captures `parent_id` then calls `parse_to_flat` into the
same FlatAST before `set_child`. Multiple iterations amplify topology stress.

## Fix

**Public `mutate:replace-pattern`:**
- Snapshot `size_before_parse` per match
- After parse: require live parent in pre-parse range + child slot still holds `match_id`
- On failure: re-derive edge from `StableNodeRef` / `parent_child_index_if_attached`

**Lockless batch:**
- Resolve parent only among pre-parse nodes (`parent_of` + limited scan)
- `is_live_node` guards on match and parent

## Tests

`tests/test_mutate_replace_pattern_stale_parent_1694.cpp`
