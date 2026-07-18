# Dead string_heap_ push on circular module load (#1692)

**Issue:** [#1692](https://github.com/cybrid-systems/aura/issues/1692)  
**Lineage:** [#1488](https://github.com/cybrid-systems/aura/issues/1488), [#1668](https://github.com/cybrid-systems/aura/issues/1668), [#1691](https://github.com/cybrid-systems/aura/issues/1691)  
**File:** `src/compiler/evaluator_module_loader.cpp` (`load_module_file`)  
**Status:** P2 memory — dead intern removed (#1488); locked by #1692.

## Problem

```cpp
if (loading_stack_.count(resolved)) {
    auto eidx = string_heap_.size();
    string_heap_.push_back("circular dependency: " + resolved); // unused
    return types::make_void();
}
```

Circular-dep early return never consumed `eidx` — pure heap pollution.

## Fix

Keep stderr log for operators; return `make_void()` without touching
`string_heap_`.

## Tests

`tests/test_module_loader_dead_heap_circular_1692.cpp`
