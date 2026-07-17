# Per-fiber mutation depth + adaptive safepoint observability (#1493)

**Parent closed-loop** over #1483 (depth max atomics + adaptive threshold + primitives).

## Exports

| Primitive | Schema | Key fields |
|-----------|--------|------------|
| `query:per-fiber-mutation-stack-stats` | **1493** | `lifetime-max`, `current-max`, `live-depth`, `hist-*`, `hist-samples` |
| `query:gc-safepoint-adaptive-stats` | **1493** | `threshold`, `defer-count`, `avg-mutation-hold-us`, `safepoint-wait-while-mutation-held-us`, `gc-frequency-tune-ratio`, `frequency-adapt-up/down` |

## Adaptive frequency (AC2)

On outermost `MutationBoundaryGuard` exit:

```
if hold_us > long_mutation_threshold_us:
    gc_frequency_tune_ratio = min(100, ratio + 10)   // more frequent checks
elif hold_us < threshold/10 and ratio > 50:
    gc_frequency_tune_ratio = max(50, ratio - 5)     // decay toward default
```

Ratio semantics (existing `gc_frequency_tune_ratio`): 50 default, 100 = check every allocation (more aggressive).

Depth-pressure adaptive (from #1483) remains: `request_gc_safepoint` defers when `mutation_stack_depth_current_max > safepoint_adaptive_threshold`.

## Wait-while-mutation

`Fiber::check_gc_safepoint` accumulates wait µs into `gc_hooks::g_safepoint_wait_while_mutation_held_*` when the fiber holds a mutation boundary during STW wait.

## Depth histogram

Buckets: 0, 1, 2, 3, 4, 5–7, 8–15, 16+. Sampled on every `bump_per_fiber_mutation_stack_depth_max`.

## Tests

`tests/test_issue_1493.cpp` — schema, histogram, frequency adapt, stress.
