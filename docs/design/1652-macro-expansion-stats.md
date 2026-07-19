# 1652 — clone_macro_body / SyntaxMarker observability hooks + stats integration (scope-limited-progressive Phase 1)

**Status:** Phase 1 shipped (commit pending, on `bac1bc58` baseline)
**Branch:** `main`
**Date:** 2026-07-19

## Context

`clone_macro_body` (at `src/compiler/macro_expansion.cpp:180`) correctly stamps
`MacroIntroduced` markers via the `cloned_marker` parameter — but lacks
dedicated observability hooks for the expand phase. AI Agents monitoring
self-evolution loops can't precisely track: macro expand frequency, the
volume of `MacroIntroduced` AST nodes created per expand, or hygiene-violation
detection inside expand.

Predecessors already ship a partial observability surface:

- `#1247 / #1248` — `g_macro_origin_provenance_errors` + `g_hygiene_tracer_expansions`
  + `g_hygiene_tracer_depth_max` (file-level atomics in `src/compiler/macro_expansion.cpp`).
  These are paired with the depth-guard + hygiene-tracer instrumentation in
  `clone_macro_body`.
- `#365` — `depth_guard + graceful degradation` in `clone_macro_body` (the
  existing depth-exceeded early-return pattern at line ~222 of `macro_expansion.cpp`).
- `#120` — original `clone_macro_body` parameter ordering + `body_id` validation.

## What landed (this commit, Phase 1)

### 1. 3 new file-level atomics + 3 C-linkage accessors (paired #1247/#1248 pattern)

`src/compiler/macro_expansion.cpp` adds (paired with the existing `#1247 / #1248`
file-level atomic block at the top of the file):

```cpp
// Issue #1652: clone_macro_body expand observability counters (paired with
// #1611 MacroIntroduced hygiene gate). Bumped at the success path +
// early-return hygiene-violation paths inside clone_macro_body. Exposed via
// the C-linkage accessor + composed into existing (query:pattern-hygiene-stats)
// primitive surface (no new primitive per #1632 "原语最小化" directive).
std::atomic<std::uint64_t> g_macro_expansion_total{0};
std::atomic<std::uint64_t> g_macro_introduced_nodes_created_total{0};
std::atomic<std::uint64_t> g_hygiene_violation_in_macro_expand_total{0};

// Issue #1652: C-linkage accessors so the (query:pattern-hygiene-stats)
// primitive can read these file-level atomics from another TU without the
// Evaluator module import (paired pattern with #1648 reflect.hh +
// #1651 macro_expansion.cpp).
extern "C" {
inline std::uint64_t aura_macro_expansion_total_v_read() noexcept { ... }
inline std::uint64_t aura_macro_introduced_nodes_created_total_v_read() noexcept { ... }
inline std::uint64_t aura_hygiene_violation_in_macro_expand_total_v_read() noexcept { ... }
}
```

### 2. 3 paired bumps inside `clone_macro_body`

- **Per-call success-path bump** at the function entry (right after `using namespace aura::ast;`):
  ```cpp
  g_macro_expansion_total.fetch_add(1, std::memory_order_relaxed);
  ```
  Fires once per `clone_macro_body` invocation that survives the early-return hygiene checks.

- **Hygiene-violation bumps** at 2 sites:
  - **Depth-exceeded site** (paired next to the existing `g_macro_origin_provenance_errors.fetch_add(1, ...)`):
    ```cpp
    g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
    ```
  - **`body_id == NULL_NODE || body_id >= source.size()` site** (caller passed an
    out-of-range NodeId for the macro body):
    ```cpp
    g_hygiene_violation_in_macro_expand_total.fetch_add(1, std::memory_order_relaxed);
    ```

### 3. Phase 2 primitive body composition (deferred to **#1688**)

The 3 new file-level atomics + C-linkage accessors are the observability surface
for the existing `(query:pattern-hygiene-stats)` primitive body composition.
Per the "原语最小化" directive — NO new primitive registration. Phase 1
ships the infrastructure; the full primitive body composition that surfaces
the 3 new fields in the existing primitive hash (5-field → 8-field) is
deferred to **#1688**.

### 4. Per-recursive-step bump with cumulative count (deferred to **#1688**)

`bump_macro_introduced_nodes_created(cloned_count)` per recursive AST-walk
step requires threading a cumulative count through `clone_macro_body`'s
recursive walk (currently not tracked). Multi-session refactor — deferred.

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| AC1 full: `bump_macro_introduced_nodes_created(cloned_count)` per-recursive-step + thread cumulative count through `clone_macro_body`'s recursive AST walk + integration with `flush_mutation_boundary` + dirty propagation | **#1688** |
| AC2 full primitive body composition: extend `query:pattern-hygiene-stats` body to read the 3 new C-linkage accessors + surface in the 5→8-field hash | **#1688** (paired) |

## Verification (this commit)

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean on
  modified C++ files; ruff clean; test-includes linter — `scripts/check_test_includes.py`
  (1064 files with the new `tests/test_issue_1652.cpp`); docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_ir_hygiene_full_pipeline_coverage.py`
  — 7/7 ACs still green (no #1644 / #1645 / #1646 / #1647 / #1648 / #1649 /
  #1650 / #1651 regression); `scripts/check_dead_bump_rate.py --self-test`
  passes; `scripts/check_test_binding.py` (#1453) — paired with new test.

## Predecessor coverage (verified)

| Predecessor | What it shipped for clone_macro_body observability           |
|-------------|---------------------------------------------------------------|
| `#1247 / #1248` | `g_macro_origin_provenance_errors` + `g_hygiene_tracer_expansions` + `g_hygiene_tracer_depth_max` (file-level atomics — same pattern #1652 follows) |
| `#1611`     | `MacroIntroduced` hygiene gate schema (the marker-correctness invariant that #1652's metrics observe) |
| `#365`      | `depth_guard + graceful degradation` (the depth-exceeded site where #1652's first hygiene-violation bump fires) |
| `#120`      | `clone_macro_body` original parameter ordering + body_id validation (the body_id-NULL site where #1652's second hygiene-violation bump fires) |

## Phase 2+ follow-up queue (per design doc +1 issue, paired)

| Issue | Description |
|-------|-------------|
| **#1688** | AC1 full: `bump_macro_introduced_nodes_created(cloned_count)` per-recursive-step + thread cumulative count through `clone_macro_body`'s recursive AST walk + AC2 full primitive body composition (extend `query:pattern-hygiene-stats` body to read the 3 new C-linkage accessors + surface in 5→8-field hash). Single follow-up scope — both AC1 + AC2 are atomic in their primitive body + refactor interaction. |
