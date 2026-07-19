# MutationBoundaryGuard enter_ts_ as std::optional (#1764)

**Issue:** [#1764](https://github.com/cybrid-systems/aura/issues/1764)  
**Sibling:** [#1253](https://github.com/cybrid-systems/aura/issues/1253),
[#1747](https://github.com/cybrid-systems/aura/issues/1747)  
**Files:** `evaluator.ixx`  
**Status:** P2 — magic-value sentinel for outermost hold clock.

## Problem

`~MutationBoundaryGuard` gated hold-duration telemetry with:

```cpp
if (outermost && enter_ts_.time_since_epoch().count() != 0)
```

Default-constructed `steady_clock::time_point` is not required by the
standard to have a zero epoch count. Nested guards left `enter_ts_`
defaulted; outermost assigned `now()`. Relying on zero as “unset” is
implementation-defined and brittle.

## Fix (Option B)

```cpp
std::optional<std::chrono::steady_clock::time_point> enter_ts_;
// ctor outermost: enter_ts_ = steady_clock::now();
// dtor: if (outermost && enter_ts_.has_value()) { … *enter_ts_ … }
```

Nested / inert guards leave `enter_ts_` empty. No magic time_point.

Hold counters and #1747 batch publish path are unchanged.

## Tests

`tests/test_guard_enter_ts_optional_1764.cpp`
