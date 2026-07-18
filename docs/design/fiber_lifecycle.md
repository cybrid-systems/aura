# Fiber lifecycle: yield → steal → resume → GC (#1631)

**Issue:** [#1631](https://github.com/cybrid-systems/aura/issues/1631)  
**Builds on:** #1490 · #1479 · #1580 · #1592 · #1608 · #1612  
**Status:** Production mandate — EnvFrame / bridge_epoch refresh + linear probe on every `Fiber::resume`.

## Lifecycle diagram

```
Fiber::yield(reason)
  ├─ check_gc_safepoint
  ├─ g_fiber_yield_checkpoint_ → stamp resume_env_hint + resume_bridge_epoch_hint
  └─ swapcontext → worker

Worker steal / re-schedule
  └─ Fiber::resume()
        ├─ g_fiber_setter_ / g_fiber_sync_mutation_stack_
        ├─ aura_evaluator_resume_fiber_migration()          // pre-swap
        │     └─ transfer_mutation_stack_to_current_fiber()
        │           └─ complete_post_resume_steal_refresh() // #1490/#1631
        ├─ swapcontext → fiber
        ├─ g_fiber_resume_validate_
        │     └─ restore_post_yield_or_rollback
        │     └─ complete_post_resume_steal_refresh()       // validate path
        ├─ panic checkpoint transfer (if pending)
        └─ aura_evaluator_post_resume_refresh()             // #1631 MANDATE
              └─ complete_post_resume_steal_refresh(fiber)
                    ├─ refresh_stale_frames_after_steal(hint_env, epoch)
                    │     ├─ version_ < defuse → refresh_stale_frame_in_walk
                    │     ├─ bridge_epoch drift → aura_jit_walk_active_closures
                    │     └─ OOB/INVALID → compact_env_frames
                    ├─ probe_and_repin_linear_on_steal()
                    ├─ auto_restamp_pinned_stable_refs_at(Steal)
                    ├─ refresh_stale_macro_frames + probe_and_repin_macro (#1612)
                    └─ linear_post_mutate_enforce(hint | all)

GC compact (arena)
  └─ on_arena_compact_hook
        ├─ re_pin_cow_children_from_snapshot
        └─ macro refresh/repin (#1612)
```

## Contract (#1631 AC)

1. **Resume main path always calls** `complete_post_resume_steal_refresh` (pre-swap migration + post-swap validate + `aura_evaluator_post_resume_refresh`).
2. **`refresh_stale_frames_after_steal`** bumps `post_steal_refresh_count` (monotonic) and repairs EnvFrame version drift; bridge drift forces JIT walk (deopt), not silent restamp.
3. **Linear safety probe** runs on the same closed loop (`probe_and_repin_linear_on_steal`).
4. **1000+ iter** steal/refresh + concurrent defuse/mutate must keep `post_steal_refresh_count` monotonic and leave the evaluator usable.

## Metrics (`query:post-steal-closed-loop-stats`, schema **1631**)

| Key | Meaning |
|-----|---------|
| `post_steal_refresh_count` | `refresh_stale_frames_after_steal` invocations |
| `resume_forced_refresh_total` | `complete_post_resume_steal_refresh` calls |
| `stale_frame_prevented` | version/bridge mismatch detections |
| `bridge_epoch_drift_post_steal` | IRClosure bridge drift detections |
| `bridge_epoch_deopt_walk_post_steal` | JIT walk deopts after drift |
| `fiber-lifecycle-mandate-active` | 1 |
| `resume-path-wired` | 1 |

## Tests

| File | Role |
|------|------|
| `tests/test_fiber_resume_lifecycle_1631.cpp` | **#1631** AC + 1000 stress + Fiber resume |
| `tests/test_fiber_resume_post_steal_1608.cpp` | Prior AC surface |
| `tests/test_issue_1490.cpp` | Original helper |
| `tests/test_fiber_macro_hygiene_refresh_1612.cpp` | Macro layer |

## Related docs

- `docs/design/fiber-resume-post-steal-1608.md`
- `docs/design/post-steal-closed-loop-1592.md`
- `docs/design/fiber-macro-hygiene-refresh-1612.md`
- `docs/design/post-steal-envframe-refresh.md`
