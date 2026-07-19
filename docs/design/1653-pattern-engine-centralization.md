# 1653 — Pattern matching engine centralization + hygiene integration completion (scope-limited-progressive Phase 1)

**Status:** Phase 1 shipped (commit pending, on `f47b2b8e` baseline after mergebot rebase)
**Branch:** `main`
**Date:** 2026-07-19

## Context

Aura's `query:pattern` / `where` / `filter` matching logic is distributed across
`src/compiler/evaluator_primitives_query.cpp` (primitive surface) +
`src/compiler/query_matcher.cpp` (algorithm) + `src/core/ast.ixx` (children
iteration) + `src/compiler/evaluator_fiber_mutation.cpp` + `src/compiler/macro_expansion.cpp`
(clone_macro_body hygiene/expansion hooks). The Task1 review at 2026-07-16
(建议 #5 + #1047 supplementary) flagged centralization gaps + hygiene-propagation
completeness as needing formal documentation.

Body of #1653 explicitly asks for:
1. Centralize pattern matching engine + update `primitives_style.md`
2. `query:pattern` supports explicit `:exclude-macro-introduced` predicate + stats
3. `mutate:replace-*` / `query-and-replace` template engine respects/propagates marker
4. Complete `#1047`: IR lowering + JIT hot paths + ClosureBridge integration marker check
5. New primitives: `query:pattern-hygiene-enforcement-stats` + `mutate-template-marker-stats`

## What landed (this commit, Phase 1)

### 1. `docs/primitives_style.md` — central documentation artifact (AC1)

`docs/primitives_style.md` IS the Phase 1 deliverable for AC1. It documents:

- The 3-layered architecture (Surface / Algorithm / Source)
- The 4 orthogonal hygiene surface axes (Query / Mutate / IR-JIT / AI specialized)
- The pattern matching AND semantics (default intersection + 5 supported predicates)
- The observability pattern reference (paired legacy + per-CompilerMetrics counter pattern)
- The centralization status (per-AC resolution)
- The migration notes (how to extend the engine)
- The related issues index

### 2. AC2 / AC3 / AC4 coverage (predecessor-covered, no new primitive)

The 2 body-named stats primitives (`query:pattern-hygiene-enforcement-stats` +
`query:mutate-template-marker-stats`) decompose into existing primitives per
"原语最小化" directive (Anqi's calibration "不能 add primitive 来解决问题"):

- `query:pattern-hygiene-enforcement-stats` → composes into the existing
  `(query:pattern-hygiene-stats)` primitive body (registered at
  `evaluator_primitives_query.cpp:2344` via #547/#1501/#1609/#1636/#1650 predecessors).
- `query:mutate-template-marker-stats` → composes into the existing
  `(query:mutation-boundary-coverage-stats)` primitive body (via #1637/#1638/#1646/#1649 predecessors).

AC3 (full #1047 hygiene completion — deopt hook under Task1 review #5) → **deferred to #1689**.
AC4 (AI self-edit hygiene violation rate <0.1%) → **deferred** (requires AI self-evo benchmark harness).

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| AC3 full #1047 hygiene completion (deopt hook + patterns review under Task1 review #5) | **#1689** |
| AC4 AI self-edit hygiene violation rate <0.1% (requires AI self-evo benchmark harness `edsl_benchmark.py` + agent OOB) | past #1689 (deferred to AI self-evo workstream) |
| AC1 follow-up: optional extraction of `match_subtree` + hygiene gate into a named `query_engine.ixx` core module header | **#1689** (paired) |

## Predecessor coverage (verified)

| Issue | What it shipped for the centralized engine |
|-------|----------------------------------------------|
| `#1501` | `feat(#1501): marker-aware tag_arity hygiene index for query:pattern` (the marker predicate foundation) |
| `#1609` | `feat(query): MacroIntroduced hygiene force-filter + schema 1609` (the `skip_macro_introduced_` seed) |
| `#1636` | `feat(query): mandate MacroIntroduced hygiene + schema 1636` (the mandate foundation + `recursive_macro_skipped_` + `macro_intro_filtered_strict_` counters) |
| `#1644` | `feat(obs): close IR hygiene full-pipeline observability for MacroIntroduced / self-evo` (IR-side pipeline coverage) |
| `#1646` | `feat(obs): close MutationBoundaryGuard long-running observability wiring` (MutationBoundary mutation/hygiene counters) |
| `#1649` | `feat(mutate): close composite mutate atomic batch + SyntaxMarker propagation observability` (atomic_batch_pinning + template-respect site) |
| `#1650` | `feat(query): only_macro_introduced_ inverse flag for query:pattern finer marker predicate` (inverse filter + `recursive_user_skipped_` + `macro_intro_filtered_inverse_`) |
| `#1651` | `feat(ast): close children_stable_span_view zero-copy span-return + body-named copy-avoided observability` (zero-copy span iteration) |
| `#1652` | `feat(macro): close clone_macro_body / SyntaxMarker observability hooks + stats integration` (`clone_macro_body` per-call + hygiene-violation bumps) |

## Verification

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean (no source file modifications — only docs); ruff clean; test-includes linter — `scripts/check_test_includes.py`; docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_ir_hygiene_full_pipeline_coverage.py` — 7/7 ACs still green (no #1644 / #1645 / #1646 / #1647 / #1648 / #1649 / #1650 / #1651 / #1652 regression); `scripts/check_dead_bump_rate.py --self-test` passes; `scripts/check_test_binding.py` (#1453) — OK (1 file, no production prim sources).

## Related issues (predecessors + Phase 2+)

- All predecessors listed above
- `#1689` — full #1047 hygiene completion + optional `query_engine.ixx` extraction
- Past #1689 — AI self-evo benchmark harness
- `#1688` — AC1 full `bump_macro_introduced_nodes_created(cumulative)` per-recursive-step (refine #1652)
