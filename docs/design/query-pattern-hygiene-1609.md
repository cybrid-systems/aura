# query:pattern MacroIntroduced hygiene force-filter (#1609)

**Issue:** [#1609](https://github.com/cybrid-systems/aura/issues/1609)  
**Builds on:** #1501 · #1047 · #547 · #421 · #593  
**Status:** Production closed-loop (core in #1501; #1609 authoritative stats + AC test).

## Contract

| Layer | Behavior |
|-------|----------|
| Default `query:pattern` | **Force skip** MacroIntroduced at root + recursive matcher |
| `:allow-macro-introduced #t` | Include macro nodes |
| Index hot path | `tag_arity_index_user_` (user-only) via `snapshot_tag_arity_bucket(..., skip_macro=true)` |
| Matcher | `skip_macro_introduced_` → hard `return false` on MacroIntroduced |

Defense in depth (all three must fire under default hygiene):

1. Root/full-walk filter in `evaluator_primitives_query_workspace.cpp`
2. Recursive `QueryMatcher::match_subtree` hygiene gate
3. User-only tag_arity index (MacroIntroduced never in default bucket)

`TagArityKey` stays `(tag, arity)` — hygiene is a **parallel index map**, not a
third key dimension (same performance, simpler pack/delta).

## Metrics (`query:pattern-hygiene-stats`, schema **1609**)

Authoritative **hash** (was bare int sum in #547):

| Key | Meaning |
|-----|---------|
| `root-skips` | MacroIntroduced skipped at root walk |
| `recursive-skips` | Matcher recursive skips |
| `hygiene-violations` | Violation counter |
| `total` | root-skips + violations (#547 sum) |
| `hygiene-index-served` | User-only index serves |
| `core-loop-force-skip-wired` | 1 |
| `schema` | **1609** |

`query:macro-hygiene-stats` mirrors the same schema lineage.

## Tests

| File | Role |
|------|------|
| `tests/test_query_pattern_hygiene_1609.cpp` | **#1609** AC |
| `tests/test_issue_1501.cpp` | Marker index |
| `tests/test_query_pattern_hygiene_macrointroduced.cpp` | Closed-loop matrix |

## Related

- `docs/design/query-pattern-hygiene-index.md`
