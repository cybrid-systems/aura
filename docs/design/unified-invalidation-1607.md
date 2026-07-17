# Unified invalidation dual-epoch protocol (#1607)

**Issue:** [#1607](https://github.com/cybrid-systems/aura/issues/1607)  
**Refines:** #1496 · #1476 · #1475 · #1491 · #1477  
**Status:** Production closed-loop (core in #1496; #1607 AC dashboard + named test).

## Single write-side entry

```
mark_define_dirty(name)          // soft: dirty bits + cascade
invalidate_function(name)        // hard: + JIT erase / dep_graph teardown
        │
        └─► atomic_bump_epochs_and_stamp_bridge(name)
              release fence
              bump_bridge_epoch()            // mutation_epoch_
              bump defuse_version_
              aura_aot_bump_func_table_epoch()
              on_typed_mutation_epoch_bump() // solve_delta wipe
              invalidate_bridge_for / JIT batch_deopt
              notify_walk_active_closures_
```

Readers (`apply_closure`, `aura_is_jit_closure_fresh`) acquire-load either
domain and take safe fallback / deopt — never half-updated state.

## Metrics (`query:unified-invalidation-stats`, schema **1607**)

| Key | Source |
|-----|--------|
| `invalidate_cascade_depth` | `invalidate_cascade_depth_total` |
| `invalidate_cascade_depth_max` | HWM |
| `bridge_epoch_bumps` | `bridge_epoch_bumps_total` |
| `live_closure_stale_prevented` | `compiler_live_closure_stale_prevented_total` |
| `unified_invalidation_protocol_total` | Protocol entries |
| `soft-hard-same-protocol` | constant 1 |

## Tests

| File | Role |
|------|------|
| `tests/test_unified_invalidation_1607.cpp` | **#1607** AC + concurrent stress |
| `tests/test_issue_1496.cpp` | Original unify suite |
| `tests/test_issue_1496_concurrent_epoch_safety.cpp` | Concurrent epoch |

## Related

- `docs/design/unified-invalidation-epoch-protocol.md`
- `docs/design/epoch-apply-hotpath-1598.md`
