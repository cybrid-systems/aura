# Atomic current_agent_fingerprint_ (#1730)

**Issue:** [#1730](https://github.com/cybrid-systems/aura/issues/1730)  
**Sibling:** [#1419](https://github.com/cybrid-systems/aura/issues/1419)  
**Files:** `evaluator.ixx`  
**Status:** P2 race — plain `uint64_t` agent fingerprint under concurrent fibers.

## Fix

- Member: `std::atomic<std::uint64_t> current_agent_fingerprint_{0}`
- Setter: `store(fp, memory_order_release)` then stamp workspace flat
- Getter: `load(memory_order_acquire)`
- `set_workspace_flat` re-stamp path uses the same acquire load

## Tests

`tests/test_agent_fingerprint_atomic_1730.cpp`
