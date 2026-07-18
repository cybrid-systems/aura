# Nested lambda per-block targeted dirty cascade (#1625)

**Issue:** [#1625](https://github.com/cybrid-systems/aura/issues/1625)  
**Builds on:** #1505 · #1514 · #1474 · #1495 · #1261  
**Status:** Production closed-loop.

## Problem

#1505 already avoided full-entry dirty for `irs.size()>2` dependents
(body-only + free_vars scan), but a free-var hit still marked **all**
blocks of that nested lambda. Multi-block nested closures paid a full
nested re-lower even when only the entry/capture site references the
mutated name.

## Solution

```
mark_define_dirty(name) / BFS cascade dependent:
  body (irs[1]) → all body blocks dirty (call site)
  for fi in 2..N:
    if nested free_vars or ConstString refs name:
      scan blocks for ConstString hits → mark only those
      else free_vars only → mark entry_block only
    else leave nested completely clean
  metrics:
    dep_graph_nested_lambda_targeted_dirty_total++
    dep_graph_nested_lambda_blocks_targeted_total += marked
    dep_graph_nested_lambda_blocks_kept_clean_total += kept
```

Helper: `mark_nested_lambda_blocks_targeted(entry, fi, mutated_name)`.

## Metrics (`query:production-sweep-1261-1265-stats`, schema **1625**)

No new `query:*-stats`.

| Key | Meaning |
|-----|---------|
| `dep-graph-nested-lambda-targeted-dirty` | Cascade targeted hits |
| `dep-graph-nested-lambda-blocks-targeted` | Blocks marked dirty |
| `dep-graph-nested-lambda-blocks-kept-clean` | Nested blocks left clean |
| `nested-lambda-per-block-targeted-wired` | 1 |
| `schema` | **1625** (lineage 1261\|1505) |

## Tests

| File | Role |
|------|------|
| `tests/test_issue_1625_nested_lambda_targeted.cpp` | **#1625** AC |
| `tests/test_issue_1505.cpp` | Lineage |
| `tests/test_production_sweep_1261_1265.cpp` | schema 1625\|1261 |

## Related

- `src/compiler/service.ixx` — `mark_define_dirty` / cascade
