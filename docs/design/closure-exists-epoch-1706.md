# Closure provenance OOR vs epoch 0 (#1706)

**Issue:** [#1706](https://github.com/cybrid-systems/aura/issues/1706)  
**Siblings:** [#1485](https://github.com/cybrid-systems/aura/issues/1485),
[#1656](https://github.com/cybrid-systems/aura/issues/1656)  
**Files:** `aura_jit_runtime.cpp`, `aura_jit_bridge.h`, `runtime_shared.h`,
`aura_jit_bridge_stub.cpp`  
**Status:** P2 contract — disambiguate provenance `0` from out-of-range.

## Problem

```cpp
aura_get_closure_bridge_epoch(id);   // 0 if OOR *or* real epoch 0
aura_get_closure_defuse_version(id); // same
```

## Fix (Option B)

```cpp
int aura_closure_exists(int64_t closure_id);
// 1 = id indexes an allocated table slot (may still be freed)
// 0 = negative / never allocated
```

Call pattern:

```c
if (!aura_closure_exists(id)) { /* missing slot */ }
if (aura_closure_is_freed(id)) { /* freed */ }
uint64_t ep = aura_get_closure_bridge_epoch(id); // 0 is valid stamp
```

Legacy OOR→0 on epoch accessors preserved for #1485 ABI.

## Tests

`tests/test_closure_exists_epoch_1706.cpp`
