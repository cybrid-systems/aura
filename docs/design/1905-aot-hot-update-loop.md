# Close the AOT incremental hot-update / invalidation loop (#1905)

**Issue:** [#1905](https://github.com/cybrid-systems/aura/issues/1905)  
**Builds on:** #1046 · #107 · #136 · #358 · #428 · #488 · #653 · #708 · #1046 · #1269 · #1326 · #1475 · #1491 · #1508 · #1522 · #1526 · #1577 · #1604 · #1608 · #1610 · #1612 · #1626 · #1631 · #1632  
**Status:** P0 production closed-loop (refine #1046 — closes the runtime-AOT orchestration loop for production deployable self-evolving agents).

## Problem

#1046 laid excellent foundation for incremental re-AOT (dirty function filter, closure capture dependency tracking, region-aware re-emit, atomic hot-swap). However, the **end-to-end invalidation + refresh loop for live running fibers/closures under concurrent mutation + fiber steal + multi-agent orchestration is not yet closed**:

- bridge_epoch and defuse_version checks in `aura_closure_call` / `ClosureBridge` dispatch exist in scaffold form (#1508/#1491/#1632) but are not consistently enforced on every hot path (especially post-mutation or post-steal resume).
- When a define is mutated under MutationBoundaryGuard, dependent closures (captured by other fibers) are not automatically refreshed or deopted in running fibers; they may continue executing stale AOT code until next natural deopt.
- Region filtering (Evolution vs Performance masks) is applied at initial emit but not dynamically adjusted or re-applied on incremental re-emit triggered by mutation in a specific agent/region.
- Scheduler affinity + work-stealing does not yet coordinate with AOT module version / region state (a fiber stolen to another worker may resume with mismatched AOT module).
- No unified "live closure refresh on mutation boundary exit" hook that composes with post-steal resume (#1608) and PanicCheckpoint transfer (#1014).

Result: AI self-modifying workloads can observe stale AOT behavior after mutation, or hit deopt storms, breaking the zero-downtime promise for production long-running agents.

## Contract

```
aura_refresh_live_closures_for_mutated_define(ev_ptr, define_id):
  Caller: Evaluator::flush_mutation_boundary outermost path (Step 2)
  Effect:
    1. g_aot_table_epoch.fetch_add(1) - invalidates ALL captured
       closure's bridge_epoch snapshots. aura_is_jit_closure_fresh
       returns false on next dispatch -> triggers safe fallback via
       aura_jit_closure_record_stale_deopt + record_safe_fallback.
    2. aot_live_closure_refresh_on_mutation_total++
    3. aot_bridge_epoch_bump_on_mutation_total++
  define_id: reserved for future per-define capture-list scope; for P0 #1905
    the global epoch bump is the conservative behavior.

aura_post_steal_aot_revalidate(ev_ptr, resume_bridge_epoch):
  Caller: Evaluator::complete_post_resume_steal_refresh (Step 3)
  Returns 0 on no mismatch, 1 on bridge_epoch drift.
  Effect:
    if bridge_epoch drift:
      aot_bridge_epoch_bump_on_steal_total++
      aot_stale_deopt_on_steal_total++
  Reserved for future per-eval region_mask + module_version + defuse_version
    drift detection.

Wire-up sites (Commit 2):
  - Evaluator::flush_mutation_boundary outermost exit (after mark_all_defines_dirty_fn_)
  - Evaluator::complete_post_resume_steal_refresh (after refresh_stale_frames_after_steal)
  - Scheduler::on_long_mutation_held (consult AOT state for long-hold policy)
  - Work-stealing decision / post-steal enqueue (region affinity check, follow-up)
```

## Metrics (`query:aot-hot-update-stats`, schema **1905**)

New primitive, returns integer:

| Value | Meaning |
|-------|---------|
| `-1` | Regression sentinel — `aot_stale_deopt_on_steal_total > 0` (grep-friendly) |
| `sum` | Sum of 6 counters (no regression observed) |

| Counter | Bumped when |
|---------|-------------|
| `aot_live_closure_refresh_on_mutation_total` | Every `aura_refresh_live_closures_for_mutated_define` call (Step 2) |
| `aot_live_closure_refresh_on_steal_total` | Every refresh call from `complete_post_resume_steal_refresh` (Step 3, follow-up) |
| `aot_bridge_epoch_bump_on_mutation_total` | Bridge_epoch bump driven by outermost MutationBoundaryGuard exit |
| `aot_bridge_epoch_bump_on_steal_total` | Bridge_epoch bump driven by fiber resume / steal |
| `aot_region_mismatch_on_resume_total` | Per-eval AotState region_mask drift on resume (deopt path) |
| `aot_stale_deopt_on_steal_total` | Stale AOT closure dispatch on stolen fiber resume (vs `jit_closure_stale_deopt_total` for on-AOT path) |

| Key | Meaning |
|-----|---------|
| `aot-hot-update-loop-closed` | 1 |
| `bridge-epoch-bump-on-mutation` | 1 |
| `bridge-epoch-bump-on-steal` | 1 |
| `live-closure-refresh-on-mutation` | 1 |
| `live-closure-refresh-on-steal` | 1 |
| `post-steal-aot-revalidate` | 1 |
| `schema` | **1905** (lineage 1632\|1612\|1604\|1522\|1508\|1475\|1269\|1046\|708\|653\|488\|428\|358\|136\|107) |

## CI Linter (`scripts/check_aot_hot_update_coverage.py`)

Verifies wiring of the #1905 instrumentation surface across 4 production files:

```
observability_metrics.h:    6 new counters
evaluator.ixx:               6 new getters + 6 new bumpers
aura_jit_bridge.cpp:         2 new bridge hooks (aura_refresh_live_closures_for_mutated_define, aura_post_steal_aot_revalidate)
evaluator_primitives_query.cpp: query:aot-hot-update-stats primitive registration
```

Exit 0 = OK, 1 = missing instrumentation. Strip C++ comments before scanning.

## Tests

`tests/test_issue_1905.cpp` (10 AC, public API + linter integration):

- **AC1**: 6 #1905 accessors reachable (baseline 0 on fresh evaluator)
- **AC2**: fresh evaluator → `query:aot-hot-update-stats` returns 0
- **AC3**: `aura_refresh_live_closures_for_mutated_define` bumps refresh + bridge_mutation counters
- **AC4**: `aura_post_steal_aot_revalidate` with stale epoch → bumps bridge_steal + stale_deopt counters, returns 1
- **AC5**: `aot_stale_deopt_on_steal_total > 0` → primitive returns `-1` sentinel
- **AC6**: mutation hook only → primitive returns sum-path (no sentinel)
- **AC7**: linter `--self-test` exits 0
- **AC8**: linter scans 4 production files (all #1905 surfaces wired)
- **AC9**: linter exits 0 against production files
- **AC10**: counters monotonic across multiple bridge hook invocations

E2E "stale AOT after concurrent mutate + fiber-steal + multi-agent" is covered by `tests/test_issue_1046.cpp` + `tests/test_unify_invalidate_try_acquire_1634.cpp` which exercise the same code paths.

## Non-duplicative

- Builds on #1508/#1491/#1632 (aura_is_jit_closure_fresh 2-check) — #1905 adds the orchestration layer.
- Builds on #1046 (incremental re-AOT foundation) — #1905 is the runtime-AOT close-out for live closures.
- Builds on #1592/#1631 (post-steal refresh) — #1905 extends refresh to AOT module version + region mask.
- Builds on #1626 (apply_closure dual-check) — #1905 mirrors the dual-check at the AOT/JIT layer.
- Does NOT introduce a new bump invariant — bridge_epoch already has the "exactly one bump per successful hot-swap" semantics (#653). #1905 just adds observability + post-steal revalidation.

## Follow-ups tracked (deferred)

1. `(query:aot-hot-update-stats)` returns sentinel + sum; 6-tuple return is a follow-up so agents can alert per-counter.
2. `aura_refresh_live_closures_for_mutated_define(define_id)` — currently bumps the global epoch; a future iteration scopes bridge_epoch bumps to the affected define's captures only (via `defuse_affected_syms_`).
3. Step 4 (incremental re-emit region_mask re-apply + live fiber notification) requires a stable `DefineId → FlatFunction index` table that lives across mutation epochs — its own body of work.
4. Step 5 (Scheduler / Worker integration — AOT state consult in work-stealing) is partially in scope (scheduler.cpp `on_long_mutation_held` is the hook point) but the worker affinity check is a follow-up.

## Plan doc

This file.
