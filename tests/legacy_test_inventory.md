# Legacy test inventory

**Issue:** [#1957](https://github.com/cybrid-systems/aura/issues/1957)
**Generated:** 2026-07-21 by `scripts/inventory_legacy_tests.py`
**Status:** living document — re-run the script after consolidations.

## Purpose

Categorize legacy per-issue regression tests so we can migrate them in batches into the preferred `tests/domain/` structure (and existing family batch drivers under `tests/test_*_batch.cpp`).

Do **not** add new `tests/issues/test_issue_*.cpp` files.

## Scope snapshot

| Location | Count | Notes |
|----------|------:|-------|
| `tests/issues/test_issue_*.cpp` | 534 | Legacy per-issue mains / bundle members |
| `tests/test_*.cpp` (issue-oriented) | 0 | Numbered root tests + `*_batch` drivers |
| `tests/domain/test_*.cpp` | 7 | Preferred destination suites |
| **Total scanned** | **541** | |

### Related artifacts

- Coarser 5-bucket Phase-2 map: [`tests/domain_classification.md`](domain_classification.md) (`scripts/classify_test_issues.py`)
- Link/bundle profiles: [`tests/fixtures/issue_link_profiles.json`](fixtures/issue_link_profiles.json)
- Domain CMake: [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake)
- Test layout rules: [`tests/README.md`](README.md)

## Theme buckets (8 + uncategorized)

Classification uses the **filename + first 50 lines** (keywords and filename token boosts). Ties break toward earlier themes in the priority order below.

| Theme | Title | Issues | Root | Domain | Total | Migration priority |
|-------|-------|-------:|-----:|-------:|------:|--------------------|
| `arena_compaction` | Arena / compaction / GC | 28 | 0 | 5 | 33 | P0 — well-contained, batch drivers already exist |
| `mutation_dirty` | Mutation / dirty propagation / provenance | 159 | 0 | 1 | 160 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 46 | 0 | 0 | 46 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 11 | 0 | 0 | 11 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 56 | 0 | 0 | 56 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 34 | 0 | 0 | 34 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 31 | 0 | 0 | 31 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 169 | 0 | 1 | 170 | P2 — often thin schema probes; collapse into obs matrix |

## Patterns, harness usage, coupling

### Harness / entry-point patterns (`tests/issues/` only)

| Pattern | Count | Meaning |
|---------|------:|---------|
| `CompilerService` | 487 | Integration path via `CompilerService` / eval |
| `test_harness` | 310 | `#include "test_harness.hpp"` + CHECK/TEST macros |
| `bundle_run_fn` | 135 | `aura_issue_*_run()` entry for issue bundles |
| `RUN_ALL_TESTS` | 62 | Harness runner main |
| `own_main` | 43 | File defines `int main()` (standalone or bundle source) |
| `issue_test_harness` | 2 | Older issue-specific harness helper |

### `@category` distribution (issues/)

- `integration`: 348
- `unknown`: 111
- `unit`: 67
- `issue_specific`: 6
- `regression`: 2

### Top includes (first 50 lines, issues/)

- `test_harness.hpp` — 291
- `compiler/observability_metrics.h` — 57
- `compiler/aura_jit_bridge.h` — 19
- `serve/scheduler.h` — 15
- `compiler/aura_jit.h` — 12
- `serve/fiber.h` — 10
- `serve/worker.h` — 6
- `compiler/shape_profiler.h` — 4
- `reflect/reflect.hh` — 4
- `compiler/aot_mangle.h` — 3
- `core/gc_hooks.h` — 3
- `compiler/messaging_bridge.h` — 3
- `compiler/runtime_shared.h` — 3
- `serve/gc_coordinator.h` — 2
- `serve/metrics.h` — 2

### Top module imports (first 50 lines, issues/)

- `aura.compiler.value` — 340
- `aura.compiler.service` — 327
- `std` — 302
- `aura.compiler.evaluator` — 299
- `aura.core.ast` — 190
- `aura.core.arena` — 102
- `aura.core.type` — 100
- `aura.compiler.ir` — 46
- `aura.core` — 34
- `aura.compiler.type_checker` — 32
- `aura.diag` — 30
- `aura.parser.parser` — 20
- `aura.compiler.pass_manager` — 17
- `aura.core.mutation` — 17
- `aura.compiler.ir_executor` — 8

### Coupling notes

1. **CompilerService-heavy** (~majority of issues/): most legacy tests are integration-style closed loops (eval → mutate → query stats). Domain migration should keep a shared CS fixture, not re-copy setup.
2. **Observability dual-path**: many files named `*_observability.cpp` or probing `query:*-stats` / `engine:metrics`. Prefer folding into `tests/domain/cases/obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp`.
3. **Bundle link profiles** (`light` / `jit` / `fiber` / `*_late*`): physical file location still `tests/issues/`; migration must update `issue_link_profiles.json` / CMake when deleting sources.
4. **Internal headers**: direct includes of `compiler/observability_metrics.h`, `serve/fiber.h`, `compiler/aura_jit*.h` couple tests to private surfaces — domain suites should prefer public query/primitives where possible.
5. **Existing consolidation path**: family `*_batch.cpp` drivers under `tests/` (listed in `AuraDomainTests.cmake`) are the intermediate step; domain suites are the long-term home.

## Multi-file issues, phase slices, low-value signals

- Issue numbers with **multiple** `tests/issues/` files: **11**
- Phase-slice files (`*_phase*`): **9**
- Small files (< 4 KiB, possible thin probes): **7**
- Existing `*_batch` drivers (migration milestones): **6**

### Multi-file issue groups (consolidate first)

- **#501** (6): `test_issue_501.cpp`, `test_issue_501_concepts.cpp`, `test_issue_501_hygiene.cpp`, `test_issue_501_phase2.cpp`, `test_issue_501_phase3.cpp`, `test_issue_501_phase4.cpp`
- **#411** (5): `test_issue_411.cpp`, `test_issue_411_followup_1.cpp`, `test_issue_411_followup_2.cpp`, `test_issue_411_followup_3.cpp`, `test_issue_411_followup_4.cpp`
- **#435** (5): `test_issue_435_phase1.cpp`, `test_issue_435_phase3.cpp`, `test_issue_435_phase4.cpp`, `test_issue_435_phase5.cpp`, `test_issue_435_phase6.cpp`
- **#178** (3): `test_issue_178.cpp`, `test_issue_178_cycle3.cpp`, `test_issue_178_reflect.cpp`
- **#159** (2): `test_issue_159.cpp`, `test_issue_159_bench.cpp`
- **#197** (2): `test_issue_197.cpp`, `test_issue_197_observability.cpp`
- **#213** (2): `test_issue_213.cpp`, `test_issue_213_panic_fiber.cpp`
- **#412** (2): `test_issue_412.cpp`, `test_issue_412_followup_1.cpp`
- **#508** (2): `test_issue_508.cpp`, `test_issue_508_observability.cpp`
- **#1408** (2): `test_issue_1408_followup_edsl.cpp`, `test_issue_1408_followup_rebind_rollback.cpp`
- **#1496** (2): `test_issue_1496.cpp`, `test_issue_1496_concurrent_epoch_safety.cpp`

### Smallest issue tests (triage for obs-matrix fold or drop)

- `test_issue_347.cpp` (2322 B) → `mutation_dirty` — test_issue_347.cpp — Issue #347 (StableNodeRef, generation_
- `test_issue_484_minimal.cpp` (2451 B) → `mutation_dirty` — 
- `test_issue_263.cpp` (3192 B) → `observability` — 
- `test_issue_264.cpp` (3686 B) → `fiber_orch` — 
- `test_issue_197_observability.cpp` (3711 B) → `observability` — test_issue_197_observability.cpp — Issue #197 Aura
- `test_issue_178.cpp` (3933 B) → `edsl_hygiene` — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `test_issue_125.cpp` (3980 B) → `mutation_dirty` — test_issue_125.cpp — Verify the per-module dirty-skip
- `test_issue_1400.cpp` (4171 B) → `mutation_dirty` — test_issue_1400.cpp — Issue #1400: bridge_epoch ↔ mutation_epoch sync
- `test_issue_131.cpp` (4176 B) → `edsl_hygiene` — test_issue_131.cpp — Verify the FFI primitives
- `test_issue_478.cpp` (4308 B) → `observability` — Validates:
- `test_issue_1399.cpp` (4337 B) → `mutation_dirty` — test_issue_1399.cpp — Issue #1399: set-car!/set-cdr! pair mutation race
- `test_issue_1401.cpp` (4472 B) → `arena_compaction` — test_issue_1401.cpp — Issue #1401: load_module_file ↔
- `test_issue_1403.cpp` (4521 B) → `fiber_orch` — test_issue_1403.cpp — Issue #1403:
- `test_issue_262.cpp` (4528 B) → `shape_soa` — 
- `test_issue_270.cpp` (4740 B) → `mutation_dirty` — test_issue_270.cpp — Issue #270: end_id snapshot + StableNodeRef
- `test_issue_285.cpp` (4862 B) → `mutation_dirty` — installation is verified indirectly through the build
- `test_issue_274.cpp` (4868 B) → `mutation_dirty` — test_issue_274.cpp — Issue #274: MutationVisitor concept +
- `test_issue_677.cpp` (4879 B) → `observability` — 
- `test_issue_1408_followup_edsl.cpp` (4927 B) → `mutation_dirty` — AC1: happy path — 3 mutations all applied
- `test_issue_287.cpp` (4952 B) → `jit_incremental` — ── AC1: set/get module_version roundtrip ──

### Batch drivers already present

- `tests/domain/arena/test_arena_batch.cpp` → theme `arena_compaction`
- `tests/domain/arena/test_compact_batch.cpp` → theme `arena_compaction`
- `tests/domain/arena/test_compact_sweep_batch.cpp` → theme `arena_compaction`
- `tests/domain/test_domain_gates_batch.cpp` → theme `mutation_dirty`
- `tests/domain/arena/test_gc_batch.cpp` → theme `arena_compaction`
- `tests/issues/test_issue_1449_demotion_batch.cpp` → theme `observability`

### Domain suites (do not regress; extend these)

- `tests/domain/arena/test_arena_batch.cpp`
- `tests/domain/arena/test_arena_defrag_concurrent.cpp`
- `tests/domain/arena/test_compact_batch.cpp`
- `tests/domain/arena/test_compact_sweep_batch.cpp`
- `tests/domain/test_domain_gates_batch.cpp`
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

### `arena_compaction` — Arena / compaction / GC (33)

**Target:** tests/domain/ (extend compact/gc family; see test_compact_*_batch)

**Priority:** P0 — well-contained, batch drivers already exist

#### domain/ (5)

- `tests/domain/arena/test_arena_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_arena_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_arena_defrag_concurrent.cpp` (—) [domain_suite, theme_arena] — tests/domain/arena/test_arena_defrag_concurrent.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_compact_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_compact_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_compact_sweep_batch.cpp` (—) [batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_compact_sweep_batch.cpp — relocated for #1959 arena pilot
- `tests/domain/arena/test_gc_batch.cpp` (—) [large, batch_driver, domain_suite, theme_arena] — tests/domain/arena/test_gc_batch.cpp — relocated for #1959 arena pilot

#### issues/ (28)

- `tests/issues/test_issue_1397.cpp` (#1397) — test_issue_1397.cpp - Issue #1397: ASTArena::request_defrag
- `tests/issues/test_issue_1401.cpp` (#1401) — test_issue_1401.cpp — Issue #1401: load_module_file ↔
- `tests/issues/test_issue_1407_constraint_solver_cache.cpp` (#1407) — test_issue_1407_constraint_solver_cache.cpp — Issue #1407 R1:
- `tests/issues/test_issue_1425.cpp` (#1425) — test_issue_1425.cpp — Issue #1425: DeadCoercionEliminationPass
- `tests/issues/test_issue_1469.cpp` (#1469) — test_issue_1469.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_1488.cpp` (#1488) — AC1: arena:adaptive-stats returns int pair (no dead heap push) — #1072
- `tests/issues/test_issue_1489.cpp` (#1489) — AC1: save_panic_checkpoint arms process-wide GC defer
- `tests/issues/test_issue_1508.cpp` (#1508) — AC1: aura_is_jit_closure_fresh matches table + defuse epochs
- `tests/issues/test_issue_1510.cpp` (#1510) — AC1: compact bumps defuse_version_ + bridge_epoch
- `tests/issues/test_issue_1518.cpp` (#1518) — Issue #1518 — full live-object compact + freelist relocate + Shape/JIT
- `tests/issues/test_issue_1519.cpp` (#1519) — #1466 (const eval expansion). This issue is hot-path Contract density
- `tests/issues/test_issue_1526.cpp` (#1526) — AC1: compact bumps defuse + bridge + AOT table epochs
- `tests/issues/test_issue_1534.cpp` (#1534) — AC1: compile() captures fn epoch via capture_fn_epoch (fresh at capture epoch)
- `tests/issues/test_issue_1543.cpp` (#1543) — AC1: registration monotonicity across audits + resync path
- `tests/issues/test_issue_1655.cpp` (#1655) — test_issue_1655.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_430_arena_compaction.cpp` (#430) — test_issue_430_arena_compaction.cpp — Issue #430: Production
- `tests/issues/test_issue_456.cpp` (#456) — primitives (query:dirty-subtree,
- `tests/issues/test_issue_464_arena_auto_compaction.cpp` (#464) — test_issue_464_arena_auto_compaction.cpp — Issue #464:
- `tests/issues/test_issue_508.cpp` (#508) — test_issue_508.cpp — Issue #508: DeadCoercionEliminationPass
- `tests/issues/test_issue_604.cpp` (#604) — Issue #604 arena auto-compact + defrag + fiber/GC safepoint
- `tests/issues/test_issue_623.cpp` (#623) — arena:auto-compact-threshold (read) + arena:set-auto-compact-threshold (write)
- `tests/issues/test_issue_642.cpp` (#642) — observability surface already covers ~70% of the AC4 surface
- `tests/issues/test_issue_685.cpp` (#685) — Issue #685 arena auto-compact policy + defrag/shape synergy
- `tests/issues/test_issue_722.cpp` (#722) — Compaction/Defrag Auto-Trigger + Dirty/Shape Hook Integration in
- `tests/issues/test_issue_731.cpp` (#731) — test_issue_731.cpp — Issue #731: Arena + SoA + EnvFrame concurrent
- `tests/issues/test_issue_764.cpp` (#764) — test_issue_764.cpp — Issue #764: Compiler Arena AST / shared_ptr<FlatAST>
- `tests/issues/test_issue_767.cpp` (#767) — test_issue_767.cpp — Issue #767: Arena Auto-Compact Policy +
- `tests/issues/test_issue_797.cpp` (#797) — test_issue_797.cpp — Issue #797: P0 high-perf C++26

### `mutation_dirty` — Mutation / dirty propagation / provenance (160)

**Target:** tests/domain/test_domain_typed_mutate.cpp + mutation_boundary batch

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (1)

- `tests/domain/test_domain_gates_batch.cpp` (—) [large, batch_driver, domain_suite] — test_domain_gates_batch.cpp — Domain suite batch: behavioral gates.

#### issues/ (159)

- `tests/issues/test_issue_125.cpp` (#125) [small, early_issue] — test_issue_125.cpp — Verify the per-module dirty-skip
- `tests/issues/test_issue_126.cpp` (#126) [early_issue] — test_issue_126.cpp — Verify the pure functions extracted
- `tests/issues/test_issue_138.cpp` (#138) [early_issue] — test_issue_138.cpp — Verify Issue #138 acceptance criteria
- `tests/issues/test_issue_139.cpp` (#139) [early_issue] — test_issue_139.cpp — Verify Issue #139 acceptance criteria
- `tests/issues/test_issue_1396.cpp` (#1396) — test_issue_1396.cpp — Issue #1396: AOT hot-reload counter helpers —
- `tests/issues/test_issue_1399.cpp` (#1399) — test_issue_1399.cpp — Issue #1399: set-car!/set-cdr! pair mutation race
- `tests/issues/test_issue_1400.cpp` (#1400) — test_issue_1400.cpp — Issue #1400: bridge_epoch ↔ mutation_epoch sync
- `tests/issues/test_issue_1405.cpp` (#1405) — test_issue_1405.cpp — Issue #1405: workspace_flat_ generation counter
- `tests/issues/test_issue_1406.cpp` (#1406) — test_issue_1406.cpp — Issue #1406: propagate_cow_pins_after_clone
- `tests/issues/test_issue_1408_followup_edsl.cpp` (#1408) [followup] — AC1: happy path — 3 mutations all applied
- `tests/issues/test_issue_1408_followup_rebind_rollback.cpp` (#1408) [followup] — AC1: bind x=1, rebind x=100, rollback, eval x → 1
- `tests/issues/test_issue_141.cpp` (#141) [early_issue] — test_issue_141.cpp — Verify Issue #141 acceptance criteria
- `tests/issues/test_issue_1414.cpp` (#1414) — test_issue_1414.cpp — Verify Issue #1414 acceptance criteria
- `tests/issues/test_issue_1419.cpp` (#1419) — test_issue_1419.cpp — Issue #1419: Compound provenance wire-up
- `tests/issues/test_issue_142.cpp` (#142) [early_issue] — test_issue_142.cpp — Verify Issue #142 acceptance criteria
- `tests/issues/test_issue_1455_occurrence_stale_propagation.cpp` (#1455) — test_issue_1455_occurrence_stale_propagation.cpp
- `tests/issues/test_issue_1456_affected_subtree_locality.cpp` (#1456) — test_issue_1456_affected_subtree_locality.cpp
- `tests/issues/test_issue_1457_type_propagation_castop_zerooverhead.cpp` (#1457) — test_issue_1457_type_propagation_castop_zerooverhead.cpp
- `tests/issues/test_issue_147.cpp` (#147) [early_issue] — test_issue_147.cpp — Verify Issue #147 acceptance criteria
- `tests/issues/test_issue_1472.cpp` (#1472) — test_issue_1472.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_148.cpp` (#148) [early_issue] — test_issue_148.cpp — Verify Issue #148 acceptance criteria
- `tests/issues/test_issue_1486.cpp` (#1486) — AC1: apply / materialize / enforce entry intercepts Moved
- `tests/issues/test_issue_1502.cpp` (#1502) — AC1: failed batch restores children() count after partial structural ops
- `tests/issues/test_issue_1503.cpp` (#1503) — AC1: append-only ensure takes delta (no full rebuild)
- `tests/issues/test_issue_1523.cpp` (#1523) — This issue is runtime verifier + metrics + concurrent invalidate.
- `tests/issues/test_issue_1524.cpp` (#1524) — AC1: successful typed_mutate bumps bridge epoch + AOT table epoch
- `tests/issues/test_issue_1529.cpp` (#1529) — #745 (occurrence-priority reverify + basic complete total),
- `tests/issues/test_issue_1531.cpp` (#1531) — AC1: use-after-move still detected (ownership baseline)
- `tests/issues/test_issue_1538.cpp` (#1538) — AC1: post_mutation_invariant_check call sites identified (visitor + typed_mutate)
- `tests/issues/test_issue_1556.cpp` (#1556) — AC1: try_acquire pass/reject typed ResourceQuotaExceeded
- `tests/issues/test_issue_159.cpp` (#159) [early_issue] — test_issue_159.cpp — Issue #159 Phase 1: incremental typecheck primitive.
- `tests/issues/test_issue_159_bench.cpp` (#159) [early_issue] — test_issue_159_bench.cpp — Issue #159 Phase 4: incremental
- `tests/issues/test_issue_1645.cpp` (#1645) — test_issue_1645.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_1649.cpp` (#1649) — test_issue_1649.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_177.cpp` (#177) [early_issue] — test_issue_177.cpp — Issue #213 verification:
- `tests/issues/test_issue_178_cycle3.cpp` (#178) [early_issue] — Validates:
- `tests/issues/test_issue_182.cpp` (#182) [large, early_issue] — test_issue_182.cpp — Issue #182: Hardware IR + Verilog Backend
- `tests/issues/test_issue_184.cpp` (#184) [early_issue] — test_issue_184.cpp — Issue #184: MutationBoundaryGuard RAII +
- `tests/issues/test_issue_188.cpp` (#188) [early_issue] — test_issue_188.cpp — Verify Issue #188 acceptance criteria
- `tests/issues/test_issue_1900.cpp` (#1900) — test_issue_1900.cpp — Issue #1900: Strengthen
- `tests/issues/test_issue_1904.cpp` (#1904) — test_issue_1904.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_191.cpp` (#191) [early_issue] — test_issue_191.cpp — Verify Issue #191 acceptance criteria
- `tests/issues/test_issue_192.cpp` (#192) [early_issue] — test_issue_192.cpp — Verify Issue #192 acceptance criteria
- `tests/issues/test_issue_196.cpp` (#196) [early_issue] — test_issue_196.cpp — Verify Issue #196 acceptance criteria
- `tests/issues/test_issue_211.cpp` (#211) — test_issue_211.cpp — Issue #211 dedicated tree pattern
- `tests/issues/test_issue_213.cpp` (#213) — test_issue_213.cpp — Issue #213 Cycle 1:
- `tests/issues/test_issue_216.cpp` (#216) — test_issue_216.cpp — Issue #216 Cycle 3:
- `tests/issues/test_issue_221.cpp` (#221) — test_issue_221.cpp — Issue #221: PersistentChildVector
- `tests/issues/test_issue_222.cpp` (#222) [large] — test_issue_222.cpp — Issue #222: structural mutation
- `tests/issues/test_issue_224.cpp` (#224) [large] — test_issue_224.cpp — Verify Issue #224 acceptance criteria
- `tests/issues/test_issue_225_bridge_invalidation.cpp` (#225) — test_issue_225_bridge_invalidation.cpp — Verify Issue #225
- `tests/issues/test_issue_227.cpp` (#227) — test_issue_227.cpp — Issue #227: Occurrence Typing narrowing +
- `tests/issues/test_issue_228.cpp` (#228) — test_issue_228.cpp — Issue #228: Hardware IR Dependent Type
- `tests/issues/test_issue_240.cpp` (#240) — test_issue_240.cpp — Issue #240: per-node occurrence-dirty bit
- `tests/issues/test_issue_249.cpp` (#249) — test_issue_249.cpp — Issue #249: StableNodeRef ergonomics
- `tests/issues/test_issue_250.cpp` (#250) — test_issue_250.cpp — Issue #250: mutate:atomic-batch truly atomic
- `tests/issues/test_issue_266.cpp` (#266) — test_issue_266.cpp — Issue #266: finer-grained SoA rollback for
- `tests/issues/test_issue_269.cpp` (#269) — test_issue_269.cpp — Issue #269: FlatAST wire format v2
- `tests/issues/test_issue_270.cpp` (#270) — test_issue_270.cpp — Issue #270: end_id snapshot + StableNodeRef
- `tests/issues/test_issue_274.cpp` (#274) — test_issue_274.cpp — Issue #274: MutationVisitor concept +
- `tests/issues/test_issue_275.cpp` (#275) — test_issue_275.cpp — Issue #275: pure mutation / rollback module.
- `tests/issues/test_issue_276.cpp` (#276) — WorkspaceTree cross-layer StableNodeRef resolution
- `tests/issues/test_issue_277.cpp` (#277) — PPA dirty bitmask + hardware backend mutate hook
- `tests/issues/test_issue_278.cpp` (#278) — (Issue #278: lib/std query/mutate wrappers +
- `tests/issues/test_issue_279.cpp` (#279) — (Issue #279: pair? returns Pair type, list? predicate,
- `tests/issues/test_issue_280.cpp` (#280) — (Issue #280).
- `tests/issues/test_issue_281.cpp` (#281) — memoization for analyze_predicate_flat (Issue #281:
- `tests/issues/test_issue_282.cpp` (#282) — (Issue #282: NarrowingRecord + query:provenance-of
- `tests/issues/test_issue_284.cpp` (#284) — accessors, and Verilog emitter (Issue #284 Phase 2
- `tests/issues/test_issue_285.cpp` (#285) — installation is verified indirectly through the build
- `tests/issues/test_issue_289.cpp` (#289) — test_issue_289.cpp — Issue #289 / #481 acceptance tests.
- `tests/issues/test_issue_291.cpp` (#291) — workspace_id + serialization
- `tests/issues/test_issue_301.cpp` (#301) — test_issue_301.cpp — Issue #301: C++26 std::meta migration
- `tests/issues/test_issue_302.cpp` (#302) — test_issue_302.cpp — Issue #302: Expand Contracts + runtime
- `tests/issues/test_issue_303.cpp` (#303) — test_issue_303.cpp — Issue #303: SafeStableNodeRef
- `tests/issues/test_issue_309.cpp` (#309) — test_issue_309.cpp — Verify Issue #309 acceptance criteria
- `tests/issues/test_issue_313.cpp` (#313) — test_issue_313.cpp — Verify Issue #313 acceptance criteria
- `tests/issues/test_issue_314.cpp` (#314) — test_issue_314.cpp — Verify Issue #314 acceptance criteria
- `tests/issues/test_issue_315.cpp` (#315) — test_issue_315.cpp — Verify Issue #315 acceptance criteria
- `tests/issues/test_issue_317.cpp` (#317) — test_issue_317.cpp — Verify Issue #317 acceptance criteria
- `tests/issues/test_issue_318.cpp` (#318) — workspace mutation + verification-dirty helpers
- `tests/issues/test_issue_319.cpp` (#319) — verification primitives) + multi-round
- `tests/issues/test_issue_327.cpp` (#327) — test_issue_327.cpp — Issue #327: Incremental Compilation
- `tests/issues/test_issue_328.cpp` (#328) — test_issue_328.cpp — Issue #328: Self-Evolution Mutation Loop
- `tests/issues/test_issue_329.cpp` (#329) — test_issue_329.cpp — Issue #329: Explicit StableNodeRef /
- `tests/issues/test_issue_333.cpp` (#333) — test_issue_333.cpp — Issue #333: FlatAST serialize/deserialize
- `tests/issues/test_issue_336.cpp` (#336) — test_issue_336.cpp — Verify Issue #336 acceptance
- `tests/issues/test_issue_342.cpp` (#342) — test_issue_342.cpp — Issue #342: Structured
- `tests/issues/test_issue_344.cpp` (#344) — test_issue_344.cpp — Verify Issue #344 acceptance
- `tests/issues/test_issue_345.cpp` (#345) — test_issue_345.cpp — Issue #345: Comprehensive stress testing
- `tests/issues/test_issue_346.cpp` (#346) — test_issue_346.cpp — Verify Issue #346 acceptance
- `tests/issues/test_issue_347.cpp` (#347) [small] — test_issue_347.cpp — Issue #347 (StableNodeRef, generation_
- `tests/issues/test_issue_348.cpp` (#348) — test_issue_348.cpp — Verify Issue #348 acceptance
- `tests/issues/test_issue_349.cpp` (#349) — test_issue_349.cpp — Verify Issue #349 acceptance
- `tests/issues/test_issue_350.cpp` (#350) — test_issue_350.cpp — Verify Issue #350 acceptance
- `tests/issues/test_issue_351.cpp` (#351) — test_issue_351.cpp — Verify Issue #351 acceptance
- `tests/issues/test_issue_357.cpp` (#357) — test_issue_357.cpp — Issue #357: End-to-end panic-checkpoint
- `tests/issues/test_issue_359.cpp` (#359) — test_issue_359.cpp — Verify Issue #359 acceptance criteria
- `tests/issues/test_issue_361.cpp` (#361) — test_issue_361.cpp — Verify Issue #361 acceptance criteria
- `tests/issues/test_issue_367.cpp` (#367) — test_issue_367.cpp — Verify Issue #367 acceptance criteria
- `tests/issues/test_issue_368.cpp` (#368) — test_issue_368.cpp — Verify Issue #368 acceptance criteria
- `tests/issues/test_issue_369.cpp` (#369) — test_issue_369.cpp — Verify Issue #369 acceptance criteria
- `tests/issues/test_issue_370.cpp` (#370) — test_issue_370.cpp — Verify Issue #370 acceptance criteria
- `tests/issues/test_issue_371.cpp` (#371) — test_issue_371.cpp — Issue #371: tag/arity index atomic
- `tests/issues/test_issue_372.cpp` (#372) — test_issue_372.cpp — Issue #372: cross-pool name-based Define
- `tests/issues/test_issue_377.cpp` (#377) — test_issue_377.cpp — Issue #377: Differential testing
- `tests/issues/test_issue_391.cpp` (#391) — integration in core mutate primitives using
- `tests/issues/test_issue_392.cpp` (#392) — test_issue_392.cpp — Issue #392: scoped / per-subtree
- `tests/issues/test_issue_394.cpp` (#394) — test_issue_394.cpp — Issue #394 follow-up to #250:
- `tests/issues/test_issue_396.cpp` (#396) — test_issue_396.cpp — Issue #396: Strengthen
- `tests/issues/test_issue_397.cpp` (#397) — test_issue_397.cpp — Issue #397: Centralize
- `tests/issues/test_issue_399.cpp` (#399) — test_issue_399.cpp — Issue #399: Avoid resize in
- `tests/issues/test_issue_401.cpp` (#401) — test_issue_401.cpp — Issue #401: invalidate_function claims BFS
- `tests/issues/test_issue_410.cpp` (#410) — test_issue_410.cpp — Issue #410 scope-limited close:
- `tests/issues/test_issue_411_followup_4.cpp` (#411) [followup] — test_issue_411_followup_4.cpp — Issue #411 fu1
- `tests/issues/test_issue_413.cpp` (#413) — test_issue_413.cpp — Issue #413: MutationLog-integrated
- `tests/issues/test_issue_429_soa.cpp` (#429) — test_issue_429_soa.cpp — Issue #429: IRFunctionSoA + FlatAST
- `tests/issues/test_issue_434.cpp` (#434) — test_issue_434.cpp — Issue #434: Per-node Occurrence
- `tests/issues/test_issue_435_phase1.cpp` (#435) [phase_slice] — — InterfaceIR + ModportIR). Demonstrates the
- `tests/issues/test_issue_435_phase3.cpp` (#435) [phase_slice] — — SequenceIR + PropertyIR). SVA sequences and
- `tests/issues/test_issue_435_phase4.cpp` (#435) [phase_slice] — — CoverpointIR + CovergroupIR + list-based IR
- `tests/issues/test_issue_435_phase5.cpp` (#435) [phase_slice] — — ConstraintIR + list IR baseline for the same).
- `tests/issues/test_issue_435_phase6.cpp` (#435) [phase_slice] — — ClassIR + list IR baseline for the same).
- `tests/issues/test_issue_445_openclaw_integration.cpp` (#445) — test_issue_445_openclaw_integration.cpp — Issue #445:
- `tests/issues/test_issue_453.cpp` (#453) — Validates:
- `tests/issues/test_issue_459.cpp` (#459) — ── AC1: atomic-batch metrics start at 0 ──
- `tests/issues/test_issue_470_stable_ref_sv_scale.cpp` (#470) — test_issue_470_stable_ref_sv_scale.cpp — Issue #470:
- `tests/issues/test_issue_474.cpp` (#474) — test_issue_474.cpp — Issue #474: Aura unified error type
- `tests/issues/test_issue_482.cpp` (#482) — replace-pattern share the same matcher (issue #482)
- `tests/issues/test_issue_484_minimal.cpp` (#484) [small, minimal] — minimal repro for #484
- `tests/issues/test_issue_487.cpp` (#487) — test_issue_487.cpp — Issue #487: Wire dirty
- `tests/issues/test_issue_488.cpp` (#488) — Issue #488 — post-mutate reflect validation + Guard impact snapshot
- `tests/issues/test_issue_501.cpp` (#501) — test_issue_501.cpp — Issue #501: Aura core Concepts
- `tests/issues/test_issue_501_concepts.cpp` (#501) — test_issue_501_concepts.cpp — Issue #501 Concepts
- `tests/issues/test_issue_501_phase2.cpp` (#501) [phase_slice] — test_issue_501_phase2.cpp — Issue #501 Phase 2:
- `tests/issues/test_issue_501_phase4.cpp` (#501) [phase_slice] — test_issue_501_phase4.cpp — Issue #501 Phase 4:
- `tests/issues/test_issue_502.cpp` (#502) — Issue #502 — Post-mutate reflect validation + Guard impact snapshot
- `tests/issues/test_issue_504.cpp` (#504) — Issue #504 — MutationBoundaryGuard impact log + query primitive
- `tests/issues/test_issue_505.cpp` (#505) — Issue #505 — closure/EnvFrame/bridge_epoch post-invalidate safety stats
- `tests/issues/test_issue_620.cpp` (#620) — query:stable-ref-provenance primitive
- `tests/issues/test_issue_637.cpp` (#637) — observability surface already covers ~70% of the AC4 surface via
- `tests/issues/test_issue_641.cpp` (#641) — AC3 surface via existing primitives + counters:
- `tests/issues/test_issue_670.cpp` (#670) — closed-loop safety).
- `tests/issues/test_issue_672.cpp` (#672) — invariants enforcement under concurrent fiber mutation (P0
- `tests/issues/test_issue_676.cpp` (#676) — test_issue_676.cpp — Issue #676: Security model, sandboxing & audit.
- `tests/issues/test_issue_712.cpp` (#712) — + auto-schema check for MacroIntroduced subtrees in Guard
- `tests/issues/test_issue_715.cpp` (#715) — full validation and provenance for multi-layer agent orchestration.
- `tests/issues/test_issue_717.cpp` (#717) — primitive, (4) targeted tests in test_issue_* for "failed mutate +
- `tests/issues/test_issue_719.cpp` (#719) — safety closed-loop).
- `tests/issues/test_issue_726.cpp` (#726) — self-evolution primitives + reliable multi-round AI Agent closed-loop
- `tests/issues/test_issue_728.cpp` (#728) — test_issue_728.cpp — Issue #728: unified structured error + provenance +
- `tests/issues/test_issue_735.cpp` (#735) — test_issue_735.cpp — Issue #735: MacroIntroduced provenance in
- `tests/issues/test_issue_761.cpp` (#761) — test_issue_761.cpp — Issue #761: End-to-end atomic batch mutate +
- `tests/issues/test_issue_770.cpp` (#770) — test_issue_770.cpp — Issue #770: Enhance solve_delta + reverify with
- `tests/issues/test_issue_771.cpp` (#771) — test_issue_771.cpp — Issue #771: Strengthen OwnershipEnv + escape
- `tests/issues/test_issue_789.cpp` (#789) — test_issue_789.cpp — Issue #789: P0 mandate
- `tests/issues/test_issue_790.cpp` (#790) — test_issue_790.cpp — Issue #790: P0 first-class
- `tests/issues/test_issue_792.cpp` (#792) — test_issue_792.cpp — Issue #792: P0
- `tests/issues/test_issue_804.cpp` (#804) — test_issue_804.cpp — Issue #804: P0 stdlib error

### `fiber_orch` — Fiber / orchestration / steal / Guard (46)

**Target:** tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume batch

**Priority:** P1 — domain suite already collapses many obs gates

#### issues/ (46)

- `tests/issues/test_issue_115.cpp` (#115) [early_issue] — test_issue_115.cpp — Standalone tests for the Issue #115 follow-ups:
- `tests/issues/test_issue_119.cpp` (#119) [early_issue] — test_issue_119.cpp — Verify the proper-blocking fiber:join fix
- `tests/issues/test_issue_135.cpp` (#135) [large, early_issue] — test_issue_135.cpp — Verify Issue #135 acceptance criteria:
- `tests/issues/test_issue_1402.cpp` (#1402) — test_issue_1402.cpp — Issue #1402: Primitive security-tier
- `tests/issues/test_issue_1403.cpp` (#1403) — test_issue_1403.cpp — Issue #1403:
- `tests/issues/test_issue_1404.cpp` (#1404) — test_issue_1404.cpp — Issue #1404: restamp_yield_checkpoint_top
- `tests/issues/test_issue_1490.cpp` (#1490) — AC1: refresh_stale_frames_after_steal callable + bumps post_steal_refresh_count
- `tests/issues/test_issue_1492.cpp` (#1492) — AC1: is_at_inner_mutation_boundary defers steal (depth>0)
- `tests/issues/test_issue_1500.cpp` (#1500) — is_valid cow_epoch enforcement + Guard/steal batch restamp of pinned refs.
- `tests/issues/test_issue_1504.cpp` (#1504) — AC1: query:mutation-boundary-depth returns int (>= 0)
- `tests/issues/test_issue_1525.cpp` (#1525) — AC1: metrics multifiber_mutate_races / multifiber_safe_fallback surface
- `tests/issues/test_issue_1544.cpp` (#1544) — AC1: 10K+ iter loop: mutation + GC safepoint + fiber steal per iter
- `tests/issues/test_issue_164.cpp` (#164) [early_issue] — test_issue_164.cpp — Issue #164: fiber:join spin-fallback elimination.
- `tests/issues/test_issue_189.cpp` (#189) [early_issue] — test_issue_189.cpp — Verify Issue #189 acceptance criteria
- `tests/issues/test_issue_195.cpp` (#195) [early_issue] — test_issue_195.cpp — Verify Issue #195 acceptance criteria
- `tests/issues/test_issue_213_panic_fiber.cpp` (#213) — test_issue_213_panic_fiber.cpp — Issue #213 follow-up cycle:
- `tests/issues/test_issue_264.cpp` (#264) [small] — fiber scheduler + MutationBoundaryGuard yield handshake
- `tests/issues/test_issue_292.cpp` (#292) — Issue #292 — guard predicates in query:pattern
- `tests/issues/test_issue_321.cpp` (#321) — test_issue_321.cpp — Issue #321: P0 Multi-Fiber Mutation
- `tests/issues/test_issue_353.cpp` (#353) — test_issue_353.cpp — Issue #353: Follow-up to #241 (scope-limited close).
- `tests/issues/test_issue_354.cpp` (#354) — test_issue_354.cpp — Verify Issue #354 acceptance
- `tests/issues/test_issue_362.cpp` (#362) — test_issue_362.cpp — Verify Issue #362 acceptance criteria
- `tests/issues/test_issue_363.cpp` (#363) — test_issue_363.cpp — Verify Issue #363 acceptance criteria
- `tests/issues/test_issue_384.cpp` (#384) — test_issue_384.cpp — Issue #384: Bidirectional inference engine
- `tests/issues/test_issue_438.cpp` (#438) — declared in fiber.h (build verifies)
- `tests/issues/test_issue_439.cpp` (#439) — MutationBoundary coordination in
- `tests/issues/test_issue_451.cpp` (#451) — Orchestration Metrics & Yield Classification
- `tests/issues/test_issue_473.cpp` (#473) — test_issue_473.cpp — Verify Issue #473 Tier 1 security fixes
- `tests/issues/test_issue_485.cpp` (#485) — SoA EnvFrame + AOT + scheduler/GC production-readiness close-out
- `tests/issues/test_issue_521_observability.cpp` (#521) [obs_named] — Issue #521 — multi-fiber-orchestration-stats hash slice
- `tests/issues/test_issue_618.cpp` (#618) — back-compat with test_issue_451)
- `tests/issues/test_issue_645.cpp` (#645) — What the issue body AC3 specifies by **exact name + fields** —
- `tests/issues/test_issue_646.cpp` (#646) — What the issue body AC3 specifies by **exact name + fields** —
- `tests/issues/test_issue_648.cpp` (#648) — What the issue body AC4 specifies by **exact name + fields** —
- `tests/issues/test_issue_649.cpp` (#649) — Re-Stamp + Size Validation on Panic Transfer + Cross-Steal —
- `tests/issues/test_issue_650.cpp` (#650) — 5-field adaptive bias summary (already covers the AC3
- `tests/issues/test_issue_651.cpp` (#651) — block_gc_for_pending_checkpoint_trampoline + Request Shim
- `tests/issues/test_issue_707.cpp` (#707) — Issue #707 bounded per-fiber stack pool + panic/steal re-stamp
- `tests/issues/test_issue_762.cpp` (#762) — test_issue_762.cpp — Issue #762: Workspace '锁定-导航-修改-执行' closed-
- `tests/issues/test_issue_773.cpp` (#773) — test_issue_773.cpp — Issue #773: Workspace closed-loop fiber/multi-
- `tests/issues/test_issue_783.cpp` (#783) — test_issue_783.cpp — Issue #783: P0 strict outermost MutationBoundary
- `tests/issues/test_issue_784.cpp` (#784) — test_issue_784.cpp — Issue #784: P0 mandatory
- `tests/issues/test_issue_785.cpp` (#785) — test_issue_785.cpp — Issue #785: P0 complete region
- `tests/issues/test_issue_787.cpp` (#787) — test_issue_787.cpp — Issue #787: P0 end-to-end
- `tests/issues/test_issue_791.cpp` (#791) — test_issue_791.cpp — Issue #791: P0 exhaustive
- `tests/issues/test_issue_803.cpp` (#803) — test_issue_803.cpp — Issue #803: P0 EDA-SV-

### `linear_ownership` — Linear ownership / borrow / consume (11)

**Target:** tests/test_linear_ownership_batch.cpp → domain/

**Priority:** P1 — small, already partially batched

#### issues/ (11)

- `tests/issues/test_issue_117.cpp` (#117) [early_issue] — test_issue_117.cpp — Verify linear ownership validation fixes
- `tests/issues/test_issue_1410.cpp` (#1410) — tests/test_issue_1410.cpp — Issue #1410: Linear ∩ Refinement
- `tests/issues/test_issue_1417.cpp` (#1417) — test_issue_1417.cpp — Issue #1417: Linear ∩ Refinement
- `tests/issues/test_issue_1458_linear_ownership_post_mutate.cpp` (#1458) — test_issue_1458_linear_ownership_post_mutate.cpp
- `tests/issues/test_issue_1535.cpp` (#1535) — AC1: Linear epoch safety check fresh after compile (no mutate)
- `tests/issues/test_issue_253.cpp` (#253) — test_issue_253.cpp — Issue #253 scope-limited close:
- `tests/issues/test_issue_283.cpp` (#283) — (Issue #283: bidirectional check-mode + OwnershipEnv
- `tests/issues/test_issue_683.cpp` (#683) — Issue #683 linear ownership + GC safepoint / fiber-steal integration
- `tests/issues/test_issue_688.cpp` (#688) — Issue #688 linear OwnershipEnv post-mutate typed-mutation wiring
- `tests/issues/test_issue_763.cpp` (#763) — test_issue_763.cpp — Issue #763: Runtime linear_ownership_state
- `tests/issues/test_issue_765.cpp` (#765) — test_issue_765.cpp — Issue #765: Full DepEntry quote/lambda tracking +

### `edsl_hygiene` — EDSL / macro hygiene / reflect (56)

**Target:** tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect batch

**Priority:** P1 — domain hygiene suite exists

#### issues/ (56)

- `tests/issues/test_issue_120.cpp` (#120) [early_issue] — test_issue_120.cpp — Verify the hygienic macro fix (Issue #120).
- `tests/issues/test_issue_131.cpp` (#131) [early_issue] — test_issue_131.cpp — Verify the FFI primitives
- `tests/issues/test_issue_137.cpp` (#137) [early_issue] — test_issue_137.cpp — Verify Issue #137 acceptance criteria
- `tests/issues/test_issue_140.cpp` (#140) [early_issue] — test_issue_140.cpp — Verify Issue #140 acceptance criteria
- `tests/issues/test_issue_146.cpp` (#146) [large, early_issue] — test_issue_146.cpp — Verify Issue #146 first extract
- `tests/issues/test_issue_1471.cpp` (#1471) — test_issue_1471.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_1501.cpp` (#1501) — AC1: default query:pattern skips MacroIntroduced (allow >= default)
- `tests/issues/test_issue_158.cpp` (#158) [early_issue] — test_issue_158.cpp — Issue #158 verification:
- `tests/issues/test_issue_161.cpp` (#161) [early_issue] — test_issue_161.cpp — Issue #161 Phase 2: parser is now a pure function.
- `tests/issues/test_issue_162.cpp` (#162) [early_issue] — test_issue_162.cpp — Issue #162 Phase 1: Type Concepts for
- `tests/issues/test_issue_163.cpp` (#163) [early_issue] — test_issue_163.cpp — Issue #163: Expand Pass concept usage and
- `tests/issues/test_issue_1644_ir_hygiene.cpp` (#1644) — test_issue_1644_ir_hygiene.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_165.cpp` (#165) [early_issue] — test_issue_165.cpp — Issue #165: macro re-expansion + SyntaxMarker
- `tests/issues/test_issue_1650.cpp` (#1650) — tests/test_issue_1650.cpp — Issue #1650 (partial-redundant-ship)
- `tests/issues/test_issue_1652.cpp` (#1652) — test_issue_1652.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_1653.cpp` (#1653) — test_issue_1653.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_174.cpp` (#174) [early_issue] — test_issue_174.cpp — Issue #174 Cycle 1 Env::bindings_
- `tests/issues/test_issue_178.cpp` (#178) [small, early_issue] — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `tests/issues/test_issue_178_reflect.cpp` (#178) [early_issue] — Non-module TU: P2996 reflection (Issue #268).
- `tests/issues/test_issue_181.cpp` (#181) [early_issue] — test_issue_181.cpp — Issue #181: EvalValue 64-bit tagged
- `tests/issues/test_issue_190.cpp` (#190) [early_issue] — test_issue_190.cpp — Verify Issue #190 acceptance criteria
- `tests/issues/test_issue_1907.cpp` (#1907) — test_issue_1907.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_197.cpp` (#197) [large, early_issue] — test_issue_197.cpp — Issue #197: branch-aware inliner + parameter
- `tests/issues/test_issue_208.cpp` (#208) — test_issue_208.cpp — Issue #208 Cycle 2 env migration
- `tests/issues/test_issue_210.cpp` (#210) — test_issue_210.cpp — Issue #210 Cycle 4 env cleanup:
- `tests/issues/test_issue_212.cpp` (#212) [large] — test_issue_212.cpp — Issue #212 Cycle 1:
- `tests/issues/test_issue_214.cpp` (#214) — test_issue_214.cpp — Issue #214 Cycle 1:
- `tests/issues/test_issue_215.cpp` (#215) — test_issue_215.cpp — Issue #215:
- `tests/issues/test_issue_217.cpp` (#217) [large] — test_issue_217.cpp — Issue #217 Cycle 1 (pilot):
- `tests/issues/test_issue_218.cpp` (#218) — test_issue_218.cpp — Issue #218 Cycle 5: reflection tests +
- `tests/issues/test_issue_244.cpp` (#244) — test_issue_244.cpp — Issue #244: SyntaxMarker query primitives
- `tests/issues/test_issue_246.cpp` (#246) — test_issue_246.cpp — Issue #246: IR inliner MacroIntroduced-awareness
- `tests/issues/test_issue_248.cpp` (#248) — test_issue_248.cpp — Issue #248: SyntaxMarker + type schema
- `tests/issues/test_issue_267.cpp` (#267) — uses CompilerService; tests #267 macro-introduced query opt-in
- `tests/issues/test_issue_290.cpp` (#290) — Validates the macro_dirty_ column + 4 Aura primitives. Key design point:
- `tests/issues/test_issue_310.cpp` (#310) — test_issue_310.cpp — Verify Issue #310 acceptance criteria
- `tests/issues/test_issue_316.cpp` (#316) — test_issue_316.cpp — Verify Issue #316 acceptance criteria
- `tests/issues/test_issue_326.cpp` (#326) — test_issue_326.cpp — Issue #326: Hygienic Macros + EDSL Integration
- `tests/issues/test_issue_364.cpp` (#364) — test_issue_364.cpp — Issue #364: Nested hygienic macros +
- `tests/issues/test_issue_365.cpp` (#365) — test_issue_365.cpp — Verify Issue #365 acceptance criteria
- `tests/issues/test_issue_366.cpp` (#366) — test_issue_366.cpp — Verify Issue #366 acceptance criteria
- `tests/issues/test_issue_373.cpp` (#373) — test_issue_373.cpp — Verify Issue #373 acceptance criteria
- `tests/issues/test_issue_440_edsl_readiness.cpp` (#440) — test_issue_440_edsl_readiness.cpp — Issue #440:
- `tests/issues/test_issue_455.cpp` (#455) — Validates:
- `tests/issues/test_issue_486.cpp` (#486) — Issue #486 — query:pattern MacroIntroduced filter + macro-hygiene-stats
- `tests/issues/test_issue_501_hygiene.cpp` (#501) — Issue #501 — IR MacroIntroduced hygiene (InlinePass + lowering)
- `tests/issues/test_issue_503.cpp` (#503) — Issue #503 — query:pattern hygiene flags + pattern-marker-stats
- `tests/issues/test_issue_714.cpp` (#714) — 1. Standalone (query:self-evolution-closedloop-stats, schema 714)
- `tests/issues/test_issue_733.cpp` (#733) — test_issue_733.cpp — Issue #733: Macro SyntaxMarker propagation + IR/JIT
- `tests/issues/test_issue_757.cpp` (#757) — test_issue_757.cpp — Issue #757: Fine-grained MacroIntroduced
- `tests/issues/test_issue_758.cpp` (#758) — test_issue_758.cpp — Issue #758: Runtime auto_validate bridge for user-defined
- `tests/issues/test_issue_759.cpp` (#759) — test_issue_759.cpp — Issue #759: Unified 'code-as-data' closed-loop
- `tests/issues/test_issue_760.cpp` (#760) — test_issue_760.cpp — Issue #760: query:pattern performance + hygiene
- `tests/issues/test_issue_786.cpp` (#786) — test_issue_786.cpp — Issue #786: P0 unified
- `tests/issues/test_issue_788.cpp` (#788) — test_issue_788.cpp — Issue #788: P0 first-class
- `tests/issues/test_issue_edsl_hygiene_atomic.cpp` (—) — test_issue_edsl_hygiene_atomic.cpp — Issue #425: EDSL hygiene

### `jit_incremental` — JIT / AOT / incremental relower (34)

**Target:** domain suite for incremental_*; keep heavy JIT in issue bundles

**Priority:** P2 — link-profile heavy; migrate AC smoke first

#### issues/ (34)

- `tests/issues/test_issue_136.cpp` (#136) [early_issue] — test_issue_136.cpp — Verify Issue #136 acceptance criteria
- `tests/issues/test_issue_1418.cpp` (#1418) — test_issue_1418.cpp — Issue #1418: DeadCoercionEliminationPass
- `tests/issues/test_issue_143.cpp` (#143) [early_issue] — test_issue_143.cpp — Verify Issue #143 partial deliverable
- `tests/issues/test_issue_1477.cpp` (#1477) — Issue #1477 — JIT-side dual-epoch fence (capture_fn_epoch +
- `tests/issues/test_issue_1485.cpp` (#1485) — test_issue_1485.cpp — Verify Issue #1485 acceptance criteria:
- `tests/issues/test_issue_1512.cpp` (#1512) — AC1: opcode_covered_mask + coverage count/pct (success path mirror)
- `tests/issues/test_issue_1516.cpp` (#1516) — AC1: exception_opcode_mask + coverage_count (0..4)
- `tests/issues/test_issue_1522.cpp` (#1522) — AC1: AuraJIT::batch_deopt_for marks pending + metrics
- `tests/issues/test_issue_1536.cpp` (#1536) — AC1: walk at same epoch → 0 stale, no deopt_pending
- `tests/issues/test_issue_1537.cpp` (#1537) — AC1: compile emits prologue helpers in LLVM IR
- `tests/issues/test_issue_1540.cpp` (#1540) — AC1: aura_jit_linear_epoch_safety_check consults linear_post_mutate_enforce
- `tests/issues/test_issue_170.cpp` (#170) [early_issue] — test_issue_170.cpp — Issue #170: Accelerate LLVM JIT Backend
- `tests/issues/test_issue_171.cpp` (#171) [large, early_issue] — test_issue_171.cpp — Issue #171: High-Impact IR Optimization Passes
- `tests/issues/test_issue_1905.cpp` (#1905) — test_issue_1905.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_193.cpp` (#193) [early_issue] — test_issue_193.cpp — Verify Issue #193 acceptance criteria
- `tests/issues/test_issue_194.cpp` (#194) [early_issue] — test_issue_194.cpp — Verify Issue #194 acceptance criteria
- `tests/issues/test_issue_237.cpp` (#237) — test_issue_237.cpp — Issue #237: AOT compilation path end-to-end.
- `tests/issues/test_issue_243.cpp` (#243) — test_issue_243.cpp — Issue #243: AOT bridge enhancement verification
- `tests/issues/test_issue_271.cpp` (#271) — test_issue_271.cpp — Issue #271: incremental tag_arity_index_
- `tests/issues/test_issue_287.cpp` (#287) — ── AC1: set/get module_version roundtrip ──
- `tests/issues/test_issue_293.cpp` (#293) — Validates:
- `tests/issues/test_issue_297.cpp` (#297) — Validates that cs.eval() (default IR + tree-walker fallback) and
- `tests/issues/test_issue_323.cpp` (#323) — test_issue_323.cpp — Issue #323: AOT/Bridge Hot-Update +
- `tests/issues/test_issue_358.cpp` (#358) — test_issue_358.cpp — Verify Issue #358 acceptance criteria
- `tests/issues/test_issue_374.cpp` (#374) — test_issue_374.cpp — Issue #374: AURA_RUNTIME_DIR CI-side lookup
- `tests/issues/test_issue_452_aot_hot_update.cpp` (#452) — test_issue_452_aot_hot_update.cpp — Issue #452:
- `tests/issues/test_issue_461.cpp` (#461) — Validates:
- `tests/issues/test_issue_590.cpp` (#590) — + multi-agent hot-update isolation + closure dispatch stale
- `tests/issues/test_issue_713.cpp` (#713) — hygiene violation detection in JIT deopt / Interpreter fallback
- `tests/issues/test_issue_720.cpp` (#720) — metadata (linear_ownership_state / shape_id / narrow_evidence /
- `tests/issues/test_issue_732.cpp` (#732) — test_issue_732.cpp — Issue #732: AOT hot-reload safe-swap at
- `tests/issues/test_issue_780.cpp` (#780) — test_issue_780.cpp — Issue #780: JIT / hot-update coverage
- `tests/issues/test_issue_793.cpp` (#793) — test_issue_793.cpp — Issue #793: P0 JIT/AOT
- `tests/issues/test_issue_794.cpp` (#794) — test_issue_794.cpp — Issue #794: P0 unified

### `shape_soa` — Shape / SoA / column layout (31)

**Target:** tests/test_soa_batch.cpp → domain/

**Priority:** P2 — small-medium; soa_batch precedent

#### issues/ (31)

- `tests/issues/test_issue_144.cpp` (#144) [early_issue] — test_issue_144.cpp — Verify Issue #144 acceptance criteria
- `tests/issues/test_issue_145.cpp` (#145) [large, early_issue] — test_issue_145.cpp — Verify Issue #145 partial deliverable
- `tests/issues/test_issue_1520.cpp` (#1520) — AC1: SafePCVSpan size/empty/data/begin/end (SoAColumnar shape)
- `tests/issues/test_issue_1521.cpp` (#1521) — AC1: on_arena_compact bumps version, preserves is_stable + history
- `tests/issues/test_issue_167.cpp` (#167) [early_issue] — test_issue_167.cpp — Issue #167: IR layer SoA/DOD migration
- `tests/issues/test_issue_220.cpp` (#220) — test_issue_220.cpp — Issue #220: per-node children linked list
- `tests/issues/test_issue_254.cpp` (#254) — test_issue_254.cpp — Issue #254 scope-limited close:
- `tests/issues/test_issue_262.cpp` (#262) — uses CompilerService + FlatAST dirty/defuse propagation APIs
- `tests/issues/test_issue_273.cpp` (#273) — test_issue_273.cpp — Issue #273: Contracts on FlatAST hot paths.
- `tests/issues/test_issue_286.cpp` (#286) — ── AC1: Env::env_version() default 0, set/get works ──
- `tests/issues/test_issue_311.cpp` (#311) — test_issue_311.cpp — Verify Issue #311 acceptance criteria
- `tests/issues/test_issue_320.cpp` (#320) — test_issue_320.cpp — Per-node epoch tracking for
- `tests/issues/test_issue_337.cpp` (#337) — test_issue_337.cpp — Verify Issue #337 acceptance
- `tests/issues/test_issue_339.cpp` (#339) — test_issue_339.cpp — Verify Issue #339 acceptance
- `tests/issues/test_issue_355.cpp` (#355) — test_issue_355.cpp — Verify Issue #355 acceptance criteria
- `tests/issues/test_issue_393.cpp` (#393) — test_issue_393.cpp — Issue #393: C++ API for explicit
- `tests/issues/test_issue_398.cpp` (#398) — test_issue_398.cpp — Issue #398: Optimize
- `tests/issues/test_issue_431_cxx26.cpp` (#431) — test_issue_431_cxx26.cpp — Issue #431: deepen C++26 Contracts
- `tests/issues/test_issue_437.cpp` (#437) — - VerifyDirtyReason enum values stable (Assertion=0x01,
- `tests/issues/test_issue_463_soa_phase2_wiring.cpp` (#463) [phase_slice] — test_issue_463_soa_phase2_wiring.cpp — Issue #463:
- `tests/issues/test_issue_501_phase3.cpp` (#501) [phase_slice] — test_issue_501_phase3.cpp — Issue #501 Phase 3:
- `tests/issues/test_issue_507.cpp` (#507) — Issue #507 — Task4 hot-path Contracts + consteval invariants
- `tests/issues/test_issue_669.cpp` (#669) — - AC1:  query:primitives-meta [name] returns hash with the
- `tests/issues/test_issue_686.cpp` (#686) — Issue #686 ShapeProfiler ring + Value dispatch + Pass dirty wiring
- `tests/issues/test_issue_721.cpp` (#721) — gap_buffer Wiring for operands / shape / metadata + Dirty Cascade to
- `tests/issues/test_issue_723.cpp` (#723) — Contracts Expansion in Tagged Dispatch/Shape Stability + Value v2 Stats /
- `tests/issues/test_issue_766.cpp` (#766) — test_issue_766.cpp — Issue #766: IR-SoA migration observability +
- `tests/issues/test_issue_768.cpp` (#768) — test_issue_768.cpp — Issue #768: Shape + Pass + Contracts hot-path
- `tests/issues/test_issue_782.cpp` (#782) — test_issue_782.cpp — Issue #782: Dedicated terminal
- `tests/issues/test_issue_795.cpp` (#795) — test_issue_795.cpp — Issue #795: P0 deep hot-path
- `tests/issues/test_issue_796.cpp` (#796) — test_issue_796.cpp — Issue #796: P0 end-to-end

### `observability` — Observability / metrics / query:*-stats (170)

**Target:** tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (1)

- `tests/domain/test_obs_schema_matrix.cpp` (—) [domain_suite] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas

#### issues/ (169)

- `tests/issues/test_issue_1449_demotion_batch.cpp` (#1449) [batch_driver] — Verifies SlimSurface progress after expanding facade-only intercept
- `tests/issues/test_issue_1450.cpp` (#1450) — test_issue_1450.cpp — Epic #1449 Phase 1 / Issue #1450:
- `tests/issues/test_issue_1451.cpp` (#1451) — test_issue_1451.cpp — Issue #1451: Primitives Governance Policy +
- `tests/issues/test_issue_1460.cpp` (#1460) — test_issue_1460.cpp — Issue #1460:
- `tests/issues/test_issue_1461.cpp` (#1461) — test_issue_1461.cpp — Issue #1461:
- `tests/issues/test_issue_1462.cpp` (#1462) — test_issue_1462.cpp — Issue #1462: Agent Migration Guide +
- `tests/issues/test_issue_1487.cpp` (#1487) — AC1: allocate_raw / allocate_checked reject over memory quota
- `tests/issues/test_issue_149.cpp` (#149) [early_issue] — test_issue_149.cpp — Verify Issue #149 acceptance criteria
- `tests/issues/test_issue_1491.cpp` (#1491) — apply_closure paths + JIT aura_closure_call (closed-loop on #1475/#1477).
- `tests/issues/test_issue_1493.cpp` (#1493) — + hold-time adaptive safepoint frequency (closed-loop on #1483).
- `tests/issues/test_issue_1494.cpp` (#1494) — MutationBoundary / invalidate paths (parent closed-loop on #1478 / #1458).
- `tests/issues/test_issue_1495.cpp` (#1495) — AC1: mark_define_dirty body-only for simple defines (irs.size==2)
- `tests/issues/test_issue_1496.cpp` (#1496) — AC1: both paths call unified_invalidation_protocol (metric grows)
- `tests/issues/test_issue_1496_concurrent_epoch_safety.cpp` (#1496) — Complements test_issue_1496.cpp AC6 with explicit steal path and
- `tests/issues/test_issue_1497.cpp` (#1497) — AC1: unified auto_restamp_pinned_stable_refs_at on all sites
- `tests/issues/test_issue_1498.cpp` (#1498) — AC1: allocate_raw / allocate_checked + Guard try_acquire typed reject
- `tests/issues/test_issue_1499.cpp` (#1499) — AC1: query:ai-closedloop-readiness-stats is hash, schema 1499
- `tests/issues/test_issue_1505.cpp` (#1505) — AC1: free-var scan of nested lambdas — free-ref nested marked
- `tests/issues/test_issue_1506.cpp` (#1506) — AC1: eval after mark/set-body exercises re-lower counters
- `tests/issues/test_issue_1509.cpp` (#1509) — #1508 (JIT dual check). This issue is the integration stress AC5.
- `tests/issues/test_issue_1511.cpp` (#1511) — #1507/#1508 (IR/JIT dual check). This issue is the bridge-path AC1.
- `tests/issues/test_issue_1513.cpp` (#1513) — safety closed loop (bridge_epoch + env_version + GC root expire).
- `tests/issues/test_issue_1514.cpp` (#1514) — AC1: AuraJIT::partial_recompile metrics + eviction
- `tests/issues/test_issue_1515.cpp` (#1515) — AC1: validate_linear_ownership_state state machine
- `tests/issues/test_issue_1517.cpp` (#1517) — AC1: SoAView / SoAViewFull concepts + IRFunctionSoAView
- `tests/issues/test_issue_1528.cpp` (#1528) — AC1: multi-define rebind affected << total (locality regression)
- `tests/issues/test_issue_1530.cpp` (#1530) — AC1: should_propagate covers extended ops (Eq..CellGet)
- `tests/issues/test_issue_1532.cpp` (#1532) — AC1: exhaustive match → empty missing, checked=true
- `tests/issues/test_issue_1539.cpp` (#1539) — AC1: bindings_linear_ownership_state_ parallel to bindings_symid_
- `tests/issues/test_issue_1542.cpp` (#1542) — AC1: materialize_call_env on Owned frame bumps linear_post_mutate_enforcements
- `tests/issues/test_issue_1545.cpp` (#1545) — AC1: walk_active_closures visits registered closures
- `tests/issues/test_issue_1555.cpp` (#1555) — AC1: re-eval same define → clean hit (relower_skipped_entirely grows;
- `tests/issues/test_issue_1557.cpp` (#1557) — AC1: walk_active_closures visits registered closures
- `tests/issues/test_issue_1558.cpp` (#1558) — AC1: apply_closure after epoch bump → safe fallback metrics
- `tests/issues/test_issue_1574.cpp` (#1574) — AC1: DefineDirtyMaskView any / is_block_dirty / is_instruction_dirty
- `tests/issues/test_issue_1625_nested_lambda_targeted.cpp` (#1625) — AC1: free-ref nested marks only entry_block (or instr-hit blocks)
- `tests/issues/test_issue_1637.cpp` (#1637) — tests/test_issue_1637.cpp — Issue #1637
- `tests/issues/test_issue_1646.cpp` (#1646) — test_issue_1646.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_1908.cpp` (#1908) — test_issue_1908.cpp — orphan restored (AC drift; not in CI batch)
- `tests/issues/test_issue_197_observability.cpp` (#197) [small, obs_named, early_issue] — test_issue_197_observability.cpp — Issue #197 Aura
- `tests/issues/test_issue_247.cpp` (#247) — test_issue_247.cpp — Issue #247: SyntaxMarker observability integration
- `tests/issues/test_issue_252.cpp` (#252) — test_issue_252.cpp — Issue #252 scope-limited close:
- `tests/issues/test_issue_255.cpp` (#255) — test_issue_255.cpp — Issue #255 scope-limited close:
- `tests/issues/test_issue_256.cpp` (#256) — test_issue_256.cpp — Issue #256 scope-limited close:
- `tests/issues/test_issue_258.cpp` (#258) — test_issue_258.cpp — Issue #258 scope-limited close:
- `tests/issues/test_issue_259.cpp` (#259) — test_issue_259.cpp — Issue #259 scope-limited close:
- `tests/issues/test_issue_263.cpp` (#263) [small] — uses CompilerService snapshot/restore + post-restore validation
- `tests/issues/test_issue_296.cpp` (#296) — Validates the Bridge Lifetime Contract documented in
- `tests/issues/test_issue_298.cpp` (#298) — Validates (engine:metrics \"query:incremental-effectiveness\") returns a 4-tuple:
- `tests/issues/test_issue_308.cpp` (#308) — test_issue_308.cpp — Verify Issue #308 acceptance criteria
- `tests/issues/test_issue_312.cpp` (#312) — test_issue_312.cpp — Verify Issue #312 acceptance criteria
- `tests/issues/test_issue_325.cpp` (#325) — test_issue_325.cpp — Verify Issue #325 acceptance
- `tests/issues/test_issue_331.cpp` (#331) — test_issue_331.cpp — Issue #331: targeted dirty bitmask +
- `tests/issues/test_issue_338.cpp` (#338) — test_issue_338.cpp — Issue #338: Enhance and/or
- `tests/issues/test_issue_340.cpp` (#340) — test_issue_340.cpp — Verify Issue #340 acceptance
- `tests/issues/test_issue_341.cpp` (#341) — test_issue_341.cpp — Issue #341: Integrate Occurrence
- `tests/issues/test_issue_343.cpp` (#343) — test_issue_343.cpp — Issue #343: StableNodeRef
- `tests/issues/test_issue_376.cpp` (#376) — test_issue_376.cpp — Issue #376: low-overhead unified
- `tests/issues/test_issue_383.cpp` (#383) — test_issue_383.cpp — Issue #383: strengthen
- `tests/issues/test_issue_385.cpp` (#385) — test_issue_385.cpp — Issue #385: mutation-aware
- `tests/issues/test_issue_386.cpp` (#386) — test_issue_386.cpp — Issue #386: Deep Occurrence Typing
- `tests/issues/test_issue_387.cpp` (#387) — test_issue_387.cpp — Issue #387: Type Dependency Graph
- `tests/issues/test_issue_388.cpp` (#388) — test_issue_388.cpp — Issue #388: Caller-side marker check +
- `tests/issues/test_issue_389.cpp` (#389) — test_issue_389.cpp — Issue #389: `(compile:snapshot)` Aura
- `tests/issues/test_issue_390.cpp` (#390) — test_issue_390.cpp — Issue #390: Auto-populate schema in
- `tests/issues/test_issue_409.cpp` (#409) — test_issue_409.cpp — Issue #409: Fine-grained constraint
- `tests/issues/test_issue_411.cpp` (#411) — test_issue_411.cpp — Issue #411 scope-limited close:
- `tests/issues/test_issue_411_followup_1.cpp` (#411) [followup] — test_issue_411_followup_1.cpp — Issue #411 follow-up #1
- `tests/issues/test_issue_411_followup_2.cpp` (#411) [followup] — test_issue_411_followup_2.cpp — Issue #411 fu1
- `tests/issues/test_issue_411_followup_3.cpp` (#411) [followup] — test_issue_411_followup_3.cpp — Issue #411 fu1
- `tests/issues/test_issue_412.cpp` (#412) — test_issue_412.cpp — Issue #412 scope-limited close:
- `tests/issues/test_issue_412_followup_1.cpp` (#412) [followup] — test_issue_412_followup_1.cpp — Issue #412 follow-up
- `tests/issues/test_issue_426.cpp` (#426) — minimal re-lower for AI mutate:rebind/set-body
- `tests/issues/test_issue_428_closure.cpp` (#428) — test_issue_428_closure.cpp — Issue #428: Strengthen Closure
- `tests/issues/test_issue_433.cpp` (#433) — test_issue_433.cpp — Issue #433: DeadCoercionEliminationPass
- `tests/issues/test_issue_443.cpp` (#443) — - query:verify-tool-stats returns an integer
- `tests/issues/test_issue_444_strategy_evolution.cpp` (#444) — test_issue_444_strategy_evolution.cpp — Issue #444:
- `tests/issues/test_issue_447.cpp` (#447) — query:pattern on large ASTs (P0). Validates:
- `tests/issues/test_issue_448.cpp` (#448) — + GC safepoint + work-steal coordination for
- `tests/issues/test_issue_457.cpp` (#457) — counters + StableNodeRef invalidation metrics
- `tests/issues/test_issue_458.cpp` (#458) — + observability stats.
- `tests/issues/test_issue_460.cpp` (#460) — dirty mask + dep_graph impact analysis.
- `tests/issues/test_issue_462_shape_aware_folding.cpp` (#462) — test_issue_462_shape_aware_folding.cpp — Issue #462:
- `tests/issues/test_issue_465_cxx26_hotpath.cpp` (#465) — test_issue_465_cxx26_hotpath.cpp — Issue #465:
- `tests/issues/test_issue_469.cpp` (#469) — - verify:parse-coverage-feedback marks
- `tests/issues/test_issue_471_dirty_sv_scale.cpp` (#471) — test_issue_471_dirty_sv_scale.cpp — Issue #471:
- `tests/issues/test_issue_478.cpp` (#478) — Validates:
- `tests/issues/test_issue_479.cpp` (#479) — test_issue_479.cpp — Verify Issue #479 per-prim fast-path hit tracking.
- `tests/issues/test_issue_489.cpp` (#489) — Issue #489 — StableNodeRef + get_safe enforcement in mutate/query hot paths
- `tests/issues/test_issue_490.cpp` (#490) — Issue #490 — proactive tag_arity_index rebuild on COW/compact + policy tuning
- `tests/issues/test_issue_491.cpp` (#491) — Issue #491 — JIT opcode coverage + hot-swap safety + stats hash
- `tests/issues/test_issue_492.cpp` (#492) — Issue #492 — ShapeProfiler deopt stability + JIT/fiber integration
- `tests/issues/test_issue_493.cpp` (#493) — Issue #493 — EDSL hot-path bottleneck measurement + stats hash
- `tests/issues/test_issue_494.cpp` (#494) — Issue #494 — Pass pipeline JIT/incremental concepts + yield + stats hash
- `tests/issues/test_issue_497.cpp` (#497) — Issue #497 — StableRef lifecycle soft compact + refresh + stats
- `tests/issues/test_issue_500.cpp` (#500) — Issue #500 — Work-stealing + MutationBoundary outermost depth safety
- `tests/issues/test_issue_508_observability.cpp` (#508) [obs_named] — Issue #508 — dead-coercion-zerooverhead-stats hash slice
- `tests/issues/test_issue_511_observability.cpp` (#511) [obs_named] — Issue #511 — workspace-snapshot-stats hash slice
- `tests/issues/test_issue_512_observability.cpp` (#512) [obs_named] — Issue #512 — runtime-orchestration-stats hash slice
- `tests/issues/test_issue_513_observability.cpp` (#513) [obs_named] — Issue #513 — aot-hot-reload-stats hash slice
- `tests/issues/test_issue_515_observability.cpp` (#515) [obs_named] — Issue #515 — consolidated-p0-production-stats hash slice
- `tests/issues/test_issue_516_observability.cpp` (#516) [obs_named] — Issue #516 — prompt6-memory-safety-stats hash slice
- `tests/issues/test_issue_522_observability.cpp` (#522) [obs_named] — Issue #522 — aot-production-reload-stats hash slice
- `tests/issues/test_issue_523_observability.cpp` (#523) [obs_named] — Issue #523 — envframe-production-safety-stats hash slice
- `tests/issues/test_issue_524_observability.cpp` (#524) [obs_named] — Issue #524 — macro-production-hygiene-stats hash slice
- `tests/issues/test_issue_525_observability.cpp` (#525) [obs_named] — Issue #525 — guard-production-impact-stats hash slice
- `tests/issues/test_issue_528_observability.cpp` (#528) [obs_named] — Issue #528 — pattern-production-index-stats hash slice
- `tests/issues/test_issue_530_observability.cpp` (#530) [obs_named] — Issue #530 — incremental-production-relower-stats hash slice
- `tests/issues/test_issue_532_observability.cpp` (#532) [obs_named] — Issue #532 — jit-consistency-stats hash slice
- `tests/issues/test_issue_533_observability.cpp` (#533) [obs_named] — Issue #533 — soa-production-columnar-stats hash slice
- `tests/issues/test_issue_534_observability.cpp` (#534) [obs_named] — Issue #534 — arena-production-compaction-stats hash slice
- `tests/issues/test_issue_535_observability.cpp` (#535) [obs_named] — Issue #535 — contracts-production-hotpath-stats hash slice
- `tests/issues/test_issue_557_observability.cpp` (#557) [obs_named] — Issue #557 — top5-commercial-coverage-stats hash slice
- `tests/issues/test_issue_568_observability.cpp` (#568) [obs_named] — Issue #568 — soa-children-columnar-migration-stats hash slice
- `tests/issues/test_issue_569_observability.cpp` (#569) [obs_named] — DEPRECATED location for new work (#1959): prefer tests/domain/arena/
- `tests/issues/test_issue_572_observability.cpp` (#572) [obs_named] — Issue #572 — pass-pipeline-dirtyaware-stats hash slice
- `tests/issues/test_issue_583_observability.cpp` (#583) [obs_named] — Issue #583 — primitives-registry-core-stats hash slice
- `tests/issues/test_issue_584_observability.cpp` (#584) [obs_named] — Issue #584 — primitives-hotpath-stats AI-agent stress slice
- `tests/issues/test_issue_585_observability.cpp` (#585) [obs_named] — Issue #585 — primitives-error-stats unified recovery slice
- `tests/issues/test_issue_589.cpp` (#589) — primitive (the AC4 surface listed in #589 body)
- `tests/issues/test_issue_591_observability.cpp` (#591) [obs_named] — Issue #591 — scheduler-mutation-coord-stats steal/GC slice
- `tests/issues/test_issue_601.cpp` (#601) — bridge_epoch refresh + forced-deopt protocol. Scope-limited observability
- `tests/issues/test_issue_603.cpp` (#603) — consumer adoption + per-block dirty_ driven minimal re-lower observability
- `tests/issues/test_issue_606.cpp` (#606) — concept-constrained visitor refactor + hot-path Contracts adoption
- `tests/issues/test_issue_614.cpp` (#614) — Scope-limited close matching the #601 / #491 / #479 / #604 / #606 pattern:
- `tests/issues/test_issue_615.cpp` (#615) — Scope-limited close matching the #601 / #491 / #479 / #604 / #606 / #614
- `tests/issues/test_issue_621.cpp` (#621) — query:pattern-index-stats-hash primitive
- `tests/issues/test_issue_622.cpp` (#622) — query:atomic-batch-stats-hash structured companion
- `tests/issues/test_issue_624.cpp` (#624) — shape-stability + JIT observability surface that #624 AC4 lists,
- `tests/issues/test_issue_625.cpp` (#625) — already exposes the full pass-pipeline + contracts + pure-
- `tests/issues/test_issue_626.cpp` (#626) — observability — query:contracts-hotpath-stats-hash structured
- `tests/issues/test_issue_631.cpp` (#631) — What the issue body AC3 specifies by **exact name + fields** —
- `tests/issues/test_issue_632.cpp` (#632) — What the issue body AC4 specifies by **exact name + fields** —
- `tests/issues/test_issue_633.cpp` (#633) — already covers ~80% of the AC5 surface via existing primitives:
- `tests/issues/test_issue_640.cpp` (#640) — Closed-Loop — query:sv-verification-closedloop-stats
- `tests/issues/test_issue_643.cpp` (#643) — introspection surface already covers ~70% of the AC2 surface
- `tests/issues/test_issue_644.cpp` (#644) — Per-Region Isolation + Metrics for Multi-Agent Orchestration
- `tests/issues/test_issue_647.cpp` (#647) — parent_, bindings_symid_ vs bindings_) Cross-Fiber Stale
- `tests/issues/test_issue_660_cache_define_bundle.cpp` (#660) — AC1: 2-define, second depends on first via direct call → returns 6
- `tests/issues/test_issue_667_primitives_apply_stats.cpp` (#667) — - AC1:  query:primitives-apply-stats reachable (schema 667)
- `tests/issues/test_issue_668.cpp` (#668) — - AC1:  query:primitives-regex-error-stats reachable (schema 668)
- `tests/issues/test_issue_671.cpp` (#671) — - capture-contract-version (kPrimCaptureContractVersion)
- `tests/issues/test_issue_675.cpp` (#675) — test_issue_675.cpp — Issue #675: Build/CI reproducibility observability.
- `tests/issues/test_issue_677.cpp` (#677) — Issue #677 deployment health endpoints + install layout
- `tests/issues/test_issue_678.cpp` (#678) — Issue #678 PCV span lifetime safety in concurrent query paths
- `tests/issues/test_issue_679.cpp` (#679) — Issue #679 nested Guard + atomic-batch rollback alignment
- `tests/issues/test_issue_680.cpp` (#680) — Issue #680 precise Define mutate IR/JIT/bridge invalidation
- `tests/issues/test_issue_681.cpp` (#681) — Issue #681 IRClosure/EnvFrame epoch enforcement post-mutate
- `tests/issues/test_issue_682.cpp` (#682) — Issue #682 compiler IRClosure/EnvId GC root coordination
- `tests/issues/test_issue_684.cpp` (#684) — Issue #684 IRSoA full wiring + incremental mutate
- `tests/issues/test_issue_687.cpp` (#687) — IR-interpreter identity fast-path + zero-overhead
- `tests/issues/test_issue_689.cpp` (#689) — Issue #689 occurrence typing deep predicate provenance
- `tests/issues/test_issue_690.cpp` (#690) — Issue #690 constraint typed-mutation reverify + blame
- `tests/issues/test_issue_691.cpp` (#691) — Issue #691 CoercionMap + NarrowingRecord provenance linkage
- `tests/issues/test_issue_692.cpp` (#692) — Issue #692 ADT exhaustiveness + pattern provenance typed-mutation
- `tests/issues/test_issue_706.cpp` (#706) — Issue #706 adaptive StealBudget + work-stealing bias for LLM bottleneck
- `tests/issues/test_issue_708.cpp` (#708) — Issue #708 AOT hot-reload refcount swap + region/panic multi-fiber safety
- `tests/issues/test_issue_709.cpp` (#709) — Issue #709 primitives registry fast dispatch + capture discipline + EDA integration
- `tests/issues/test_issue_711.cpp` (#711) — This PR adds the closed-loop integration test that wires those
- `tests/issues/test_issue_716.cpp` (#716) — 4. Test verifies: primitive shape, fresh-zero state, schema sentinel,
- `tests/issues/test_issue_718.cpp` (#718) — summarize_block_dirty + block_dirty_ bitmask into CompilerService::invalidate_function
- `tests/issues/test_issue_756.cpp` (#756) — test_issue_756.cpp — Issue #756: EnvFrame dual-path consistency
- `tests/issues/test_issue_769.cpp` (#769) — test_issue_769.cpp — Issue #769: Implement DeadCoercionEliminationPass
- `tests/issues/test_issue_772.cpp` (#772) — test_issue_772.cpp — Issue #772: Consolidated SV Verification EDSL +
- `tests/issues/test_issue_774.cpp` (#774) — test_issue_774.cpp — Issue #774: Verification feedback-driven
- `tests/issues/test_issue_775.cpp` (#775) — test_issue_775.cpp — Issue #775: Formal Primitives Extension
- `tests/issues/test_issue_776.cpp` (#776) — test_issue_776.cpp — Issue #776: Integrated Primitives Hot-Path
- `tests/issues/test_issue_781.cpp` (#781) — test_issue_781.cpp — Issue #781: High-performance byte
- `tests/issues/test_issue_806.cpp` (#806) — test_issue_806.cpp — Issue #806: P0 stdlib AI-native

## Regenerating

```bash
python3 scripts/inventory_legacy_tests.py
python3 scripts/inventory_legacy_tests.py --check
```

The coarser Phase-2 5-domain classifier remains available as `scripts/classify_test_issues.py` for historical comparison; **this inventory (#1957) is the planning source of truth** for domain migration.
