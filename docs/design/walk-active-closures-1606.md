# walk_active_closures + linear live scan (#1606)

**Issue:** [#1606](https://github.com/cybrid-systems/aura/issues/1606)  
**Builds on:** #1545 · #1478 · #1458 · #1543 · #1568 · #1596  
**Status:** Production closed-loop (core shipped in #1545; #1606 AC surface + schema 1606).

## Problem

Linear ownership state existed on IR / interpreter / JIT, but invalidate /
compact / ResourceTracker needed a single **active closure walk** that marks
linear-capturing closures invalid so apply never use-after-moves.

## API

```cpp
// Evaluator (tree-walker closures_)
void walk_active_closures(const ActiveClosureWalkFn& fn);

LinearLiveClosureScanResult
scan_live_closures_for_linear_captures(
    bool mark_invalid = true,
    bool only_if_moved = false,
    EnvId filter_env_id = NULL_ENV_ID);
// → bumps linear_live_closure_scans_total
// → on mark: Closure::bridge_epoch = 0 + linear_live_closures_marked_invalid_total

// AuraJIT (fn epoch trackers / ResourceTracker)
std::size_t walk_active_closures(uint64_t current_bridge_epoch);
// Pre-evict: aura_jit_linear_live_closure_scan() → host scan_live_closures…
```

## Wire points

| Path | Action |
|------|--------|
| `invalidate_function` | pre-cascade `scan_live_closures_for_linear_captures(true)` + enforce_all |
| `mark_define_dirty` | scan with `only_if_moved=true` |
| `compact_env_frames` | pre-compact full scan + enforce_all |
| JIT `ResourceTracker` remove | `aura_jit_linear_live_closure_scan` host callback |
| apply_closure | `is_bridge_stale(bridge_epoch=0)` → safe_fallback |

## Metrics (`query:linear-boundary-consistency-stats`, schema **1606**)

| Key | Meaning |
|-----|---------|
| `linear_live_closure_scans_total` | Scan invocations |
| `linear_live_closures_marked_invalid_total` | Closures stamped epoch=0 |
| `walk-active-closures-wired` | 1 |
| `invalidate-scan-wired` / `compact-scan-wired` / `jit-resource-tracker-scan-wired` | 1 |

## Tests

| File | Role |
|------|------|
| `tests/test_walk_active_closures_1606.cpp` | **#1606** AC consolidation |
| `tests/test_issue_1545.cpp` | Original live-scan unit |
| `tests/test_linear_boundary_consistency_1568.cpp` | Boundary consistency |

## Related

- `docs/design/linear-gc-roots.md`
- `docs/design/linear-ownership-runtime-1596.md`
