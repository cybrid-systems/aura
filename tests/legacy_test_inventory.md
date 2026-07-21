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
| `tests/issues/test_issue_*.cpp` | 226 | Legacy per-issue mains / bundle members |
| `tests/test_*.cpp` (issue-oriented) | 0 | Numbered root tests + `*_batch` drivers |
| `tests/domain/test_*.cpp` | 8 | Preferred destination suites |
| **Total scanned** | **234** | |

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
| `mutation_dirty` | Mutation / dirty propagation / provenance | 53 | 0 | 1 | 54 | P0 — high volume; strong domain suite foothold |
| `fiber_orch` | Fiber / orchestration / steal / Guard | 24 | 0 | 1 | 25 | P1 — domain suite already collapses many obs gates |
| `linear_ownership` | Linear ownership / borrow / consume | 4 | 0 | 0 | 4 | P1 — small, already partially batched |
| `edsl_hygiene` | EDSL / macro hygiene / reflect | 40 | 0 | 0 | 40 | P1 — domain hygiene suite exists |
| `jit_incremental` | JIT / AOT / incremental relower | 17 | 0 | 0 | 17 | P2 — link-profile heavy; migrate AC smoke first |
| `shape_soa` | Shape / SoA / column layout | 15 | 0 | 0 | 15 | P2 — small-medium; soa_batch precedent |
| `observability` | Observability / metrics / query:*-stats | 72 | 0 | 1 | 73 | P2 — often thin schema probes; collapse into obs matrix |

## Patterns, harness usage, coupling

### Harness / entry-point patterns (`tests/issues/` only)

| Pattern | Count | Meaning |
|---------|------:|---------|
| `CompilerService` | 216 | Integration path via `CompilerService` / eval |
| `test_harness` | 81 | `#include "test_harness.hpp"` + CHECK/TEST macros |
| `bundle_run_fn` | 78 | `aura_issue_*_run()` entry for issue bundles |
| `RUN_ALL_TESTS` | 56 | Harness runner main |
| `own_main` | 1 | File defines `int main()` (standalone or bundle source) |

### `@category` distribution (issues/)

- `integration`: 136
- `unknown`: 49
- `unit`: 34
- `issue_specific`: 6
- `regression`: 1

### Top includes (first 50 lines, issues/)

- `test_harness.hpp` — 75
- `serve/scheduler.h` — 7
- `serve/worker.h` — 5
- `reflect/reflect.hh` — 4
- `serve/fiber.h` — 2
- `compiler/aot_mangle.h` — 2
- `compiler/aura_jit.h` — 2
- `compiler/spec_jit_controller.h` — 2
- `../src/core/persistent_child_vector.hh` — 2
- `compiler/shape.h` — 1
- `compiler/shape_profiler.h` — 1
- `nodeview_wire.hh` — 1
- `issues/test_issue_178_bridge.h` — 1
- `compiler/value_tags.h` — 1
- `reflect/reflect_schema.hh` — 1

### Top module imports (first 50 lines, issues/)

- `aura.compiler.value` — 110
- `aura.compiler.evaluator` — 102
- `aura.compiler.service` — 93
- `std` — 72
- `aura.core.ast` — 53
- `aura.core.type` — 45
- `aura.core.arena` — 35
- `aura.core` — 21
- `aura.diag` — 20
- `aura.compiler.ir` — 19
- `aura.compiler.type_checker` — 14
- `aura.parser.parser` — 11
- `aura.compiler.pass_manager` — 9
- `aura.compiler.ir_executor` — 3
- `aura.compiler.evaluator_pure` — 2

### Coupling notes

1. **CompilerService-heavy** (~majority of issues/): most legacy tests are integration-style closed loops (eval → mutate → query stats). Domain migration should keep a shared CS fixture, not re-copy setup.
2. **Observability dual-path**: many files named `*_observability.cpp` or probing `query:*-stats` / `engine:metrics`. Prefer folding into `tests/domain/cases/obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp`.
3. **Bundle link profiles** (`light` / `jit` / `fiber` / `*_late*`): physical file location still `tests/issues/`; migration must update `issue_link_profiles.json` / CMake when deleting sources.
4. **Internal headers**: direct includes of `compiler/observability_metrics.h`, `serve/fiber.h`, `compiler/aura_jit*.h` couple tests to private surfaces — domain suites should prefer public query/primitives where possible.
5. **Existing consolidation path**: family `*_batch.cpp` drivers under `tests/` (listed in `AuraDomainTests.cmake`) are the intermediate step; domain suites are the long-term home.

## Multi-file issues, phase slices, low-value signals

- Issue numbers with **multiple** `tests/issues/` files: **3**
- Phase-slice files (`*_phase*`): **1**
- Small files (< 4 KiB, possible thin probes): **1**
- Existing `*_batch` drivers (migration milestones): **6**

### Multi-file issue groups (consolidate first)

- **#159** (2): `test_issue_159.cpp`, `test_issue_159_bench.cpp`
- **#178** (2): `test_issue_178.cpp`, `test_issue_178_reflect.cpp`
- **#213** (2): `test_issue_213.cpp`, `test_issue_213_panic_fiber.cpp`

### Smallest issue tests (triage for obs-matrix fold or drop)

- `test_issue_178.cpp` (3933 B) → `edsl_hygiene` — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `test_issue_131.cpp` (4176 B) → `edsl_hygiene` — test_issue_131.cpp — Verify the FFI primitives
- `test_issue_677.cpp` (4879 B) → `observability` — 
- `test_issue_622.cpp` (6638 B) → `observability` — query:atomic-batch-stats-hash structured companion
- `test_issue_515_observability.cpp` (6640 B) → `observability` — 
- `test_issue_136.cpp` (6672 B) → `jit_incremental` — test_issue_136.cpp — Verify Issue #136 acceptance criteria
- `test_issue_684.cpp` (6712 B) → `observability` — 
- `test_issue_523_observability.cpp` (6718 B) → `observability` — 
- `test_issue_682.cpp` (6816 B) → `observability` — 
- `test_issue_692.cpp` (6822 B) → `observability` — 
- `test_issue_503.cpp` (6839 B) → `edsl_hygiene` — 
- `test_issue_530_observability.cpp` (6904 B) → `observability` — 
- `test_issue_525_observability.cpp` (6912 B) → `observability` — 
- `test_issue_225_bridge_invalidation.cpp` (6947 B) → `mutation_dirty` — test_issue_225_bridge_invalidation.cpp — Verify Issue #225
- `test_issue_524_observability.cpp` (7002 B) → `observability` — 
- `test_issue_691.cpp` (7024 B) → `observability` — 
- `test_issue_534_observability.cpp` (7037 B) → `observability` — 
- `test_issue_218.cpp` (7038 B) → `edsl_hygiene` — test_issue_218.cpp — Issue #218 Cycle 5: reflection tests +
- `test_issue_533_observability.cpp` (7120 B) → `observability` — 
- `test_issue_675.cpp` (7175 B) → `observability` — test_issue_675.cpp — Issue #675: Build/CI reproducibility observability.

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

### `mutation_dirty` — Mutation / dirty propagation / provenance (54)

**Target:** tests/domain/test_domain_typed_mutate.cpp + mutation_boundary batch

**Priority:** P0 — high volume; strong domain suite foothold

#### domain/ (1)

- `tests/domain/test_domain_gates_batch.cpp` (—) [large, batch_driver, domain_suite] — test_domain_gates_batch.cpp — Domain suite batch: behavioral gates.

#### issues/ (53)

- `tests/issues/test_issue_141.cpp` (#141) [early_issue] — test_issue_141.cpp — Verify Issue #141 acceptance criteria
- `tests/issues/test_issue_142.cpp` (#142) [early_issue] — test_issue_142.cpp — Verify Issue #142 acceptance criteria
- `tests/issues/test_issue_147.cpp` (#147) [early_issue] — test_issue_147.cpp — Verify Issue #147 acceptance criteria
- `tests/issues/test_issue_148.cpp` (#148) [early_issue] — test_issue_148.cpp — Verify Issue #148 acceptance criteria
- `tests/issues/test_issue_159.cpp` (#159) [early_issue] — test_issue_159.cpp — Issue #159 Phase 1: incremental typecheck primitive.
- `tests/issues/test_issue_159_bench.cpp` (#159) [early_issue] — test_issue_159_bench.cpp — Issue #159 Phase 4: incremental
- `tests/issues/test_issue_177.cpp` (#177) [early_issue] — test_issue_177.cpp — Issue #213 verification:
- `tests/issues/test_issue_182.cpp` (#182) [large, early_issue] — test_issue_182.cpp — Issue #182: Hardware IR + Verilog Backend
- `tests/issues/test_issue_184.cpp` (#184) [early_issue] — test_issue_184.cpp — Issue #184: MutationBoundaryGuard RAII +
- `tests/issues/test_issue_188.cpp` (#188) [early_issue] — test_issue_188.cpp — Verify Issue #188 acceptance criteria
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
- `tests/issues/test_issue_289.cpp` (#289) — test_issue_289.cpp — Issue #289 / #481 acceptance tests.
- `tests/issues/test_issue_291.cpp` (#291) — workspace_id + serialization
- `tests/issues/test_issue_401.cpp` (#401) — test_issue_401.cpp — Issue #401: invalidate_function claims BFS
- `tests/issues/test_issue_429_soa.cpp` (#429) — test_issue_429_soa.cpp — Issue #429: IRFunctionSoA + FlatAST
- `tests/issues/test_issue_445_openclaw_integration.cpp` (#445) — test_issue_445_openclaw_integration.cpp — Issue #445:
- `tests/issues/test_issue_470_stable_ref_sv_scale.cpp` (#470) — test_issue_470_stable_ref_sv_scale.cpp — Issue #470:
- `tests/issues/test_issue_482.cpp` (#482) — replace-pattern share the same matcher (issue #482)
- `tests/issues/test_issue_488.cpp` (#488) — Issue #488 — post-mutate reflect validation + Guard impact snapshot
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

### `fiber_orch` — Fiber / orchestration / steal / Guard (25)

**Target:** tests/domain/test_domain_fiber_orchestration.cpp + fiber_resume batch

**Priority:** P1 — domain suite already collapses many obs gates

#### domain/ (1)

- `tests/domain/test_fiber_integration_batch.cpp` (—) [batch_driver, domain_suite] — tests/domain/test_fiber_integration_batch.cpp — Wave 8 of #1957 migration.

#### issues/ (24)

- `tests/issues/test_issue_115.cpp` (#115) [early_issue] — test_issue_115.cpp — Standalone tests for the Issue #115 follow-ups:
- `tests/issues/test_issue_119.cpp` (#119) [early_issue] — test_issue_119.cpp — Verify the proper-blocking fiber:join fix
- `tests/issues/test_issue_135.cpp` (#135) [large, early_issue] — test_issue_135.cpp — Verify Issue #135 acceptance criteria:
- `tests/issues/test_issue_189.cpp` (#189) [early_issue] — test_issue_189.cpp — Verify Issue #189 acceptance criteria
- `tests/issues/test_issue_195.cpp` (#195) [early_issue] — test_issue_195.cpp — Verify Issue #195 acceptance criteria
- `tests/issues/test_issue_213_panic_fiber.cpp` (#213) — test_issue_213_panic_fiber.cpp — Issue #213 follow-up cycle:
- `tests/issues/test_issue_473.cpp` (#473) — test_issue_473.cpp — Verify Issue #473 Tier 1 security fixes
- `tests/issues/test_issue_485.cpp` (#485) — SoA EnvFrame + AOT + scheduler/GC production-readiness close-out
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

### `linear_ownership` — Linear ownership / borrow / consume (4)

**Target:** tests/test_linear_ownership_batch.cpp → domain/

**Priority:** P1 — small, already partially batched

#### issues/ (4)

- `tests/issues/test_issue_117.cpp` (#117) [early_issue] — test_issue_117.cpp — Verify linear ownership validation fixes
- `tests/issues/test_issue_253.cpp` (#253) — test_issue_253.cpp — Issue #253 scope-limited close:
- `tests/issues/test_issue_763.cpp` (#763) — test_issue_763.cpp — Issue #763: Runtime linear_ownership_state
- `tests/issues/test_issue_765.cpp` (#765) — test_issue_765.cpp — Issue #765: Full DepEntry quote/lambda tracking +

### `edsl_hygiene` — EDSL / macro hygiene / reflect (40)

**Target:** tests/domain/test_domain_hygiene_dirty.cpp + macro_reflect batch

**Priority:** P1 — domain hygiene suite exists

#### issues/ (40)

- `tests/issues/test_issue_120.cpp` (#120) [early_issue] — test_issue_120.cpp — Verify the hygienic macro fix (Issue #120).
- `tests/issues/test_issue_131.cpp` (#131) [early_issue] — test_issue_131.cpp — Verify the FFI primitives
- `tests/issues/test_issue_137.cpp` (#137) [early_issue] — test_issue_137.cpp — Verify Issue #137 acceptance criteria
- `tests/issues/test_issue_140.cpp` (#140) [early_issue] — test_issue_140.cpp — Verify Issue #140 acceptance criteria
- `tests/issues/test_issue_146.cpp` (#146) [large, early_issue] — test_issue_146.cpp — Verify Issue #146 first extract
- `tests/issues/test_issue_158.cpp` (#158) [early_issue] — test_issue_158.cpp — Issue #158 verification:
- `tests/issues/test_issue_161.cpp` (#161) [early_issue] — test_issue_161.cpp — Issue #161 Phase 2: parser is now a pure function.
- `tests/issues/test_issue_162.cpp` (#162) [early_issue] — test_issue_162.cpp — Issue #162 Phase 1: Type Concepts for
- `tests/issues/test_issue_163.cpp` (#163) [early_issue] — test_issue_163.cpp — Issue #163: Expand Pass concept usage and
- `tests/issues/test_issue_165.cpp` (#165) [early_issue] — test_issue_165.cpp — Issue #165: macro re-expansion + SyntaxMarker
- `tests/issues/test_issue_174.cpp` (#174) [early_issue] — test_issue_174.cpp — Issue #174 Cycle 1 Env::bindings_
- `tests/issues/test_issue_178.cpp` (#178) [small, early_issue] — test_issue_178.cpp — Issue #178 / #268: production NodeView
- `tests/issues/test_issue_178_reflect.cpp` (#178) [early_issue] — Non-module TU: P2996 reflection (Issue #268).
- `tests/issues/test_issue_181.cpp` (#181) [early_issue] — test_issue_181.cpp — Issue #181: EvalValue 64-bit tagged
- `tests/issues/test_issue_190.cpp` (#190) [early_issue] — test_issue_190.cpp — Verify Issue #190 acceptance criteria
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
- `tests/issues/test_issue_290.cpp` (#290) — Validates the macro_dirty_ column + 4 Aura primitives. Key design point:
- `tests/issues/test_issue_440_edsl_readiness.cpp` (#440) — test_issue_440_edsl_readiness.cpp — Issue #440:
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

### `jit_incremental` — JIT / AOT / incremental relower (17)

**Target:** domain suite for incremental_*; keep heavy JIT in issue bundles

**Priority:** P2 — link-profile heavy; migrate AC smoke first

#### issues/ (17)

- `tests/issues/test_issue_136.cpp` (#136) [early_issue] — test_issue_136.cpp — Verify Issue #136 acceptance criteria
- `tests/issues/test_issue_143.cpp` (#143) [early_issue] — test_issue_143.cpp — Verify Issue #143 partial deliverable
- `tests/issues/test_issue_170.cpp` (#170) [early_issue] — test_issue_170.cpp — Issue #170: Accelerate LLVM JIT Backend
- `tests/issues/test_issue_171.cpp` (#171) [large, early_issue] — test_issue_171.cpp — Issue #171: High-Impact IR Optimization Passes
- `tests/issues/test_issue_193.cpp` (#193) [early_issue] — test_issue_193.cpp — Verify Issue #193 acceptance criteria
- `tests/issues/test_issue_194.cpp` (#194) [early_issue] — test_issue_194.cpp — Verify Issue #194 acceptance criteria
- `tests/issues/test_issue_237.cpp` (#237) — test_issue_237.cpp — Issue #237: AOT compilation path end-to-end.
- `tests/issues/test_issue_243.cpp` (#243) — test_issue_243.cpp — Issue #243: AOT bridge enhancement verification
- `tests/issues/test_issue_293.cpp` (#293) — Validates:
- `tests/issues/test_issue_452_aot_hot_update.cpp` (#452) — test_issue_452_aot_hot_update.cpp — Issue #452:
- `tests/issues/test_issue_590.cpp` (#590) — + multi-agent hot-update isolation + closure dispatch stale
- `tests/issues/test_issue_713.cpp` (#713) — hygiene violation detection in JIT deopt / Interpreter fallback
- `tests/issues/test_issue_720.cpp` (#720) — metadata (linear_ownership_state / shape_id / narrow_evidence /
- `tests/issues/test_issue_732.cpp` (#732) — test_issue_732.cpp — Issue #732: AOT hot-reload safe-swap at
- `tests/issues/test_issue_780.cpp` (#780) — test_issue_780.cpp — Issue #780: JIT / hot-update coverage
- `tests/issues/test_issue_793.cpp` (#793) — test_issue_793.cpp — Issue #793: P0 JIT/AOT
- `tests/issues/test_issue_794.cpp` (#794) — test_issue_794.cpp — Issue #794: P0 unified

### `shape_soa` — Shape / SoA / column layout (15)

**Target:** tests/test_soa_batch.cpp → domain/

**Priority:** P2 — small-medium; soa_batch precedent

#### issues/ (15)

- `tests/issues/test_issue_144.cpp` (#144) [early_issue] — test_issue_144.cpp — Verify Issue #144 acceptance criteria
- `tests/issues/test_issue_145.cpp` (#145) [large, early_issue] — test_issue_145.cpp — Verify Issue #145 partial deliverable
- `tests/issues/test_issue_167.cpp` (#167) [early_issue] — test_issue_167.cpp — Issue #167: IR layer SoA/DOD migration
- `tests/issues/test_issue_220.cpp` (#220) — test_issue_220.cpp — Issue #220: per-node children linked list
- `tests/issues/test_issue_254.cpp` (#254) — test_issue_254.cpp — Issue #254 scope-limited close:
- `tests/issues/test_issue_431_cxx26.cpp` (#431) — test_issue_431_cxx26.cpp — Issue #431: deepen C++26 Contracts
- `tests/issues/test_issue_463_soa_phase2_wiring.cpp` (#463) [phase_slice] — test_issue_463_soa_phase2_wiring.cpp — Issue #463:
- `tests/issues/test_issue_669.cpp` (#669) — - AC1:  query:primitives-meta [name] returns hash with the
- `tests/issues/test_issue_721.cpp` (#721) — gap_buffer Wiring for operands / shape / metadata + Dirty Cascade to
- `tests/issues/test_issue_723.cpp` (#723) — Contracts Expansion in Tagged Dispatch/Shape Stability + Value v2 Stats /
- `tests/issues/test_issue_766.cpp` (#766) — test_issue_766.cpp — Issue #766: IR-SoA migration observability +
- `tests/issues/test_issue_768.cpp` (#768) — test_issue_768.cpp — Issue #768: Shape + Pass + Contracts hot-path
- `tests/issues/test_issue_782.cpp` (#782) — test_issue_782.cpp — Issue #782: Dedicated terminal
- `tests/issues/test_issue_795.cpp` (#795) — test_issue_795.cpp — Issue #795: P0 deep hot-path
- `tests/issues/test_issue_796.cpp` (#796) — test_issue_796.cpp — Issue #796: P0 end-to-end

### `observability` — Observability / metrics / query:*-stats (73)

**Target:** tests/domain/test_obs_schema_matrix.cpp + cases/obs_schema_cases.hpp

**Priority:** P2 — often thin schema probes; collapse into obs matrix

#### domain/ (1)

- `tests/domain/test_obs_schema_matrix.cpp` (—) [domain_suite] — test_obs_schema_matrix.cpp — Domain suite: observability + production schemas

#### issues/ (72)

- `tests/issues/test_issue_149.cpp` (#149) [early_issue] — test_issue_149.cpp — Verify Issue #149 acceptance criteria
- `tests/issues/test_issue_247.cpp` (#247) — test_issue_247.cpp — Issue #247: SyntaxMarker observability integration
- `tests/issues/test_issue_252.cpp` (#252) — test_issue_252.cpp — Issue #252 scope-limited close:
- `tests/issues/test_issue_255.cpp` (#255) — test_issue_255.cpp — Issue #255 scope-limited close:
- `tests/issues/test_issue_256.cpp` (#256) — test_issue_256.cpp — Issue #256 scope-limited close:
- `tests/issues/test_issue_258.cpp` (#258) — test_issue_258.cpp — Issue #258 scope-limited close:
- `tests/issues/test_issue_259.cpp` (#259) — test_issue_259.cpp — Issue #259 scope-limited close:
- `tests/issues/test_issue_296.cpp` (#296) — Validates the Bridge Lifetime Contract documented in
- `tests/issues/test_issue_428_closure.cpp` (#428) — test_issue_428_closure.cpp — Issue #428: Strengthen Closure
- `tests/issues/test_issue_444_strategy_evolution.cpp` (#444) — test_issue_444_strategy_evolution.cpp — Issue #444:
- `tests/issues/test_issue_462_shape_aware_folding.cpp` (#462) — test_issue_462_shape_aware_folding.cpp — Issue #462:
- `tests/issues/test_issue_465_cxx26_hotpath.cpp` (#465) — test_issue_465_cxx26_hotpath.cpp — Issue #465:
- `tests/issues/test_issue_471_dirty_sv_scale.cpp` (#471) — test_issue_471_dirty_sv_scale.cpp — Issue #471:
- `tests/issues/test_issue_479.cpp` (#479) — test_issue_479.cpp — Verify Issue #479 per-prim fast-path hit tracking.
- `tests/issues/test_issue_489.cpp` (#489) — Issue #489 — StableNodeRef + get_safe enforcement in mutate/query hot paths
- `tests/issues/test_issue_490.cpp` (#490) — Issue #490 — proactive tag_arity_index rebuild on COW/compact + policy tuning
- `tests/issues/test_issue_500.cpp` (#500) — Issue #500 — Work-stealing + MutationBoundary outermost depth safety
- `tests/issues/test_issue_515_observability.cpp` (#515) [obs_named] — Issue #515 — consolidated-p0-production-stats hash slice
- `tests/issues/test_issue_523_observability.cpp` (#523) [obs_named] — Issue #523 — envframe-production-safety-stats hash slice
- `tests/issues/test_issue_524_observability.cpp` (#524) [obs_named] — Issue #524 — macro-production-hygiene-stats hash slice
- `tests/issues/test_issue_525_observability.cpp` (#525) [obs_named] — Issue #525 — guard-production-impact-stats hash slice
- `tests/issues/test_issue_528_observability.cpp` (#528) [obs_named] — Issue #528 — pattern-production-index-stats hash slice
- `tests/issues/test_issue_530_observability.cpp` (#530) [obs_named] — Issue #530 — incremental-production-relower-stats hash slice
- `tests/issues/test_issue_533_observability.cpp` (#533) [obs_named] — Issue #533 — soa-production-columnar-stats hash slice
- `tests/issues/test_issue_534_observability.cpp` (#534) [obs_named] — Issue #534 — arena-production-compaction-stats hash slice
- `tests/issues/test_issue_557_observability.cpp` (#557) [obs_named] — Issue #557 — top5-commercial-coverage-stats hash slice
- `tests/issues/test_issue_589.cpp` (#589) — primitive (the AC4 surface listed in #589 body)
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
- `tests/issues/test_issue_680.cpp` (#680) — Issue #680 precise Define mutate IR/JIT/bridge invalidation
- `tests/issues/test_issue_682.cpp` (#682) — Issue #682 compiler IRClosure/EnvId GC root coordination
- `tests/issues/test_issue_684.cpp` (#684) — Issue #684 IRSoA full wiring + incremental mutate
- `tests/issues/test_issue_687.cpp` (#687) — IR-interpreter identity fast-path + zero-overhead
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
