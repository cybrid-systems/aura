# query:pattern Hygiene + Marker-Aware tag_arity Index

**Issues:** #1501 (marker index), #140 / #421 / #547 / #593 (matcher hygiene)

## Contract

| Path | MacroIntroduced handling |
|------|--------------------------|
| Default `query:pattern` | **Skip** MacroIntroduced roots + recursive subtrees |
| `:allow-macro-introduced #t` / `:include-macro-introduced #t` | Include |
| `:exclude-macro-introduced #t` | Explicit skip (default) |
| `:respect-hygiene` | Alias (see #547; value maps to include flag) |

## Index structure (#1501)

Evaluator maintains **two** `(tag, arity) → [NodeId]` maps under `tag_arity_index_mtx_`:

| Map | Contents |
|-----|----------|
| `tag_arity_index_` | All live indexable nodes |
| `tag_arity_index_user_` | Only nodes with `!is_macro_introduced` |

`snapshot_tag_arity_bucket(key, trigger, skip_macro)`:

- `skip_macro=true` (default query path) → serve **user** map; bump `tag_arity_hygiene_index_served_total`
- `skip_macro=false` → serve full map

Matcher still applies root + recursive MacroIntroduced filters as defense in depth.

## Stats

```
(engine:metrics "query:pattern-hygiene-stats")   ; hash schema 1609 (authoritative, #1609)
(engine:metrics "query:macro-hygiene-stats")     ; hash schema 1609 (lineage 1501)
```

| Field | Meaning |
|-------|---------|
| `root-skips` | MacroIntroduced skipped at root walk |
| `recursive-skips` | Recursive matcher skips |
| `hygiene-violations` | Violation counter |
| `total` | root-skips + violations (#547 sum) |
| `macro-markers` | Workspace MacroIntroduced tally |
| `hygiene-index-served` | User-only tag_arity snapshot serves (#1501) |
| `schema` | **1609** |

## Tests

- `tests/test_query_pattern_hygiene_1609.cpp` — **#1609** AC
- `tests/test_issue_1501.cpp` — index hygiene + flags + stats
- `tests/test_query_pattern_hygiene_macrointroduced.cpp` — closed-loop matrix
