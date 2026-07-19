# AURA_MUTATION_BOUNDARY_PROTECT without bare break (#1745)

**Issue:** [#1745](https://github.com/cybrid-systems/aura/issues/1745)  
**Files:** `evaluator.ixx`  
**Status:** P2 — macro used bare `break` after failed `try_acquire`.

## Problem

The protect macro used:

```cpp
if (!_aura_mbp_gr) {
    _aura_mbp_ok = false;
    break;
}
```

inside `do { ... } while (0)`. While C++ `break` targets the nearest
loop (so the do/while, not an outer `switch`), bare `break` in macros
is a classic audit/review footgun and confuses expansions inside
`switch` cases. Prefer control flow that cannot be misread as escaping
the switch.

## Fix

Restructure to `if (gr) { BODY } else { ok = false; }` — no `break`.
`do/while(0)` retained so the macro is still a single statement.

## Tests

`tests/test_mbp_macro_no_break_1745.cpp`
