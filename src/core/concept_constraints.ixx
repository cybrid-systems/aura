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
// #2060 adds DirtySoAEntryPass + RequiresDirtySoAEntryPass (was 10).
// #2258 adds PureWrapPass (was 12).
// #3329 adds DirtyPropagatorAwarePass + ProductionPipelinePass.
inline constexpr int kPassConceptCount = 17;
// Issue #3329: compile-time purity gate for production pipeline entry.
inline constexpr int kPassPurityGateIssue = 3329;

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
//
// Purpose: uniform pipeline entry for optimization / analysis units
// Pre: P implements run(IRModule&) and has_error()
// Post: pipeline may short-circuit when has_error() is true after run
// Safety Class: P1 (incremental pipeline correctness)
// Issue: #1577 / #1886
// AI-Native Rationale: agents discover pass shape without reading each TU;
//   DirtyAware/Incremental refinements enable partial re-run after mutate
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
//
// Issue #2143 / #2907: IRModuleV2 dirty-only fold uses a separate concept
// `SoaDirtyAwarePass` (void run_dirty(IRModuleV2&)) in pass_manager.ixx
// so this block-level hook stays ABI-stable. Production DirtyAware kinds
// implement run_dirty + for_each_block(dirty_only) via
// run_production_soa_dirty_hot_pack; SoAtoAoSBridgePass is test-only (#2907).
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
// IR (DOD). Used for soft metrics and #1517/#1619 concept enforcement.
//
// Requirements:
//   - bool uses_soa_view() const
//
// Related: aura.compiler.soa_view::SoAView / SoAViewFull (IR column views
// with columnar_accessor + shape_id + linear_ownership). Pass pipeline
// enforces kRequireSoAView → SoAViewAwarePass via static_assert (#1619).
//
// Issue #1241 Phase 1 · #1619 refine.
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

// ── HotPassDodCompliant (#1918 / #2060 / #2258) ────────────────
//
// Production hot-path pass is either SoAViewAware (reports uses_soa_view)
// or explicitly marked LegacyPass. Used by check_pass_dod_compliance
// soft metrics + tests; hard static_assert for kRequireSoAView remains
// RequiresSoAViewPass → SoAViewAwarePass.
//
// Issue #2060 contract:
//   Non-Legacy HotPassDodCompliant stages that participate in
//   run_incremental_dirty_pipeline MUST also satisfy DirtySoAEntryPass
//   (provide run_on_dirty_blocks_only, or be Incremental+DirtyAware+SoA
//   so the pipeline can route a dirty-only / SoA-columnar entry).
//   Pure whole-module analysis (ArityWrap) may remain SoAViewAware without
//   DirtySoAEntry — define-level any() short-circuit still applies.
//
// Issue #2258 contract:
//   Any DirtyAwarePass / IncrementalPass registered into the production
//   incremental / partial-relower pipeline MUST satisfy HotPassDodCompliant
//   (compile-time reject via check_pass_dod_compliance). Prefer PureWrapPass
//   style (kPureWrap / PureAnalysisPass) so identical inputs + dirty mask
//   yield identical outputs with no hidden globals.
//
// Issue #2434 contract:
//   EVERY stage in run_pipeline / run_incremental_dirty_pipeline /
//   run_dirty_pipeline packs must be HotPassDodCompliant (unmarked soft
//   skip removed). Prefer SoAViewAware + kPureWrap; explicit kLegacyPass
//   only with a documented sunset. Production config keeps
//   pass_pipeline_concept_rejection_total == 0.
//
// Issue #3042 contract:
//   Production PureWrap dirty predicates are BlockDirtyPred /
//   InstructionDirtyPred (column view or non-capturing function pointer).
//   No std::function members or setters on PureWrap stages.
//
// Issue #2907 contract:
//   Production packs contain zero SoAtoAoSBridgePass. Hot DirtyAware +
//   SoAViewAware stages implement run_dirty(IRModuleV2&) (SoaDirtyAwarePass)
//   or DirtySoAEntryPass; sparse re-lower uses run_production_soa_dirty_hot_pack.
template <typename P>
concept HotPassDodCompliant = SoAViewAwarePass<P> || LegacyPass<P>;

// ── PureWrapPass (#2258) ───────────────────────────────────────
//
// Pass is a pure-function Wrap: either it declares
//   static constexpr bool kPureWrap = true;
// (stateful wrapper over pure free functions / columnar pure helpers)
// or it is PureAnalysisPass (const run, mutable accumulators only).
//
// Used for pass_pipeline_pure_wrap_total metrics and property tests that
// identical IR + dirty mask → identical outputs under dirty short-circuit.
template <typename P>
concept PureWrapPass =
    Pass<P> &&
    (PureAnalysisPass<P> || requires { requires std::remove_cvref_t<P>::kPureWrap == true; });

// Issue #3405: tighter concept for DirtyAware members of the
// production incremental pack. Requires BOTH the `kPureWrap` flag
// AND a SoA dirty-block-only entry (`run_on_dirty_blocks_only` over
// `IRFunctionSoA&` + `BlockDirtyPred`). Refuses any Wrap that sets
// `kPureWrap = true` and writes workspace / process globals via a
// full `run(IRModule&)` AoS walk — the only production surface for
// DirtyAware PureWrap stages must be the SoA hot path. `set_block_
// dirty_pred` (the AoS dirty-only path) is also rejected — the SoA
// columnar path replaces it.
//
// Source-cite anchor: AC1 — concept or `check_pass_dod_compliance`
// rejects a DirtyAware `kPureWrap` stage that only has `run(IRModule&)`
// from the production incremental pack. AC3 — existing CK/CF/TP/
// Shape/Escape suites keep their legacy `run_on_dirty_blocks_only
// (IRFunction&)` signatures and are accepted by `DirtySoAEntryPass`
// (the legacy sibling); NEW production members must satisfy
// `ProductionPureWrapPass` (this concept).
//
// Migration note: the existing 5 Wrap types in `optimization_passes.ixx`
// still expose the legacy `run_on_dirty_blocks_only(IRFunction&, ...)`
// signature. They are accepted by `DirtySoAEntryPass` (legacy) and
// `run_incremental_dirty_pipeline` continues to dispatch through the
// `IRFunction&` path. Migrating them to the new SoA per-function
// signature is a follow-up scope (#3405 AC3 stays — the tightened
// concept catches NEW production members; legacy stays grandfathered).
template <typename P>
concept ProductionPureWrapPass =
    PureWrapPass<P> &&
    SoAViewAwarePass<P> &&
    DirtyAwarePass<P> &&
    (requires(P& p, aura::ir::IRFunctionSoA& f, aura::core::arena_policy::BlockDirtyPred pred) {
         { p.run_on_dirty_blocks_only(f, pred) } -> std::same_as<void>;
     } ||
     requires(P& p, aura::ir::IRFunction& f) {
         { p.run_on_dirty_blocks_only(f) } -> std::same_as<void>;
     });

// ── DirtySoAEntryPass (#2060) ──────────────────────────────────
//
// Pass provides a dirty-only / SoA-columnar entry for sparse
// incremental re-lower. Preferred surface:
//   void run_on_dirty_blocks_only(IRFunction&)
// Accepted equivalent (pipeline routes via dirty peel + set_block_dirty_pred):
//   IncrementalPass + DirtyAwarePass + SoAViewAwarePass
//
// check_pass_dod_compliance / run_incremental_dirty_pipeline assert
// this for DirtyAware hot stages so clean blocks never pay full
// function walks under AI multi-round mutate.
template <typename P>
concept DirtySoAEntryPass = (SoAViewAwarePass<P> && DirtyAwarePass<P> && IncrementalPass<P>) ||
                            (SoAViewAwarePass<P> && requires(P& p, aura::ir::IRFunction& f) {
                                { p.run_on_dirty_blocks_only(f) } -> std::same_as<void>;
                            });

// Explicit opt-in marker: pass declares
//   static constexpr bool kRequireDirtySoAEntry = true;
// and MUST satisfy DirtySoAEntryPass (consteval in pass_manager).
template <typename P>
concept RequiresDirtySoAEntryPass =
    Pass<P> && requires { requires std::remove_cvref_t<P>::kRequireDirtySoAEntry == true; };

// ── DirtyPropagatorAwarePass (#3329) ───────────────────────────
//
// Pass participates in the DirtyPropagator cascade (concepts.ixx
// mark_dirty_upward) rather than a residual single-mark loop, OR
// is a PureWrap (no workspace write / unbounded alloc on the fold).
// DirtyAwarePass exposes is_block_dirty for the pipeline peel;
// PureWrapPass is the documented pure-function Wrap.
template <typename P>
concept DirtyPropagatorAwarePass = DirtyAwarePass<P> || PureWrapPass<P>;

// ── ProductionPipelinePass (#3329) ─────────────────────────────
//
// Compile-time purity / SoA / dirty gate for the production default
// fold. Impure Passes (workspace write, residual single-mark, no SoA,
// Legacy sunset) fail to instantiate run_production_pipeline.
// Soft / unit keep run_pipeline constrained only by Pass + DOD.
// Concepts erase — zero runtime cost on the happy path (AC5).
template <typename P>
concept ProductionPipelinePass =
    AnalysisPass<P> && SoAViewAwarePass<P> && DirtyPropagatorAwarePass<P> && !LegacyPass<P>;

// Issue #3329 AC1: a deliberately impure stub (workspace-write / no SoA /
// no dirty-upward / no PureWrap) must not satisfy ProductionPipelinePass.
// Compile-time only — never instantiated into a production pack.
namespace pass_purity_detail {
    struct ImpureWorkspaceWriteStub {
        void run(aura::ir::IRModule&) {}
        bool has_error() const { return false; }
    };
    struct ImpureNamedSoaNoDirtyStub {
        void run(aura::ir::IRModule&) {}
        bool has_error() const { return false; }
        std::string_view name() const { return "impure-soa"; }
        bool uses_soa_view() const { return true; }
    };
} // namespace pass_purity_detail
static_assert(Pass<pass_purity_detail::ImpureWorkspaceWriteStub>,
              "Issue #3329: impure stub is still a Pass (Soft/unit admit)");
static_assert(!ProductionPipelinePass<pass_purity_detail::ImpureWorkspaceWriteStub>,
              "Issue #3329: impure workspace-write stub fails ProductionPipelinePass");
static_assert(AnalysisPass<pass_purity_detail::ImpureNamedSoaNoDirtyStub> &&
                  SoAViewAwarePass<pass_purity_detail::ImpureNamedSoaNoDirtyStub>,
              "Issue #3329: named SoA stub is Analysis+SoA");
static_assert(!ProductionPipelinePass<pass_purity_detail::ImpureNamedSoaNoDirtyStub>,
              "Issue #3329: SoA without DirtyPropagator/PureWrap fails production gate");
static_assert(pass_concepts::kPassPurityGateIssue == 3329, "Issue #3329 stamp");

} // namespace aura::compiler
