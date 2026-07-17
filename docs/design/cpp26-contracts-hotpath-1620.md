# C++26 Contracts + consteval hot-path expand (#1620)

**Issue:** [#1620](https://github.com/cybrid-systems/aura/issues/1620)  
**Builds on:** #1321 · #1466 · #1519 · #742 · #431  
**Status:** Production closed-loop.

## Problem

Contracts and consteval covered many Arena/Value/Shape/Pass paths after
#1321/#1519, but gaps remained on FlatAST `get`/`type_id`/`mark_dirty`,
Value Special bit encodings, Arena max-tier constants, and SoAView phase
alignment — missing early capture of mutation-induced corruption.

## Solution

| Layer | Change |
|-------|--------|
| `cxx26_invariants.ixx` | +12 consteval asserts (Arena max, dirty depth, NodeTag pack, Special encodings, SoAView phase) → total **77** |
| `FlatAST::get` / `type_id` / `set_type` / `mark_dirty_upward` | `record_hotpath_invariant_hit` + `contract_assert` |
| `inline_shape_of` Special path | low2 bit contracts for bool/void |
| Coverage flags | `hotpath_contracts_1620_active`, arena/value/shape/flatast flags |
| `kContractHotPathsShipped` | 48 → **56** |

## Metrics (`query:cpp26-contracts-stats`, schema **1620**)

| Key | Meaning |
|-----|---------|
| `consteval-checks` | 77 (compile-time baked) |
| `hotpath-invariant-hits` | runtime probes |
| `hotpath-contracts-1620-active` | 1 |
| `arena-tier-contracts-active` | 1 |
| `value-as-star-contracts-active` | 1 |
| `shape-bit-test-contracts-active` | 1 |
| `flatast-get-type-contracts-active` | 1 |
| `schema` | **1620** (lineage 742) |

## Tests

| File | Role |
|------|------|
| `tests/test_cpp26_contracts_hotpath_1620.cpp` | **#1620** AC |
| `tests/test_cpp26_contracts_hotpath_arena_soa_value_shape_pass.cpp` | Lineage |
| `tests/test_production_sweep_1321_1324.cpp` | consteval ≥ 36 |

## Related

- `docs/contributing.md` (Contracts checklist)
- `src/core/cpp26_contract_stats.h`
