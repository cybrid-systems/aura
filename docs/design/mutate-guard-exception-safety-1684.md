# MutationBoundaryGuard::run_or_rollback (#1684)

**Issue:** [#1684](https://github.com/cybrid-systems/aura/issues/1684)  
**Files:** `evaluator.ixx` (helper), `evaluator_primitives_mutate.cpp` (rebind/set-body)  
**Status:** P1 correctness — throws must not commit partial mutates.

## Problem

`mutate:rebind` (and similar) called schema validate / post-mutate typecheck /
ownership validation inside a Guard with `ok=true`. A throw left `ok` true,
so `~MutationBoundaryGuard` **committed** an incomplete rebind.

## Fix

```cpp
// MutationBoundaryGuard
bool run_or_rollback(F&& fn, std::string* err_out = nullptr);
void mark_failed() noexcept;
```

On catch: `*flag_ = false`, bump `mutation_boundary_exception_rollback_total`.

Wired on **mutate:rebind** (schema validate, typecheck, ownership) and
**mutate:set-body** (typecheck, ownership). Other `add_mutate` bodies can
migrate to the same helper.

## Tests

`tests/test_mutate_guard_exception_safety_1684.cpp`
