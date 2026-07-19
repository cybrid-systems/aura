# Inline typecheck helpers never throw (#1769)

**Issue:** [#1769](https://github.com/cybrid-systems/aura/issues/1769)  
**Sibling:** [#107](https://github.com/cybrid-systems/aura/issues/107),
[#526](https://github.com/cybrid-systems/aura/issues/526),
[#1684](https://github.com/cybrid-systems/aura/issues/1684)  
**Files:** `evaluator_typecheck.cpp`, `observability_metrics.h`  
**Status:** P1 — uncaught throw from inline TC could skip Guard/fuzzer cleanup.

## Problem

`run_typecheck_no_lock` / `_bool` / `run_post_mutate_typecheck_no_lock`
are used on fuzzer hot paths and inside mutate Guards (`run_or_rollback`).
An exception from `infer_flat` / partial recheck could skip remaining
caller cleanup (or force terminate if another exception is active).

Issue body suggested per-call-site try/catch. Centralizing in the
helpers covers all call sites (fuzzer + mutate) once.

## Fix

Each helper wraps its body:

```cpp
try { /* existing TC */ }
catch (...) {
  bump inline_typecheck_exception_total;
  return failure; // string / false
}
```

Mutate path already has `run_or_rollback` as a second belt; helpers
now never throw into it.

## Tests

`tests/test_inline_typecheck_exception_1769.cpp`
