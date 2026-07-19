# Distinguish env-frame stale vs invalid id (#1754)

**Issue:** [#1754](https://github.com/cybrid-systems/aura/issues/1754)  
**Files:** `evaluator.ixx`, `evaluator_env.cpp`  
**Status:** P1 — `is_env_frame_stale` conflated NULL/OOB with version drift.

## Problem

`is_env_frame_stale(id)` returned true for:

1. Live frames with `version_ < defuse_version_` (true staleness)
2. `NULL_ENV_ID` / OOB ids (no frame exists)

Callers could not tell "refresh this frame" from "there is no frame".

## Fix

| Predicate | Meaning |
|-----------|---------|
| `is_env_frame_invalid_id(id)` | NULL or OOB — frame does not exist |
| `is_env_frame_invalid(id)` | INVALID_VERSION sentinel (or NULL/OOB safety) |
| `is_env_frame_stale(id)` | frame **exists** and `version_ < defuse` |

`materialize_call_env` refreshes only when `!invalid_id && stale`.

Apply paths that already use `invalid || stale` remain correct
because `is_env_frame_invalid` still covers NULL/OOB.

## Tests

`tests/test_env_frame_stale_vs_invalid_id_1754.cpp`
