# Post-steal closed loop (#1592)

**Builds on:** #1490 (EnvFrame refresh), #1497 (StableNodeRef auto-restamp),
#1580 (resume closed loop), #1479 (epoch sync)

## Problem

After work-steal, EnvFrame `version_` / `bridge_epoch`, pinned `StableNodeRef`,
and linear captures can lag. Drift detection without a forced resume main-path
refresh risks UAF under concurrent mutate + GC + steal.

## Contract (production)

```
Fiber::resume()
  ├─ pre-swap: aura_evaluator_resume_fiber_migration()
  │     └─ transfer_mutation_stack_to_current_fiber()
  │           └─ complete_post_resume_steal_refresh(fiber)
  ├─ swapcontext …
  └─ post-swap: g_fiber_resume_validate_
        └─ aura_evaluator_post_resume_refresh()
              └─ complete_post_resume_steal_refresh(fiber)
                    ├─ refresh_stale_frames_after_steal(hint_env, epoch)
                    │     └─ compact_env_frames on OOB/INVALID
                    ├─ probe_and_repin_linear_on_steal()
                    ├─ auto_restamp_pinned_stable_refs_at(Steal)
                    └─ linear_post_mutate_enforce(hint) | enforce_all(if drift)
```

## Metrics (`query:post-steal-closed-loop-stats`, schema **1592**)

| Field | Source |
|-------|--------|
| `post-steal-refresh-count` | `post_steal_refresh_count_` |
| `stable-ref-steal-auto-refresh-total` | CompilerMetrics |
| `boundary-pinned-refresh-count` | CompilerMetrics |
| `linear-post-mutate-enforcements` | CompilerMetrics |
| `envframe-version-mismatch-post-steal` | CompilerMetrics |
| `envframe-dualpath-repair` | CompilerMetrics |
| `resume-path-wired` | constant 1 |

## Tests

- `tests/test_post_steal_closed_loop_1592.cpp` — API + 1000-iter stress  
- `tests/test_issue_1490.cpp`, `tests/test_issue_1497.cpp`,  
  `tests/test_fiber_resume_post_steal_refresh.cpp` — prior coverage  
