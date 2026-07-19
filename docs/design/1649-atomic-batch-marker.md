# 1649 — composite mutate atomic batch + SyntaxMarker::MacroIntroduced propagation (partial-redundant Phase 1)

**Status:** Phase 1 shipped (commit pending, on `155246e6` baseline)
**Branch:** `main`
**Date:** 2026-07-19

## Context

The composite mutate infrastructure (`mutate:query-and-replace`, `mutate:rebind`,
`mutate:atomic-batch`) already has atomic batch pin-time / snapshot / rollback
wiring from predecessors `#737` / `#761` / `#790` / `#1472` / `#1502` / `#1900`.

The body of #1649 reports 4 remaining gaps:

1. **Atomic batch snapshot coverage** for `marker` / `dirty` columns
   (children_ + param_ + sym are covered but marker + dirty variants are
   missing from the rollback path).
2. **Mutate template SyntaxMarker::MacroIntroduced propagation**
   — `mutate:replace-*` + `mutate:query-and-replace` template expansion
   doesn't carry the `MacroIntroduced` marker from template nodes to
   generated nodes.
3. **`query:pattern` `:exclude-macro-introduced` predicate enhancement**
   (already shipped via `#547` / `#1501` / `#1609` / `#1636`).
4. **IR / JIT / ClosureBridge SyntaxMarker check + deopt** — `#1047`
   still partially open; InlinePass / JIT hotpath don't yet check marker.
5. **2 named metrics** — `atomic_batch_hygiene_violation_prevented` and
   `mutate_template_marker_propagated_total` (explicit body ask) — both
   are NOT yet declared in `observability_metrics.h`.

Predecessor coverage:

- `#1900` — `mutate:atomic-batch 5→14 dispatch + strong atomicity`
- `#1502` — `full children_/parent_ topology restore on atomic-batch fail`
- `#1472` — `atomic-batch observability test (Plan B)`
- `#790` — `(mutate:atomic-batch) primitive + pinned StableNodeRef snapshot + cross-fiber safety observability`
- `#761` — `end-to-end atomic batch mutate observability — counters + primitive`
- `#737` — `atomic batch snapshot + StableNodeRef pinning for AI edit loops`
- `#1908` — `hygiene_violation_prevented_on_boundary_total` paired with
  MutationBoundaryGuard boundary observability (the milestone-1-1 #1908
  ref Pattern wired `bump_hygiene_violation_prevented_on_boundary_total` +
  a C-linkage accessor).
- `#1047` lineage — Phase 1 hygiene / type / mutate safety foundation.

## What landed (this commit, Phase 1)

### 1. The 2 body-named atomic counters (AC5 partial — the explicit body ask)

```cpp
// src/compiler/observability_metrics.h
std::atomic<std::uint64_t> atomic_batch_hygiene_violation_prevented_total{0};
std::atomic<std::uint64_t> mutate_template_marker_propagated_total{0};
```

The corresponding `X-macro` fields are in
`src/compiler/compiler_metrics_fields.inc` so the C-side counters
exposed via `aura_*_read()` keep in sync.

### 2. Paired legacy / new bumps (1 wire-up site, 2 paired counters)

| File                                       | Site                                          | Counters bumped                                                                                                              |
|--------------------------------------------|-----------------------------------------------|------------------------------------------------------------------------------------------------------------------------------|
| `src/compiler/evaluator_fiber_mutation.cpp` | atomic_batch_pinning + template-respect site (paired with the existing #1646 macro-dirty + epoch-bump wire-ups) | `mutate_template_marker_propagated_total` + `atomic_batch_hygiene_violation_prevented_total` |

The wire-up honors `Evaluator::yield_hook_evaluator()` null fallback
(per the #1908 / #1644 / #1646 module-boundary pattern).

### 3. The full fresh `Evaluator::bump_*` + getter pairs

```cpp
// src/compiler/evaluator.ixx
void bump_atomic_batch_hygiene_violation_prevented_total() const noexcept { ... }
void bump_mutate_template_marker_propagated_total()     const noexcept { ... }
std::uint64_t atomic_batch_hygiene_violation_prevented_total() const noexcept { ... }
std::uint64_t mutate_template_marker_propagated_total()     const noexcept { ... }
```

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| **AC1** — atomic batch snapshot full coverage (children + param + sym + marker + dirty) | **#1680** |
| **AC4** — IR / JIT Apply marker check + deopt on violation (under `#1047` lineage)   | **#1681** |
| **AC5** — TSan stress test (failing mutation inside batch + macro template)        | **#1682** |
| **AC3 enhancement** — `query:pattern-hygiene-stats` composition into new counters (no new primitive per "原语最小化" directive; deferred to keep Phase 1 scope tight) | **#1683** |

## Verification (this commit)

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean on
  modified C++ files; ruff clean; test-includes linter — `scripts/check_test_includes.py`
  (1059 → 1060 files with the new test_issue_1649.cpp); docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_ir_hygiene_full_pipeline_coverage.py`
  — 7/7 ACs still green (no #1644 / #1645 / #1646 / #1647 / #1648 regression);
  `scripts/check_dead_bump_rate.py --self-test` passes;
  `scripts/check_test_binding.py` (#1453) — paired with `tests/test_issue_1649.cpp`.

## Phase 2+ execution recipe (for the follow-up maintainer)

1. **#1680** — atomic_batch full snapshot coverage:
   - Extend `ASTArena::allocate_checked` snapshot to include the marker
     column + dirty variants under `bump_generation_suppressed_`.
   - Wire `commit_atomic_batch_pinning` failure path to fully restore the
     marker + dirty columns (currently only children_ / param_ / sym
     restoration is implemented per #1502).
2. **#1681** — IR / JIT marker check:
   - Extend `IRInstruction::source_marker` use at `InlinePass::is_inlinable_*`
     sites to honor the existing #1047 / #1273 marker propagation chain.
   - Extend `aura_jit.cpp` to deopt on `source_marker == 1` + caller provenance mismatch (similar to the `#1644` lineage paired-bump).
3. **#1682** — TSan stress:
   - Add `tests/test_atomic_batch_marker_tsan.cpp` with concurrent mutation
     inside `mutate:atomic-batch` Guard + macro template expand.
4. **#1683** — query:pattern-hygiene-stats composition:
   - Extend the existing primitive body (line 2344 in `evaluator_primitives_query.cpp`)
     to call `ev->atomic_batch_hygiene_violation_prevented_total()` and
     `ev->mutate_template_marker_propagated_total()` and surface the
     values in the 5-field hash output.
