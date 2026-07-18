# Thread-safe PRNG for synthesize:optimize (#1716)

**Issue:** [#1716](https://github.com/cybrid-systems/aura/issues/1716)  
**Files:** `evaluator_primitives_agent.cpp`  
**Status:** P1 race — `std::rand()` is not thread-safe.

## Problem

`synthesize:optimize` GA used global `std::rand()` for mutations and
crossover. Concurrent fibers race on the shared C RNG state.

## Fix (Option A)

```cpp
thread_local std::mt19937 rng{std::random_device{}()};
// agent_rand_below(n) / agent_rand_unit()
```

All former `std::rand()` sites in the optimize loop use these helpers.

## Out of scope

- `(query:auto-evolve-prng-seed)` for reproducibility (follow-up)

## Tests

`tests/test_synthesize_optimize_prng_1716.cpp`
