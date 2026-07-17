# Fiber resume post-steal EnvFrame / linear refresh (#1608)

**Issue:** [#1608](https://github.com/cybrid-systems/aura/issues/1608)  
**Builds on:** #1490 · #1479 · #1580 · #1592 · #1475  
**Status:** Production closed-loop (core in #1490/#1592; #1608 AC surface + schema 1608).

## Contract

```
Fiber::resume()
  ├─ pre-swap: aura_evaluator_resume_fiber_migration()
  │     └─ transfer_mutation_stack (+ complete refresh on migration path)
  ├─ swapcontext …
  ├─ g_fiber_resume_validate_
  └─ aura_evaluator_post_resume_refresh()          // #1490/#1608
        └─ complete_post_resume_steal_refresh(fiber)
              ├─ refresh_stale_frames_after_steal(hint_env, epoch)
              │     └─ version_ < defuse → restamp; OOB → compact_env_frames
              ├─ probe_and_repin_linear_on_steal()
              ├─ auto_restamp StableNodeRef (Steal site)
              └─ linear_post_mutate_enforce(hint | all)
```

## Metrics (`query:post-steal-closed-loop-stats`, schema **1608**)

| Key | Meaning |
|-----|---------|
| `post_steal_refresh_count` | `refresh_stale_frames_after_steal` invocations |
| `stale_frame_prevented` | Alias of version/bridge mismatch detections |
| `resume-path-wired` | 1 |
| `refresh-stale-frames-helper-wired` | 1 |
| `linear-probe-repin-wired` | 1 |
| `post-resume-refresh-hook-wired` | 1 |

## Tests

| File | Role |
|------|------|
| `tests/test_fiber_resume_post_steal_1608.cpp` | **#1608** AC + 1000 stress |
| `tests/test_post_steal_closed_loop_1592.cpp` | Prior closed-loop |
| `tests/test_issue_1490.cpp` | Original helper |

## Related

- `docs/design/post-steal-closed-loop-1592.md`
- `docs/design/post-steal-envframe-refresh.md`
