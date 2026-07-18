# aura_prim_call argc clamp (#1711)

**Issue:** [#1711](https://github.com/cybrid-systems/aura/issues/1711)  
**Files:** `aura_jit_runtime.cpp`  
**Status:** P1 overflow — 3-element stack `args[]` vs unbounded `count`.

## Problem

```cpp
int64_t args[3] = {a, b, 0};
g_prim_dispatcher(slot, args, count); // count may be > 3 → stack OOB read
```

## Fix (Option A)

```cpp
int32_t safe_count = clamp(count, 0, 3);
g_prim_dispatcher(slot, args, safe_count);
```

## Tests

`tests/test_prim_call_count_clamp_1711.cpp`
