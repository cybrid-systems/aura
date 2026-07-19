# Issue #1637 — PanicCheckpoint lifecycle hardening (post-steal / post-compact / post-hot-swap)

## 来源
Runtime multi-fiber concurrency + Mutation safety production-readiness
review refinement (2026-07-19), building on the dual-epoch +
linear_post_mutate_enforce + walk_active_closures foundation laid by
#1490/#1580/#1592/#1595/#1597/#1612/#1631 and the file-scope atomic
fallback pattern from #1908 (Macro clone provenance).

## 问题描述
`MutationBoundaryGuard` + `PanicCheckpoint` (save / restore / commit on
`evaluator.ixx`) provide excellent single-mutation rollback, but the
lifecycle close was incomplete on three concurrent-recovery paths:
fiber steal / migration, GC compact, self-mutation hot-swap / JIT
deopt. Concretely:

- `restore_panic_checkpoint_on_fiber_resume_if_needed`
  (evaluator_workspace_tree.cpp:402 — Issue #596) only bumps two
  counters when `restore_panic_checkpoint()` returns true. It does not
  truncate env_frames back to the checkpoint snapshot, bump
  env_generation to invalidate stale closure captures, walk active
  closures to refresh their bridge_epoch stamps (dual-epoch rebound
  per #1631), or clear the pending panic checkpoint fields.

- `on_arena_compact_hook` (evaluator_fiber_mutation.cpp:535 — #1446)
  re-pins StableNodeRef / COW children but does not call any panic
  checkpoint restore. A pending checkpoint survives a GC compact
  without its env_frames / closures / bridge_epoch state being
  re-aligned with the checkpoint snapshot.

- The hot-swap deopt path (Issue #97 — `aura_aot_record_deopt_on_steal`
  + `hot_swap_fn_`) is similarly missing the panic restore, so
  self-mutation hot-swap can leave a pending panic checkpoint
  dangling against the new closure body.

- No 5-way observability split for the three recovery paths plus the
  two outcome dimensions (heal success / safe-under-boundary race).
  Dashboards cannot distinguish which event triggered the restore or
  whether the heal actually rebuilt state vs. raced under a held
  boundary.

## 代码证据 (code anchors)

Before:

```cpp
// evaluator_workspace_tree.cpp:402
void Evaluator::restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept {
    if (!has_panic_checkpoint() || !outermost_mutation_success_flag_) return;
    if (*outermost_mutation_success_flag_) return;
    if (!panic_auto_rollback_) return;
    if (restore_panic_checkpoint()) {
        bump_guard_panic_reflect_restores_on_resume();
        bump_macro_hygiene_panic_restamp_from_workspace();
    }
}
```

After (Issue #1637 hardened):

```cpp
void Evaluator::restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept {
    if (!has_panic_checkpoint() || !outermost_mutation_success_flag_) return;
    if (*outermost_mutation_success_flag_) return;
    if (!panic_auto_rollback_) return;
    bump_post_steal_checkpoint_restore_total();
    if (!restore_panic_checkpoint()) return;
    run_post_restore_lifecycle_close(/*safe_total_event=*/true);
}
// + restore_panic_checkpoint_on_arena_compact_if_needed()
// + restore_panic_checkpoint_on_hot_swap_if_needed()
// + run_post_restore_lifecycle_close (shared body):
//   - truncate_env_frames_to_checkpoint()
//   - env_generation_ += 1
//   - invalidate_post_rollback_env_frames()
//   - walk_active_closures([this](ClosureId, Closure& cl) {
//       if (is_bridge_stale(cl.bridge_epoch, current_bridge_epoch()))
//           cl.bridge_epoch = current_bridge_epoch();
//     })
//   - clear_panic_checkpoint()
//   - bump_guard_panic_reflect_restores_on_resume() + macro hygiene
//   - bump cross_fiber_panic_heal_success always;
//     bump_mutation_boundary_steal_safe_total when fiber resume
//     path + boundary held at moment of restore.
```

## 精确改动位置 (file-by-file)

1. **src/compiler/evaluator_workspace_tree.cpp** — beef up
   `restore_panic_checkpoint_on_fiber_resume_if_needed` (replaces
   existing body). Add the two new variants
   `restore_panic_checkpoint_on_arena_compact_if_needed` and
   `restore_panic_checkpoint_on_hot_swap_if_needed`. Add
   `run_post_restore_lifecycle_close` shared body.

2. **src/compiler/evaluator_fiber_mutation.cpp:535** — extend
   `on_arena_compact_hook` body with the panic restore call (after
   the existing StableNodeRef / MacroIntroduced repin steps).
   Plus three per-Evaluator C trampolines
   (`aura_evaluator_post_steal_panic_restore` etc.) using the
   `Evaluator::yield_hook_evaluator()` pattern.

3. **src/compiler/aura_jit_bridge.cpp** — add 5 file-scope atomic
   fallbacks (g_1637_*_fallback_total) + 3 `void* ev_ptr` bridge
   hook trampolines + 5 C accessors, mirroring the #1908
   `aura_macro_provenance_repin_on_steal` dual-write pattern
   (so module-unaware callers + the C accessor surface see the
   same unified counters).

4. **src/compiler/evaluator_primitives_obs_eval_05.cpp** — extend
   `query:mutation-boundary-coverage-stats` kv list with the 5
   new keys (post-steal-checkpoint-restore-total /
   post-compact-checkpoint-restore-total /
   post-hot-swap-checkpoint-restore-total /
   cross-fiber-panic-heal-success /
   mutation-boundary-steal-safe-total) and bump schema 1444 → 1637.

5. **src/compiler/evaluator_primitives_types.cpp** — extend the
   `(hot-swap:fn "name" "new-source")` primitive callback to call
   `restore_panic_checkpoint_on_hot_swap_if_needed()` after the
   underlying `hot_swap_fn_` returns (regardless of success / failure —
   the underlying method early-returns when no checkpoint is
   pending, so the steady-state cost stays negligible).

6. **src/compiler/evaluator.ixx** — declare 5 `bump_*_total` +
   5 `get_*_total` per-Evaluator methods + the 3
   `restore_<event>_if_needed` variants + the shared
   `run_post_restore_lifecycle_close` helper.

7. **src/compiler/observability_metrics.h** — add 5 atomic counter
   slots with `std::memory_order_relaxed` access (same pattern as
   `panic_transfer_on_steal` / `cow_repin_on_steal` /
   `checkpoint_lost_on_compact` next to which they live).

8. **src/compiler/compiler_metrics_fields.inc** — add 5
   `AURA_COMPILER_METRICS_FIELD(...)` X-macro entries.

9. **tests/test_issue_1637.cpp** (new, ~280 lines, 9 ACs).

10. **scripts/check_panic_checkpoint_lifecycle_coverage.py** (new,
    ~150 lines, 10 ACs).

11. **CMakeLists.txt** — add `aura_add_issue_test(test_issue_1637)`
    entry + `aura_issue_test_link_llvm_jit(test_issue_1637)` paired
    link step (same shape as `test_issue_1908` /
    `test_gc_roots_bridge_epoch_drift_1734`).

## 验收标准 (AC)

| AC | Verification |
| --- | --- |
| AC1 | `test_issue_1637` AC1: full closed-loop restore body in `evaluator_workspace_tree.cpp` (truncate_env_frames + env_generation + invalidate + walk_active_closures + clear_panic_checkpoint all present) |
| AC2 | AC2: 3 restore variants + helper declared in `evaluator.ixx` and defined in `evaluator_workspace_tree.cpp` |
| AC3 | AC3: 5 metric slots in `observability_metrics.h` + 5 X-macro fields in `compiler_metrics_fields.inc` |
| AC4 | AC4: 5 bump_/getter pairs declared in `evaluator.ixx` |
| AC5 | AC5: 4 prod files (evaluator_workspace_tree / evaluator_fiber_mutation / evaluator_primitives_types / aura_jit_bridge) wire their restore_<event>_if_needed callsite(s) |
| AC6 | AC6: 5 new keys present in `query:mutation-boundary-coverage-stats` output, schema bumped to 1637 |
| AC7 | AC7: 3 file-scope atomic fallbacks + 5 C accessors in `aura_jit_bridge.cpp` |
| AC8 | AC8: `hot-swap:fn` callback wires panic restore after `hot_swap_fn_` returns |
| AC9 | AC9: cross-layer baseline regression — `CompilerService` can be constructed and a `(set-code) + (eval-current)` round-trip still survives the wire-up |
| AC10 | `scripts/check_panic_checkpoint_lifecycle_coverage.py` exits 0 with all 10 ACs green |

## 预期收益
- Zero-downtime self-healing under concurrent fiber steal, GC compact,
  or hot-swap deopt with a pending PanicCheckpoint.
- Foundation for long-running AI Agent + concurrent steal + GC +
  self-mutation production loads (no manual panic checkpoint recovery
  required across these events).
- 5-way observability split: dashboards can distinguish the triggering
  event path (steal / compact / hot-swap) from the outcome dimension
  (heal succeeded / safe-under-boundary race resolved).

## 优先级
**P0** (production stability core — long-running Agent blocker).

## 标签
P0, stability, fiber, mutation, gc, panic-checkpoint,
production-readiness, dual-epoch

## 相关 Issues
- #1014 (original harden PanicCheckpoint + MutationBoundaryGuard lifecycle)
- #1490 / #1580 / #1592 / #1595 / #1597 / #1612 / #1631 (post-steal refresh
  foundation — `complete_post_resume_steal_refresh`,
  `refresh_stale_frames_after_steal`, `probe_and_repin_linear_on_steal`,
  JIT active-closure walk on bridge drift)
- #1373 (yield-path forced rollback / `mutation_boundary_cross_thread_migration_total`
  — pairs with new `mutation_boundary_steal_safe_total` for the dual-race
  observability surface)
- #1446 (nested Guard + re_pin_cow_children_from_snapshot pattern reference;
  `on_arena_compact_hook` AC2 baseline)
- #1612 (MacroIntroduced marker / provenance refresh on resume / steal / GC —
  `on_arena_compact_hook` AC1 baseline)
- #1908 (Macro clone provenance — Counter + bridge hook dual-write pattern
  reference; linter + test AC shape)
- #1907 (reflect/EDSL bridge — file-scope atomic fallback pattern reference)
- #1734 (bridge_epoch drift in GC root collection — slim surface freeze
  pattern; raised SlimSurface interim ceiling 520→521 in the same window)

## 验证方式
- `tests/test_issue_1637.cpp`: 9 ACs all green
- `scripts/check_panic_checkpoint_lifecycle_coverage.py`: 10 ACs all green
- pre-commit hooks: clang-format clean, `./build.py docs` regen clean,
  `gen_docs.py --check` verify clean
- pre-push gates: primitive surface freeze (no new primitives added —
  existing `query:mutation-boundary-coverage-stats` surface extended
  within the 521 budget) + test-registry (#1572) + test binding +
  coverage (#1453)
- Same PR cycle as #1908 / #1907: edit → build → run tests → descriptive
  commit → push `main` (direct, no PR review per MEMORY.md workflow).
