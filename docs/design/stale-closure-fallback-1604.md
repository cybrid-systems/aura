# Stale closure fallback on apply + JIT (#1604)

**Issue:** [#1604](https://github.com/cybrid-systems/aura/issues/1604)  
**Refines:** #1475 · #1476 · #1477 · #1485 · #1491 · #1508 · #1511 · #1558 · #1598  
**Status:** Production closed-loop (hotpath gates shipped in sisters; #1604
aligns AC metrics + named stress test + schema 1604).

## Problem

Long-lived closures / JIT specialized calls can outlive
`mutate:rebind` / `mark_define_dirty` / `invalidate_function`. Without
mandatory dual-epoch checks, `apply_closure` or `aura_closure_call` may
touch dangling FlatAST / EnvFrame (UAF or wrong semantics).

## Contract

```
apply_closure (map + bridge):
  closure_needs_safe_fallback(cl)  // bridge_epoch + EnvFrame version + linear
    → stale_closure_prevented++
    → if epoch_stale: closure_epoch_mismatch_fallback++
    → invoke_closure_bridge_checked / nullopt  (never eval dangling flat*)

aura_closure_call (JIT OpApply/OpCall):
  aura_is_jit_closure_fresh(cap_bridge, cap_defuse)
    → stale: aura_jit_closure_record_stale_deopt()
         + stale_closure_prevented++ / closure_epoch_mismatch_fallback++  (#1604)
    → return 0 (host re-enters interpreter)

invalidate / mark_define_dirty:
  atomic_bump_epochs_and_stamp_bridge
    → mutation_epoch_ + defuse_version_ + g_aot_table_epoch + bridges
```

## Metrics (`query:epoch-apply-hotpath-stats`, schema **1604**)

| Key | Meaning |
|-----|---------|
| `stale_closure_prevented` | Apply + JIT deopt prevented dispatches |
| `closure_epoch_mismatch_fallback` | Epoch-domain fallbacks |
| `jit_closure_dual_check_total` | JIT freshness probes |
| `jit_closure_stale_deopt_total` | JIT refuse-native |
| `jit-deopt-bumps-ac-metrics` | constant 1 (#1604 wire) |
| `apply-path-wired` / `jit-path-wired` | constants 1 |
| `schema` | **1604** (lineage 1598) |

## Tests

| File | Role |
|------|------|
| `tests/test_stale_closure_fallback.cpp` | **#1604** named AC + concurrent mutate/apply |
| `tests/test_epoch_apply_hotpath_1598.cpp` | Closed-loop inventory (schema 1604\|1598) |
| `tests/test_issue_1491.cpp` / `1485` / `1508` | Sister unit surfaces |

## Related docs

- `docs/design/apply-closure-epoch-safety.md`
- `docs/design/epoch-apply-hotpath-1598.md`
- `docs/design/closure-dual-epoch-apply.md`
