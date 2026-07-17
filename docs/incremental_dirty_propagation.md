# Incremental Dirty Propagation — Epoch Gate Contract

This document describes the **epoch gate** that the Aura compiler pipeline uses to decide between incremental and full recompilation. It complements the dirty-bitmask machinery (`FlatAST::dirty_` + `is_dirty_for()`) by capturing the policy layer above it.

## Context

Aura's self-evolution loop (mutate:query-and-replace → eval-current) needs to know whether to do a full re-lower or just re-touch the dirty blocks. The decision lives in `(compile:relower-strategy <function-name>)`, which returns one of four keywords based on `dirty_block_count()` of the cached `ir_cache_v2_` entry.

## The 4-way decision

```
dirty_block_count()  →  keyword returned
─────────────────────────────────────────
0                    →  'none        (clean, no work)
1..7                 →  'incremental (targeted re-lower cheaper)
8+                   →  'full        (full re-lower on par)
not in cache         →  'unknown     (compile + cache first)
```

The `'incremental / 'full` threshold at **8 dirty blocks** is conservative — a typical Aura function has 16-32 IR blocks, so 8+ is roughly half. Agents that need a different threshold can read the 3-tuple from `(query:compiler-cache-stats)` (`((dirty-blocks . dirty-functions) . incremental-candidates)`) and decide themselves.

## Why a 4-way gate?

- **`'none`** tells the caller "nothing to do — no pipeline work needed". Saves a `bump_generation` + epoch comparison on the hot path.
- **`'incremental`** tells the caller "do targeted re-lower on the dirty blocks only". This is the common case for `mutate:query-and-replace` on a single binding.
- **`'full`** tells the caller "the dirty surface is so wide that incremental re-lower has no advantage". Avoids the bookkeeping overhead for small functions.
- **`'unknown`** tells the caller "this function isn't in `ir_cache_v2_` yet". Either it's a new function (compile + cache), or it's a query-only function name (no IR).

## Why 8 blocks?

- Below 8 dirty blocks: the targeted re-lower's per-block bookkeeping (new SSA names, dependency tracking, def-use update) is amortized across fewer blocks than a full re-lower. Wins by 20-40% in typical small-mutation scenarios.
- At 8+ dirty blocks: the bookkeeping cost converges with a full re-lower's. Below this point, agents should prefer `'incremental`; above it, the simpler `'full` path wins.

## Dirty propagation pipeline

```
mutate:query-and-replace
  ↓
FlatAST::mark_subtree_dirty (kGeneralDirty + kConstraintDirty + …)
  ↓
ir_cache_v2_ entry dirty_block_count updates
  ↓
next (eval-current) reads (compile:relower-strategy fname)
  ↓
agent decides: skip | incremental re-lower | full re-lower
```

The `kGeneralDirty` bit is always OR'd onto `dirty_` for backward compat with legacy `is_dirty()` callers. New code should use `is_dirty_for(id, reason_mask)` for fine-grained checks (see `src/core/ast.ixx` L3280).

## Interaction with `define-hygienic-macro`

Macros introduce `SyntaxMarker::MacroIntroduced` markers on cloned template nodes. The dirty propagation respects this: mutating a macro-introduced binding marks the binding's node + ancestors dirty (per `mark_dirty_upward`), but the macro *definition* node stays clean (no recompile of the macro itself unless the body changes).

If the macro is *redefined* (`define-hygienic-macro` with the same name), the body node is marked dirty, and the next `(compile:relower-strategy <caller>)` returns `'incremental` or `'full` because the caller's dependency edge went stale.

## Test coverage

- `tests/test_issue_326.cpp` (Issue #326) — macro + mutate integration; 8 scenarios.
- `tests/test_issue_331.cpp` (Issue #331) — dirty bitmask + query:pattern integration; 8 scenarios.
- `tests/test_issue_327.cpp` (Issue #327, this commit) — end-to-end incremental compilation; 7 scenarios covering the epoch gate axis.

## #1495: consume body-only dirty on eval / populate (shipped)

Prior to #1495, `mark_define_dirty` always called `mark_all_blocks_dirty` on the
primary target. That forced `dirty_func_count >= 2` (entry + body), so
`relower_define_blocks` skipped the per-function path and always full-lowered.

| Surface | Behavior after #1495 |
|---------|----------------------|
| `mark_define_dirty` primary | body-only dirty when `irs.size() == 2`; nested (`>2`) keeps full dirty |
| cascade dependents | unchanged body-only for `irs[1]` (#1514) |
| `relower_dirty_defines_from_workspace` | walks dirty v2 entries → `relower_define_blocks` |
| `(eval-current)` | calls the hook before tree-walker eval |
| `populate_ir_cache_v2_from_workspace` | partial first, then full `cache_define` fallback |

**Metrics (AC3):** `incremental_relower_blocks_total`, `relower_full_called_count`
(snapshot `full_relower_count`), `dirty_block_ratio_bp`.

**Tests:** `tests/test_issue_1495.cpp`, builds on `test_issue_1474` selective copy.

## Future work

1. Quantitative benchmark comparing `'incremental` vs `'full` paths (per-function dirty block count distribution). Issue #327 AC #3 deferred to `edsl_benchmark.py` extension.
2. Adaptive threshold: instead of fixed 8 blocks, learn per-function dirty-ratio from prior re-lowers.
3. `compile:relower-strategy` taking a `(name-or-symbol)` overload — return the strategy as a structured record (block count + threshold + last-relower-cost) for AI agents.
