# Key MutationBoundaryGuard depth slot by instance_id (#1746)

**Issue:** [#1746](https://github.com/cybrid-systems/aura/issues/1746)  
**Files:** `evaluator.ixx`, `evaluator_ctor.cpp`, `evaluator_fiber_mutation.cpp`  
**Status:** P1 UAF — TLS depth map was keyed by `Evaluator*`.

## Problem

`mutation_boundary_depth_slot` used:

```cpp
thread_local unordered_map<Evaluator*, int> depths;
```

If Evaluator E1 is destroyed and a later E2 is allocated at the same
address, TLS still holds the old depth for that pointer → wrong nesting
/ UAF-class corruption when Guard ctor/dtor touch the slot.

## Fix (Option A)

1. Assign a process-unique `instance_id_` in `Evaluator` ctor
   (`atomic` counter starting at 1).
2. Key the TLS map by `std::uint64_t` (`instance_id()`), not raw address.
3. Null `Evaluator*` uses a dedicated thread_local zero slot.

## Tests

`tests/test_depth_slot_instance_id_1746.cpp`
