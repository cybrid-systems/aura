# CAS-loop for mutation_hold_duration_us_max (#1765)

**Issue:** [#1765](https://github.com/cybrid-systems/aura/issues/1765)  
**Sibling:** [#1747](https://github.com/cybrid-systems/aura/issues/1747),
[#1253](https://github.com/cybrid-systems/aura/issues/1253)  
**Files:** `evaluator.ixx`  
**Status:** P2 — lost-update race on hold-duration max.

## Problem

After #1747 batched publish, outermost Guard dtor updated
`mutation_hold_duration_us_max` with:

```cpp
if (update_max)
    max.store(hold_us);  // after a prior relaxed load
```

Two concurrent dtors can both pass the local “greater than prev”
check; the later store may install a *lower* sample and lose the true max.

## Fix

```cpp
auto prev = max.load(relaxed);
while (hold_us > prev &&
       !max.compare_exchange_weak(prev, hold_us, relaxed)) {
}
```

Still one common-path max write attempt (CAS), within #1747’s ≤6
atomic budget for the common publish block. Rare-path counters
unchanged.

## Tests

`tests/test_guard_hold_max_cas_1765.cpp`
