# Linear Ownership + Coercion synergy (#1615)

**Issue:** [#1615](https://github.com/cybrid-systems/aura/issues/1615)  
**Builds on:** #746 · #691 · #537 · #1538 · #1478 · #1614  
**Status:** Production closed-loop.

## Problem

`is_coercible` rejects `Linear ~ Dynamic`, but post-`apply_coercion_map`
there was no ownership revalidation. `narrow_evidence` on CoercionNodes
was not fully accounted into linear/GuardShape observability.

## Solution

| Piece | Behavior |
|-------|----------|
| `revalidate_linear_after_coercion` | After apply: discover linear bindings on coercion sites → `OwnershipEnv::validate_ownership` |
| Wire sites | `infer_flat_apply`, TypeCheckPass `check_before_lowering`, Evaluator typecheck paths, `post_mutation_invariant_check` (dirty Coercion nodes) |
| GuardShape | Pre-existing: `coercion_narrow_evidence_hits_total` when `narrow_evidence != 0` |
| Metrics | `linear_coercion_reval_count`, `narrow_evidence_propagated_total`, … |

## Query (`query:jit-typed-mutation-stats`, schema **1615**)

| Key | Meaning |
|-----|---------|
| `linear_coercion_reval_count` | Post-coercion revalidation invocations |
| `narrow_evidence_guardshape_hits` | GuardShape narrow_evidence hits (interpreter) |
| `narrow-evidence-propagated` | Coercion sites carrying narrow_evidence |
| `post-coercion-reval-wired` / `guardshape-narrow-wired` | Wire flags |
| `schema` | **1615** (lineage 746) |

## Related

- `docs/design/linear-validation.md`
- CoercionMap provenance (#537 / #691)
