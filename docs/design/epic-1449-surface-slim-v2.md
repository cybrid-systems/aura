# Epic #1449 — EDSL Surface Slim v2.0 progress

**Goal**: public engine primitives ≤ **420**; observability via facade; convenience via `lib/std`.

| Phase | Issue | Status |
|-------|-------|--------|
| Infra (gate, deprecation counter, design) | #1448 | **Done** |
| Observability facade + residual stats | #1450 | **Done** — Phase1 facade + Phase2 residual public `*-stats` → `register_stats_impl` |
| Governance policy doc | #1451 | **Done** — `primitive-governance-policy.md` + `(primitive:validate-new)` |
| Testing framework entry | #1452 | **Done** — harness + binding gate + declarative self-test |
| Prim test binding hard gate | #1453 | **Done** — coverage umbrella + TestRegistry + PR template |
| Agent migration guide + shims | #1462 | **Done** (Plan A) |
| **Demotion batch: dashboards + Tier-1 siblings** | #1449 progress | **Done** (this batch) |
| Hard removal → public ≤420 | epic remainder | **Open** |

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

## Demotion batch (post-#1450) — facade dashboards + siblings

| Change | Effect |
|--------|--------|
| Expand `is_legacy_stats_name` | **All** `*-stats` + `query:*` health/readiness/slo/score/fidelity dashboards → `register_stats_impl` (not public `add()`) |
| Hard-remove `query:siblings` | Tier-1 complete for this name; use `std/compat` or compose `children`/`parent` |
| Interim hard ceiling | `700` → **`620`** in `check_primitive_surface.py` (ratchet; still soft-note above 420) |
| Tests | `tests/test_issue_1449_demotion_batch.cpp`, `#1462` AC7, suite updates |

Access path for demoted dashboards:

```scheme
(stats:get "query:edsl-readiness")
(engine:metrics "query:edsl-readiness")
;; NOT (query:edsl-readiness) as a public primitive
```

## Snapshot

| Metric | After #1450 Phase 2 | After this batch |
|--------|--------------------:|-----------------:|
| Public `add()` | ~609–610 | **587** (dashboards + siblings + residual `*-stats`) |
| Public `add()` `*-stats` | **0** | **0** |
| Stats catalog | ~385–400 | +dashboards via facade |
| Interim ceiling | 700 | **600** |
| Target | 420 | 420 (**remaining gap ~167**) |

Exact counts: run `python3 scripts/check_primitive_surface.py --strict`.

## Next under this epic

1. More Tier-1/2 query demotions (find-by-name already stdlib-only; expand compose helpers).  
2. Convenience domain packs: `git-*` / `tcp-*` / `auto-evolve-*` / optional `eda:` s0 gating.  
3. Hard removal of grandfathered blocked-prefix names that are true stdlib duplicates (careful: `string-*` / `hash-*` stay — language core).  
4. Final public ≤420 validation + close epic #1449.  

---

*Updated with demotion batch for dashboards + `query:siblings` (#1449 progress).*
