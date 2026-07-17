// concept_constraints.ixx — Issue #1577: centralized Pass / pipeline concepts.
//
// Previously these lived inline in pass_manager.ixx. They are concentrated
// here so optimization_passes, lowering, JIT, and tests can import a single
// module without pulling the full pass runtime.
//
// Path: src/core/concept_constraints.ixx (AC #1577).
// Namespace: aura::compiler (ABI-compatible with historical pass_manager
// definitions — existing `aura::compiler::Pass` etc. keep working).
//
// Layering note: this file lives under src/core/ but imports
// aura.compiler.ir because Pass constraints are IR-shaped. It is listed
// in AURA_CXX_MODULE_COMPILER (after ir.ixx) in cmake/AuraModules.cmake.

module;

export module aura.core.concept_constraints;

import std;
import aura.compiler.ir;

// Phase / inventory for Agent dashboards (#1577).
export namespace aura::compiler::pass_concepts {

inline constexpr int kConceptConstraintsPhase = 1;
// Number of named Pass-related concepts exported below (keep in sync).
inline constexpr int kPassConceptCount = 10;

inline std::atomic<std::uint64_t> concept_constraints_import_hits{0};

inline void note_concept_constraints_import() noexcept {
    concept_constraints_import_hits.fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::compiler::pass_concepts

export namespace aura::compiler {

// ═══════════════════════════════════════════════════════════════
// Pass pipeline concepts (migrated from pass_manager.ixx — #1577)
// ═══════════════════════════════════════════════════════════════

// ── Pass ───────────────────────────────────────────────────────
//
// Any optimization / analysis unit that can run over an IRModule.
//
// Requirements:
//   - void run(IRModule&)  — entry for full-module execution
//   - bool has_error()     — true if the last run failed (pipeline short-circuit)
//
// Relationship to dirty / contracts:
//   - DirtyAwarePass and IncrementalPass refine Pass for partial re-runs.
//   - C++26 contracts on run() (see optimization_passes.ixx #1576) are
//     optional but recommended for production passes.
//
// Issue #274: FlatAST mutation visitors follow a parallel fold pattern
// via aura::ast::MutationVisitor (not this concept).
template <typename P>
concept Pass = requires(P& p, aura::ir::IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
    { p.has_error() } -> std::convertible_to<bool>;
};

// ── AnalysisPass ───────────────────────────────────────────────
//
// Narrower than Pass: also exposes name() for logging / registries.
// Authors SHOULD treat run() as read-only on IR (not compile-time enforced).
//
// Use for: EscapeAnalysis, TypePropagation, LinearOwnership-style analyses.
// AnalysisPass is a subset of Pass plus name() — additive requirement.
template <typename A>
concept AnalysisPass = requires(A& a, aura::ir::IRModule& m) {
    { a.run(m) } -> std::same_as<void>;
    { a.has_error() } -> std::convertible_to<bool>;
    { a.name() } -> std::convertible_to<std::string_view>;
};

// ── PureAnalysisPass ───────────────────────────────────────────
//
// AnalysisPass whose run(IRModule&) is const-qualified — same observable
// results on repeated runs without mutating the pass's logical state
// (accumulators may still be mutable members).
//
// Issue #606 / #1204: ComputeKindWrap / ArityWrap / ShapeWrap patterns.
template <typename P>
concept PureAnalysisPass = AnalysisPass<P> && requires(const P& p, aura::ir::IRModule& m) {
    { p.run(m) } -> std::same_as<void>;
};

// ── IncrementalPass ────────────────────────────────────────────
//
// Pass that exposes per-function and per-block entry points for partial
// re-execution after mutation (instead of re-running the whole module).
//
// Requirements:
//   - void run(IRFunction&)
//   - void run(BasicBlock&)
//
// Used by: run_incremental_pipeline / run_incremental_dirty_pipeline.
// Semantics: run(func) must match run(module) restricted to that function.
//
// Issue #381 / #606: ConstantFoldingWrap aliases fold_function / fold_block.
template <typename P>
concept IncrementalPass =
    Pass<P> && requires(P& p, aura::ir::IRFunction& f, aura::ir::BasicBlock& b) {
        { p.run(f) } -> std::same_as<void>;
        { p.run(b) } -> std::same_as<void>;
    };

// ── DirtyAwarePass ─────────────────────────────────────────────
//
// Pass that can consult per-block dirty state and skip clean blocks.
// Companion to IRFunctionSoA / IRCacheEntry block_dirty bitmasks
// (#196 / #1574 / #1575 dirty_propagation cascade).
//
// Requirements:
//   - bool is_block_dirty(block_id) const  — true = needs work
//
// Pipeline short-circuit: if all blocks clean, skip the entire function
// (or whole pass when DefineDirtyMaskView::any() is false).
//
// Issue #381: load-bearing property for incremental hot paths.
template <typename P>
concept DirtyAwarePass = Pass<P> && requires(const P& p, std::uint32_t block_id) {
    { p.is_block_dirty(block_id) } -> std::convertible_to<bool>;
};

// ── InstructionDirtyAwarePass ──────────────────────────────────
//
// Optional refinement of DirtyAwarePass for instruction-level skips.
//
// Requirements:
//   - bool is_instruction_dirty(block_id, inst_id) const
//
// Issue #1197: dirty pipeline counts clean instruction probes for
// observability; real peel uses block size when available.
template <typename P>
concept InstructionDirtyAwarePass =
    DirtyAwarePass<P> && requires(const P& p, std::uint32_t block_id, std::uint32_t inst_id) {
        { p.is_instruction_dirty(block_id, inst_id) } -> std::convertible_to<bool>;
    };

// ── ShapeStableAwarePass ───────────────────────────────────────
//
// DirtyAware pass that may also skip work when ShapeProfiler reports
// the enclosing function's shape as stable (speculative opt preserved).
//
// Currently an alias of DirtyAwarePass; shape stability is consulted
// via g_fn_shape_stable_probe in the dirty pipeline (Issue #744).
template <typename P>
concept ShapeStableAwarePass = DirtyAwarePass<P>;

// ── JITFriendlyPass ────────────────────────────────────────────
//
// Pass that exposes a pipeline epoch hint for JIT / mutation_epoch
// coordination (advisory, relaxed ordering).
//
// Requirements:
//   - uint64_t pipeline_epoch_hint() const
//
// Issue #494: CompilerService may set_pipeline_epoch from mutation_epoch_
// before incremental re-lower.
template <typename P>
concept JITFriendlyPass = Pass<P> && requires(const P& p) {
    { p.pipeline_epoch_hint() } -> std::convertible_to<std::uint64_t>;
};

// ── SoAViewAwarePass ───────────────────────────────────────────
//
// Pass that can report whether its hot path uses SoAView / columnar
// IR (DOD). Used for soft metrics and #1517 concept enforcement.
//
// Requirements:
//   - bool uses_soa_view() const
//
// Issue #1241 Phase 1.
template <typename P>
concept SoAViewAwarePass = Pass<P> && requires(const P& p) {
    { p.uses_soa_view() } -> std::convertible_to<bool>;
};

// ── LegacyPass ─────────────────────────────────────────────────
//
// Explicit opt-out for passes that intentionally remain AoS during
// migration. Declares: static constexpr bool kLegacyPass = true;
//
// Issue #1517: note_pass_soa_enforcement treats these as transitional.
template <typename P>
concept LegacyPass = Pass<P> && requires { requires std::remove_cvref_t<P>::kLegacyPass == true; };

// ── RequiresSoAViewPass ────────────────────────────────────────
//
// Strict mode: pass declares static constexpr bool kRequireSoAView = true
// and MUST also satisfy SoAViewAwarePass (enforced by
// check_pass_dod_compliance in pass_manager).
//
// Issue #1517: hot-path zero-overhead DOD gate.
template <typename P>
concept RequiresSoAViewPass =
    Pass<P> && requires { requires std::remove_cvref_t<P>::kRequireSoAView == true; };

} // namespace aura::compiler
