# Unify apply_closure dual-path + EnvFrame SoA staleness (#1660)

**Issue:** [#1660](https://github.com/cybrid-systems/aura/issues/1660)  
**Builds on:** #1632 · #1626 · #1604 · #1511 · #1475 · #1365 · #1269  
**Status:** Production mandate — single `closure_is_epoch_or_env_stale` + SoA version_/parent_id_.

## Contract

| Path | Gate |
|------|------|
| `apply_closure` map | `closure_needs_safe_fallback` → uses `closure_is_epoch_or_env_stale` |
| `apply_closure` bridge | `invoke_closure_bridge_checked` dual-check |
| `materialize_call_env` | SoA `version_` refresh + linear enforce; probes unified helper |
| JIT `aura_closure_call` | dual-epoch deopt (bridge + defuse) |
| IR apply | `ir_closure_needs_safe_fallback` → tree-walker apply |

### Unified helper (`Evaluator::closure_is_epoch_or_env_stale`)

```
true  ⇔  is_bridge_stale(bridge_epoch, current)
      OR (env_id valid AND (is_env_frame_invalid OR is_env_frame_stale))
// linear-only is NOT included — use linear_post_mutate_enforce
```

### EnvFrame SoA

| Field | Role |
|-------|------|
| `env_frames_` | dense SoA deque of frames |
| `parent_id_` | parent-chain index walk (no raw Env*) |
| `version_` | anti-staleness vs `defuse_version_` |

On stale → safe fallback / empty Env / refresh — never eval dangling flat*/pool*.

## Metrics (`query:epoch-apply-hotpath-stats`, schema **1660**)

| Key | Meaning |
|-----|---------|
| `epoch-stale-total` | bridge epoch mismatch hits |
| `env-stale-total` | EnvFrame version_ stale |
| `linear-stale-total` | linear ownership prevented |
| `stale-EnvFrame-prevented` | alias of envframe_stale |
| `unified-stale-helper-wired` | 1 |
| `envframe-soa-version-wired` / `envframe-parent-id-walk-wired` | 1 |
| `materialize-call-env-stale-wired` | 1 |
| `apply-envframe-soa-mandate-active` | 1 |
| `schema` | **1660** (lineage 1632…1598) |

## Tests

| File | Role |
|------|------|
| `tests/test_apply_closure_envframe_soa_1660.cpp` | **#1660** AC |
| `tests/test_epoch_apply_mandate_1632.cpp` | Prior mandate |
| `tests/test_stale_closure_fallback.cpp` | Concurrent mutate → apply |

## Related

- `docs/design/epoch-apply-mandate-1632.md`
- `docs/design/apply-closure-epoch-safety.md`
- `docs/design/closure-dual-check-1626.md`
