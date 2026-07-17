# Runtime linear ownership + live-closure scan refine (#1596)

**Issue:** [#1596](https://github.com/cybrid-systems/aura/issues/1596)  
**Refines:** #1458, #1478, #1486, #1494, #1545, #1557, #1543, #1568  
**Status:** Closed-loop shipped (implementation in sister issues; this doc is the AC inventory + verification map).

## AC closed-loop map

| AC | Requirement | Surface | Shipped |
|----|-------------|---------|---------|
| 1 | `walk_active_closures(fn)` under lock | `Evaluator::walk_active_closures` | **#1545** |
| 2 | Wire walk/scan into 6 touchpoints | see table below | **#1545/#1557/#1568** |
| 3 | `force_drop_or_mark_invalid` | `bridge_epoch=0` + audit | **#1568** |
| 4 | GC root audit across touchpoints | `run_linear_gc_root_audit` + ring log | **#1543** |
| 5 | Metrics: enforcements / live scans / prevented | CompilerMetrics + query schema **1596** | **#1478/#1545/#1596** |
| 6 | Use-after-move + 10k stress tests | `test_linear_ownership_post_mutate` + siblings | **#1596** |

## Six mutation touchpoints

| Path | Scan / enforce | Audit path tag |
|------|----------------|----------------|
| `invalidate_function` / `mark_define_dirty` | pre-cascade `scan_live_closures_for_linear_captures` + `enforce_linear_boundary_consistency` | `invalidate_function` |
| `compact_env_frames` | pre-compact scan + enforce | `compact_env_frames` |
| JIT ResourceTracker / deopt | `aura_jit_linear_live_closure_scan` + host enforce | `jit_hot_swap` |
| fiber steal / resume | `probe_linear_ownership_on_fiber_steal` → full boundary consistency | `fiber_steal` |
| MutationBoundary / Guard exit / typed_mutate | `enforce_linear_boundary_consistency(TypedMutate, only_moved)` | `typed_mutate` |
| GC safepoint | `sync_linear_roots_and_bridge_epoch` + audit | `gc_safepoint` |

## Core APIs

```
Evaluator::walk_active_closures(fn)          // unique-lock walk of closures_
Evaluator::scan_live_closures_for_linear_captures(mark, only_if_moved)
  → linear_live_closure_scans_total++
  → optional force bridge_epoch=0 + violation_prevented on Moved
Evaluator::force_drop_or_mark_invalid(id)    // named Drop
Evaluator::linear_post_mutate_enforce(env)   // use-after-move intercept
Evaluator::enforce_linear_boundary_consistency(path, mark_all)
  → scan + enforce_all + epoch fence + run_linear_gc_root_audit
```

**Force Drop:** no separate free; `bridge_epoch=0` forces `is_bridge_stale` / safe_fallback so the capture cannot be applied.

## Metrics (`query:linear-boundary-consistency-stats`, schema **1596**)

| Key | Source |
|-----|--------|
| `linear_post_mutate_enforcements` | CompilerMetrics |
| `linear_live_closure_scans_total` | CompilerMetrics |
| `linear_ownership_violation_prevented` | CompilerMetrics |
| `linear_gc_root_audit_checks_total` | CompilerMetrics |
| `boundary-consistency-total` / `force-drop-total` / `epoch-fence-enforce-total` | #1568 |
| `walk-active-closures-wired` / `force-drop-wired` | constants 1 |
| `issue` / `schema` | **1596** |

## Tests

| File | Role |
|------|------|
| `tests/test_linear_ownership_post_mutate.cpp` | **#1596** AC consolidation + 10k stress |
| `tests/test_issue_1557.cpp` | walk + scan + invalidate/compact/steal/JIT |
| `tests/test_linear_boundary_consistency_1568.cpp` | unified enforce + use-after-move |
| `tests/test_issue_1458_linear_ownership_post_mutate.cpp` | harden validation |
| `tests/test_issue_1478.cpp` / `test_issue_1545.cpp` | MVP + live-closure |

## Out of scope (issue body)

- Full linear type system in AOT
- Cross-closure linear taint beyond capture scan

## Related docs

- `docs/design/linear-gc-roots.md`
- `docs/design/linear-validation.md`
- `docs/design/stress-testing.md`
