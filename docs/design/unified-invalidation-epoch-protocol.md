# Unified Invalidation + Dual-Epoch Protocol

**Issues:** #1496 (parent closed-loop), #1476 (MVP), #1475 / #1491 (apply dual-check), #1522–#1524 (atomic helper + lock order)

## Problem

Two write-side invalidation paths existed:

| Path | Soft vs hard | Pre-#1496 epoch protocol |
|------|--------------|--------------------------|
| `mark_define_dirty` | Soft (keep IR, dirty bits) | `atomic_bump_epochs_and_stamp_bridge` |
| `invalidate_function` | Hard (JIT erase, dep_graph teardown) | Hand-rolled dual bump |

Hand-rolled bumps could desync AOT table epoch / solve_delta wipe / walk_active_closures relative to the soft path, opening a multi-fiber half-update window for live closures.

## Single protocol (#1496)

Both paths call **`atomic_bump_epochs_and_stamp_bridge(name)`** under `mutate_mtx_`:

```
release fence
  → bump_bridge_epoch()          // mutation_epoch_ release
  → bump defuse_version_         // #1475 EnvFrame readers
  → aura_aot_bump_func_table_epoch()
  → on_typed_mutation_epoch_bump()  // solve_delta wipe
  → invalidate_bridge_for(name)     // stamp + JIT batch_deopt
  → notify_walk_active_closures_    // live_closure_stale_prevented
```

Then:

- **Soft:** body-only / full dirty + BFS cascade (may re-enter protocol per dependent)
- **Hard:** per-block dirty + BFS erase dep_graph + hard JIT invalidate + impact-scope bridge refresh

Readers (`apply_closure`, `aura_is_jit_closure_fresh`) acquire-load either domain and take safe fallback / deopt — never eval dangling flat*/pool*.

## Metrics (AC5)

| Metric | Meaning |
|--------|---------|
| `unified_invalidation_protocol_total` | Protocol entries (soft + hard) |
| `bridge_epoch_bumps_total` | Per `bump_bridge_epoch` |
| `invalidate_cascade_depth_max` / `_total` | BFS depth HWM + sum |
| `compiler_live_closure_stale_prevented_total` | Walk/active stale marks |

## Tests

- `tests/test_issue_1496.cpp` — unified protocol + concurrent mutate/apply
- `tests/test_issue_1476.cpp` — MVP dual-epoch lockstep (still green)
- `tests/test_issue_1491.cpp` — apply/JIT dual-check closed-loop
