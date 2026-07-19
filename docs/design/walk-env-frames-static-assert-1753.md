# walk_env_frames static_assert F signature (#1753)

**Issue:** [#1753](https://github.com/cybrid-systems/aura/issues/1753)  
**Files:** `evaluator.ixx`  
**Status:** P2 — `requires AuraInvocable` only; unclear diagnostics.

## Problem

`walk_env_frames` constrained F solely via a C++20 `requires`
`AuraInvocable` clause. A wrong callable signature produced concept-
failure noise rather than a direct message. (The tree is already
`CMAKE_CXX_STANDARD 26`, so a silent C++17 no-op is not a production
path — but clear diagnostics still matter.)

## Fix (Option B)

Drop the `requires` clause; add two `static_assert`s:

1. `std::is_invocable_v<F, EnvId, const EnvFrame&>`
2. return type convertible to `bool` via `std::invoke_result_t`

Body unchanged (parent-chain walk, false stops).

## Tests

`tests/test_walk_env_frames_static_assert_1753.cpp`
