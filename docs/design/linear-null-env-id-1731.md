# Observe linear_post_mutate_enforce(NULL_ENV_ID) (#1731)

**Issue:** [#1731](https://github.com/cybrid-systems/aura/issues/1731)  
**Sibling:** [#1478](https://github.com/cybrid-systems/aura/issues/1478), [#1542](https://github.com/cybrid-systems/aura/issues/1542)  
**Files:** `evaluator_env.cpp`, `observability_metrics.h`  
**Status:** P1 — NULL_ENV_ID makes linear enforce a no-op (by design).

## Problem

`linear_post_mutate_enforce(NULL_ENV_ID)` returns true without scanning
captures. Closures without a captured env (top-level / skip-alloc paths)
therefore skip linear validation at materialize entry.

## Fix (Option A)

1. Metric `linear_post_mutate_null_env_id_total` on every
   `linear_post_mutate_enforce(NULL_ENV_ID)`.
2. `materialize_call_env` explicitly calls enforce for NULL_ENV_ID
   before empty-Env fallback so TCO/top-level paths are counted.

## Tests

`tests/test_linear_null_env_id_1731.cpp`
