# TypedMutationAudit real invariant enforcement (#1614)

**Issue:** [#1614](https://github.com/cybrid-systems/aura/issues/1614)  
**Builds on:** #1589 · #1538 · #1478 · #1611 · #610  
**Status:** Production closed-loop (phase 3).

## Problem

#1589 trail + sampling existed, but did not run type / linear / provenance
checks — insufficient for AI self-evolution safety.

## Solution

| Leg | Implementation |
|-----|----------------|
| Type | `PostMutationInvariantVisitor` / `post_mutation_invariant_check` |
| Linear | `Evaluator::linear_post_mutate_enforce_all` |
| Provenance | `post_mutation_reflect_validate` |
| Gate | `typed_audit::should_audit(mutation_id)` (Off/Full/Sampled) |
| Hot path | `exit_mutation_boundary` success + `nodes_changed > 0` |
| Entry | `Evaluator::run_typed_mutation_invariant_audit` |

## Query (`query:typed-mutation-audit-trail`, schema **1614**)

| Key | Meaning |
|-----|---------|
| `invariant-audits` | Suite invocations |
| `type-invariant-ok` / `type-invariant-fail` | Type leg |
| `linear-invariant-ok` / `linear-invariant-fail` | Linear leg |
| `provenance-invariant-ok` / `fail` | Reflect/provenance leg |
| `invariant-violations-caught` | Any leg failed |
| `invariant-all-pass` | All three passed |
| `*-wired` | Wire flags |
| `schema` | **1614** (lineage 1589) |

## Related

- `docs/design/typed-mutation-audit.md` (#1589)
- `docs/design/linear-validation.md` (#1538)
