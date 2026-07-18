# Dead string_heap_ push on module prefix inject (#1693)

**Issue:** [#1693](https://github.com/cybrid-systems/aura/issues/1693)  
**Lineage:** [#1488](https://github.com/cybrid-systems/aura/issues/1488), [#1668](https://github.com/cybrid-systems/aura/issues/1668), [#1691](https://github.com/cybrid-systems/aura/issues/1691), [#1692](https://github.com/cybrid-systems/aura/issues/1692)  
**File:** `src/compiler/evaluator_primitives_module.cpp` (`import` prefix path)  
**Status:** P2 memory — dead intern removed (#1488); locked by #1693.

## Problem

```cpp
auto prefixed = prefix + name;
auto psid = ev.string_heap_.size();
ev.string_heap_.push_back(prefixed);  // unused
ev.top_.bind(prefixed, val);          // uses stack string
```

Each prefixed export interned permanently without consumers.

## Fix

`Env::bind` copies keys; pass `prefixed` directly. No `psid` / heap push.

## Tests

`tests/test_module_prefix_dead_heap_1693.cpp`
