# Mutation-boundary fair scheduling (#1591)

**Builds on:** #1504 (safe-yield), #1493 (per-fiber depth / histogram),
#1492 (inner-steal starvation mitigation), #1483 (adaptive safepoint)

## Problem

Multi-Agent orchestration needs:

1. First-class **safe yield** when Guard depth is 0  
2. **Per-fiber mutation depth** + histogram for pressure  
3. **Steal starvation mitigation** when nested Guards block steal  
4. **Safepoint wait** while mutation is held  

These existed as separate primitives; #1591 unifies Agent-facing surfaces.

## Surfaces

| Primitive | Schema | Role |
|-----------|--------|------|
| `query:mutation-boundary-safe-yield` | **1591** | Side-effect yield if safe + fairness fields |
| `query:mutation-boundary-safe-yield-stats` | **1591** | Lifetime counters + avg hold |
| `query:per-fiber-mutation-depth-stats` | **1591** | Alias of stack-stats + wait_us |
| `query:per-fiber-mutation-stack-stats` | 1493 | Original histogram surface |
| `query:mutation-boundary-fairness-stats` | **1591** | **Unified** dashboard |
| `query:orchestration-steal-stats` | 1492 | Steal boost / deferred-inner |

### Fairness hash (`query:mutation-boundary-fairness-stats`)

- `boundary-depth`, `held-now`  
- `per-fiber-stack-depth-max` / `current-max`  
- `safe-yield-ok-total` / `skipped-held-total`  
- `avg-hold-time-us`, `hold-samples`  
- `safepoint-wait-while-mutation-held-us` / `count`  
- `steal-inner-deferred-starvation-mitigated-count`  
- `mutation-stack-depth-histogram-samples`  
- `schema` = **1591**

## Agent usage

```scheme
;; Between mutate batches
(engine:metrics "query:mutation-boundary-safe-yield")
;; Or unified fairness probe
(let ((f (engine:metrics "query:mutation-boundary-fairness-stats")))
  (when (> (hash-ref f "avg-hold-time-us") 1000)
    (mutate:safe-yield)))  ; stdlib bridge
```

## Steal path

See `docs/design/inner-steal-starvation-mitigation.md` — inner Guard defer
applies priority boost + `steal_inner_deferred_starvation_mitigated_count`.

## Tests

`tests/test_mutation_boundary_fairness_1591.cpp`
