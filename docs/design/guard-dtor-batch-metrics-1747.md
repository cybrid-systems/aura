# Batch MutationBoundaryGuard dtor metrics (#1747)

**Issue:** [#1747](https://github.com/cybrid-systems/aura/issues/1747)  
**Files:** `evaluator.ixx`  
**Status:** P0 perf — outermost Guard dtor did 15+ atomics per call.

## Problem

`~MutationBoundaryGuard` published hold-time / starvation / histogram
telemetry with many scattered `fetch_add` / `store` / CAS on every
outermost exit. High-frequency `mutate:*` paths pay 75–225ns and bounce
metric cache lines.

## Fix

1. Fill a local `BatchMutationMetrics` (hold_us, holds, over_1ms,
   too_long, starvation, hist_bucket, …) with pure computation.
2. Publish the common path with ≤6 atomic writes:
   dual hold counters (#1253 + #1373), histogram bucket, optional max.
3. Rare path (too_long / extreme / over_1ms) keeps extra atomics only
   when needed.
4. `runtime_obs_export_ready`: load + conditional store (not every dtor).

Max update uses a single relaxed store when the local sample may raise
the max (telemetry; intermediate races are acceptable).

## Tests

`tests/test_guard_dtor_batch_metrics_1747.cpp`
