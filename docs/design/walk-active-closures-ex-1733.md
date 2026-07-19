# Isolate walk_active_closures callback exceptions (#1733)

**Issue:** [#1733](https://github.com/cybrid-systems/aura/issues/1733)  
**Sibling:** [#1545](https://github.com/cybrid-systems/aura/issues/1545)  
**Files:** `evaluator_env.cpp`, `observability_metrics.h`  
**Status:** P1 — throwing callback aborted mid-walk (partial GC/invalidate).

## Fix (Option B)

Per-callback try/catch inside `walk_active_closures`:

1. Catch `std::exception` and `...`.
2. Bump `walk_active_closures_callback_exceptions`.
3. Continue iterating remaining closures.
4. Lock still released via `unique_lock` RAII.

Partial walk completion is preferred over aborting root registration.

## Tests

`tests/test_walk_active_closures_ex_1733.cpp`
