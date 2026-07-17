# Incremental re-lower in eval / eval_ir (#1605)

**Issue:** [#1605](https://github.com/cybrid-systems/aura/issues/1605)  
**Refines:** #1474 · #1495 · #1505 · #1506 · #1555 · #1601  
**Status:** Production consumer closed-loop (core in sisters; #1605 AC names +
named test + schema 1605).

## Problem

Per-block dirty bitmasks on `IRCacheEntry` were orphan until eval / eval_ir /
`define_function` preferred partial re-lower over full `cache_define`.

## Contract (shipped)

```
eval() / eval_ir():
  relower_dirty_defines_from_workspace()   // dirty v2 entries first
  define path → cache_define_prefer_partial
    lookup_define_v2
      clean hit  → skip (relower_skipped_entirely)
      dirty+hash → relower_only_dirty_blocks (= relower_define_blocks)
                     dirty_func_count==1 → relower_define_function
                       per-block copy + limited CK/CF/TP/Shape
                     else full-bundle fallback
      miss       → cache_define (full)

define_function (serve) → cache_define_prefer_partial  (#1601)
```

## Metrics (`query:incremental-relower-stats`, schema **1605**)

| Key | Meaning |
|-----|---------|
| `incremental_relower_blocks` | Blocks replaced on partial path |
| `full_relower_count` | Alias of `relower_full_called_count` (#1605 AC3) |
| `relower_full_called_count` | Same counter (underscore lineage) |
| `dirty_block_ratio` / `_bp` | hits/(hits+saved)×10000 |
| `eval-prefer-partial-wired` | 1 |
| `eval-ir-prefer-partial-wired` | 1 |
| `relower-define-blocks-wired` | 1 |
| `schema` | **1605** (lineage 1601 / 718) |

## Tests

| File | Role |
|------|------|
| `tests/test_incremental_relower.cpp` | **#1605** named AC + 1000× set-body + quote/lambda/recursion |
| `tests/test_incremental_relower_consumer_1601.cpp` | #1601 consumer |
| `tests/test_issue_1506.cpp` / `1555` / `1495` / `1474` | Sisters |

## Related

- `docs/design/incremental-relower-consumer-1601.md`
- `docs/incremental_dirty_propagation.md`
