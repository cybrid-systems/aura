# SoAView concept enforcement + EDSL hot-path migration (#1619)

**Issue:** [#1619](https://github.com/cybrid-systems/aura/issues/1619)  
**Builds on:** #1241 · #1517 · #1057 · #1577 · #1578  
**Status:** Production closed-loop.

## Problem

Pass pipeline concepts (`Pass` / `DirtyAwarePass` / `SoAViewAwarePass` / …)
and soft metrics existed (#1517), but SoAView lacked a mandatory
`columnar_accessor()`, pack-level `static_assert` was not explicit at every
pipeline entry, and EDSL hot-path helpers (tag_arity / children / apply_closure)
were incomplete for ≥80% migration observability.

## Solution

| Piece | Behavior |
|-------|----------|
| `SoAView` concept | Requires `columnar_accessor()` + shape_id + linear_ownership |
| `IRFunctionSoAView` | Implements columnar_accessor → opcodes SafePCVSpan |
| `check_pipeline_dod_compliance` | Pack-level consteval at run_pipeline / analysis / incremental |
| Production wraps | ConstantFolding / DeadCoercion / TypePropagation → SoAViewAwarePass |
| EDSL helpers | `consult_tag_arity`, `record_edsl_children_soa_path`, `consult_closure_shape_linear` |
| `migration_ratio_bp()` | hits/(hits+misses)×10000 |

## Metrics (`query:soa-view-enforcement-stats`, schema **1619**)

No new `query:*-stats` (#1448 freeze). Also bumps `query:soa-adoption-stats` schema to 1619.

| Key | Meaning |
|-----|---------|
| `concept-enforcement-hits` | SoA-aware pass uses_soa_view path |
| `soa-view-pass-skipped` | legacy / unmarked / uses_soa_view=false |
| `edsl-soa-migration-progress` | EDSL hot-path migration counter |
| `soa-view-hits` / `misses` | IRFunctionSoAView consults |
| `migration-ratio-bp` | hits ratio in basis points |
| `soa-view-full-compliant` / `static-assert-enforced` | 1 |
| `columnar-accessor-required` / `pipeline-pack-check` | 1 |
| `schema` | **1619** (lineage 1517) |

## Tests

| File | Role |
|------|------|
| `tests/test_soa_view_enforcement_1619.cpp` | **#1619** AC |
| `tests/test_issue_1517.cpp` | Lineage accepts 1619 |

## Related

- `src/compiler/soa_view.ixx`
- `src/core/concept_constraints.ixx`
- `src/compiler/pass_manager.ixx`
