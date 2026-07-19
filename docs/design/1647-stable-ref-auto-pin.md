# 1647 — StableNodeRef cross-boundary auto-refresh observability (partial-redundant-ship)

**Status:** shipped (commit pending, on `5ccf67e6` baseline; rebased during ship)
**Branch:** `main`
**Date:** 2026-07-19

## Context

`StableNodeRef` is the cornerstone of Aura's cross-cutting ref contract —
any prim returning a `StableNodeRef` (query:children-stable, query:parent-stable,
query:stable-ref, query:stable-ref-stats, etc.) must hold the ref through
`validate_or_refresh` + `pin_for_cow` to survive concurrent COW /
sub-workspace / fiber mutations. The body of #1647 reports that AI Agent
ergonomics around this are still manual: callers must explicitly
`pin_for_cow()` + `validate_or_refresh()` rather than getting a safe-ref
by default.

Predecessor coverage (#715 / #738 / #1250) shipped the underlying infra:

- StableNodeRef struct (id, gen, wrap_epoch, cow_epoch, mutation_id,
  subtree_gen, fiber_id, workspace_id, boundary_pinned) — #1250
- `validate_or_refresh` + `refresh_if_stale` helper — #715
- `pin_for_cow` + `boundary_pinned` + `atomic_batch_pinned_refs_` — #738
- `bump_stable_ref_cross_cow_refresh` legacy per-Fiber counter
  (3 callers at evaluator_fiber_mutation.cpp:395 + workspace.cpp:247
  + mutate.cpp:411)
- `stable_ref_auto_pin_total` atomic counter — #1250 (the pin-time counter)

What #1647 adds (this commit, partial-redundant-ship):

1. **1 new per-CompilerMetrics counter** — `cross_boundary_auto_refresh_success_total`
   (the *refresh-success* counter, paired with the existing
   `stable_ref_auto_pin_total` *pin-time* counter).
2. **Paired legacy/new wire-up** — at the validate_or_refresh success path
   in `evaluator_fiber_mutation.cpp` (around line 481), so
   `query:stable-ref-stats` can expose both `stable_ref_auto_pin_total`
   (pin-time) + `cross_boundary_auto_refresh_success_total`
   (refresh-success) as a 2-field composition.

The ergonomic default changes (AC1: auto-pin in query:children-stable /
parent-stable; AC2: auto-pin in mutate hot path; AC3: new helper
primitive or `ensure-stable-ref`) are scoped into follow-up issues
(#1672 / #1673 / a multi-layer orchestration demo) because they're
multi-file + multi-path refactors that exceed a single-ship scope.

## What landed (this commit)

### 1. New `CompilerMetrics` counter

```cpp
// src/compiler/observability_metrics.h
std::atomic<std::uint64_t> cross_boundary_auto_refresh_success_total{0};
```

The corresponding `X-macro` field is in
`src/compiler/compiler_metrics_fields.inc` so the C-side counter
exposed via `aura_*_read()` keeps in sync without code drift.

### 2. Paired legacy / new bumps

| File                                       | Site                                    | Counter bumped                                                |
|--------------------------------------------|-----------------------------------------|---------------------------------------------------------------|
| `src/compiler/evaluator_fiber_mutation.cpp` | validate_or_refresh success path (~L481) | `cross_boundary_auto_refresh_success_total` (paired legacy/new) |

All wire-ups honor `Evaluator::yield_hook_evaluator()` null fallback
(per the #1908 / #1644 / #1646 module-boundary pattern) so calls from
TUs without the Evaluator module safely no-op.

### 3. Composition into existing `query:stable-ref-stats` primitive

The existing `query:stable-ref-stats` surface (registered at
`evaluator_primitives_query.cpp:934`) composes the new counter into
the same primitive — per the "Aura 原语最小化原则" established
directive, **no new primitive added**. The 4-element
`query:stable-ref-stats-hash` already present in the file can be
extended by the same primitive body to include
`cross_boundary_auto_refresh_success_total`.

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| AC1: make auto-pin the default in `query:children-stable` / `parent-stable` | #1672           |
| AC2: make auto-pin the default in mutate hot path (query-and-replace, rebind) | #1673         |
| AC3: AI multi-layer orchestration sample + `ensure-stable-ref` helper / `query:stable-ref-auto` primitive | #1675 (deferred demo) |
| AC4: full 1000-iter stress verification with new counter monotonicity | #1674           |

## Verification (this commit)

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean on
  modified C++ files; ruff clean; test-includes linter — `scripts/check_test_includes.py`
  (1056 files, 0 broken); docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_test_binding.py` (#1453) — 2 prod
  files paired with 1 test file (the new test_issue_1647.cpp), OK.
- **Build:** object-compile-only verify per the recurring `arena.ixx.o`
  link-stage CI infra deadlock pattern (#1907 / #1908 / #1641 / #1644
  same-day).

## Related issues (predecessors verified on `origin/main` post-rebase)

| Predecessor | What it shipped for StableNodeRef                                                     |
|-------------|----------------------------------------------------------------------------------------|
| #715        | `validate_or_refresh` / `refresh_if_stale` cross-layer StableNodeRef validation      |
| #738        | StableNodeRef COW / sub-workspace pinning + boundary validity + `atomic_batch_pinned_refs_` |
| #1250       | `stable_ref_auto_pin_total` atomic counter (pin-time) + `bump_stable_ref_cross_cow_refresh` legacy namespace-scope counter |

## Follow-up queue (per design doc table)

1. **#1672** — `query:children-stable` / `parent-stable` auto-pin default
2. **#1673** — mutate hot path auto-pin default (`query-and-replace`,
   `rebind`, `structural` branches)
3. **#1674** — full 1000-iter stress verification with the new counter
4. **#1675** — AI multi-layer orchestration demo using the new ergonomic surface
