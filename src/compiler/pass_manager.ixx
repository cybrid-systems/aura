// ──────────────────────────────────────────────────────────────
// pass_manager.ixx — Issue #2524 Phase C facade
// ──────────────────────────────────────────────────────────────
//
// Historical home of pipeline folds + concrete pass implementations.
// Split (Issue #2524) for compile-time / review hygiene:
//
//   aura.compiler.pass_pipeline_core  — metrics, DefineDirtyMaskView,
//                                       run_pipeline / dirty-aware folds
//   aura.compiler.pass_impls          — ComputeKindWrap … SoAtoAoSBridgePass
//   aura.compiler.pass_manager        — this facade (export import both)
//
// Importers keep `import aura.compiler.pass_manager` with no API renames.
// Hot TUs that only need folds/metrics may import pass_pipeline_core.
// Concepts remain in aura.core.concept_constraints (#1577).
//
// ## Module partition map (#2524) — source of truth for AC4
//
// Giant interface units (baseline sizes pre-Phase C):
//   evaluator.ixx ~754 KB | ast.ixx ~482 KB | pass_manager.ixx ~260 KB
//
// Target architecture (phased; no public API renames):
//
//   Phase A — evaluator (planned): facade → evaluator_core /
//     evaluator_mutation; primitives already same-module .cpp partitions.
//   Phase B — ast (planned): facade → ast_soa / ast_mutate / ast_query;
//     ast_impl.cpp / ast_stability.cpp already same-module bodies.
//   Phase C — pass_manager (this ship): pass_pipeline_core + pass_impls
//     + thin facade. Dependency: pass_impls → pass_pipeline_core
//     → concept_constraints / IR. Facade only re-exports (no cycles).
//
// CMake: cmake/AuraModules.cmake lists core then impls then facade.
// Layering: Compiler may depend on Core (#1885).

module;

export module aura.compiler.pass_manager;

// Re-export pipeline core + pass implementations so existing importers
// see the full historical surface under one module name.
export import aura.compiler.pass_pipeline_core;
export import aura.compiler.pass_impls;

// Issue #1577 lineage: concepts are also re-exported from pass_pipeline_core
// (and concept_constraints). Facade documents the umbrella for Agents.
// No public API renames — importers keep `import aura.compiler.pass_manager`.
export namespace aura::compiler {
inline constexpr int kPassManagerFacadeIssue = 2524;
inline constexpr int kPassManagerPartitionPhase = 1; // Phase C first
} // namespace aura::compiler
