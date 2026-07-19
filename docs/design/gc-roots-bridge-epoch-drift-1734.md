# Detect bridge_epoch drift in collect_compiler_managed_gc_roots (#1734)

**Issue:** [#1734](https://github.com/cybrid-systems/aura/issues/1734)  
**Sibling:** [#682](https://github.com/cybrid-systems/aura/issues/682), [#1728](https://github.com/cybrid-systems/aura/issues/1728)  
**Files:** `evaluator_gc.cpp`, `observability_metrics.h`  
**Status:** P1 — snapshot parameter unused for drift vs live epoch.

## Fix

At entry of `collect_compiler_managed_gc_roots`:

1. Read `live_epoch = current_bridge_epoch()`.
2. If `live_epoch != snapshot` → bump `gc_roots_bridge_epoch_drift_total`.
3. Filter roots with `eff_epoch = live_epoch` (prefer live when drifted).

## Tests

`tests/test_gc_roots_bridge_epoch_drift_1734.cpp`
