# IR SoA dual-emit feature flag default-off (#1629)

**Issue:** [#1629](https://github.com/cybrid-systems/aura/issues/1629)  
**Builds on:** #1377 · #684 · #1318 · #1619  
**Status:** Production closed-loop (default single-emit).

## Problem

Unconditional dual-emit (AoS + SoA columns every lower) paid ~50% memory
and 10–30% CPU with no production consumer (passes still convert back via
`to_aos_view`). Scaffold remains for Phase 2+ migration.

## Solution

```
CompilerService::enable_soa_dual_emit_{false}   // default
ir_soa_migration::g_enable_soa_dual_emit{false}

lower_to_ir_impl:
  if (soa_dual_emit_enabled()) {
    state.enable_soa_dual_emit();
    record_dual_emit_bridge();
  } else {
    record_dual_emit_skipped();
    clear g_last_soa_snapshot;
  }

absorb_lower_soa_snapshot: early-out if !enabled
SoAtoAoSBridgePass::run: early-out if !enabled || empty
```

Opt-in: `cs.set_soa_dual_emit(true)` or
`ir_soa_migration::set_soa_dual_emit_enabled(true)`.

## Metrics (`query:soa-adoption-stats`, schema **1629**)

| Key | Meaning |
|-----|---------|
| `soa-dual-emit-enabled` | 0/1 live flag |
| `soa-dual-emit-default-off` | 1 |
| `soa-dual-emit-bridge-count` | dual-emit lowers |
| `soa-dual-emit-skipped-total` | single-emit skips |
| `soa-dual-emit-flag-wired` | 1 |
| `schema` | **1629** (lineage 1619\|1517\|1377) |

## Tests

| File | Role |
|------|------|
| `tests/test_ir_soa_dual_emit_flag_1629.cpp` | **#1629** AC |
| `tests/test_ir_soa_dual_emit.cpp` | #1377 lineage |

## Related

- `src/compiler/jit_typed_mutation_stats.h` — process flag
- `src/compiler/lowering_impl.cpp` — lower gate
