# Post-steal EnvFrame / bridge_epoch refresh (#1490)

**Related:** #1479 (version/epoch sync), #1475 (`is_env_frame_stale`), #1489 (GC defer), #683 (linear probe)

## Problem

After work-steal, a fiber may resume on a different worker while:

- `EnvFrame.version_` lags `defuse_version_`
- `Closure.bridge_epoch` lags the service bridge epoch
- pinned COW / `StableNodeRef` / linear SoA state is unpinned relative to the new worker

`restore_post_yield_or_rollback` detects drift on yield checkpoints but did not **force** a full EnvFrame/linear refresh on the resume main path.

## Contract

```
Fiber::resume()
  ├─ (pre-swap) aura_evaluator_resume_fiber_migration()
  │     └─ transfer_mutation_stack_to_current_fiber()
  │           ├─ refresh_stale_frames_after_steal(0, 0)
  │           └─ probe_and_repin_linear_on_steal()
  ├─ swapcontext  … fiber runs …
  └─ (post-swap) g_fiber_resume_validate_
        └─ aura_evaluator_post_resume_refresh()
              ├─ refresh_stale_frames_after_steal
              └─ probe_and_repin_linear_on_steal
```

| Helper | Behavior |
|--------|----------|
| `refresh_stale_frames_after_steal(hint_env, expected_epoch)` | Walk live closures (+ optional env hint); `refresh_stale_frame_in_walk` when `version_ < defuse`; detect bridge drift; on OOB/INVALID → `compact_env_frames()`; bump post-steal metrics |
| `probe_and_repin_linear_on_steal()` | `probe_linear_ownership_on_fiber_steal` + `re_pin_cow_children_from_snapshot` + `restamp_pinned_stable_refs` |

`hint_env_id` / `expected_epoch` may be `0` (full scan / current epoch). Fiber does not yet carry sticky env/epoch fields; full scan is the production default.

## Metrics

| Counter | When |
|---------|------|
| `Evaluator::post_steal_refresh_count_` | Every refresh pass |
| `envframe_version_mismatch_post_steal_total` | version or bridge mismatch observed |
| `envframe_dualpath_repair_total` | frames refreshed / compact repair |
| `envframe_cross_fiber_stale_total` | OOB / INVALID frames |

## Tests

`tests/test_issue_1490.cpp` — API, stale refresh, re-pin, transfer path, metrics smoke, multi-iter stress under concurrent defuse bumps.
