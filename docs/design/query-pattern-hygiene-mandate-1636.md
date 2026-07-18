# Mandate MacroIntroduced hygiene in query:pattern (#1636)

**Issue:** [#1636](https://github.com/cybrid-systems/aura/issues/1636)  
**Builds on:** #1609 · #1501 · #1047 · #547 · #850  
**Status:** Production mandate — default force-skip + user-only tag_arity index.

## Contract

| Layer | Behavior |
|-------|----------|
| Default `query:pattern` | **Force skip** MacroIntroduced (root + full walk + recursive matcher) |
| `:allow-macro-introduced #t` | Opt-in include macro nodes |
| Index hot path | `tag_arity_index_user_` via `snapshot_tag_arity_bucket(..., skip_macro=true)` |
| Marker dimension | Parallel user-only map (**not** packing marker into `TagArityKey`) |

Defense in depth:

1. Root / full-walk filter in `evaluator_primitives_query_workspace.cpp`
2. Recursive `QueryMatcher::match_subtree` hygiene gate
3. User-only tag_arity index (MacroIntroduced never in default bucket)

## Metrics (`query:pattern-hygiene-stats`, schema **1636**)

| Key | Meaning |
|-----|---------|
| `root-skips` / `recursive-skips` | Skip counters |
| `macro_introduced_skipped_in_pattern_total` | root + recursive (#1636 AC name) |
| `hygiene_violation_prevented_total` | hygiene-violations alias |
| `hygiene-index-served` | User-only index serves |
| `marker-dimension-via-user-index-wired` | 1 |
| `core-loop-force-skip-wired` | 1 |
| `schema` | **1636** (lineage 1609 / 1501 / 547) |

## Tests

| File | Role |
|------|------|
| `tests/test_query_pattern_hygiene_mandate_1636.cpp` | **#1636** AC |
| `tests/test_query_pattern_hygiene_1609.cpp` | Prior force-filter |
| `tests/test_query_pattern_hygiene_macrointroduced.cpp` | Closed-loop matrix |

## Related

- `docs/design/query-pattern-hygiene-1609.md`
- `docs/design/query-pattern-hygiene-index.md`
