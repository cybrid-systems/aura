# Forced dual-check on apply_closure + JIT aura_closure_call (#1626)

**Issue:** [#1626](https://github.com/cybrid-systems/aura/issues/1626)  
**Builds on:** #1485 · #1491 · #1508 · #1511 · #1537 · #1604 · #1607  
**Status:** Production closed-loop (refine metrics + force all three arms).

## Problem

`apply_closure` (map + bridge) and JIT `aura_closure_call` already had
bridge_epoch / EnvFrame dual-check helpers (#1485–#1604), but Agents lacked:

- Explicit **`compiler_closure_envframe_stale_total`** (EnvFrame domain vs bridge)
- Linear third-arm on **bridge entry** + **IR apply** (parity with map path)
- Schema lineage for #1626 AC keys

## Contract

```
apply_closure (map path):
  closure_needs_safe_fallback(cl)
    bridge_epoch stale  → compiler_closure_epoch_mismatch_hits++
    EnvFrame stale      → compiler_closure_envframe_stale_total++  (#1626)
    linear_post_mutate  → linear_ownership_violation_prevented++
    → safe fallback via invoke_closure_bridge_checked / nullopt

apply_closure (bridge path):
  same three arms on provenance  (#1626 linear arm added)

IR apply (ir_executor):
  ir_closure_needs_safe_fallback → envframe + linear arms  (#1626)

aura_closure_call (JIT):
  aura_is_jit_closure_fresh(bridge, defuse)
    → jit_closure_dual_check_total++
    stale → jit_closure_stale_deopt_total++ / refuse native
```

## Metrics (`query:epoch-apply-hotpath-stats`, schema **1626**)

No new `query:*-stats`.

| Key | Meaning |
|-----|---------|
| `compiler_closure_envframe_stale_total` | EnvFrame-domain stale hits |
| `jit_closure_dual_check_total` | JIT freshness probes |
| `dual-check-forced` / `apply-dual-check-wired` / `jit-dual-check-wired` | 1 |
| `linear-dual-check-wired` | 1 |
| `schema` | **1626** (lineage 1607\|1604\|1598\|1508) |

## Tests

| File | Role |
|------|------|
| `tests/test_closure_dual_check_1626.cpp` | **#1626** AC |
| `tests/test_stale_closure_fallback.cpp` | Lineage accepts 1626 |

## Related

- `docs/design/stale-closure-fallback-1604.md`
- `docs/design/apply-closure-epoch-safety.md`
