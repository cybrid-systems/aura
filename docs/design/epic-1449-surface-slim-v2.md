# Epic #1449 — EDSL Surface Slim v2.0 progress

**Goal**: public engine primitives ≤ **420**; observability via facade; convenience via `lib/std`.

| Phase | Issue | Status |
|-------|-------|--------|
| Infra (gate, deprecation counter, design) | #1448 | **Done** (`a1276ed2`) |
| Observability facade + residual stats | #1450 | **This PR** — `stats:get`/`stats:prefix`/`engine:surface` |
| Governance policy doc | #1451 | Open (policy text in issue; file optional) |
| Convenience → stdlib | #1452 (epic text) / testing epic | Open |
| Hard removal + migration guide | #1455 | Open |

## #1450 deliverables

| Item | Detail |
|------|--------|
| `(stats:get name)` | Engine primitive; routes `register_stats_impl` + public residual aliases |
| `(stats:prefix p)` | Engine list of catalog names with prefix |
| `(engine:surface)` | Inventory hash: public-count, stats-catalog-count, budgets, deprecation counter |
| Residual 10 public `*-stats` | Catalogued + `PrimMeta.deprecated` (prefer facade) |
| Catalog size | Still ≤420 (`stats:count`) |

**Naming note**: issue text asked for `query:primitive-surface-stats`.  
#1448 freeze forbids new public `*-stats` names → canonical form is **`(engine:surface)`**.

## Snapshot (after #1450)

- Public `add()`: ~616 (target 420; interim ceiling 700)
- Stats catalog: ~400 (includes residual 10)
- Residual public stats aliases: **10** (<<50 AC)

## Next under this epic

1. Demote / hide residual 10 public stats (keep only via `stats:get` if needed).  
2. Tier-1 query demotion batch (siblings / find-by-name / …) + #1462 migration.  
3. Convenience string/json/math remaining surface → stdlib only.  
