# strategy:set-strategy tagged errors (#1714)

**Issue:** [#1714](https://github.com/cybrid-systems/aura/issues/1714)  
**Files:** `evaluator_primitives_agent.cpp`  
**Status:** P2 UX — silent `make_void()` on invalid strategy names.

## Problem

`strategy:set-strategy` returned `make_void()` for bad args, OOR
string index, and unknown strategy names — indistinguishable from
success-shaped voids elsewhere, and type-inconsistent with the
valid path (`make_int(name.size())`).

## Fix

Use `Evaluator::make_merr(kind, msg)`:

| Path | Tag | Message sketch |
|------|-----|----------------|
| empty / non-string | `bad-arg` | usage: (strategy:set-strategy strategy-name) |
| string idx OOR | `bad-arg` | string index out of range |
| unknown name | `unknown-strategy` | expected whitelist |

Valid path unchanged: set `active_strategy_`, bump hits, return int.

## Tests

- `tests/test_strategy_set_errors_1714.cpp`
- `tests/test_issue_444_strategy_evolution.cpp` AC6 updated (merr pair)
