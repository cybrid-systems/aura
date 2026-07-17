# Join / Mailbox / parallel_intend linear enforcement (#1595)

**Issue:** [#1595](https://github.com/cybrid-systems/aura/issues/1595)  
**Builds on:** #1584 (Fiber::join), #1585 (MultiFiberMailbox), #1586 (parallel_orch),
#1592 (post-steal closed loop), #1490 / #1497 (linear + StableNodeRef)

## Problem

Parallel orchestration joins and mailbox handoffs can race with steal / GC compact /
mutate. Without forced linear ownership probe and StableNodeRef restamp on these
paths, joiner-side EnvFrame / pins can lag and dangle.

## Contract

```
Fiber::join(target) → Ok
  ├─ Fiber::join_linear_enforcement_total++          // always
  └─ if host-thread joiner (g_current_fiber == null):
        aura_evaluator_on_fiber_join(target)
          └─ Evaluator::complete_post_join_linear_enforcement
                ├─ refresh_stale_frames_after_steal(hint)  // when hints/drift
                ├─ probe_and_repin_linear_on_steal()
                ├─ auto_restamp_pinned_stable_refs_at(Join)
                └─ linear_post_mutate_enforce(hint|all|liveness)

// Fiber-context join only bumps process counter (small fiber stacks).
// Call complete_post_join_linear_enforcement from host/agent after batch join.

MultiFiberMailbox::push
  linear_checks++
  if payload starts with "linear-viol:" → Closed + linear_violations++
  (deep probe: Evaluator::probe_mailbox_linear_and_stable_refs on host)

parallel_intend / parallel_run
  Fiber::join(span) → per-target Ok path above (#1584 + #1595)
```

### Payload linear claims

| Prefix | Behavior |
|--------|----------|
| (none) | Deliver |
| `linear-viol:…` | Reject at push (`Closed`); process `linear_violations++` |
| `linear-env:<id>` | Host probe via `probe_mailbox_linear_and_stable_refs` |

## Metrics (`query:join-linear-enforcement-stats`, schema **1595**)

| Key | Source |
|-----|--------|
| `linear_join_enforcement_total` | CompilerMetrics |
| `mailbox_linear_violation_count` | CompilerMetrics |
| `stable_ref_post_join_repin_total` | CompilerMetrics (Join-site restamp) |
| `fiber_join_linear_enforcement_total` | `Fiber::join_linear_enforcement_total` |
| `join-path-wired` / `mailbox-path-wired` / `parallel-intend-path-wired` | constants 1 |

## Tests

- `tests/test_fiber_join_linear.cpp` — AC1–AC6, 1000+ iter stress
- Prior: `test_fiber_join`, `test_multi_fiber_mailbox`, `test_parallel_orch`,
  `test_post_steal_closed_loop_1592`

## Related docs

- `docs/design/fiber-join.md`
- `docs/design/multi-fiber-mailbox.md`
- `docs/design/parallel-orch.md`
- `docs/design/post-steal-closed-loop-1592.md`
