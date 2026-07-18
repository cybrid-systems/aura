# Mandate full JIT opcode coverage + strict consistency default (#1658)

**Issue:** [#1658](https://github.com/cybrid-systems/aura/issues/1658)  
**Builds on:** #1512 · #1289 · #532 · #427 · #1288 · #1534 · #1535  
**Status:** Production mandate — all 54 IROpcode lowered or fail-fast safe-deopt; strict mode default ON.

## Contract

| Layer | Behavior |
|-------|----------|
| `lower()` switch | Explicit cases for Nop…TopCellLoad (54 opcodes) |
| GuardShape | Dual-epoch fence + shape check + deopt branch (`OpGuardShape`) |
| Linear* | Epoch fence + pass-through / move-zero / drop calls |
| PrimCall | Runtime primitive dispatch path |
| Unhandled | **Fail-fast**: `compile()` returns `nullptr` → interpreter (Issue #1289) |
| Strict consistency | **Default ON** — unhandled compile fail bumps `consistency_violations` |
| Debug check | `AuraJIT::force_jit_consistency_check()` (no new public EDSL primitive) |

## Metrics (`query:jit-consistency-stats`, schema **1658**)

| Key | Meaning |
|-----|---------|
| `opcode-tracked-total` | 54 |
| `guard-shape-lowered-wired` | 1 |
| `linear-ops-lowered-wired` | 1 |
| `primcall-lowered-wired` | 1 |
| `fail-fast-unhandled-wired` | 1 |
| `safe-deopt-on-unhandled-wired` | 1 |
| `strict-consistency-default-on` | 1 |
| `force-jit-consistency-check-wired` | 1 |
| `consistency-mandate-active` | 1 |
| `schema` | **1658** (lineage 532 / 1512 / 1289 / 427) |

Legacy #532 keys (`unhandled-count`, `hotswap-*`, `linear-check-hits`, …) remain.

## C++ API

```cpp
AuraJIT jit; // strict_consistency_mode() == true by default
jit.set_strict_consistency_mode(false); // opt-out for debug
bool ok = jit.force_jit_consistency_check();
// kTrackedOpcodeCount == 54
// kFullyLoweredOpcodeMask == (1ull<<54)-1
```

## Tests

| File | Role |
|------|------|
| `tests/test_jit_full_opcode_coverage_1658.cpp` | **#1658** AC |
| `tests/test_issue_1512.cpp` | Coverage API + strict lineage (default ON updated) |
| `tests/test_jit_consistency.cpp` | #427 observability |

## Related

- `src/compiler/aura_jit.cpp` — `lower()` full switch + fail-fast default
- `src/compiler/aura_jit.h` — strict default, `force_jit_consistency_check`
- `docs/PRODUCTION_ISSUES_TRACKER_REFINED.md` (#427)
