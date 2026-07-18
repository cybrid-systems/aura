# EvalValue v2 consteval dispatch + hot-path Contracts (#1622)

**Issue:** [#1622](https://github.com/cybrid-systems/aura/issues/1622)  
**Builds on:** #571 · #723 · #613 · #181 · #1519  
**Status:** Production closed-loop.

## Problem

Value v2 encoding + `classify_eval_value_tag` + Contracts on `as_*` shipped
(#571), but lacked a pure consteval classifier aligned with the low-2-bit
dispatch table, and the hash `query:value-dispatch-stats` (schema 723) did
not surface process-wide hit rate / collision / contract counters for Agents.

## Solution

| Piece | Behavior |
|-------|----------|
| `kTagPatterns` / low2 table | Documented primary tag set |
| `classify_eval_value_tag_consteval` | Pure consteval mirror of runtime classify |
| Runtime `classify_eval_value_tag` | Uses low2 table first; fixnum with low2==2 still Fixnum |
| `as_int` / `as_string_idx` | Extra Contracts on tag bits / bias range |
| `is_valid_tagged_value` | Unknown probe for hot paths |
| Stats | schema **1622** with hit-rate-bp + process atomics |

## Metrics (`query:value-dispatch-stats`, schema **1622**)

| Key | Meaning |
|-----|---------|
| `dispatch-hits` / `dispatch-misses` | process-wide |
| `dispatch-hit-rate-bp` | hits/(hits+misses)×10000 |
| `contract-violation-count` | debug contract tally |
| `v2-string-collision-attempts` | expect 0 |
| `classify-calls` | classify invocations |
| `consteval-table-wired` / `hotpath-contracts-wired` | 1 |
| `schema` | **1622** (lineage 723\|571) |

## Tests

| File | Role |
|------|------|
| `tests/test_value_encoding_v2_dispatch_contracts_1622.cpp` | **#1622** AC |
| `tests/test_value_encoding_v2_dispatch_contracts_task4.cpp` | Lineage #571 |
| `tests/test_issue_756.cpp` / `757` | schema 1622\|723 |

## Related

- `src/compiler/value_tags.h`
- `src/compiler/value.ixx`
