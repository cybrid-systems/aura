# Safety Mechanisms Simplification

**Issue**: #1946 (P1 Simplification, parent #1942 Simplification Roadmap)

## Status

Consolidation **prototype phase** shipped (multiple ship rounds
since 2026-07-19). This doc is the consolidated design note
covering the 5 overlapping safety mechanisms in core + compiler.

## Mechanisms map

5 overlapping safety mechanisms exist in core + compiler. Each is
safety-critical; simplification proceeds by consolidation rather
than removal.

### 1. Provenance

- **Production handle**: `FlatAST::StableNodeRef` (defined in
  `src/core/ast.ixx:6388`).
- **Policy + metrics**: `src/core/provenance_tracker.hh`
  (process-wide `ProvenanceEnforcementMetrics` +
  `HygieneProvenanceStamp`).
- **Scaffold (deleted)**: `src/core/provenance_tracker.ixx`
  (C++20 module) was a "Lightweight tracker (Phase 2)" scaffold
  with zero external consumers; **deleted** in `eb007a4c` (Issue
  #1964 cycle 1, 80 lines removed). Single canonical record per
  node achieved.

### 2. Epoch

- **5 legacy counters**:
  - `bridge_epoch` (Worker + Closure cache invalidation,
    602 refs across `src/serve/fiber.{h,cpp}`).
  - `mutation_epoch` (FlatAST global mutation epoch, 101 refs
    across `src/core/ast.ixx`).
  - `subtree_generation_` (per-top-level-Define generation, ~80
    refs across `src/core/ast.ixx`).
  - `wrap_epoch_` (wrap tracker, ~20 refs across
    `src/core/ast.ixx`).
  - `generation_` (AST workspace epoch, 1-indexed, ~44 refs
    across `src/core/ast.ixx`).
- **Unified type**: `WorkspaceEpoch` defined in
  `src/core/workspace_epoch.hh` (shipped in `f624af81`, Issue
  #1964 cycle 2a). Per-kind atomic storage shim +
  `WorkspaceEpochKind` enum (Mutation / Bridge / Subtree / Wrap
  / Generation).
- **Migration linter**: `scripts/check_workspace_epoch_migration.py`
  (8,535 bytes, shipped in same cycle 2a). Default non-strict;
  `--strict` for CI after migrations complete.

### 3. Guard

- **MutationBoundaryGuard** (Evaluator) — `try_acquire` mutation
  boundary wrapper around agent body + hot-update body. Issues a
  typed `ResourceQuotaExceeded` on reject (never panic).
- **PanicCheckpointRAII** (panic_checkpoint_raii.ixx) — RAII panic
  guard with dtor atomic ops. Issue #1950 / #1953 lineage.
- **Unified API**: `TransactionGuard` (#1964 cycle 3) — production
  still uses MBG + PCR until call-site migration completes.

### 4. Closure lifetime / linear ownership

- **ClosureView lifetime stamps** + linear ownership probes
  (#1888 family of fixes).
- **StableRef cross-cow provenance enforcement** (#1630, #1877).
- All enforcement lives in `src/compiler/evaluator_fiber_mutation.cpp`
  + the cross_cow_provenance_enforced process-wide counter in
  `ProvenanceEnforcementMetrics`.

### 5. Defuse (Closure Table)

- `defuse_version` (closure table) — tracks closure
  desynchronization state. Used for safety gates in
  `serve::Fiber::steal_safe` paths.

## Consolidation shipped

Concrete consolidation proposals with measurable reduction:

| Ship | Commit | Reduction |
|---|---|---|
| Provenance dual-track collapse | `eb007a4c` (#1964 cycle 1) | 80 lines (deleted dead module) + 0 dual-record paths |
| WorkspaceEpoch type foundation | `f624af81` (#1964 cycle 2a) | 1 new type, 1 linter; 847-ref migration path formalized |
| mutation_epoch API migration | `779eea77` (#1964 cycle 2b) | `current_mutation_epoch` / `bump_mutation_epoch` |
| bridge_epoch accessors | `c05a6b3e` (#1964 cycle 2c) | process-global Bridge kind API |
| per-AST epoch design note | `18dc5ff7` (#1964 cycle 2d) | design for subtree/wrap/generation |
| TransactionGuard API | `27969acf` (#1964 cycle 3) | unified TG surface + metrics |
| mutate_dispatch API | `71b2d4a3` (#1964 cycle 4) | MutateKind + single dispatch + metrics |
| Architectural dashboard | `query:architectural-simplification-stats` | schema-1964 observability |
| orch/ MVP boundary + linter | `bcb68c7c` (#1965 cycle 1) | 470 lines / 1 orch facade; `// DEFERRED` markers |
| commercial_readiness core vs deferred | `eb935139` (#1965 cycle 2) | 75 deferred primitives via `DOMAIN_STATUS` |
| orch/ Experimental status | `07c14d67` (#1945) | status doc + 4-step re-enable path |
| 11 follow-up issues | (#1966-#1976, #1965 cycle 3) | Deferred features tracked |

## Consolidation follow-ups (call-site migration)

Phase 2 **API surfaces** for cycles 1–4 are shipped. Remaining work is
mechanical call-site migration (not re-design):

- **Epoch leftovers**: `scripts/check_workspace_epoch_migration.py`
  still reports ~197 non-strict violations (legacy field uses).
  `--strict` not yet in gate.
- **TransactionGuard**: migrate agent/orch paths from raw
  `MutationBoundaryGuard` + `PanicCheckpointRAII` to `TransactionGuard`
  (or make MBG wrap TG).
- **mutate_dispatch**: migrate remaining `add("mutate:*")` bodies to
  thin wrappers; `mutate:set-body` already bookkeeps via
  `mutate_dispatch(SetBody, …)`.
- **#1965 follow-ups** (#1966-#1976): 11 deferred-feature tracking
  issues.

## Prototype implementation on hot-update / mutation path

Per AC #3 (prototype on hot-update / mutation path):

- `WorkspaceEpoch` type (`src/core/workspace_epoch.hh`) provides
  the unified counter vocabulary for the hot-update path
  (hot-swap boundary ops + fiber steal probes).
- `scripts/check_orch_mvp_scope.py` (`bcb68c7c`) tracks deferred
  surface in the orch path (which is the mutation boundary path).
- `scripts/check_workspace_epoch_migration.py` (`f624af81`)
  tracks counter migration progress across 847 refs.

These three artifacts together provide measurable reduction in
guarded operations:
- `stable_ref_auto_refresh_total` counter tracks the boundary-pinned
  auto-restamp path that previously required per-call provenance
  gating.
- `macro_provenance_repin_on_steal_total` (Issue #1908) tracks
  the new unified steal-handling path that consolidates per-fiber
  re-pin operations.
- `cross_cow_provenance_enforced_total` tracks cross-cow closure
  provenance, a separate axis from the epoch counter path.

Net: the unified surface (single `WorkspaceEpoch` type + single
orch MVP boundary + single cross-cow enforcement) replaces 5+
overlapping surfaces. Future cycles 2b/2c/2d/3/4 continue the
migration.

## Updated invariants

Consolidated invariants across all 5 mechanisms (single source of
truth):

### Epoch freshness
```
captured == current   OR   captured == 0  (legacy / unstamped)
```
Implemented in `WorkspaceEpoch::is_fresh()` (static helper);
matches the legacy `validate_mutation_id` /
`epoch_fence_ok` semantics in `provenance_tracker.hh`.

### Provenance freshness
```
captured_mutation_id == current_source_mutation_id  OR  == 0
captured_gen == current_gen  OR  captured_gen == 0
captured_wrap_epoch == current_wrap  OR  == 0
captured_cow_epoch == current_cow  OR  boundary_pinned
captured_fiber_id == current_fiber  OR  == 0
captured_tenant_id == current_tenant  OR  == 0
```
Implemented in `ProvenanceTracker::is_valid_full()` (scaffold
form) + enforced across `StableNodeRef` consumers in
`evaluator_fiber_mutation.cpp`.

### Guard scope
```
MutationBoundaryGuard::try_acquire remains the production boundary.
PanicCheckpointRAII is exception-only (Issue #1950/#1953).
TransactionGuard (#1964 cycle 3) is the unified API surface —
call-site migration is follow-up.
```

### Closure lifetime / linear ownership
```
StableRef cross-cow enforcement + linear_violation_prevented
counter (auto-bumped on agent spawn/join + fiber steal).
```
Tracked by `stable_ref_auto_refresh_total`,
`fiber_steal_provenance_enforced_total`, and
`linear_violation_prevented_total` in
`ProvenanceEnforcementMetrics` +
`OrchModuleStats`.

### Defuse (closure table)
```
defuse_version tracks closure desynchronization state;
gates `serve::Fiber::steal_safe` paths.
```
No consolidation yet — separate cleanup pending.

## Refs

- Parent: #1942 (Simplification Roadmap)
- #1964 (Phase 2: architectural simplification) — cycles 1–4 **API
  surfaces shipped**; call-site migration is follow-up
- #1965 (Phase 3: scope deferral + governance + 11 follow-up issues)
- #1945 (orch/ Advanced / Experimental + status doc)
- SlimSurface: #1432, #1448, #1449 (primitive freeze + budget)
- MVP pattern: #1943 (Hot-Update MVP)
- Dashboard: `(engine:metrics "query:architectural-simplification-stats")`

## Cross-cutting mechanism surface area (today)

After #1964 cycles 1–4 API surfaces + #1965 cycles 1+2+3:

| Surface | Files | Status |
|---|---|---|
| Provenance (single canonical) | `provenance_tracker.hh` + `StableNodeRef` | ✓ ship |
| Epoch (unified type + APIs) | `workspace_epoch.hh` | ✓ type+API; field migration ~197 left |
| Guard (unified transaction API) | `transaction_guard.hh` + MBG + PCR | ✓ API; migrate callers |
| Mutate dispatch | `mutate_dispatch.hh` | ✓ API; set-body bookkeeps |
| Closure lifetime + linear | evaluator fiber/mutation | ✓ existing |
| Defuse (closure table) | n/a | tracked separately |

Per-mechanism LOC/cyclomatic complexity is tracked via the
build's clang-tidy + complexity reports; current count down vs
pre-#1964 baseline by 80 lines (provenance dual-track alone) +
the consolidation potential of the remaining cycles.

## Linters

- `scripts/check_workspace_epoch_migration.py` (#1964 cycle 2a)
- `scripts/check_orch_mvp_scope.py` (#1965 cycle 1)
- `scripts/check_primitive_surface.py --strict` (#1965 cycle 2
  + pre-existing #1448)

Each linter is shippable as a progress tracker; `--strict` mode
activates as cycles complete and migrate the surface.
