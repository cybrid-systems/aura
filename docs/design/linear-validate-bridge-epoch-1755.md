# validate_linear_ownership_state bridge_epoch drift (#1755)

**Issue:** [#1755](https://github.com/cybrid-systems/aura/issues/1755)  
**Sibling:** [#1515](https://github.com/cybrid-systems/aura/issues/1515), [#1734](https://github.com/cybrid-systems/aura/issues/1734)  
**Files:** `evaluator_gc.cpp`, `evaluator_env.cpp`, `observability_metrics.h`  
**Status:** P2 — drift check existed (#1515); #1755 adds observability.

## Background

`validate_linear_ownership_state` already rejected
`bridge_epoch != current_bridge_epoch` (when `bridge_epoch != 0`)
per #1515 dual-epoch coordination. The issue body claimed the
parameters were unused — that was stale relative to #1515.

## Fix (#1755)

1. Document the bridge half explicitly (#1755).
2. Optional `std::atomic<uint64_t>* bridge_epoch_drift_counter`
   out-parameter so static purity is kept while call sites with
   `CompilerMetrics` can observe
   `linear_validate_bridge_epoch_drift_total`.
3. Wire GC safepoint probe + `materialize_call_env` to pass the
   metric field.

## Semantics (unchanged)

| linear_state | result |
|--------------|--------|
| 0 Untracked | always true |
| 4 Moved | always false |
| 1–3 | false if frame_version < current_version |
| 1–3 | false if bridge_epoch != 0 and != current |

## Tests

`tests/test_linear_validate_bridge_epoch_1755.cpp`
