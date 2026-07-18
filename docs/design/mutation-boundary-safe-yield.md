# Mutation Boundary Safe Yield (Issue #1504 / #1635)

## Contract

Multi-Agent orchestration must **not** yield while a `MutationBoundaryGuard`
holds `workspace_mtx_` (deadlock — see #362). Agents need a first-class
EDSL surface to:

1. Read current Guard nesting depth  
2. Cooperatively yield **only** at safe points (`YieldReason::MutationBoundary`)  
3. Observe per-fiber depth / skip counters for scheduler fairness  
4. Align with GC `request_gc_safepoint` (defers when depth>0) — **#1635 mandate**

## Surfaces (via `engine:metrics` / `register_stats_impl`)

| Name | Kind | Behavior |
|------|------|----------|
| `query:mutation-boundary-depth` | int | `mutation_boundary_depth()` (0 = yield/steal-safe) |
| `query:mutation-boundary-safe-yield` | hash (side-effect) | Attempt cooperative yield |
| `ast:yield-at-boundary` | hash (side-effect) | Alias of safe-yield (**#1635 first-class**) |
| `query:mutation-boundary-safe-yield-stats` | hash | Lifetime counters + depth (**schema 1635**) |
| `query:mutation-boundary-fairness-stats` | hash | Unified fairness dashboard (**schema 1635**) |
| `query:per-fiber-mutation-depth-stats` | hash | Per-fiber depth alias (**schema 1635**) |

### Safe-yield algorithm (`Evaluator::try_safe_yield_at_boundary`)

```
if depth > 0 OR mutation_boundary_held OR any_active_mutation_boundary:
    count skipped-held; return 1
else:
    call g_fiber_yield_mutation_boundary (or g_fiber_yield)
    if fiber active: count ok; return 0
    else: count no-fiber; return 0   // safe no-op (test/stdin)
```

Optional `timeout-ms` argument is reserved (MVP ignored); future soft deadline.

### Action hash fields (schema **1635**, lineage 1591 / 1504)

- `yielded` (0/1), `skipped-held` (0/1)  
- `boundary-depth`, `depth-slot`, `held-now`  
- `safe-yield-ok-total`, `safe-yield-skipped-held-total`, `safe-yield-no-fiber-total`  
- `avg-hold-time-us`, `safepoint-wait-while-mutation-held-us`,  
  `steal-inner-deferred-starvation-mitigated-count`  
- #1635: `yield-reason-mutation-boundary`, `gc-safepoint-depth-check-wired`,  
  `ast-yield-at-boundary-wired`, `safe-yield-mandate-active`

### Stats hash fields

- Same lifetime counters  
- `nested-guard-depth-max`, `per-fiber-stack-depth-max`  
- `schema` = **1635**

## Agent usage (multi-step self-edit)

```scheme
;; Between mutate batches — never inside a naked long boundary
(let ((d (engine:metrics "query:mutation-boundary-depth")))
  (when (= d 0)
    (engine:metrics "query:mutation-boundary-safe-yield")))
;; Or first-class alias:
(engine:metrics "ast:yield-at-boundary")
```

If `skipped-held` is 1, finish the current Guard body first, then yield.

## Related

#213, #362, #1014, #1373 (hold-stats), #1444 (Guard mandatory), #1504, #1591 (fairness),
#1635 (mandate), `fiber:yield`, `docs/design/mutation-boundary-fairness.md`
