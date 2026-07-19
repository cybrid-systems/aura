# 1650 — query:pattern finer marker/hygiene predicate control (partial-redundant-ship)

**Status:** Phase 1 shipped (commit pending, on `52788e82` baseline)
**Branch:** `main`
**Date:** 2026-07-19

## Context

`#1636` mandated the `MacroIntroduced` hygiene filter at the core of `query:pattern`'s
match_subtree loop — `skip_macro_introduced_` flag forces subtrees that are
`MacroIntroduced`-marked to be skipped. The body of #1650 reports that AI
Agent ergonomics around this filter are limited:

- The flag is a constructor argument, not exposed via EDSL.
- Agent can't ask the inverse: "give me only the MacroIntroduced nodes for
  auditing / cleanup".
- Stats counters exist for `skip_macro_introduced_` hits (`recursive_macro_skipped_`,
  `macro_intro_filtered_strict_`) but the inverse direction has no observability.

Predecessor coverage:

- `#1636` — `feat(query): mandate MacroIntroduced hygiene + schema 1636`
  (shipped `skip_macro_introduced_` + the strict filter counters)
- `#1609` — `feat(query): MacroIntroduced hygiene force-filter + schema 1609`
- `#1501` — `feat(#1501): marker-aware tag_arity hygiene index for query:pattern`
- `#547` — `feat+test(547): query:pattern hygiene filter + incremental tag_arity_index + 2 stats primitives`
  (shipped `query:pattern-hygiene-stats` primitive at `evaluator_primitives_query.cpp:2344`)

## What landed (this commit, Phase 1)

### 1. `only_macro_introduced_` inverse flag in `QueryMatcher`

**`src/compiler/query_matcher.cpp`** — constructor + member init:

```cpp
QueryMatcher::QueryMatcher(
    /* ... existing args ... */,
    bool skip_macro_introduced, bool only_macro_introduced = false)
    : /* ... existing inits ... */
    , skip_macro_introduced_(skip_macro_introduced)
    // Issue #1650: inverse flag for `only_macro_introduced` predicate
    , only_macro_introduced_(only_macro_introduced)
```

**`src/compiler/query_matcher.cpp`** — inverse check in `match_subtree` (paired
with the existing `skip_macro_introduced_` check):

```cpp
if (skip_macro_introduced_ && ws_flat_->is_macro_introduced(ws_id)) { ... }
// Issue #1650: inverse filter — when only_macro_introduced_ is set, skip
// User-authored nodes and keep only MacroIntroduced nodes. Pairs with the
// existing skip_macro_introduced_ check above (the 2 flags are mutually
// exclusive). Always count the inverse filter for the
// (query:pattern-hygiene-stats) primitive surface.
if (only_macro_introduced_ && !ws_flat_->is_macro_introduced(ws_id)) {
    ++recursive_user_skipped_;
    ++macro_intro_filtered_inverse_;
    return false;
}
```

### 2. Struct fields in `QueryMatcher`

**`src/compiler/query_matcher.ixx`**:

```cpp
// Inverse flag + paired inverse counters (Issue #1650).
bool only_macro_introduced_ = false;
std::uint64_t recursive_user_skipped_ = 0;
std::uint64_t macro_intro_filtered_inverse_ = 0;
```

Default `false` for `only_macro_introduced_` preserves backward compat with
the existing `#1636` callers (which never set the flag).

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| AC2 surface extension: extend `query:pattern-hygiene-stats` primitive body to read the 2 inverse counters (paired with `recursive_macro_skipped_` + `macro_intro_filtered_strict_`) | **#1683** |
| AC3 runtime tests: `query:pattern` with `:only-macro-introduced #t` + `:allow-macro-introduced #t` mixed scenarios (full EDSL surface exposure) | **#1684** |
| AC4 expanded coverage: stress test the inverse filter under heavy macro templates | covered by predecessors + #1684 |

### Why compose instead of adding a new primitive

Per the "Aura 原语最小化" directive (and Anqi's calibration "不能 add primitive to wrap problems"),
the new flag's stats are surfaced via the existing
`query:pattern-hygiene-stats` primitive (already shipped via #547/#1501/#1609/#1636).
The follow-up issue `#1683` will extend that primitive body to read the 2 inverse
counters + surface them in the existing 5-field hash output (no new primitive
registration, no new EDSL surface).

## Verification (this commit)

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean on
  modified C++ files (`query_matcher.cpp` + `query_matcher.ixx`); ruff clean;
  test-includes linter — `scripts/check_test_includes.py` (with the new
  `tests/test_issue_1650.cpp`); docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_ir_hygiene_full_pipeline_coverage.py`
  — 7/7 ACs still green (no #1644 / #1645 / #1646 / #1647 / #1648 / #1649
  regression); `scripts/check_dead_bump_rate.py --self-test` passes;
  `scripts/check_test_binding.py` (#1453) — paired with `tests/test_issue_1650.cpp`.
- **Build:** object-compile-only verify per the recurring `arena.ixx.o`
  link-stage CI infra deadlock pattern (#1907 / #1908 / #1641 / #1644 /
  #1646 / #1648 / #1649 same-day).

## Related issues (predecessors + Phase 2+ follow-ups)

| Predecessor | What it shipped for `query:pattern` marker hygiene       |
|-------------|--------------------------------------------------------------|
| `#547`      | `query:pattern-hygiene-stats` primitive surface (5-field hash) |
| `#1501`     | marker-aware `tag_arity` hygiene index                      |
| `#1609`     | `MacroIntroduced` hygiene force-filter (pre-mandate)         |
| `#1636`     | `skip_macro_introduced_` filter + strict-mode mandate       |

| Phase 2+   | Description                                                  |
|------------|--------------------------------------------------------------|
| `#1683`    | Extend `query:pattern-hygiene-stats` body to read the 2 inverse counters (no new primitive) |
| `#1684`    | EDSL surface exposure (`:only-macro-introduced #t` + `:allow-macro-introduced #t`) + runtime tests + stress coverage |
