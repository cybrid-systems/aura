# Wire relower_define_blocks into eval/eval_ir hot paths (#1623)

**Issue:** [#1623](https://github.com/cybrid-systems/aura/issues/1623)  
**Builds on:** #1506 · #1555 · #1601 · #1605 · #1474 · #1495  
**Status:** Production closed-loop (refine metrics + AC).

## Problem

`relower_define_blocks` + dirty bitmasks shipped, and eval/eval_ir already
preferred partial via `cache_define_prefer_partial` (#1506/#1601/#1605), but
Agents lacked explicit **eval-path** counters (`incremental_eval_relower_hits`,
`eval_path_relower_total`) and schema lineage for #1623 AC.

## Contract

```
eval() / eval_ir():
  relower_dirty_defines_from_workspace()     // dirty v2 entries first
  define Lambda → cache_define_prefer_partial(..., from_eval_ir?)
    lookup_define_v2
      0 clean → skip
      1 dirty (bitmasks) → relower_only_dirty_blocks (= relower_define_blocks)
           // #1623: try even when source_hash changed (mutate:set-body)
           true partial (per-fn/blocks) → incremental_eval_relower_hits++
           always on try → eval_path_relower_total++ (or eval_ir_path)
           internal full-fallback still via relower_define_blocks
      miss / fail → cache_define (full)
```

## Metrics (`query:incremental-relower-stats`, schema **1623**)

| Key | Meaning |
|-----|---------|
| `incremental_eval_relower_hits` | Partial wins on EDSL define path |
| `eval_path_relower_total` | eval() partial attempts |
| `eval_ir_path_relower_total` | eval_ir() partial attempts |
| `incremental_relower_blocks` | Blocks replaced (lineage) |
| `relower_full_called_count` / `full_relower_count` | Full fallback |
| `lookup-define-v2-prefer-partial` | 1 |
| `schema` | **1623** (lineage 1605\|1601\|1506) |

## Tests

| File | Role |
|------|------|
| `tests/test_eval_relower_hotpath_1623.cpp` | **#1623** AC + 200× set-body |
| `tests/test_issue_1506.cpp` / incremental_relower* | Lineage |

## Related

- `docs/design/incremental-relower-1605.md`
- `docs/design/incremental-relower-consumer-1601.md`
