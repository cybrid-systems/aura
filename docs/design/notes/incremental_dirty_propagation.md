# Incremental Dirty Propagation Fix (Issue #28)

**Status**: Design
**Design Author**: Ani

## Problem

Issue #28: `FlatAST` has `dirty_` flags and `mark_dirty_upward()` for incremental type checking, but mutation paths in the evaluator call dirty propagation inconsistently. Result: stale cached types are silently reused, causing incorrect type checking after self-modification.

### Root Cause: Incomplete dirty marking in mutation primitives

Only two mutation paths (`mutate:rebind`, `mutate:set-body`) properly call `flat.mark_dirty_upward()` via the DefUse index. All other paths either:

| Mutation | Dirty marking | Status |
|----------|--------------|--------|
| `mutate:rebind` | ✅ `mark_dirty_upward(dep_callers)` | Correct |
| `mutate:set-body` | ✅ `mark_dirty_upward(dep_callers)` | Correct |
| `mutate:replace-type` | ❌ None | Bug |
| `mutate:replace-value` | ❌ None | Bug |
| `mutate:remove-node` | ❌ None | Bug |
| `mutate:insert-child` | ❌ None | Bug |
| `mutate:tweak-literal` | ❌ None | Bug |
| `mutate:replace-pattern` | ❌ None | Bug |
| `extract-function` | ❌ Only `flat.mark_dirty(define_id)` (no upward) | Bug |
| `inline-call` | ❌ None | Bug |

Even `extract-function`, which creates a full new definition, only marks the define node itself dirty — never its ancestors in the workspace root.

### How the cache works

In `synthesize_flat()` (type_checker_impl.cpp):

```cpp
if (!flat.is_dirty(id)) {
    auto cached = flat.type_id(id);
    if (cached > 0 && reg_.tag_of(cached) != TYPE_VAR)
        return cached;  // ← clean → return stale cached type!
}
flat.set_type(id, result.index);
flat.clear_dirty(id);
```

When `mark_dirty_upward` is skipped, the node is "clean" → the cached type is returned → changes invisible to the type system.

### Impact on Agent OS

- `typecheck-current` returns incorrect results after mutations (silent type errors)
- `mutate:*` + `query-and-fix` pipelines produce wrong code
- Long-running agents accumulate stale type info → unpredictable behavior

## Design

### Phase A: Unify dirty propagation across all mutation paths

**Strategy**: Every structural/literal mutation must call `flat.mark_dirty_upward(node)` on the affected node (and its parents/ancestors) to invalidate the type cache chain.

#### Rule set

1. **Structural mutations** that add/remove/reorder children (remove-node, insert-child, replace-child, splice):
   - Mark the **parent** node dirty + upward
   - If a child is removed/added, also mark the sibling chain dirty (via parent)

2. **Leaf value mutations** (replace-type, replace-value, tweak-literal):
   - Mark the **node itself** dirty + upward

3. **Definition mutations** (rebind, set-body):
   - Already correct via dep_callers + `mark_dirty_upward` — keep as-is

4. **Composite mutations** (extract-function, inline-call, replace-pattern):
   - Mark all new/modified nodes dirty + upward

#### Implementation

Add a helper that marks both the FlatAST and the scope chain:

```cpp
void mark_dirty_ast_and_scope(NodeId node) {
    auto& flat = *workspace_flat_;
    flat.mark_dirty_upward(node);
    mark_dirty(node); // scope-level dirty for DefUse rebuild
}
```

Apply to each mutation path:

```
mutate:replace-type      → mark_dirty_ast_and_scope(node)
mutate:replace-value     → mark_dirty_ast_and_scope(node)
mutate:remove-node       → mark_dirty_ast_and_scope(parent_of_target)
mutate:insert-child      → mark_dirty_ast_and_scope(parent)
mutate:tweak-literal     → mark_dirty_ast_and_scope(node)
mutate:replace-pattern   → mark_dirty_ast_and_scope(root_of_match)
extract-function         → mark_dirty_ast_and_scope(define_id)
                           + mark_dirty_ast_and_scope(call_id)
                           + mark_dirty_ast_and_scope(ws_root)
inline-call              → mark_dirty_ast_and_scope(call_node)
```

### Phase B: Incremental typecheck-entry (future)

Currently `typecheck-current` calls `tc.infer_flat(root)` which traverses the full tree (though `synthesize_flat` skips clean nodes). A proper incremental entry point would:

1. Only traverse from the dirty frontier (nodes marked dirty + their parents)
2. Skip clean subtrees entirely
3. Clear dirty flags only for visited nodes (not `clear_all_dirty`)

This is kept for Phase B (roadmap Level 2+) since it requires TypeChecker API changes.

## Implementation Plan

1. Add `mark_dirty_ast_and_scope(NodeId)` helper to Evaluator (evaluator_impl.cpp)
2. Fix all mutation paths listed above to call the helper
3. Add tests:
   - `incr_replace_type` — replace type, check typecheck picks it up
   - `incr_remove_node` — remove node, verify type invalidation
   - `incr_insert_child` — insert new code, verify re-typecheck
   - `incr_tweak_literal` — modify literal, verify type change
   - `incr_extract_function` — extract function, verify types consistent

## References

- Issue #28
- `docs/design/aura_typesystem.md` §5.1
- `src/compiler/evaluator_impl.cpp` (mutation primitives)
- `src/core/ast.ixx` (`mark_dirty_upward`, `is_dirty`, `clear_dirty`)
- `src/compiler/type_checker_impl.cpp` (`synthesize_flat` cache logic)
- Roadmap Level 2
