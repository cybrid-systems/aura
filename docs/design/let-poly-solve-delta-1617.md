# Let-Poly dirty invalidation + solve_delta reverify (#1617)

**Issue:** [#1617](https://github.com/cybrid-systems/aura/issues/1617)  
**Builds on:** #798 · #745 · #466 · #409 · #692 · #518 · #148  
**Status:** Production closed-loop.

## Problem

`ConstraintSystem::solve_delta` has worklist + bounded clean reverify
(`touched_roots_`, `occurrence_priority_roots_`, `effective_reverify_limit`).
Under high-churn typed mutations involving **Let-Polymorphism**, ADT match
exhaustiveness, and occurrence narrowing, truncation can drop
re-generalization checks and miss dirty propagation under Let bindings.

## Solution

| Piece | Behavior |
|-------|----------|
| `let_poly_dirty_roots_` | Priority roots (pri 3) between occurrence (4) and plain touched (1) |
| `mark_let_poly_dirty` | Called on Let free-vars at generalization + Let scopes in propagate |
| `solve_delta` | Worklist prefers occurrence → Let-Poly → touched; peak worklist metric |
| Truncated reverify fallback | After scan cap, force remaining pri≥3 (occurrence/Let-Poly) up to 128 |
| `propagate_narrowing_to_uses` | Enclosing Let/LetRec use-sites + Let-Poly stamp |
| `post_mutation_invariant_check` | Let/LetRec dirty → expand scope, re-occurrence, ADT revalidate |

## Metrics (`query:type-incremental-fidelity-stats`, schema **1617**)

No new `query:*-stats` name (#1448 freeze). Keys fold into #798 surface.

| Key | Meaning |
|-----|---------|
| `let-poly-dirty-roots` | `mark_let_poly_dirty` hits |
| `let-poly-regeneralize` | re-generalize check invocations |
| `let-poly-truncation-fallback` | targeted reverify after truncation |
| `let-poly-priority-reverify` | clean constraints on Let-Poly roots |
| `let-poly-post-mutation-scope` | post_mutation Let/LetRec scopes |
| `reverify-truncated` | global `reverify_truncated_total` |
| `solve-delta-worklist-peak` | max solve_delta worklist size |
| `let-poly-wired` | 1 |
| `schema` | **1617** (lineage 798) |

## Tests

| File | Role |
|------|------|
| `tests/test_let_poly_solve_delta_1617.cpp` | **#1617** AC |
| `tests/test_typechecker_incremental_guard_steal_fidelity.cpp` | Lineage accepts 1617 |

## Related

- `docs/design/primitives-style.md` (fidelity-stats fields)
- `ConstraintSystem` in `type_checker.ixx` / `type_checker_impl.cpp`
- #745 occurrence priority, #692 ADT typed mutation scope
