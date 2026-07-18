# PersistentChildVector / pmr SoAColumnarFull DOD + Contracts (#1624)

**Issue:** [#1624](https://github.com/cybrid-systems/aura/issues/1624)  
**Builds on:** #1520 · #431 · #370 · #568 · #1619  
**Status:** Production closed-loop (refine Concepts + Contracts + metrics).

## Problem

IR/FlatAST already use SoA + PCV COW, and #1520 shipped `children_columnar` /
`SafePCVSpan` metrics, but Agents lacked:

- **`SoAColumnarFull`** (columnar_accessor + stable_shape_id) for PCV / SafePCVSpan
- Contracts on **`get_child` / `set_child`**
- Explicit **`soa_dod_migration_progress` / `pcv_columnar_hit_rate`** on the
  existing `query:children-column-stats` surface (no new query:*-stats)

## Contract

```
SoAColumnar        size/empty/data          (#431)
SoAColumnarFull    + columnar_accessor
                   + stable_shape_id        (#1624)
ChildColumnar      SoAColumnar + begin/end  (#1520)

PersistentChildVector / SafePCVSpan → SoAColumnarFull
std::pmr::vector columns            → SoAColumnar

FlatAST::get_child(id, idx)  → children_columnar path + pre(id valid)
FlatAST::set_child(...)      → pre(id valid); post size preserved
```

## Metrics (`query:children-column-stats`, schema **1624**)

No new `query:*-stats` (#1448 freeze).

| Key | Meaning |
|-----|---------|
| `soa_dod_migration_progress` | Columnar hits (progress units) |
| `pcv_columnar_hit_rate` / `_bp` | col/(col+raw)×10000 |
| `soa-columnar-concept-enforced` | 1 |
| `soa-columnar-full-enforced` | 1 |
| `pmr-columns-soa-columnar` | 1 |
| `get-set-child-contracts` | 1 |
| `schema` | **1624** (lineage 1520\|568\|370) |

## Tests

| File | Role |
|------|------|
| `tests/test_soa_dod_pcv_pmr_1624.cpp` | **#1624** AC |
| `tests/test_issue_1520.cpp` | Lineage accepts 1624\|1520 |

## Related

- `src/core/concepts.ixx` — SoAColumnarFull
- `src/core/persistent_child_vector.hh` — PCV / SafePCVSpan adapters
- `src/core/ast.ixx` — get_child / set_child contracts
