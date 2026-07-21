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
| `tests/issues/test_issue_*.cpp` | 42 | Legacy per-issue mains / bundle members |
| `tests/test_*.cpp` (issue-oriented) | 0 | Numbered root tests + `*_batch` drivers |
| `tests/domain/test_*.cpp` | 8 | Preferred destination suites |
| **Total scanned** | **50** | |

### Related artifacts

- Coarser 5-bucket Phase-2 map: [`tests/domain_classification.md`](domain_classification.md) (`scripts/classify_test_issues.py`)
- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)
- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)
- Test layout rules: [`tests/README.md`](README.md)

## Theme buckets (8 + uncategorized)

Classification uses the **filename + first 50 lines** (keywords and filename token boosts). Ties break toward earlier themes in the priority order below.

| Theme | Title | Issues | Root | Domain | Total | Migration priority |
|-------|-------|-------:|-----:|-------:|------:|--------------------|
| `arena_compaction` | Arena / compaction / GC | 1 | 0 | 5 | 6 | P0 — well-contained, batch drivers already exist |
| `mutation_dirty` | Mutation / dirty propagation / provenance | 15 | 0 | 1 | 16 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 5 | 0 | 1 | 6 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 0 | 0 | 0 | 0 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 12 | 0 | 0 | 12 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 3 | 0 | 0 | 3 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 2 | 0 | 0 | 2 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 4 | 0 | 1 | 5 | P2 — often thin schema probes; collapse into obs matrix |

## Patterns, harness usage, coupling

### Harness / entry-point patterns (`tests/issues/` only)

| Pattern | Count | Meaning |
|---------|------:|---------|
| `CompilerService` | 39 | Integration path via `CompilerService` / eval |
| `test_harness` | 29 | `#include "test_harness.hpp"` + CHECK/TEST macros |
| `RUN_ALL_TESTS` | 25 | Harness runner main |
| `bundle_run_fn` | 5 | `aura_issue_*_run()` entry for issue bundles |
| `own_main` | 1 | File defines `int main()` (standalone or bundle source) |

### `@category` distribution (issues/)

- `unit`: 15
- `integration`: 15
- `unknown`: 11
- `regression`: 1

### Top includes (first 50 lines, issues/)

- `test_harness.hpp` — 26
- `reflect/reflect.hh` — 3
- `serve/fiber.h` — 1
- `serve/scheduler.h` — 1
- `serve/worker.h` — 1
- `compiler/aura_jit.h` — 1
- `compiler/spec_jit_controller.h` — 1
- `nodeview_wire.hh` — 1
- `issues/test_issue_178_bridge.h` — 1
- `compiler/value_tags.h` — 1
- `../src/core/persistent_child_vector.hh` — 1
- `serve/serve_async.h` — 1

### Top module imports (first 50 lines, issues/)

- `std` — 22
- `aura.compiler.value` — 11
- `aura.core.type` — 10
- `aura.compiler.ir` — 9
- `aura.core.ast` — 8
- `aura.compiler.evaluator` — 8
- `aura.core` — 7
- `aura.core.arena` — 6
- `aura.compiler.service` — 6
- `aura.diag` — 5
- `aura.compiler.type_checker` — 4
- `aura.parser.parser` — 3
- `aura.compiler.pass_manager` — 3
- `aura.compiler.evaluator_pure` — 2
- `aura.compiler.ir_executor` — 2

### Coupling notes

1. **CompilerService-heavy** (~majority of issues/): most legacy tests are integration-style closed loops (eval → mutate → query stats). Domain migration should keep a shared CS fixture, not re-copy setup.
2. **Observability dual-path**: many files named `*_observability.cpp` or probing `query:*-stats` / `engine:metrics`. Prefer folding into `tests/domain/cases/obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp`.
3. **Bundle link profiles** (`light` / `jit` / `fiber` / `*_late*`): physical file location still `tests/issues/`; migration must update `issue_link_profiles.json` / CMake when deleting sources.
4. **Internal headers**: direct includes of `compiler/observability_metrics.h`, `serve/fiber.h`, `compiler/aura_jit*.h` couple tests to private surfaces — domain suites should prefer public query/primitives where possible.
5. **Existing consolidation path**: family `*_batch.cpp` drivers under `tests/` (listed in `AuraDomainTests.cmake`) are the intermediate step; domain suites are the long-term home.

## Multi-file issues, phase slices, low-value signals

- Issue numbers with **multiple** `tests/issues/` files: **1**
- Phase-slice files (`*_phase*`): **0**
- Small files (< 4 KiB, possible thin probes): **1**
- Existing `*_batch` drivers (migration milestones): **6**

### Multi-file issue groups (consolidate first)

- **#178** (2): `test_issue_178.cpp`, `test_issue_178_reflect.cpp`

### Smallest issue tests (triage for obs-matrix fold or drop)

- `test_issue_178.cpp` (3933 B) → `edsl_hygiene` — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `test_issue_131.cpp` (4176 B) → `edsl_hygiene` — test_issue_131.cpp — Verify the FFI primitives
- `test_issue_677.cpp` (4879 B) → `observability` — 
- `test_issue_178_reflect.cpp` (8177 B) → `edsl_hygiene` — Non-module TU: P2996 reflection (Issue #268).
- `test_issue_115.cpp` (8242 B) → `fiber_orch` — test_issue_115.cpp — Standalone tests for the Issue #115 follow-ups:
- `test_issue_159_bench.cpp` (9186 B) → `mutation_dirty` — test_issue_159_bench.cpp — Issue #159 Phase 4: incremental
- `test_issue_215.cpp` (10934 B) → `edsl_hygiene` — test_issue_215.cpp — Issue #215:
- `test_issue_228.cpp` (11341 B) → `mutation_dirty` — test_issue_228.cpp — Issue #228: Hardware IR Dependent Type
- `test_issue_221.cpp` (14203 B) → `mutation_dirty` — test_issue_221.cpp — Issue #221: PersistentChildVector
- `test_issue_149.cpp` (15170 B) → `observability` — test_issue_149.cpp — Verify Issue #149 acceptance criteria
- `test_issue_191.cpp` (16166 B) → `mutation_dirty` — test_issue_191.cpp — Verify Issue #191 acceptance criteria
- `test_issue_167.cpp` (16351 B) → `shape_soa` — test_issue_167.cpp — Issue #167: IR layer SoA/DOD migration
- `test_issue_196.cpp` (17145 B) → `mutation_dirty` — test_issue_196.cpp — Verify Issue #196 acceptance criteria
- `test_issue_188.cpp` (17713 B) → `mutation_dirty` — test_issue_188.cpp — Verify Issue #188 acceptance criteria
- `test_issue_195.cpp` (19078 B) → `fiber_orch` — test_issue_195.cpp — Verify Issue #195 acceptance criteria
- `test_issue_776.cpp` (19317 B) → `observability` — test_issue_776.cpp — Issue #776: Integrated Primitives Hot-Path
- `test_issue_289.cpp` (19721 B) → `mutation_dirty` — test_issue_289.cpp — Issue #289 / #481 acceptance tests.
- `test_issue_170.cpp` (19913 B) → `jit_incremental` — test_issue_170.cpp — Issue #170: Accelerate LLVM JIT Backend
- `test_issue_473.cpp` (20248 B) → `fiber_orch` — test_issue_473.cpp — Verify Issue #473 Tier 1 security fixes
- `test_issue_756.cpp` (20351 B) → `observability` — test_issue_756.cpp — Issue #756: EnvFrame dual-path consistency

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

### `arena_compaction` — Arena / compaction / GC (6)

**Target:** tests/domain/ (extend compact/gc family; see test_compact_*_batch)

**Priority:** P0 — well-contained, batch drivers already exist

#### domain/ (5)

- `tests/domain/arena/test_arena_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_arena_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_arena_defrag_concurrent.cpp` (—) [domain_suite, theme_arena] — tests/domain/arena/test_arena_defrag_concurrent.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_compact_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_compact_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_compact_sweep_batch.cpp` (—) [batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_compact_sweep_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_gc_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_gc_batch.cpp — relocated for #1959 arena pilot

#### issues/ (1)

- `tests/issues/test_issue_797.cpp` (#797) — test_issue_797.cpp — Issue #797: P0 high-perf C++26

### `mutation_dirty` — Mutation / dirty propagation / provenance (16)

**Target:** tests/domain/test_domain_typed_mutate.cpp + mutation_boundary batch

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (1)

- `tests/domain/test_domain_gates_batch.cpp` (—) [large, batch_driver, domain_suite] — test_domain_gates_batch.cpp — Domain suite batch: behavioral gates.

#### issues/ (15)

- `tests/issues/test_issue_141.cpp` (#141) [early_issue] — test_issue_141.cpp — Verify Issue #141 acceptance criteria
- `tests/issues/test_issue_147.cpp` (#147) [early_issue] — test_issue_147.cpp — Verify Issue #147 acceptance criteria
- `tests/issues/test_issue_159_bench.cpp` (#159) [early_issue] — test_issue_159_bench.cpp — Issue #159 Phase 4: incremental
- `tests/issues/test_issue_182.cpp` (#182) [large, early_issue] — test_issue_182.cpp — Issue #182: Hardware IR + Verilog Backend
- `tests/issues/test_issue_188.cpp` (#188) [early_issue] — test_issue_188.cpp — Verify Issue #188 acceptance criteria
- `tests/issues/test_issue_191.cpp` (#191) [early_issue] — test_issue_191.cpp — Verify Issue #191 acceptance criteria
- `tests/issues/test_issue_196.cpp` (#196) [early_issue] — test_issue_196.cpp — Verify Issue #196 acceptance criteria
- `tests/issues/test_issue_221.cpp` (#221) — test_issue_221.cpp — Issue #221: PersistentChildVector
- `tests/issues/test_issue_222.cpp` (#222) [large] — test_issue_222.cpp — Issue #222: structural mutation
- `tests/issues/test_issue_224.cpp` (#224) [large] — test_issue_224.cpp — Verify Issue #224 acceptance criteria
- `tests/issues/test_issue_228.cpp` (#228) — test_issue_228.cpp — Issue #228: Hardware IR Dependent Type
- `tests/issues/test_issue_240.cpp` (#240) — test_issue_240.cpp — Issue #240: per-node occurrence-dirty bit
- `tests/issues/test_issue_289.cpp` (#289) — test_issue_289.cpp — Issue #289 / #481 acceptance tests.
- `tests/issues/test_issue_401.cpp` (#401) — test_issue_401.cpp — Issue #401: invalidate_function claims BFS
- `tests/issues/test_issue_735.cpp` (#735) — test_issue_735.cpp — Issue #735: MacroIntroduced provenance in

### `fiber_orch` — Fiber / orchestration / steal / Guard (6)

**Target:** tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume batch

**Priority:** P1 — domain suite already collapses many obs gates

#### domain/ (1)

- `tests/domain/test_fiber_integration_batch.cpp` (—) [batch_driver, domain_suite] — tests/domain/test_fiber_integration_batch.cpp — Wave 8 of #1957 migration.

#### issues/ (5)

- `tests/issues/test_issue_115.cpp` (#115) [early_issue] — test_issue_115.cpp — Standalone tests for the Issue #115 follow-ups:
- `tests/issues/test_issue_135.cpp` (#135) [large, early_issue] — test_issue_135.cpp — Verify Issue #135 acceptance criteria:
- `tests/issues/test_issue_195.cpp` (#195) [early_issue] — test_issue_195.cpp — Verify Issue #195 acceptance criteria
- `tests/issues/test_issue_473.cpp` (#473) — test_issue_473.cpp — Verify Issue #473 Tier 1 security fixes
- `tests/issues/test_issue_803.cpp` (#803) — test_issue_803.cpp — Issue #803: P0 EDA-SV-

### `edsl_hygiene` — EDSL / macro hygiene / reflect (12)

**Target:** tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect batch

**Priority:** P1 — domain hygiene suite exists

#### issues/ (12)

- `tests/issues/test_issue_131.cpp` (#131) [early_issue] — test_issue_131.cpp — Verify the FFI primitives
- `tests/issues/test_issue_137.cpp` (#137) [early_issue] — test_issue_137.cpp — Verify Issue #137 acceptance criteria
- `tests/issues/test_issue_146.cpp` (#146) [large, early_issue] — test_issue_146.cpp — Verify Issue #146 first extract
- `tests/issues/test_issue_178.cpp` (#178) [small, early_issue] — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `tests/issues/test_issue_178_reflect.cpp` (#178) [early_issue] — Non-module TU: P2996 reflection (Issue #268).
- `tests/issues/test_issue_181.cpp` (#181) [early_issue] — test_issue_181.cpp — Issue #181: EvalValue 64-bit tagged
- `tests/issues/test_issue_197.cpp` (#197) [large, early_issue] — test_issue_197.cpp — Issue #197: branch-aware inliner + parameter
- `tests/issues/test_issue_212.cpp` (#212) [large] — test_issue_212.cpp — Issue #212 Cycle 1:
- `tests/issues/test_issue_215.cpp` (#215) — test_issue_215.cpp — Issue #215:
- `tests/issues/test_issue_217.cpp` (#217) [large] — test_issue_217.cpp — Issue #217 Cycle 1 (pilot):
- `tests/issues/test_issue_733.cpp` (#733) — test_issue_733.cpp — Issue #733: Macro SyntaxMarker propagation + IR/JIT
- `tests/issues/test_issue_757.cpp` (#757) — test_issue_757.cpp — Issue #757: Fine-grained MacroIntroduced

### `jit_incremental` — JIT / AOT / incremental relower (3)

**Target:** domain suite for incremental_*; keep heavy JIT in issue bundles

**Priority:** P2 — link-profile heavy; migrate AC smoke first

#### issues/ (3)

- `tests/issues/test_issue_170.cpp` (#170) [early_issue] — test_issue_170.cpp — Issue #170: Accelerate LLVM JIT Backend
- `tests/issues/test_issue_171.cpp` (#171) [large, early_issue] — test_issue_171.cpp — Issue #171: High-Impact IR Optimization Passes
- `tests/issues/test_issue_732.cpp` (#732) — test_issue_732.cpp — Issue #732: AOT hot-reload safe-swap at

### `shape_soa` — Shape / SoA / column layout (2)

**Target:** tests/test_soa_batch.cpp → domain/

**Priority:** P2 — small-medium; soa_batch precedent

#### issues/ (2)

- `tests/issues/test_issue_145.cpp` (#145) [large, early_issue] — test_issue_145.cpp — Verify Issue #145 partial deliverable
- `tests/issues/test_issue_167.cpp` (#167) [early_issue] — test_issue_167.cpp — Issue #167: IR layer SoA/DOD migration

### `observability` — Observability / metrics / query:*-stats (5)

**Target:** tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (1)

- `tests/domain/test_obs_schema_matrix.cpp` (—) [domain_suite] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas

#### issues/ (4)

- `tests/issues/test_issue_149.cpp` (#149) [early_issue] — test_issue_149.cpp — Verify Issue #149 acceptance criteria
- `tests/issues/test_issue_677.cpp` (#677) — Issue #677 deployment health endpoints + install layout
- `tests/issues/test_issue_756.cpp` (#756) — test_issue_756.cpp — Issue #756: EnvFrame dual-path consistency
- `tests/issues/test_issue_776.cpp` (#776) — test_issue_776.cpp — Issue #776: Integrated Primitives Hot-Path

## Regenerating

```bash
python3 scripts/inventory_legacy_tests.py
python3 scripts/inventory_legacy_tests.py --check
```

The coarser Phase-2 5-domain classifier remains available as `scripts/classify_test_issues.py` for historical comparison; **this inventory (#1957) is the planning source of truth** for domain migration.
