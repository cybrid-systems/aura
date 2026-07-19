# Guard dtor invariant probes are noexcept (#1766)

**Issue:** [#1766](https://github.com/cybrid-systems/aura/issues/1766)  
**Sibling:** [#417](https://github.com/cybrid-systems/aura/issues/417),
[#422](https://github.com/cybrid-systems/aura/issues/422)  
**Files:** `evaluator.ixx`, `evaluator_fiber_mutation.cpp`  
**Status:** P1 report — dtor “exception leaks depth slot”.

## Analysis

Issue body recommended try/catch around `ensure_mutation_invariants`
in `~MutationBoundaryGuard`. Current code already satisfies the
correctness contract:

1. **Depth is decremented first** (`prev = (*slot)--`) before any
   hold metrics, lock release, panic checkpoint, or invariant probes.
2. **Probes are `noexcept`:**
   - `ensure_mutation_invariants() noexcept`
   - `ensure_hygiene_violation_detection() const noexcept`
   - `probe_arena_auto_policy_on_boundary_exit(bool) noexcept`

A throw from a `noexcept` function calls `std::terminate` — it does
not skip remaining dtor statements via catchable exceptions. Wrapping
`noexcept` calls in try/catch would not intercept such a throw.

`ensure_mutation_invariants` only compares stack emptiness vs depth
and bumps `total_invariant_violations_` — no allocation, no throw
paths.

## Fix shipped (#1766)

Document the contract in source (dtor + impl) and lock it with a
source/API regression test. No try/catch added (would be misleading
and ineffective under `noexcept`).

## Tests

`tests/test_guard_dtor_invariant_noexcept_1766.cpp`
