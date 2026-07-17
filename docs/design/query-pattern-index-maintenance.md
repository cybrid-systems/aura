# query:pattern tag_arity_index Incremental Maintenance (Issue #1503)

## Problem

`tag_arity_index_` (FlatAST + Evaluator) is built O(N) and marked dirty on
`mark_dirty_upward`, but until #1503 the refresh policy was coarse:

- append-only growth → delta append
- any in-place dirty with same size → **full O(N) rebuild**

On large ASTs (10k+ nodes) AI self-edit loops paid a hidden O(N) tax on every
`query:pattern` after mutate.

## Policy (#1503)

### FlatAST `ensure_tag_arity_index`

1. Clean + empty → full rebuild  
2. Dirty flag only, **no dirty bits**, no growth → full rebuild (unknown change)  
3. Append-only (no dirty bits, size grew) → `rebuild_tag_arity_index_delta`  
4. Dirty fraction **> threshold_pct** (default **25%**) → full rebuild  
   (`tag_arity_index_threshold_full_rebuilds_`)  
5. Else → **incremental** `patch_tag_arity_index_dirty_nodes` using per-node
   packed keys (`tag_arity_node_key_`) for O(1) remove + insert  

### Live patch on dirty cascade

`mark_dirty_upward` / `_fast` / `mark_dirty_upward_with_index_update` call
`patch_tag_arity_index_node(seed)` when the index is already warm so arity/tag
re-keys stay O(1) on the mutate hot path.

### Evaluator + Guard

- `build_tag_arity_index_unlocked` uses the same dirty-fraction threshold before
  `sync_after_mutation` vs full rebuild.
- **Lazy + warm**: outermost successful `MutationBoundaryGuard` exit auto-syncs
  the Evaluator index (`pattern_index_auto_warm_syncs_`) so first query builds
  lazily, then mutate→query stays incremental.
- **EagerAfterMutate** / **EagerAfterCow** unchanged (#490).

### Agent controls

| Surface | Role |
|---------|------|
| `(mutate:set-pattern-index-policy "lazy"\|"eager-after-mutate"\|"eager-after-cow")` | #490 policy |
| `(engine:metrics "query:pattern-index-policy")` | read policy string |
| `(engine:metrics "query:pattern-index-rebuild-stats")` | **schema 1503** counters |
| `FlatAST::set_tag_arity_index_full_rebuild_threshold_pct(1..100)` | C++ threshold |

### `query:pattern-index-rebuild-stats` keys (schema 1503)

- `lazy-rebuilds`, `eager-mutate-rebuilds`, `eager-cow-rebuilds`
- `auto-warm-syncs` — Guard Lazy+warm auto syncs
- `flat-rebuilds`, `flat-rebuild-time-us`, `flat-delta-hits`
- `threshold-full-rebuilds`, `incremental-patches`, `threshold-pct`
- `schema` = **1503**

`query:pattern-index-stats-hash` remains **schema 621** (back-compat).

## Related

#211, #490, #547, #554, #850, #1371, #1501
