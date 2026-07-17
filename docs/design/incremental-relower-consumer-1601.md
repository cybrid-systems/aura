# Incremental re-lower consumer wiring (#1601)

**Issue:** [#1601](https://github.com/cybrid-systems/aura/issues/1601)  
**Builds on:** #1474 (bitmasks), #1495 (workspace relower), #1505 (nested cascade),
#1506 / #1555 (eval prefer partial)

## Problem

`IRCacheEntry` dirty bitmasks + `relower_define_*` helpers existed, but production
`eval` / `eval_ir` / serve `define_function` still risked full `cache_define`.

## Contract

```
cache_define_prefer_partial(name, source, …)
  lookup_define_v2(name, hash)
    0 clean hit     → relower_skipped_entirely_count++; reuse
    1 dirty + same hash
      → relower_only_dirty_blocks  (= relower_define_blocks)
           dirty_func_count==1 → relower_define_function
             per-block selective copy when shapes match
             limited dirty pipeline (CK/CF/TP/Shape)
           else full-bundle re-lower path
    2 miss / failed → cache_define (full)

eval / eval_ir define path → cache_define_prefer_partial  (#1506/#1555)
define_function            → cache_define_prefer_partial  (#1601)
(set-code) workspace       → relower_dirty_defines_from_workspace (#1495)
```

## Metrics (`query:incremental-relower-stats`, schema **1601**)

| Key | Meaning |
|-----|---------|
| `incremental_relower_blocks` | Blocks replaced on partial path |
| `relower_per_function_called_count` | Per-function re-lower entries |
| `relower_skipped_entirely_count` | Clean-hit skips |
| `relower_full_called_count` | Full-bundle re-lower |
| `dirty_block_ratio` / `_bp` | hits/(hits+saved) × 10000 |
| `eval-prefer-partial-wired` | constant 1 |

## Tests

- `tests/test_incremental_relower_consumer_1601.cpp` — 1000× set-body + metrics
- Prior: `test_issue_1506`, `test_issue_1555`, `test_issue_1474`, `test_issue_1505`

## Related

- `docs/design/apply-closure-epoch-safety.md` (orthogonal safety)
