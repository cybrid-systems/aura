# Render hot-path observability completion (#1674)

**Issue:** [#1674](https://github.com/cybrid-systems/aura/issues/1674)  
**Builds on:** #968 (snapshot FnMetrics) · #1144 (flat_hash insert helper) · #1673 (render-stats)  
**Status:** P0 observability — wire dead render `bump_*`, dedupe stats builders, expose metrics.

## Problem (issue body vs tree)

| Claim | Status |
|-------|--------|
| `snapshot()` leaves FnMetrics zero | **Fixed earlier (#968)**; #1674 adds synthetic `terminal-present-batch` row |
| 39× insert_kv lambdas | Shared helper `#1144`; #1674 converts **16** more blocks in obs_jit_06/10 |
| Render `bump_*` dead | **#1674 wires all 22** render/term_buf/term_render bumps |

## Wired call sites

| Bump family | Call site |
|-------------|-----------|
| `bump_term_buf_diff*` | `terminal-diff-update` |
| `bump_term_render_present*` / zero-copy / soa / hp-* | `terminal-present-batch` |
| `bump_term_render_draw_batch` | `render-draw-batch` |
| `bump_term_render_dirty_region` / `hp_mutation_impact` | `terminal-mark-dirty*` |
| `bump_term_render_clear` | `make-terminal-buffer` |
| `bump_term_render_present` | `tui:present` |
| `bump_render_obs_v2*` | `query:render-stats` query path |

## `query:render-stats` (schema **1674**)

Facade-only `(stats:get "query:render-stats")` now includes term_render_*, term_buf_diff_*, hp-*, jit-soa_*, obs-v2_*, zerocopy-views in addition to #1673 engine counters.

## Snapshot

After JIT `FnMetrics` fill (#968), append synthetic:

```
name=terminal-present-batch
total_calls=terminal_present_batch_total
hit/miss from present vs hotpath skip
deopt_count=render_jit_deopt_applied
```

## Tests

`tests/test_render_hotpath_observability_1674.cpp`

## Non-goals

- Wiring all 400+ global dead bumps (out of scope; script remains for audit).  
- Full rewrite of every obs_jit insert_kv (incremental via #1144 helper).
