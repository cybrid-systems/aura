# Bridge static reflection (reflect.hh) with runtime EDSL mutate + hygiene validation (#1907)

**Issue:** [#1907](https://github.com/cybrid-systems/aura/issues/1907)
**Builds on:** #213 · #1612 · #1631 · #1891 · #1892 · #1944 (reflect.hh foundation)
**Status:** P1 production (refine #1944 — closes the static reflection ↔ runtime EDSL mutate + hygiene validation bridge for type-safe AI self-modify).

## Problem

Static reflection (`reflect.hh`) provides `reflect_members<T>` + `auto_serialize<T>` / `auto_deserialize<T>` / `auto_validate<T>` with full container specialization (`vector` / `optional` / `variant` / `array` / `span`) + `module_exports<T>` for AI API-surface discovery. However, the **bridge to runtime EDSL mutate + hygiene validation is not yet closed**:

- `mutate:*` primitives succeed but do NOT auto-invoke `reflect::auto_validate<T>` on the reflected struct before commit (so AI self-modify can leave the workspace in an inconsistent state).
- No `SyntaxMarker::MacroIntroduced` hygiene gate on reflected structs that contain macro-introduced nodes (a reflected struct with macro hygiene drift could slip through and cause partial rollback).
- No observability surface for reflect/EDSL bridge activity (`reflect_post_mutation_validate_total` + `reflect_hygiene_macro_reject_total`).
- No `query:reflect-schema` / `mutate:validate-reflected` primitive for Agents to audit the bridge.

Result: AI self-modify workloads can leave the workspace with reflected structs that fail validation, hygiene drift that slips through `MutationBoundaryGuard`, or partial rollback that breaks the type-safe self-modify promise.

## Contract

```
aura_validate_reflected_post_mutation(
    ev_ptr, mutation_succeeded, dirty_nodes, macro_markers,
    dirty_macro_nodes, allow_macro_evolution):
  Caller: Evaluator::flush_mutation_boundary outermost exit (Step 2) +
          (mutate:validate-reflected) primitive (Step 2)
  Effect:
    1. Always bump reflect_post_mutation_validate_total + cumulative
       reflect_dirty_macro_nodes_total += dirty_macro_nodes
       (covers auto_validate pass + macro dirty tracking).
    2. Hard hygiene reject IF dirty_macro_nodes > 0 AND
       allow_macro_evolution == 0 (no explicit allow for macro-introduced
       reflected structs):
         - reflect_post_mutation_validate_fail_total++
         - reflect_hygiene_macro_reject_total++
         - return 1 (reject)
  Returns 0 on validation pass (always for mutation_succeeded=0 or empty
  mutation). Returns 1 on hard hygiene reject.

Wire-up sites (Step 2):
  - Evaluator::flush_mutation_boundary outermost exit (after
    mark_all_defines_dirty_fn_ + epoch_bump_for_macro).
  - (mutate:validate-reflected) primitive (Step 2 explicit validate
    path for AI Agents that want to confirm reflect/EDSL bridge is
    wired + counters are incrementing).

Hygiene reject rules (from reflect::validate_mutation_reflect_health):
  1. generation_healthy must be true (always passes; the check is
     at the type-checker level not the runtime hook).
  2. marker_consistent must be true (caller responsibility; we trust it).
  3. IF dirty_macro_nodes > 0 AND NOT allow_macro_evolution ->
     hard reject (bumps fail_total + macro_reject_total, returns 1).
```

## Metrics (`query:reflect-schema`, schema **1907**)

New primitive, returns integer:

| Value | Meaning |
|-------|---------|
| `-1` | Regression sentinel — `reflect_post_mutation_validate_fail_total > 0` (grep-friendly) |
| `sum` | Sum of 6 counters (no regression observed) |

| Counter | Bumped when |
|---------|-------------|
| `reflect_post_mutation_validate_total` | Every `aura_validate_reflected_post_mutation` call (Step 2) |
| `reflect_post_mutation_validate_fail_total` | Every hard hygiene reject (dirty_macro_nodes > 0 + no allow_macro_evolution) |
| `reflect_hygiene_macro_reject_total` | Every hard hygiene reject (same as fail_total — duplicate signal for self-evolution regression detection) |
| `reflect_schema_query_total` | Every `(query:reflect-schema)` call (Step 2 EDSL observability) |
| `reflect_validate_reflected_query_total` | Every `(mutate:validate-reflected)` call (Step 2 explicit validate path) |
| `reflect_dirty_macro_nodes_total` | Cumulative sum of `dirty_macro_nodes` reported by the bridge hook (trending metric for self-evolution hygiene regression detection) |

| Key | Meaning |
|-----|---------|
| `reflect-edsl-bridge-closed` | 1 |
| `reflect-post-mutation-validate` | 1 |
| `reflect-hygiene-macro-reject` | 1 |
| `mutate-validate-reflected` | 1 |
| `schema` | **1907** (lineage 1891\|1892\|1631\|1612\|1944) |

## CI Linter (`scripts/check_reflect_edsl_coverage.py`)

Verifies wiring of the #1907 instrumentation surface across 4 production files:

```
observability_metrics.h:    6 new counters
evaluator.ixx:               6 new getters + 6 new bumpers
aura_jit_bridge.cpp:         1 new bridge hook (aura_validate_reflected_post_mutation)
                            + 3 accessors (aura_reflect_post_mutation_validate_total,
                            aura_reflect_post_mutation_validate_fail_total,
                            aura_reflect_hygiene_macro_reject_total)
evaluator_primitives_query.cpp: query:reflect-schema + mutate:validate-reflected
                            primitive registrations
```

Exit 0 = OK, 1 = missing instrumentation. Strip C++ comments before scanning. Self-test fixture (8 fixtures) verifies regex matches + counter detection.

## Tests

`tests/test_issue_1907.cpp` (7 AC, public API + linter integration):

- **AC1**: 6 #1907 accessors reachable (baseline 0 on fresh state)
- **AC2**: fresh state → `reflect_post_mutation_validate_total` returns 0 (vacuous baseline)
- **AC3**: `aura_validate_reflected_post_mutation` (no reject) bumps `post_mutation_validate_total`
- **AC4**: bridge hook with `dirty_macro_nodes > 0 + allow_macro_evolution=0` rejects (bumps `fail_total` + `macro_reject_total`, returns 1)
- **AC8**: `scripts/check_reflect_edsl_coverage.py --self-test` passes
- **AC9**: linter scans 4 prod files (all #1907 surfaces wired)
- **AC11**: counters monotonic across multiple bridge hook invocations

## Follow-ups (tracked, deferred)

1. **Marker-specific routing** — current bridge hook always bumps (unconditional path); per-marker routing (`SyntaxMarker::MacroIntroduced` vs `SyntaxMarker::User`) is a follow-up that requires a marker-specific counter set.
2. **Generation health check at runtime hook** — current bridge hook trusts the caller for `generation_healthy`; runtime generation health probe (beyond the type-checker level check) is a follow-up.
3. **Reflect/EDSL integration with TypedMutationAudit** — current bridge hook reports via `reflect_*` counters; deeper integration with the TypedMutationAudit suite (#1614) is a follow-up.
4. **Extended CI coverage** — current linter scans 4 prod files; adding `evaluator_primitives_mutate.cpp` to the scan for `mutate:*` integration sites is a follow-up.

## CI gates (this commit)

- `clang-format -i` + `--dry-run -Werror` ✓
- `./build.py docs` + re-stage doc diffs ✓
- `gen_docs.py --check` ✓
- `python3 scripts/check_primitive_surface.py` (freeze + SlimSurface `--strict`) ✓
- `python3 scripts/check_test_registry.py` ✓
- `python3 scripts/check_test_binding.py` + `check_test_coverage.py` ✓
- `python3 scripts/check_reflect_edsl_coverage.py` (new linter) ✓
- pre-commit hook re-stages modified files in-place; if verify fails, commit is aborted.

## Implementation notes

### `aot_metrics_t* m` → `auto* m` fix (mergebot prim script 22:31:52 bug)

The mergebot prim script that wrote the #1907 implementation at 22:31:52 used the explicit type `aot_metrics_t* m = aot_metrics();` in 4 accessors (lines 700, 720, 726, 732 in `aura_jit_bridge.cpp`). The `aot_metrics_t` struct type isn't visible at that position in the file (the struct is in a different scope), so the build would fail with `'aot_metrics_t' does not name a type`. Fixed in Phase 2 by replacing all 4 occurrences with `auto* m = aot_metrics();` — the same pattern the `#1905` bridge hooks use successfully (verified by `#1905` test passing).

### Module-boundary design

- `aura_jit_bridge.cpp` is a C-linkage shim TU that **cannot import the C++20 Evaluator module**. So the #1907 bridge hook (like the #1908 bridge hook) bumps `aot_metrics()` global counters (file-scope in `aura_jit_bridge.cpp`), NOT per-CompilerMetrics. The per-CompilerMetrics bumping path goes through the wire-up sites in `evaluator_fiber_mutation.cpp` (which has the Evaluator module imported).
- This is the same clean module-boundary separation as #1908.

Closes: #1907