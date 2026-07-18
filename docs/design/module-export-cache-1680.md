# query:module-exports mtime cache (#1680)

**Issue:** [#1680](https://github.com/cybrid-systems/aura/issues/1680)  
**File:** `src/compiler/evaluator_primitives_query.cpp`  
**Status:** P2 perf — amortize re-read/re-parse of module export lists.

## Problem

Every `(query:module-exports path)` opened the resolved file, read it fully,
and hand-scanned for `(export …)` — O(file_size) I/O + allocation per call.

## Fix

Process-wide mtime-keyed cache (`module_export_cache`):

1. `last_write_time(resolved)` as cache key  
2. Hit → copy cached export names (no I/O)  
3. Miss → read + `parse_module_exports` + store  

Parser extracted and hardened: skips `;` line comments, `#| |#` blocks, and
`"…"` strings (simple `\` escapes). Still only the **first** `(export …)` form
(historical contract).

## Observability (facade-only)

`(stats:get "query:module-export-cache-stats")` schema **1680**:

| Key | Meaning |
|-----|---------|
| hits / misses | cache counters |
| hit-rate-bp | hits/(hits+misses) × 10000 |
| entries | map size |
| stat-fail / open-fail | filesystem failures |

No new public `add()` (SlimSurface ceiling).

## Tests

`tests/test_module_export_cache_1680.cpp`
