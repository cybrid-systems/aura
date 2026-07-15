# Epic #1449 — EDSL Surface Slim v2.0 progress

**Goal**: **core** public engine primitives ≤ **420**; observability via facade; convenience via `lib/std`.  
Domain verticals (EDA/TUI/git/…) tracked separately and **excluded from the core budget**.

| Phase | Status |
|-------|--------|
| Infra #1448 | **Done** |
| Observability facade #1450 | **Done** |
| Governance / testing #1451–#1453 | **Done** |
| Agent migration #1462 | **Done** |
| Demotion batches 1–3 | **Done** |
| **Core ≤ 420** | **Met** (412 core / 499 total) |

## Final metrics (batch 3)

| Metric | Value |
|--------|------:|
| Public `add()` **total** | **499** |
| Public `add()` **core** | **412** ≤ **420** |
| Domain verticals | **87** |
| Public `*-stats` | **0** |
| Interim hard ceiling (total) | **520** |
| Stats catalog | ~390 |

Domain prefixes (not in core):  
`eda:` `seva:` `verify:` `tui:` `terminal:` `git-` `tcp-` `auto-evolve-` `channel:` `m4-` `strategy:` `synthesize:`

## What was demoted (summary)

### Batch 1
- Query dashboards (health/readiness/slo/score/…) → facade  
- Hard-remove `query:siblings` (use `std/compat` or compose)

### Batch 2
- `dirty:*`, render samples/histograms, more query dashboards  
- `atomic-batch:stats`, `closure:stats`  
- Core vs domain reporting in `check_primitive_surface.py`

### Batch 3
- Compile observability counts/status/cache/epoch/hw-coercion  
- Mutation counters/history/log-size/lightweight records  
- JIT exception metrics; GC/arena readouts (not control `gc-heap`/`gc-temp`/…)  
- AST summary/generation/version/nodes/defs/list-snapshots  
- Workspace/mutation counts; concurrency version snapshots; AOT getters  
- Remaining production-health / serve-health / meta-catalog lists  

**Control primitives stay public** (e.g. `gc-heap`, `gc-temp`, `gc-freeze`, `gc-module`, mutate/query core, arena compact/defrag actions).

## Access path

```scheme
(stats:get "compile:status")
(stats:get "dirty:summary")
(stats:get "atomic-batch:stats")
(stats:get "ast:generation")
(engine:metrics "query:edsl-readiness")
```

## Follow-ups (optional, post-epic)

1. Further shrink **total** (domain s0-gating, git→stdlib shell).  
2. More agent migration for any remaining bare demoted names in out-of-tree prompts.  
3. Keep interim ceiling ratcheting if total grows.

Run: `python3 scripts/check_primitive_surface.py --strict`

---

*Epic core budget achieved in batch 3. Epic #1449 may be closed.*
