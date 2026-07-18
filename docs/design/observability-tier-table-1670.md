# Observability peel tier table (#1670)

**Issue:** [#1670](https://github.com/cybrid-systems/aura/issues/1670)  
**Builds on:** #909 (peel monolith) · #1434 / #1450 (post-reg deprecations)  
**Status:** P2 architecture — collapse orchestrator boilerplate; peel bodies retained.

## Reality check (issue body vs tree)

The issue text described 105 “single-line `add()` wrappers”. On current main
that is **stale**: each `register_eval_pN` / `register_jit_pN` lives in
`evaluator_primitives_obs_{eval,jit}_*.cpp` and contains real registration
bodies (`add(...)` + `register_stats_impl(...)` with capturing lambdas).

Those peel TUs **cannot** become a static `{name, PrimFn}` table without a
much larger rewrite (each lambda closes over `Evaluator& ev` and often
multi-prim clusters).

## What #1670 ships

| Before | After |
|--------|--------|
| `register_eval_all` = 105 sequential calls | function-pointer table + loop |
| `register_jit_all` = 114 sequential calls | function-pointer table + loop |
| 219 hand-written decls in `observability_prims_decl.inc` | X-macro over tier index lists |

### Single source of tier counts

| File | Role |
|------|------|
| `src/compiler/observability_eval_tiers.inc` | `OBS_EVAL_TIER(0)` … `OBS_EVAL_TIER(104)` (105) |
| `src/compiler/observability_jit_tiers.inc` | `OBS_JIT_TIER(0)` … `OBS_JIT_TIER(113)` (114) |
| `observability_prims_decl.inc` | expands to member decls |
| `register_eval_all` / `register_jit_all` | expands to `&register_*_pN` table |

Adding a new peel tier: append one line to the appropriate `.inc`, implement
`register_*_pN` in a peel TU — no edit to the orchestrator call chain.

## Non-goals (this issue)

- Merging peel TU bodies into one mega-function.
- Replacing peel lambdas with a catalog-driven `{name, PrimFn}` table
  (that is closer to #1672 catalog drift work / future stats-only path).
- Public surface change.

## Tests

`tests/test_observability_tier_table_1670.cpp` — CS boots, sample
`engine:metrics` / stats path works; tier counts match `.inc` files.

## Net

~200 lines removed from `evaluator_primitives_observability.cpp` orchestrator;
decls collapsed from ~220 lines to ~30. Behavior unchanged.
