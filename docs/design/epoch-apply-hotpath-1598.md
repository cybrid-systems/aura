# Epoch apply hotpath closed-loop (#1598)

**Issue:** [#1598](https://github.com/cybrid-systems/aura/issues/1598)  
**Refines:** #1475, #1476, #1477, #1485, #1490, #1491, #1496, #1558, #1592  
**Status:** Closed-loop shipped (core in sister issues; this doc is AC inventory + verification).

## AC closed-loop map

| AC | Requirement | Surface | Shipped |
|----|-------------|---------|---------|
| 1 | `apply_closure` map + bridge mandatory dual-check | `closure_needs_safe_fallback` + `invoke_closure_bridge_checked` | **#1475/#1491/#1558** |
| 2 | JIT `aura_closure_call` / OpApply stale → deopt | `aura_is_jit_closure_fresh` | **#1477/#1508/#1491** |
| 3 | `refresh_stale_frames_after_steal` on resume + compact | Fiber resume + `compact_env_frames` tail | **#1490/#1592/#1598** |
| 4 | Unify invalidate + mark_define_dirty | `atomic_bump_epochs_and_stamp_bridge` + JIT batch deopt | **#1476/#1496** |
| 5 | Metrics AC names | `query:epoch-apply-hotpath-stats` schema **1598** | **#1598** |
| 6 | 1000+ concurrent steal + mutate + apply + compact | `test_epoch_apply_hotpath_1598` | **#1598** |

## Contract (every apply)

```
if bridge_stale(cl.bridge_epoch, current)
   || env_invalid/stale(cl.env_id)
   || linear_post_mutate_enforce fails:
    bump stale_closure_prevented
    if epoch_stale: bump closure_epoch_mismatch_fallback
    → safe fallback / bridge recovery / JIT deopt
    (never eval dangling flat*/pool*)
```

| Path | Gate |
|------|------|
| `apply_closure` local map | `closure_needs_safe_fallback` |
| `apply_closure` bridge | `invoke_closure_bridge_checked` |
| `materialize_call_env` | `is_env_frame_stale` → refresh |
| IR OpApply | `ir_closure_needs_safe_fallback` |
| JIT `aura_closure_call` | `aura_is_jit_closure_fresh` → deopt |

## Post-steal / compact

```
Fiber::resume
  → aura_evaluator_post_resume_refresh
       → complete_post_resume_steal_refresh
            → refresh_stale_frames_after_steal + probe_and_repin

compact_env_frames
  → pre: scan_live_closures + linear_post_mutate_enforce_all
  → remap + dual-epoch bump + restamp
  → post (#1598): refresh_stale_frames_after_steal + probe_and_repin
```

## Invalidate unify (#1476)

`atomic_bump_epochs_and_stamp_bridge`:
release fence + `bridge_epoch` / `defuse_version_` / `mutation_epoch` +
JIT `notify_batch_deopt_and_remove` / walk_active_closures.

## Metrics (`query:epoch-apply-hotpath-stats`, schema **1598**)

| Key | Source |
|-----|--------|
| `stale_closure_prevented` | CompilerMetrics |
| `closure_epoch_mismatch_fallback` | CompilerMetrics |
| `post_steal_refresh_count` | Evaluator |
| `bridge_epoch_bumps` | `bridge_epoch_bumps_total` |
| `invalidate_cascade_depth` | `invalidate_cascade_depth_total` |
| path-wired flags | constants 1 |

## Tests

| File | Role |
|------|------|
| `tests/test_epoch_apply_hotpath_1598.cpp` | **#1598** AC consolidation + 1000 stress |
| `tests/test_issue_1491.cpp` / `test_issue_1558.cpp` | apply + JIT dual-check |
| `tests/test_issue_1490.cpp` / `test_post_steal_closed_loop_1592.cpp` | post-steal |
| `tests/test_issue_1496*.cpp` | invalidate unify + concurrent |

## Related docs

- `docs/design/apply-closure-epoch-safety.md`
- `docs/design/closure-dual-epoch-apply.md`
- `docs/design/unified-invalidation-epoch-protocol.md`
- `docs/design/post-steal-closed-loop-1592.md`
