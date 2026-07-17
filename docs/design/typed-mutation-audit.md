# TypedMutationAuditPass — production audit trail

**Issues:** #1614 (invariant enforcement), #1589 (production trail), #1216 (Phase 1 scaffold)  
**API:** `src/compiler/typed_mutation_audit.h`  
**Module inventory:** `src/compiler/typed_mutation_audit_pass.ixx`  
**Invariant suite:** `docs/design/typed-mutation-invariant-audit-1614.md`

## Problem

Phase-1 scaffold only had a non-atomic `should_audit` and counters — no event
capture, no trail, no thread safety — insufficient for AI self-evolution audit.

## Solution

| Piece | Behavior |
|-------|----------|
| `AuditStrategy` | Off / Sampled (configurable ratio) / Full — atomic |
| `should_audit(id)` | Thread-safe gate |
| `TypedMutationAuditEvent` | mutation_id, name, kind, epochs, outcome, target, fiber |
| Ring trail | 256 slots, mutex writers, `trail_latest` / `trail_at_seq` |
| Integration | `Evaluator::exit_mutation_boundary` success + rollback |
| Query | `query:typed-mutation-audit-trail` (schema **1614**, lineage 1589) |
| Control | C++ `set_strategy` / `set_sample_ratio` (no extra public prim — SlimSurface) |
| Invariants (#1614) | type (`post_mutation_invariant_check`) + linear (`linear_post_mutate_enforce_all`) + provenance (`post_mutation_reflect_validate`) on Guard success when `should_audit` |

## Counters (query keys)

- `contextual-total` — events captured (AC: typed_mutation_audit_contextual_total)  
- `trail-size` — live ring occupancy (AC: typed_mutation_audit_trail_size)  
- `audits-considered`, `samples-skipped`, `rollbacks`, `errors`, `strategy`, `sample-ratio`  
- #1614: `invariant-audits`, `type-invariant-ok/fail`, `linear-invariant-ok/fail`, `provenance-invariant-ok/fail`, `invariant-violations-caught`, `invariant-all-pass`

## Related

- #676 mutation audit log / WAL (security ring — complementary)  
- #839 / #864 prior typed-mutation-audit-* stats scaffolds  
