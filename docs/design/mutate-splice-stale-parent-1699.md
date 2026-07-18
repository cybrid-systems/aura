# mutate:splice post-parse parent re-validate (#1699)

**Issue:** [#1699](https://github.com/cybrid-systems/aura/issues/1699)  
**Siblings:** [#1685](https://github.com/cybrid-systems/aura/issues/1685),
[#1690](https://github.com/cybrid-systems/aura/issues/1690),
[#1694](https://github.com/cybrid-systems/aura/issues/1694),
[#1697](https://github.com/cybrid-systems/aura/issues/1697)  
**Files:** `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — 6th capture-before-`parse_to_flat` instance
(multi-arg **loop**; #1698 already closed via #1900).

## Problem

`mutate:splice` captures `parent` once, then for each code string calls
`parse_to_flat` and uses `parent` for `insert_child` / `add_mutation` /
`mark_dirty_upward`. Each loop iteration can stress topology; after the
first append, later iterations must not apply to a free-list-recycled
or dead parent.

#1690 wired fail-hard `is_live_node` + pre-parse size bounds. #1699
locks the contract with a gen-tagged `StableNodeRef` and a dedicated
multi-arg regression.

## Fix

```cpp
for (each code string) {
  const auto size_before_parse = flat.size();
  auto pr = parse_to_flat(...);  // restamps all node_gen_ (#273)
  if (parent >= size_before_parse || !flat.is_live_node(parent))
    return stale-ref;  // no re-derive: parent is user-supplied insert target
  insert_child / add_mutation / mark_dirty_upward(parent);
}
```

**Pitfall:** do **not** use `StableNodeRef::is_valid_in` after
`parse_to_flat` — success path calls `restamp_all_node_generations()`,
so pre-parse refs false-fail. Use `is_live_node` + pre-parse size.

Unlike replace-subtree (#1697), splice parent is not re-derivable from a
child edge — fail-hard is correct.

Public + lockless batch paths both updated. Also hardened #1697
re-derive to drop `is_valid_in` for the same restamp reason.

## Note on #1698

`mutate:atomic-batch` weak atomicity / 5-of-14 dispatch was closed by
[#1900](https://github.com/cybrid-systems/aura/issues/1900) (dispatch
expanded to 14 lockless helpers under outer Guard). No further work.

## Tests

`tests/test_mutate_splice_stale_parent_1699.cpp`
