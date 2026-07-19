# try_lock_workspace_* updates lock_order TLS (#1768)

**Issue:** [#1768](https://github.com/cybrid-systems/aura/issues/1768)  
**Sibling:** [#1523](https://github.com/cybrid-systems/aura/issues/1523),
[#1388](https://github.com/cybrid-systems/aura/issues/1388)  
**Files:** `evaluator.ixx`  
**Status:** P2 — try_lock skipped lock_order while blocking locks did not.

## Problem

`lock_workspace_shared` / `unique` call `lock_order::on_acquire` /
`on_release`. `try_lock_workspace_shared` only called
`try_lock_shared()`, so a successful try left TLS depth at 0 while
holding the mutex — under-reporting for `(query:lock-order-stats)`
and weakening inversion detection.

## Fix

```cpp
bool try_lock_workspace_shared() {
    lock_order::on_acquire(Level::Workspace);
    if (workspace_mtx_.try_lock_shared()) return true;
    lock_order::on_release(Level::Workspace); // roll back failed try
    return false;
}
```

Same pattern for new `try_lock_workspace_unique()`. Success path
pairs with existing `unlock_workspace_*` which still call
`on_release`.

## Tests

`tests/test_try_lock_workspace_lock_order_1768.cpp`
