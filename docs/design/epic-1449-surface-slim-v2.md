# Epic #1449 — EDSL Surface Slim v2.0 progress

**Goal**: **core** public engine primitives ≤ **420**; observability via facade; convenience via `lib/std`. Domain verticals (EDA/TUI/git/…) are tracked separately.

| Phase | Issue | Status |
|-------|-------|--------|
| Infra (gate, deprecation counter, design) | #1448 | **Done** |
| Observability facade + residual stats | #1450 | **Done** |
| Governance / testing / binding | #1451–#1453 | **Done** |
| Agent migration guide + shims | #1462 | **Done** |
| Demotion batch 1: dashboards + siblings | #1449 | **Done** |
| Demotion batch 2: dirty/render + stats leftovers + core budget | #1449 | **Done** (this) |
| Hard removal → core ≤420 | epic remainder | **Open** (gap ~64 core) |

## SlimSurface metrics (batch 2)

| Metric | After batch 1 | After batch 2 |
|--------|-------------:|-------------:|
| Public `add()` **total** | 587 | **571** |
| Public `add()` **core** (ex domain) | ~500 | **484** |
| Domain verticals | ~87 | **87** |
| Public `*-stats` | 0 | **0** |
| Interim hard ceiling (total) | 600 | **590** |
| Core target | 420 | 420 (**gap ~64**) |

Domain prefixes (excluded from core budget):  
`eda:` `seva:` `verify:` `tui:` `terminal:` `git-` `tcp-` `auto-evolve-` `channel:` `m4-` `strategy:` `synthesize:`

## Batch 2 demotions (source → `register_stats_impl`)

- `dirty:reasons` / `dirty:ppa-reasons` / `dirty:counts` / `dirty:summary`
- `render-prim-latency-samples` / `render-frame-time-samples` / `render-hotpath-depth` / `query:render-frame-time-histogram`
- `query:arena-fragmentation-snapshot` / `query:render-ffi-available` / `query:seva-audit-log`
- `query:epoch-delta-since-last-query` / `query:incremental-effectiveness` / `query:tag-arity-count`
- `atomic-batch:stats` / `closure:stats`

Access: `(stats:get "dirty:summary")`, `(stats:get "atomic-batch:stats")`, etc.  
`std/agent` decision metrics updated to use `stats:get` for atomic-batch.

## Access path

```scheme
(stats:get "query:edsl-readiness")
(stats:get "dirty:summary")
(stats:get "atomic-batch:stats")
(engine:metrics "query:edsl-readiness")
```

## Next under this epic

1. More core demotion (compile convenience, mutation-lightweight*, check-* audits).  
2. Optional: gate domain packs harder on s0; keep full for EDA/TUI demos.  
3. Close epic when **core public ≤ 420**.

Run: `python3 scripts/check_primitive_surface.py --strict`

---

*Batch 2: dirty/render/stats leftovers + core-vs-domain budget reporting.*
