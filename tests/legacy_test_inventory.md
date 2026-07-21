# Legacy test inventory

**Issue:** [#1957](https://github.com/cybrid-systems/aura/issues/1957)
**Generated:** 2026-07-22 by `scripts/inventory_legacy_tests.py`
**Status:** living document — re-run the script after consolidations.

## Purpose

Categorize legacy per-issue regression tests so we can migrate them in batches into the preferred `tests/domain/` structure (and existing family batch drivers under `tests/test_*_batch.cpp`).

Do **not** add new `tests/issues/test_issue_*.cpp` files.

## Scope snapshot

| Location | Count | Notes |
|----------|------:|-------|
| `tests/issues/test_issue_*.cpp` | 0 | Legacy per-issue mains / bundle members |
| `tests/test_*.cpp` (issue-oriented) | 0 | Numbered root tests + `*_batch` drivers |
| `tests/domain/test_*.cpp` | 8 | Preferred destination suites |
| **Total scanned** | **8** | |

### Related artifacts

- Coarser 5-bucket Phase-2 map: [`tests/domain_classification.md`](domain_classification.md) (`scripts/classify_test_issues.py`)
- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)
- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)
- Test layout rules: [`tests/README.md`](README.md)

## Theme buckets (8 + uncategorized)

Classification uses the **filename + first 50 lines** (keywords and filename token boosts). Ties break toward earlier themes in the priority order below.

| Theme | Title | Issues | Root | Domain | Total | Migration priority |
|-------|-------|-------:|-----:|-------:|------:|--------------------|
| `arena_compaction` | Arena / compaction / GC | 0 | 0 | 5 | 5 | P0 — well-contained, batch drivers already exist |
| `mutation_dirty` | Mutation / dirty propagation / provenance | 0 | 0 | 1 | 1 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 0 | 0 | 1 | 1 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 0 | 0 | 0 | 0 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 0 | 0 | 0 | 0 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 0 | 0 | 0 | 0 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 0 | 0 | 0 | 0 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 0 | 0 | 1 | 1 | P2 — often thin schema probes; collapse into obs matrix |

## Patterns, harness usage, coupling

### Harness / entry-point patterns (`tests/issues/` only)

| Pattern | Count | Meaning |
|---------|------:|---------|

### `@category` distribution (issues/)


### Top includes (first 50 lines, issues/)


### Top module imports (first 50 lines, issues/)


### Coupling notes

1. **CompilerService-heavy** (~majority of issues/): most legacy tests are integration-style closed loops (eval → mutate → query stats). Domain migration should keep a shared CS fixture, not re-copy setup.
2. **Observability dual-path**: many files named `*_observability.cpp` or probing `query:*-stats` / `engine:metrics`. Prefer folding into `tests/domain/cases/obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp`.
3. **Bundle link profiles** (`light` / `jit` / `fiber` / `*_late*`): physical file location still `tests/issues/`; migration must update `issue_link_profiles.json` / CMake when deleting sources.
4. **Internal headers**: direct includes of `compiler/observability_metrics.h`, `serve/fiber.h`, `compiler/aura_jit*.h` couple tests to private surfaces — domain suites should prefer public query/primitives where possible.
5. **Existing consolidation path**: family `*_batch.cpp` drivers under `tests/` (listed in `AuraDomainTests.cmake`) are the intermediate step; domain suites are the long-term home.

## Multi-file issues, phase slices, low-value signals

- Issue numbers with **multiple** `tests/issues/` files: **0**
- Phase-slice files (`*_phase*`): **0**
- Small files (< 4 KiB, possible thin probes): **0**
- Existing `*_batch` drivers (migration milestones): **6**

### Multi-file issue groups (consolidate first)


### Smallest issue tests (triage for obs-matrix fold or drop)


### Batch drivers already present

- `tests/domain/arena/test_arena_batch.cpp` → theme `arena_compaction`
- `tests/domain/arena/test_compact_batch.cpp` → theme `arena_compaction`
- `tests/domain/arena/test_compact_sweep_batch.cpp` → theme `arena_compaction`
- `tests/domain/test_domain_gates_batch.cpp` → theme `mutation_dirty`
- `tests/domain/test_fiber_integration_batch.cpp` → theme `fiber_orch`
- `tests/domain/arena/test_gc_batch.cpp` → theme `arena_compaction`

### Domain suites (do not regress; extend these)

- `tests/domain/arena/test_arena_batch.cpp`
- `tests/domain/arena/test_arena_defrag_concurrent.cpp`
- `tests/domain/arena/test_compact_batch.cpp`
- `tests/domain/arena/test_compact_sweep_batch.cpp`
- `tests/domain/test_domain_gates_batch.cpp`
- `tests/domain/test_fiber_integration_batch.cpp`
- `tests/domain/arena/test_gc_batch.cpp`
- `tests/domain/test_obs_schema_matrix.cpp`

## Migration priority roadmap

Suggested order starts with well-contained groups (per #1957) and leverages existing batch/domain footholds. Each wave should:

1. Pick a theme slice (or multi-file issue group).
2. Port ACs into a domain suite or family batch driver.
3. Delete or EXCLUDE the old `test_issue_*.cpp` + update bundles/CMake.
4. Re-run this inventory script; commit the refreshed markdown.

| Wave | Theme / slice | Why first | Suggested follow-up issue |
|-----:|---------------|-----------|---------------------------|
| 1 | `arena_compaction` + compact/gc batches | Contained core; `test_compact_batch` / `test_compact_sweep_batch` / `test_gc_batch` exist | Open: *Migrate arena/compaction issue tests → domain* |
| 2 | Multi-file phase groups (#436, #435, #501, #411) | Obvious consolidate wins (same issue, many mains) | Open: *Collapse phase/followup issue test clusters* |
| 3 | `mutation_dirty` thin obs probes | Largest issues/ bucket; domain typed-mutate + mutation_boundary batch | Open: *Mutation/dirty issue tests → domain* |
| 4 | `fiber_orch` remaining gates | Domain fiber orchestration suite already swallowed #810/#812/#813/#875-style checks | Open: *Finish fiber/orch obs migration* |
| 5 | `linear_ownership` + `shape_soa` | Small counts; batch drivers exist | Open: *Linear + SoA batch → domain* |
| 6 | `edsl_hygiene` | Domain hygiene suite + macro_reflect batch | Open: *Hygiene/EDSL issue tests → domain* |
| 7 | `observability` schema-only files | Fold into `obs_schema_cases.hpp` matrix | Open: *Obs schema matrix completion* |
| 8 | `jit_incremental` smoke ACs | Keep heavy JIT stress in bundles; move light AC gates only | Open: *JIT/incremental AC smoke → domain* |
| 9 | `uncategorized` + early_issue (<#200) | Manual triage; some may be obsolete vs suite/regression | Open: *Legacy early-issue triage* |

### Acceptance checkpoints per wave

- No new `test_issue_*.cpp` introduced.
- Domain or batch binary covers former ACs (or intentional drop documented).
- `python3 scripts/inventory_legacy_tests.py --check` stays green after refresh.
- Bundle profiles / CMake targets updated when sources removed.

## Per-theme file lists

Files listed as ``location/name`` with issue id and one-line summary.

### `arena_compaction` — Arena / compaction / GC (5)

**Target:** tests/domain/ (extend compact/gc family; see test_compact_*_batch)

**Priority:** P0 — well-contained, batch drivers already exist

#### domain/ (5)

- `tests/domain/arena/test_arena_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_arena_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_arena_defrag_concurrent.cpp` (—) [domain_suite, theme_arena] — tests/domain/arena/test_arena_defrag_concurrent.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_compact_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_compact_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_compact_sweep_batch.cpp` (—) [batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_compact_sweep_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_gc_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_gc_batch.cpp — relocated for #1959 arena pilot

### `mutation_dirty` — Mutation / dirty propagation / provenance (1)

**Target:** tests/domain/test_domain_typed_mutate.cpp + mutation_boundary batch

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (1)

- `tests/domain/test_domain_gates_batch.cpp` (—) [large, batch_driver, domain_suite] — test_domain_gates_batch.cpp — Domain suite batch: behavioral gates.

### `fiber_orch` — Fiber / orchestration / steal / Guard (1)

**Target:** tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume batch

**Priority:** P1 — domain suite already collapses many obs gates

#### domain/ (1)

- `tests/domain/test_fiber_integration_batch.cpp` (—) [batch_driver, domain_suite] — tests/domain/test_fiber_integration_batch.cpp — Wave 8 of #1957 migration.

### `observability` — Observability / metrics / query:*-stats (1)

**Target:** tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (1)

- `tests/domain/test_obs_schema_matrix.cpp` (—) [domain_suite] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas

## Regenerating

```bash
python3 scripts/inventory_legacy_tests.py
python3 scripts/inventory_legacy_tests.py --check
```

The coarser Phase-2 5-domain classifier remains available as `scripts/classify_test_issues.py` for historical comparison; **this inventory (#1957) is the planning source of truth** for domain migration.
