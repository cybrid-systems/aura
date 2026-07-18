# Multi-node mutation log uses NULL_NODE (#1696)

**Issue:** [#1696](https://github.com/cybrid-systems/aura/issues/1696)  
**Siblings:** [#1694](https://github.com/cybrid-systems/aura/issues/1694),
[#1695](https://github.com/cybrid-systems/aura/issues/1695),
[#1275](https://github.com/cybrid-systems/aura/issues/1275)  
**Files:** `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P2 contract — multi-node mutators must not attribute log entries to NodeId 0.

## Problem

`mutate:replace-pattern` and `mutate:rename-symbol` are multi-node
operations (N ≥ 0 sites). Both used:

```cpp
flat.add_mutation(0, "replace-pattern", ...);
flat.add_mutation(0, "rename-symbol", ...);
```

`0` is a **real** `NodeId` (first SoA slot). `NULL_NODE` is `~0u`
(`mutation.ixx`), not zero. Logging under `0` made
`(query:mutation-log :node-id 0)` incorrectly attribute multi-node
ops to the first node and muddied audit / rollback semantics.

## Fix (Option A)

```cpp
// public (using namespace aura::ast)
flat.add_mutation(NULL_NODE, "replace-pattern", ...);
flat.add_mutation(NULL_NODE, "rename-symbol", ...);

// lockless (fully qualified)
flat.add_mutation(aura::ast::NULL_NODE, "replace-pattern", ...);
flat.add_mutation(aura::ast::NULL_NODE, "rename-symbol", ...);
```

`add_mutation_with_rollback` previously required `node < tag_.size()`,
which rejects `NULL_NODE` (`~0u`) and would abort under Contracts.
Updated to:

```cpp
pre(node == NULL_NODE || node < tag_.size())
// skip mark_dirty_upward / node_first_mutation_ when node == NULL_NODE
```

Four call sites only (grep audit). Other mutators already target a
specific parent/node. Option B (per-match log) and Option C
(`add_multi_mutation` API) deferred as follow-ups.

## Tests

`tests/test_mutate_multi_node_log_null_1696.cpp`
