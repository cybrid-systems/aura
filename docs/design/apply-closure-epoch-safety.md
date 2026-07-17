# apply_closure + JIT dual-epoch safety (#1491 / #1598)

**Parent closed-loop** over #1475 (EnvFrame version helper), #1477 (JIT dual-epoch fence),
#1508 (aura_closure_call dual check), #1511 (bridge entry dual check), #1525 (multi-fiber).  
**Refine map:** `docs/design/epoch-apply-hotpath-1598.md` (schema 1598 dashboard).

## Contract

Every closure application entry must dual-check before using captured AST / EnvFrame:

| Domain | Capture | Current | Stale when |
|--------|---------|---------|------------|
| Bridge | `Closure.bridge_epoch` / JIT table stamp | `current_bridge_epoch()` / `g_aot_table_epoch` | mismatch, or capture=0 while current≠0 (strict) |
| Defuse / Env | `EnvFrame.version_` / JIT defuse stamp | `defuse_version_` / `g_aot_defuse_version` | frame &lt; current, or capture=0 while current≠0 |

On stale → **safe fallback** (bridge re-dispatch / interpreter) or **JIT deopt** (refuse native, `return 0`) — never eval dangling `flat*` / `pool*`.

`AURA_BRIDGE_EPOCH_LEGACY_TRUST=1` restores pre-#1365 / pre-#1491 “0 is ok” trust for fixtures.

## Entry points

| Path | Gate |
|------|------|
| `apply_closure` local map | `closure_needs_safe_fallback` → `invoke_closure_bridge_checked` |
| `apply_closure` flat* race window | inline `is_bridge_stale` + bridge recovery |
| `apply_closure` local-miss bridge | `invoke_closure_bridge_checked` (provenance null; IR has own checks) |
| IR `call_closure` / OpApply | `ir_closure_needs_safe_fallback` → tree-walker `apply_closure` |
| JIT `aura_closure_call` | `aura_is_jit_closure_fresh` → deopt + invalidate cache |

## Compact collaboration (#1526 / AC3)

`compact_env_frames` rewrites `Closure.env_id` under lock and restamps bridge_epoch so remapped IDs stay dual-check consistent.

## Metrics (observability)

- `compiler_closure_safe_fallbacks`, `closure_bridge_epoch_safety_enforced`
- `closure_stale_apply_count_total`, `closure_safe_fallback_apply_count_total`
- `jit_closure_dual_check_total`, `jit_closure_stale_deopt_total`, `jit_closure_safe_fallbacks`
- `multifiber_mutate_races_detected_total`, `multifiber_safe_fallback_total`

## Tests

| File | Role |
|------|------|
| `tests/test_issue_1491.cpp` | Parent closed-loop (all entries + concurrent) |
| `tests/test_issue_1475.cpp` | Pure `is_env_frame_stale` helper |
| `tests/test_issue_1508.cpp` | JIT dual-check unit |
| `tests/test_issue_1511.cpp` | Bridge-entry dual-check |
| `tests/test_issue_1525.cpp` | Multi-fiber stress |
