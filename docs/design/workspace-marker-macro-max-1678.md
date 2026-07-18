# workspace_marker_macro_introduced max semantics (#1678)

**Issue:** [#1678](https://github.com/cybrid-systems/aura/issues/1678)  
**File:** `src/compiler/evaluator_primitives_query.cpp`  
**Status:** P1 correctness — never undercount MacroIntroduced provenance.

## Bug

```cpp
return count > 0 ? count : snapshot;  // walk alone when both > 0
```

When snapshot was 5 (prior COW) and the live walk found 1 marker, the helper
returned **1**, dropping 4 markers from agent-facing `macro-markers` stats.

## Fix

1. Read **snapshot under `WorkspaceSharedLock`** (same lock as walk).  
2. Return **`max(walk, snapshot)`** (monotonic floor from snapshot, live column
   wins when higher).

Not additive (`snapshot + walk`) — that would double-count the same markers.

## Call sites

All consumers go through `workspace_marker_macro_introduced` (pattern/macro
hygiene dashboards). Direct `get_macro_markers_in_snapshot()` reads remain
snapshot-only for impact-log fields that intentionally track the ring stamp.

## Tests

`tests/test_workspace_marker_macro_max_1678.cpp`
