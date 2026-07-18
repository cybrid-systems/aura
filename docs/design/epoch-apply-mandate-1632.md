# Mandate dual-epoch check on apply_closure + JIT + IR (#1632)

**Issue:** [#1632](https://github.com/cybrid-systems/aura/issues/1632)  
**Builds on:** #1491 · #1475 · #1477 · #1604 · #1626 · #1627  
**Status:** Production mandate — bridge_epoch + defuse/EnvFrame check on every hot apply path.

## Problem

Live closures held across `mutate:rebind` / `invalidate_function` can dangle
if `apply_closure` (map + bridge), JIT `aura_closure_call`, or IR apply skip
epoch/version checks after `bridge_epoch` / `defuse_version_` bumps.

## Contract

```
apply_closure (map path):
  closure_needs_safe_fallback(cl)
    bridge_epoch stale     → epoch_mismatch + live_closure_stale_prevented
    EnvFrame version stale → envframe_stale + live_closure_stale_prevented
    linear post-mutate     → linear_ownership_violation_prevented
    → safe fallback via invoke_closure_bridge_checked / nullopt
  race window after materialize_call_env → re-check + same metrics

apply_closure (bridge path):
  invoke_closure_bridge_checked — same dual-check on provenance

IR apply (ir_executor call_closure):
  ir_closure_needs_safe_fallback → metrics + re-enter apply_closure

JIT aura_closure_call:
  aura_is_jit_closure_fresh(bridge, defuse)
    stale → aura_jit_closure_record_stale_deopt
         → stale_closure_prevented / bridge_epoch_mismatch_fallback
         → live_closure_stale_prevented / deopt (return 0)
```

## Metrics (`query:epoch-apply-hotpath-stats`, schema **1632**)

No new public `query:*-stats` (SlimSurface).

| Key | Source |
|-----|--------|
| `live_closure_stale_prevented` | `compiler_live_closure_stale_prevented_total` |
| `bridge_epoch_mismatch_fallback` | alias of `closure_epoch_mismatch_fallback` |
| `stale_closure_prevented` | apply + JIT deopt |
| `apply-epoch-mandate-active` / `jit-epoch-mandate-active` | 1 |
| `defuse-version-check-wired` / `bridge-epoch-check-wired` | 1 |
| `schema` | **1632** (lineage 1627…1598) |

## Tests

| File | Role |
|------|------|
| `tests/test_epoch_apply_mandate_1632.cpp` | **#1632** AC |
| `tests/test_closure_dual_check_1626.cpp` | Prior dual-check |
| `tests/test_stale_closure_fallback.cpp` | Concurrent mutate → apply |

## Related

- `docs/design/closure-dual-check-1626.md`
- `docs/design/stale-closure-fallback-1604.md`
- `docs/design/apply-closure-epoch-safety.md`
- `docs/design/epoch-apply-hotpath-1598.md`
