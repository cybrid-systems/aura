# Shape + linear collaborative pass mandate (#1661)

**Issue:** [#1661](https://github.com/cybrid-systems/aura/issues/1661)  
**Builds on:** #462 · #1531 · #606 · #538 · #629  
**Status:** Production mandate — ShapeAwareFoldingPass × EscapeAnalysis × Linear × GuardShape × CF.

## Contract

| Collaborator | Role |
|--------------|------|
| **EscapeAnalysisPass** | Fills `IRFunction::escape_map` (0=non-escaping) |
| **LinearOwnershipPass** | Validates ownership before fold |
| **ConstantFolding** | Runs first; stable IR for shape/linear fold |
| **ShapeAwareFoldingPass** | Linear MoveOp elision + narrow CastOp fold + GuardShape signal |
| **Re-lower after mutate** | Pipeline re-runs full pass set (dirty-aware) |

### Folds

1. **Linear DCE**: `MoveOp` on Owned + non-escaping source → `Nop`
2. **Narrow CastOp**: `CastOp` with `narrow_evidence != 0` → `Nop` (#1661 completes #462 Cycle 2)
3. **Specialized signal**: `specialized_for != 0` / `GuardShape` → opportunity counter

## Metrics (`query:shape-folding-stats`, schema **1661**)

| Key | Meaning |
|-----|---------|
| `shape_aware_fold_hits` | alias of shape-fold-count |
| `linear_ownership_dce_savings` | alias of shape-linear-elide-count |
| `guardshape_inserted_count` | alias of guard-shape-hits (presence) |
| `specialized-shape-fold-opportunities` | specialized_for / GuardShape collab |
| `escape-analysis-collab-wired` | 1 |
| `narrow-evidence-cast-fold-wired` | 1 |
| `shape-linear-collaborative-mandate-active` | 1 |
| `schema` | **1661** (lineage 462) |

## Tests

| File | Role |
|------|------|
| `tests/test_shape_linear_collaborative_pass_1661.cpp` | **#1661** AC |
| `tests/test_issue_462_shape_aware_folding.cpp` | Prior pass unit |

## Related

- `docs/design/shape-stability.md`
- `src/compiler/pass_manager.ixx` — `ShapeAwareFoldingPass`
