# Epic #1449 — EDSL Surface Slim v2.0 progress

**Goal**: public engine primitives ≤ **420**; observability via facade; convenience via `lib/std`.

| Phase | Issue | Status |
|-------|-------|--------|
| Infra (gate, deprecation counter, design) | #1448 | **Done** (`a1276ed2`) |
| Observability facade + residual stats | #1450 | **Done** — Phase1 facade + Phase2 residual public `*-stats` → `register_stats_impl` |
| Governance policy doc | #1451 | **Done** — `primitive-governance-policy.md` + `(primitive:validate-new)` |
| Testing framework entry | #1452 | **Done** — harness + binding gate + declarative self-test |
| Prim test binding hard gate | #1453 | **Done** — coverage umbrella + TestRegistry + PR template |
| Hard removal + migration guide | #1455 | Open |

## #1450 deliverables

| Item | Detail |
|------|--------|
| `(stats:get name)` | Engine primitive; routes `register_stats_impl` + public residual aliases |
| `(stats:prefix p)` | Engine list of catalog names with prefix |
| `(engine:surface)` | Inventory hash: public-count, stats-catalog-count, budgets, deprecation counter |
| Residual 10 public `*-stats` | **Removed from public registry** — only via `stats:get` / `engine:metrics` |
| Catalog size | Still ≤420 (`stats:count`) |

**Naming note**: issue text asked for `query:primitive-surface-stats`.  
#1448 freeze forbids new public `*-stats` names → canonical form is **`(engine:surface)`**.

## Snapshot (after #1450 Phase 2)

- Public `add()`: ~609 (was ~619; −10 residual stats)
- Public `add()` `*-stats`: **0**
- Stats catalog: ~385–400 (still ≤420)
- Access path: `(stats:get "gc-stats")` / `(engine:metrics "…")`

## Next under this epic

1. Tier-1 query demotion batch (siblings / find-by-name / …) + #1462 migration.  
2. Convenience string/json/math remaining surface → stdlib only.  
3. Hard removal of grandfathered blocked-pattern names still in freeze baseline.  
