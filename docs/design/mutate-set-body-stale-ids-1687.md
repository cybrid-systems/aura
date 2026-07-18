# mutate:set-body double-stale NodeId after parse (#1687)

**Issue:** [#1687](https://github.com/cybrid-systems/aura/issues/1687)  
**Sibling:** [#1685](https://github.com/cybrid-systems/aura/issues/1685) (rebind single-id)  
**Files:** `ast.ixx`, `evaluator_primitives_mutate.cpp`, `evaluator_eval_flat.cpp`  
**Status:** P1 correctness — set-body captures Define + Lambda before parse.

## Problem

`mutate:set-body` captures `id` (Define) and `lambda_id` (Lambda child) then
calls `parse_to_flat` into the same FlatAST. Using either NodeId after append
without re-validation is a **double-stale** risk (worse than rebind’s single
`old_define`). The same pattern existed on the lockless batch helper
`eval_flat_apply_mutate_set_body`.

## Fix

Shared FlatAST helpers:

```cpp
NodeId resolve_define_after_parse(SymId sym, NodeId preferred, size_t size_before);
NodeId resolve_lambda_child_of_define(NodeId define_id);
```

Wired on:

- public `mutate:set-body` (Define + Lambda re-resolve)
- lockless batch `set-body` / `rebind` / `insert-child` / `replace-subtree`
- public `replace-pattern` + `query-and-replace` parent/slot re-validate after parse

## Tests

`tests/test_mutate_set_body_stale_ids_1687.cpp`
