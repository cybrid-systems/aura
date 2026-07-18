# QueryMatcher StableNodeRef captures (#1695)

**Issue:** [#1695](https://github.com/cybrid-systems/aura/issues/1695)  
**Files:** `query_matcher.ixx`, `query_matcher.cpp`, `evaluator_primitives_mutate.cpp`,
`evaluator_primitives_query_workspace.cpp`  
**Status:** P2 correctness — matcher captures carry generation-tagged refs.

## Problem

`QueryMatchState::captures` stored raw `NodeId`. `mutate:replace-pattern`
re-`make_ref`'d those ids when building `capture_refs`, after the match
walk. Combined with post-match `parse_to_flat` growth (#1694), provenance
was weaker than the codebase StableNodeRef contract (#818 / #270).

## Fix (Option A)

```cpp
// QueryMatchState / PendingGuard
std::vector<std::pair<SymId, FlatAST::StableNodeRef>> captures;
```

- Bind via `ws_flat_->make_ref(ws_id)` at capture sites (`?x`, `...`, Kleene)
- Equality / rebind reads `.id`
- `replace-pattern` copies `kv.second` into `capture_refs` (no second make_ref)
- `query:pattern` guards read `.id` for let bindings

Apply-time `is_valid_in` still skips dead captures under gen bumps outside
atomic batch (batch suppresses gen bumps for multi-replace).

## Tests

`tests/test_matcher_stable_captures_1695.cpp`
