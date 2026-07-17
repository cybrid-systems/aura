# Macro hygiene closed-loop health observability (#1613)

**Issue:** [#1613](https://github.com/cybrid-systems/aura/issues/1613)  
**Builds on:** #1609 · #1610 · #1611 · #1612 · #1593 · #1589 · #1501  
**Status:** Production closed-loop (consolidated stats + audit trail).

## Primitives (via `engine:metrics`)

| Name | Schema | Role |
|------|--------|------|
| `query:macro-hygiene-stats` | **1613** | Authoritative macro hygiene health + breakdown |
| `query:ai-closedloop-readiness-stats` | **1613** | Macro submodule folded into overall readiness |

No new public `query:*-stats` (#1448 freeze).  
`ai-closedloop-macro-health` is the **submodule key set** on readiness + the
dedicated `query:macro-hygiene-stats` surface (same health formula).

## `query:macro-hygiene-stats` fields

| Key | Meaning |
|-----|---------|
| `root-skips` / `recursive-skips` | query:pattern MacroIntroduced filters |
| `hygiene-violations` | Violation counter |
| `macro-markers` / `hygiene-index-served` | Workspace markers + user-only index |
| `ir-hygiene-stamped-count` | IR MacroIntroduced stamps (#1610) |
| `macro-stale-ref-prevented` / `macro-provenance-repin-total` | Fiber refresh (#1612) |
| `reflect-macro-hygiene-checks` / `rejects` | Reflect gate (#1611) |
| `naked-macro-mutate-attempts` | Blocked mutate without allow (#373) |
| `macro-audit-*` / `audit-trail-writes` | TypedMutationAuditPass (#1589/#1613) |
| `health-score` / `hygiene-health-score` | 0–100 (higher better) |
| `recommendation` / `action` | 0=ok … 4=hygiene-critical |
| `schema` | **1613** |

## Readiness submodule keys

`macro-health-score`, `macro-hygiene-violations`, `macro-naked-mutate-attempts`,
`macro-stale-ref-prevented`, `macro-query-skips`, `macro-reflect-rejects`,
`macro-audit-blocked`, `macro-audit-events`, `macro-hygiene-submodule-wired`.

Overall `health-score` is soft-penalized when `macro-health-score < 80`.

## Audit trail

`typed_audit::capture_macro_hygiene_audit` records hygiene-protected mutate
blocks (`MutationKind::MacroHygiene`) always-on (bypasses Sampled gate).

## Tests

| File | Role |
|------|------|
| `tests/test_macro_hygiene_closedloop_health_1613.cpp` | **#1613** AC |
| Lineage readiness / #1501 tests | Accept schema 1613 |

## Related

- `docs/design/ai-closedloop-readiness.md`
- `docs/design/query-pattern-hygiene-1609.md`
