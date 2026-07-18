# aura_free_closure free_list before freed (#1708)

**Issue:** [#1708](https://github.com/cybrid-systems/aura/issues/1708)  
**Files:** `aura_jit_runtime.cpp`  
**Status:** P1 correctness — exception ordering between freed bit and free_list.

## Problem

```cpp
g_closure_freed[cid] = 1;
g_closure_free_list.push_back(cid); // if this throws → permanent slot leak
```

## Fix (Option B)

```cpp
g_closure_free_list.push_back(cid); // first
g_closure_freed[cid] = 1;           // second
```

If `push_back` throws, the slot stays live (`freed==0`). Under the unique
table lock, no concurrent alloc can observe an intermediate state.

## Tests

`tests/test_closure_free_list_order_1708.cpp`
