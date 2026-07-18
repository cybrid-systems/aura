# Dead string_heap_ push in mutate:refactor/extract (#1691)

**Issue:** [#1691](https://github.com/cybrid-systems/aura/issues/1691)  
**Lineage:** [#1488](https://github.com/cybrid-systems/aura/issues/1488), [#1668](https://github.com/cybrid-systems/aura/issues/1668)  
**File:** `src/compiler/evaluator_primitives_mutate.cpp` (`mutate:refactor/extract`)  
**Status:** P2 memory — dead intern removed (already fixed in #1488; locked by #1691).

## Problem

```cpp
auto define_idx = ev.string_heap_.size();
ev.string_heap_.push_back(define_str);  // never used
parse_to_flat(define_str, ...);         // uses stack string
```

Each extract allocated an unreferenced `string_heap_` entry permanently.

## Fix

Keep `define_str` as a local `std::string` and pass it directly to
`parse_to_flat`. Do not capture `define_idx` or `push_back`.

Sibling sites from the same audit (`module_loader` circular-dep message,
module prefix inject) were cleaned in #1488; `audit_dead_heap_push.py`
reports **0 candidates** (gate #1668).

## Tests

`tests/test_mutate_dead_heap_define_str_1691.cpp`
