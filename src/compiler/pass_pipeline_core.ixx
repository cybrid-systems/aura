module;
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/cpp26_contract_stats.h"
#include "core/workspace_epoch.hh"             // Issue #2822: current_mutation_epoch auto-wire
#include "compiler/observability_metrics.h"    // Issue #1425: dead_coercion_eliminated_total
#include "compiler/jit_typed_mutation_stats.h" // Issue #1629: dual-emit flag early-out

// Issue #2524 Phase C: pipeline core extracted from pass_manager.ixx
// (metrics, DefineDirtyMaskView, run_pipeline / dirty-aware folds).
// Pass class implementations live in pass_impls.ixx.
export module aura.compiler.pass_pipeline_core;
import std;
import aura.core;
import aura.compiler.ir;
import aura.core.type;
import aura.compiler.ir;
import aura.compiler.compute_kind;
import aura.compiler.ir_soa;
import aura.compiler.arity;
import aura.compiler.constant_folding;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.ir_soa;
import aura.compiler.soa_view;
import aura.compiler.dirty_propagation;
import aura.compiler.ir_cache_pure; // Issue #2109: should_partial_relower
// Issue #1577: Pass concepts centralized in concept_constraints.
// export import re-exports aura::compiler::Pass / DirtyAwarePass / …
// so existing `import aura.compiler.pass_manager` consumers keep working.
export import aura.core.concept_constraints;
import aura.diag;

// Issue #1885: Compiler layer may depend on Core (+ Parser via other TUs).
// Layering authority: src/core/module_boundary.ixx (aura.core.module_boundary).
// Re-exported via import aura.core → aura::core::boundary::ModuleLayer.
static_assert(aura::core::boundary::AllowedDependency<aura::core::boundary::ModuleLayer::Compiler,
                                                      aura::core::boundary::ModuleLayer::Core>);
static_assert(aura::core::boundary::kModuleBoundaryIssue == 1885);

namespace aura::compiler {

// Issue #1517 / #1619: forward declare so analysis/full pipelines can share
// the same SoAView enforcement (defined below with SoAViewAwarePass).
// Pass / AnalysisPass / … concepts: see aura.core.concept_constraints (#1577).
export template <typename P> void note_pass_soa_enforcement(P& pass) noexcept;
export template <typename P> consteval void check_pass_dod_compliance();
export template <typename... Passes> consteval void check_pipeline_dod_compliance();

// ── run_analysis_pipeline — fold over analysis passes ────────────
//
// Same fold semantics as run_pipeline, but constrained to
// AnalysisPass types. Useful for separating analysis
// (read-only) from transform (mutating) passes in the
// pipeline — analysis runs first, transforms after, but
// the type system enforces the separation.
export template <AnalysisPass... Passes>
bool run_analysis_pipeline(aura::ir::IRModule& mod, Passes&... passes) {
    // Issue #1517 / #1619: SoAView DOD pack enforcement at analysis entry.
    check_pipeline_dod_compliance<Passes...>();
    (note_pass_soa_enforcement(passes), ...);
    return (run_analysis_one(mod, passes) && ...);
}

export template <AnalysisPass P> bool run_analysis_one(aura::ir::IRModule& mod, P& pass) {
    pass.run(mod);
    return !pass.has_error();
}

// Issue #494: optional yield hook between pass stages (wired from service).
export using PipelineYieldHook = bool (*)() noexcept;
export inline std::atomic<std::uint64_t> pipeline_yield_count{0};
export inline std::atomic<std::uint64_t> passes_skipped_dirty_pipeline{0};
// Issue #744: blocks skipped because fn shape is stable and block is clean.
export inline std::atomic<std::uint64_t> passes_skipped_shape_stable_blocks{0};
// Issue #625: lifetime # of full run_pipeline() invocations.
// Bumped once per full pipeline run (NOT per-pass). Pairs with
// passes_skipped_dirty_pipeline so the Agent can compute the
// short-circuit ratio (skips / runs * average-fns-per-run).
export inline std::atomic<std::uint64_t> pass_pipeline_runs_total{0};
// Issue #1322 Phase 1: DirtyAware + SoAView + epoch coordination metrics.
export inline std::atomic<std::uint64_t> pipeline_dirty_short_circuit_total{0};
export inline std::atomic<std::uint64_t> pipeline_epoch_sync_total{0};
// Issue #2822: run_one auto-resolved epoch because TLS pipeline epoch was 0
// (caller never called set_pipeline_mutation_epoch).
export inline std::atomic<std::uint64_t> pipeline_epoch_unset_runs_total{0};
// Issue #2822: floor when process mutation epoch is also 0 so JITFriendly
// passes always leave run_one with a non-zero pipeline_epoch_hint.
export inline constexpr std::uint64_t kPipelineEpochBaseFloor = 1;
export inline std::atomic<std::uint64_t> pipeline_hotpath_light_analysis_total{0};

// ── Issue #1574: define-level dirty bitmask → optimization pipeline ──
//
// IRCacheEntry holds block_dirty_per_func_ / instruction_dirty_per_func_
// but pass_manager must not depend on CompilerService. This view is the
// ABI between service.ixx (producer) and run_incremental_dirty_pipeline
// (consumer). Optional pointer — nullptr means "legacy: trust the pass".
export struct DefineDirtyMaskView {
    // Parallel to IRCacheEntry::block_dirty_per_func_ [func][block] = 1 dirty.
    const std::vector<std::vector<std::uint8_t>>* block_dirty_per_func = nullptr;
    // Parallel to IRCacheEntry::instruction_dirty_per_func_ [func][abs_inst] = 1.
    // Indices are absolute in the function instruction stream (block-major).
    const std::vector<std::vector<std::uint8_t>>* instruction_dirty_per_func = nullptr;
    // Issue #2133: [func][block] = instruction count — converts (block, inst_in_block)
    // → absolute index for instruction_dirty_per_func. When null, inst_id is
    // treated as absolute (legacy single-block / test views).
    const std::vector<std::vector<std::uint32_t>>* block_instr_counts = nullptr;

    // True if any block bit is set. Empty / null → treated as dirty (safe).
    [[nodiscard]] bool any() const noexcept {
        if (!block_dirty_per_func || block_dirty_per_func->empty())
            return true;
        for (const auto& fb : *block_dirty_per_func) {
            for (auto b : fb) {
                if (b)
                    return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_block_dirty(std::size_t func_idx, std::uint32_t block_id) const noexcept {
        if (!block_dirty_per_func)
            return true;
        if (func_idx >= block_dirty_per_func->size())
            return true;
        const auto& fb = (*block_dirty_per_func)[func_idx];
        if (block_id >= fb.size())
            return true;
        return fb[block_id] != 0;
    }

    // Instruction-level: prefer instruction mask when present; else block.
    // inst_id is in-block index when block_instr_counts is set (#2133).
    [[nodiscard]] bool is_instruction_dirty(std::size_t func_idx, std::uint32_t block_id,
                                            std::uint32_t inst_id) const noexcept {
        if (!is_block_dirty(func_idx, block_id))
            return false;
        if (!instruction_dirty_per_func || func_idx >= instruction_dirty_per_func->size())
            return true;
        const auto& idf = (*instruction_dirty_per_func)[func_idx];
        std::uint32_t abs = inst_id;
        if (block_instr_counts && func_idx < block_instr_counts->size()) {
            const auto& counts = (*block_instr_counts)[func_idx];
            abs = 0;
            for (std::uint32_t bi = 0; bi < block_id && bi < counts.size(); ++bi)
                abs += counts[bi];
            abs += inst_id;
        }
        if (abs >= idf.size())
            return true;
        return idf[abs] != 0;
    }

    // Observability helpers for dirty_block_relower_ratio.
    [[nodiscard]] std::uint64_t dirty_block_count() const noexcept {
        if (!block_dirty_per_func)
            return 0;
        std::uint64_t n = 0;
        for (const auto& fb : *block_dirty_per_func)
            for (auto b : fb)
                if (b)
                    ++n;
        return n;
    }
    [[nodiscard]] std::uint64_t total_block_count() const noexcept {
        if (!block_dirty_per_func)
            return 0;
        std::uint64_t n = 0;
        for (const auto& fb : *block_dirty_per_func)
            n += fb.size();
        return n;
    }
};

// Entire optimization pass skipped because define-level mask is clean.
export inline std::atomic<std::uint64_t> optimization_passes_skipped_by_define_dirty{0};
// Sum of dirty blocks / total blocks seen when define_cache is consulted
// (basis points, updated on each pipeline entry with a non-null cache).
export inline std::atomic<std::uint64_t> dirty_block_relower_ratio_bp{0};
export inline std::atomic<std::uint64_t> define_dirty_blocks_seen_total{0};
export inline std::atomic<std::uint64_t> define_total_blocks_seen_total{0};
// Issue #2060: dirty-only / SoA-columnar entry path observability.
export inline std::atomic<std::uint64_t> dirty_only_entry_hits_total{0};
export inline std::atomic<std::uint64_t> dirty_only_blocks_run_total{0};
export inline std::atomic<std::uint64_t> dirty_only_blocks_skipped_total{0};
export inline std::atomic<std::uint64_t> run_one_dirty_calls_total{0};
export inline std::atomic<std::uint64_t> hot_pass_dirty_soa_wired{1};
// Issue #2143: IRModuleV2 run_dirty fold pipeline observability
// (clean_skips / dirty_runs aggregate from for_each_block).
export inline std::atomic<std::uint64_t> run_dirty_pipeline_invocations_total{0};
export inline std::atomic<std::uint64_t> run_dirty_pipeline_pass_runs_total{0};
export inline std::atomic<std::uint64_t> run_dirty_pipeline_clean_skips_total{0};
export inline std::atomic<std::uint64_t> run_dirty_pipeline_dirty_runs_total{0};
export inline std::atomic<std::uint64_t> soa_dirty_aware_pass_wired{1};
// Issue #2258 / #2434: pure Wrap pipeline metrics + concept-rejection.
// pure_wrap: stages that are PureWrapPass (kPureWrap / PureAnalysisPass).
// concept_rejection: Legacy / uses_soa_view()==false samples (migration).
// Under production packs (#2434) concept_rejection must stay 0.
export inline std::atomic<std::uint64_t> pass_pipeline_pure_wrap_total{0};
export inline std::atomic<std::uint64_t> pass_pipeline_concept_rejection_total{0};
export inline std::atomic<std::uint64_t> hot_pass_dod_mandatory_wired{1};
export inline std::atomic<std::uint64_t> pure_wrap_enforcement_wired{1};
// Issue #2434: hard HotPassDodCompliant for all pipeline stages (no unmarked).
export inline std::atomic<std::uint64_t> pass_pipeline_hard_dod_wired{1};
export inline std::atomic<std::uint64_t> pass_pipeline_production_pack_inventory_wired{1};

inline void note_define_dirty_mask_stats(const DefineDirtyMaskView& view) noexcept {
    const auto dirty = view.dirty_block_count();
    const auto total = view.total_block_count();
    define_dirty_blocks_seen_total.fetch_add(dirty, std::memory_order_relaxed);
    define_total_blocks_seen_total.fetch_add(total, std::memory_order_relaxed);
    if (total > 0) {
        const auto bp = (dirty * 10000ull) / total;
        dirty_block_relower_ratio_bp.store(bp, std::memory_order_relaxed);
    }
}

namespace pass_pipeline_detail {
    inline PipelineYieldHook g_pipeline_yield_hook = nullptr;
    // Issue #1322: execution context — fiber/render hot-path soft gate for run_one.
    inline thread_local int g_pipeline_hotpath_depth = 0;
    inline thread_local std::uint64_t g_pipeline_mutation_epoch = 0;
} // namespace pass_pipeline_detail

export void enter_pipeline_hotpath_context() noexcept {
    ++pass_pipeline_detail::g_pipeline_hotpath_depth;
}
export void exit_pipeline_hotpath_context() noexcept {
    if (pass_pipeline_detail::g_pipeline_hotpath_depth > 0)
        --pass_pipeline_detail::g_pipeline_hotpath_depth;
}
export [[nodiscard]] bool in_pipeline_hotpath_context() noexcept {
    return pass_pipeline_detail::g_pipeline_hotpath_depth > 0;
}
export void set_pipeline_mutation_epoch(std::uint64_t epoch) noexcept {
    pass_pipeline_detail::g_pipeline_mutation_epoch = epoch;
    pipeline_epoch_sync_total.fetch_add(1, std::memory_order_relaxed);
}
export [[nodiscard]] std::uint64_t pipeline_mutation_epoch() noexcept {
    return pass_pipeline_detail::g_pipeline_mutation_epoch;
}

export void set_pipeline_yield_hook(PipelineYieldHook hook) noexcept {
    pass_pipeline_detail::g_pipeline_yield_hook = hook;
}

export [[nodiscard]] PipelineYieldHook pipeline_yield_hook() noexcept {
    return pass_pipeline_detail::g_pipeline_yield_hook;
}

// SoAViewAwarePass / LegacyPass / RequiresSoAViewPass: concept_constraints (#1577).

// Issue #1517 / #1619 / #1918 / #2060 / #2258 / #2434: compile-time DOD compliance.
// - Passes with kRequireSoAView=true MUST be SoAViewAwarePass (static_assert).
// - Soft metrics always: SoA aware → concept_enforcement_hits;
//   Legacy / uses_soa_view()false → soa_view_pass_skipped + concept_rejection.
// - #1619/#1918: pack-level check_pipeline_dod_compliance at every pipeline entry.
// - #1918: HotPassDodCompliant (SoAViewAware || Legacy) is the production target.
// - #2060: kRequireDirtySoAEntry / DirtyAware+SoA hot stages must provide
//   DirtySoAEntryPass (run_on_dirty_blocks_only or Incremental+Dirty+SoA).
// - #2258: DirtyAwarePass / IncrementalPass MUST be HotPassDodCompliant.
// - #2434: EVERY pipeline stage must be HotPassDodCompliant (unmarked soft
//   skip removed). Prefer PureWrap (kPureWrap); explicit kLegacyPass only
//   with documented sunset. Production packs should keep concept_rejection=0
//   (all SoAViewAware with uses_soa_view()==true).
export template <typename P> consteval void check_pass_dod_compliance() {
    using T = std::remove_cvref_t<P>;
    // Issue #2434 AC1: no unmarked soft-skip stages on any production pack.
    static_assert(HotPassDodCompliant<T>,
                  "Pipeline stage must be HotPassDodCompliant: implement uses_soa_view() "
                  "(preferred + kPureWrap) or explicit static constexpr kLegacyPass=true "
                  "with a documented sunset issue (#2434). Soft Legacy skips on hot path "
                  "are no longer allowed without an explicit marker.");
    if constexpr (RequiresSoAViewPass<T>) {
        static_assert(SoAViewAwarePass<T>,
                      "Hot pass declared kRequireSoAView must implement uses_soa_view() "
                      "for zero-overhead DOD (#1517/#1619/#1918)");
        // Explicit LegacyPass + kRequireSoAView is contradictory.
        static_assert(!LegacyPass<T>,
                      "Pass cannot declare both kRequireSoAView and kLegacyPass (#1619/#1918)");
        static_assert(HotPassDodCompliant<T>,
                      "kRequireSoAView pass must be HotPassDodCompliant (#1918)");
    }
    // Issue #2258: incremental / dirty-aware production path — hard HotPass gate.
    if constexpr (DirtyAwarePass<T> || IncrementalPass<T>) {
        static_assert(HotPassDodCompliant<T>,
                      "Incremental/DirtyAware pass must be HotPassDodCompliant "
                      "(SoAViewAware or explicit LegacyPass) for zero-overhead "
                      "partial-relower (#2258)");
    }
    // Issue #2060: explicit dirty/SoA entry requirement.
    if constexpr (RequiresDirtySoAEntryPass<T>) {
        static_assert(DirtySoAEntryPass<T>,
                      "HotPassDodCompliant pass declared kRequireDirtySoAEntry must provide "
                      "run_on_dirty_blocks_only or Incremental+DirtyAware+SoAView (#2060)");
        static_assert(SoAViewAwarePass<T>,
                      "kRequireDirtySoAEntry pass must consume IR SoA columns (#2060)");
        static_assert(!LegacyPass<T>,
                      "Pass cannot declare both kRequireDirtySoAEntry and kLegacyPass (#2060)");
    }
    // Issue #2060: DirtyAware + SoAViewAware production stages must offer a
    // dirty/SoA entry (pipeline routes run_on_dirty_blocks_only / block peel).
    if constexpr (SoAViewAwarePass<T> && DirtyAwarePass<T> && !LegacyPass<T>) {
        static_assert(DirtySoAEntryPass<T>,
                      "SoAViewAware DirtyAware hot pass must provide DirtySoAEntryPass "
                      "(run_on_dirty_blocks_only or Incremental+Dirty+SoA) (#2060)");
    }
}

// Issue #1619: fold-expression pack enforcement at pipeline entry
// (run_pipeline / run_analysis_pipeline / incremental variants).
export template <typename... Passes> consteval void check_pipeline_dod_compliance() {
    (check_pass_dod_compliance<Passes>(), ...);
}

// Metric: pipeline stages that report SoAView awareness (#1241).
export inline std::atomic<std::uint64_t> passes_soa_view_aware_total{0};
// Issue #1517: concept enforcement + legacy skip + migration progress mirrors.
export inline std::atomic<std::uint64_t> concept_enforcement_hits_total{0};
export inline std::atomic<std::uint64_t> soa_view_pass_skipped_total{0};
export inline std::atomic<std::uint64_t> edsl_soa_migration_progress_total{0};

// Issue #1517 / #2258 / #2434: per-pass enforcement bookkeeping (shared by pipelines).
// Unmarked stages are compile-time rejected by check_pass_dod_compliance (#2434).
// concept_rejection only advances for explicit LegacyPass or uses_soa_view()==false.
export template <typename P> void note_pass_soa_enforcement(P& pass) noexcept {
    using T = std::remove_cvref_t<P>;
    check_pass_dod_compliance<T>();
    // Issue #2258 AC4 / #2434 AC3: pure Wrap counters.
    if constexpr (PureWrapPass<T>) {
        pass_pipeline_pure_wrap_total.fetch_add(1, std::memory_order_relaxed);
    }
    if constexpr (LegacyPass<T>) {
        // Explicit sunset Legacy only — production packs must not include these
        // if concept_rejection_total is required to stay 0 (#2434 AC2).
        pass_pipeline_concept_rejection_total.fetch_add(1, std::memory_order_relaxed);
        soa_view_pass_skipped_total.fetch_add(1, std::memory_order_relaxed);
        soa_view::record_soa_view_pass_skipped();
        (void)pass;
        return;
    }
    if constexpr (SoAViewAwarePass<T>) {
        if (pass.uses_soa_view()) {
            passes_soa_view_aware_total.fetch_add(1, std::memory_order_relaxed);
            concept_enforcement_hits_total.fetch_add(1, std::memory_order_relaxed);
            soa_view::record_concept_enforcement_hit();
            soa_view::record_edsl_soa_migration_progress(1);
            edsl_soa_migration_progress_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            soa_view_pass_skipped_total.fetch_add(1, std::memory_order_relaxed);
            soa_view::record_soa_view_pass_skipped();
            // uses_soa_view() false → concept rejection (migration debt).
            pass_pipeline_concept_rejection_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Forward declare run_one so run_pipeline's fold can resolve it
// at definition time (needed when external Pass types instantiate
// the template from another TU — two-phase lookup / modules).
// Contracts must match the out-of-line definition (C++26).
export template <Pass P>
bool run_one(aura::ir::IRModule& mod, P& pass) pre(&pass != nullptr)
    post(r : r == !pass.has_error());

// ── run_pipeline — fold over passes with short-circuit ──────────
//
// Issue #381: added a contract on the parameter pack. C++26
// contracts (enabled via -fcontracts in the build) surface
// misuse in debug builds — a zero-pass pipeline is almost
// always a bug (the caller probably meant to add at least one
// pass). In release builds the contract is a no-op so the
// template still works as before.
//
// Issue #1517: SoAView DOD enforcement at every pipeline entry —
// compile-time for kRequireSoAView passes; soft metrics for all.
export template <Pass... Passes>
bool run_pipeline(aura::ir::IRModule& mod, Passes&... passes) pre(sizeof...(Passes) > 0) {
    aura::core::cpp26::record_hotpath_invariant_hit();
    // Issue #625: bump the pass-pipeline-runs counter once per
    // full invocation (NOT per-pass). Pairs with the dirty-block
    // short-circuit counters from #494/#606 so the Agent can see
    // how often the full pipeline runs vs how often the dirty
    // short-circuit short-circuits each pass. Bumped unconditionally
    // here (NOT gated on dirty awareness) so it captures the
    // whole-pipeline-run rate including compact / pure-run cases.
    pass_pipeline_runs_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #1241 / #1517 / #1619: SoAView concept pack enforcement.
    // static_assert via check_pipeline_dod_compliance for kRequireSoAView.
    check_pipeline_dod_compliance<Passes...>();
    (note_pass_soa_enforcement(passes), ...);
    return (run_one(mod, passes) && ...);
}

// ── run_one — execute a single pass, return true if no error ────
//
// Issue #381: added contracts. `pre` guards against calling
// `run_one` with no passes, and `post` documents the
// "no error → return true" invariant. The post is informational
// (the return value is observable, so callers can already
// verify it); the pre is the load-bearing guard.
// Issue #983: post-condition was vacuous (`|| true`). Real contract:
// return value equals !pass.has_error() after run.
export template <Pass P>
bool run_one(aura::ir::IRModule& mod, P& pass) pre(&pass != nullptr)
    post(r : r == !pass.has_error()) {
    if (pass_pipeline_detail::g_pipeline_yield_hook &&
        pass_pipeline_detail::g_pipeline_yield_hook()) {
        pipeline_yield_count.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1322 / #2822: sync JITFriendlyPass epoch hint.
    // Prior: if (epoch != 0) only — silently skipped when callers never
    // wired set_pipeline_mutation_epoch (TLS default 0), so JIT-friendly
    // passes kept pipeline_epoch_hint()==0 and pipeline_epoch_sync_total
    // undercounted. #2822 auto-wires from process mutation epoch and
    // floors at kPipelineEpochBaseFloor so sync always runs.
    if constexpr (requires(P& p) {
                      p.set_pipeline_epoch(std::uint64_t{});
                      { p.pipeline_epoch_hint() } -> std::convertible_to<std::uint64_t>;
                  }) {
        auto epoch = pass_pipeline_detail::g_pipeline_mutation_epoch;
        if (epoch == 0) {
            // Issue #2822: TLS unset — auto-init from process-global mutation epoch.
            pipeline_epoch_unset_runs_total.fetch_add(1, std::memory_order_relaxed);
            const auto proc = aura::core::current_mutation_epoch();
            if (proc != 0) {
                epoch = proc;
                // Cache real process epoch so later passes in this pipeline
                // share one resolve; set_pipeline_mutation_epoch still overrides.
                pass_pipeline_detail::g_pipeline_mutation_epoch = proc;
            } else {
                epoch = kPipelineEpochBaseFloor;
            }
        }
        pass.set_pipeline_epoch(epoch);
        pipeline_epoch_sync_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1322: under render/JIT hot path, count light-analysis samples
    // (full pass still runs in Phase 1; lighter skip policy is follow-up).
    if (pass_pipeline_detail::g_pipeline_hotpath_depth > 0)
        pipeline_hotpath_light_analysis_total.fetch_add(1, std::memory_order_relaxed);
    pass.run(mod);
    // Issue #1575: DirtyAwarePass → auto-flush cascade roots into
    // dirty_propagation::g_global_dirty when a DepGraph is registered
    // via dirty::set_pipeline_dep_graph + note_pipeline_cascade_root.
    // Uses requires-expression (not DirtyAwarePass<> by name) because
    // this template is defined above the DirtyAwarePass concept.
    if constexpr (requires(const P& p, std::uint32_t block_id) {
                      { p.is_block_dirty(block_id) } -> std::convertible_to<bool>;
                  }) {
        (void)aura::compiler::dirty::flush_pipeline_cascade_roots();
    }
    return !pass.has_error();
}

// PureAnalysisPass / IncrementalPass / DirtyAwarePass /
// InstructionDirtyAwarePass / ShapeStableAwarePass / JITFriendlyPass:
// defined in aura.core.concept_constraints (#1577).

// Metric: instruction-level dirty skips (#1197).
export inline std::atomic<std::uint64_t> passes_skipped_instruction_dirty{0};
// Issue #2133: process-wide clean-instr skips during DirtyAware peel
// (mirrors CompilerMetrics::instr_level_pass_skipped_clean for pure tests).
export inline std::atomic<std::uint64_t> instr_level_pass_skipped_clean_total{0};
export inline std::atomic<std::uint64_t> instr_level_pass_runs_total{0};

// ── Issue #744: ShapeStable probe hooks (runtime; concept is centralized) ──
export using FnShapeStableProbeFn = bool (*)(std::string_view fn_name) noexcept;
export inline FnShapeStableProbeFn g_fn_shape_stable_probe = nullptr;

export void set_fn_shape_stable_probe(FnShapeStableProbeFn probe) noexcept {
    g_fn_shape_stable_probe = probe;
}

// ── Issue #381: run_incremental_pipeline — fold over per-function / per-block work ──────────
//
// Mirrors `run_pipeline` but constrained to `IncrementalPass`.
// For each pass, calls `run_function` per function in the
// module, short-circuiting on first `has_error()`. Useful
// for incremental compilation: the caller can pre-compute
// the dirty-function set and only call this template for
// functions that need re-running.
//
// Note: this template assumes the pass's per-function
// `run_function` is semantically equivalent to
// `run(IRModule&)` restricted to that one function. Pass
// authors documenting their class as IncrementalPass are
// committing to that equivalence.
export template <IncrementalPass P>
bool run_incremental_pipeline(aura::ir::IRModule& mod, P& pass) {
    // Issue #1517 / #1619: enforce DOD compliance at incremental entry.
    check_pipeline_dod_compliance<P>();
    note_pass_soa_enforcement(pass);
    for (auto& func : mod.functions) {
        pass.run(func);
        if (pass.has_error())
            return false;
    }
    return true;
}

// Issue #686: incremental pipeline with DirtyAware short-circuit —
// skip functions whose blocks are all clean when the pass exposes
// is_block_dirty().
// Issue #1197: when InstructionDirtyAwarePass, also count clean
// instruction slots (observability for instruction-level short-circuit).
// Issue #1574: optional define_cache (IRCacheEntry dirty bitmasks via
// DefineDirtyMaskView). When non-null and fully clean, the entire pass
// is skipped (optimization_passes_skipped_by_define_dirty++). When
// partially dirty, block dirtiness prefers the define mask over the
// pass's own is_block_dirty, and set_block_dirty_fn is installed when
// the pass supports it so fold/propagate only touch dirty blocks.
// Issue #2060: prefer run_on_dirty_blocks_only / dirty block peel so
// clean regions never pay residual full-function walks under sparse
// AI re-lower. consteval DirtySoAEntryPass enforced for DirtyAware+SoA.
export template <IncrementalPass P>
    requires DirtyAwarePass<P>
bool run_incremental_dirty_pipeline(aura::ir::IRModule& mod, P& pass,
                                    const DefineDirtyMaskView* define_cache = nullptr) {
    // Issue #1517 / #1619 / #2060 / #2258: DOD + HotPass + dirty/SoA entry.
    static_assert(HotPassDodCompliant<P>,
                  "run_incremental_dirty_pipeline requires HotPassDodCompliant "
                  "(SoAViewAware or LegacyPass) (#2258)");
    check_pipeline_dod_compliance<P>();
    note_pass_soa_enforcement(pass);
    // #2060: DirtyAware + SoA stages must offer DirtySoAEntry (concept check).
    if constexpr (SoAViewAwarePass<P>) {
        static_assert(DirtySoAEntryPass<P>,
                      "run_incremental_dirty_pipeline requires DirtySoAEntryPass for "
                      "SoAViewAware stages (#2060)");
    }

    // Issue #2109 / #2190: consult storm-aware partial gate at DirtyAware
    // entry so Agents can correlate partial vs full with pass skip metrics
    // and StormLevel Global force-full.
    if (define_cache && define_cache->block_dirty_per_func) {
        std::size_t dirty_n = 0;
        for (const auto& fb : *define_cache->block_dirty_per_func)
            for (auto b : fb)
                if (b)
                    ++dirty_n;
        (void)should_partial_relower_storm_aware(dirty_n); // #2190 Global gate
    }

    // AC3 (#1574): early-skip whole pass when define-level mask is clean.
    if (define_cache && define_cache->block_dirty_per_func && !define_cache->any()) {
        note_define_dirty_mask_stats(*define_cache);
        optimization_passes_skipped_by_define_dirty.fetch_add(1, std::memory_order_relaxed);
        passes_skipped_dirty_pipeline.fetch_add(1, std::memory_order_relaxed);
        pipeline_dirty_short_circuit_total.fetch_add(1, std::memory_order_relaxed);
        aura::core::cpp26::record_hotpath_invariant_hit();
        return true;
    }
    if (define_cache && define_cache->block_dirty_per_func)
        note_define_dirty_mask_stats(*define_cache);

    for (std::size_t fi = 0; fi < mod.functions.size(); ++fi) {
        auto& func = mod.functions[fi];
        const bool fn_shape_stable =
            g_fn_shape_stable_probe != nullptr && g_fn_shape_stable_probe(func.name);

        // Issue #1574: wire define mask into pass when it supports
        // set_block_dirty_fn (ConstantFoldingWrap / EscapeAnalysis / …).
        if constexpr (requires(P& p, std::function<bool(std::uint32_t)> f) {
                          p.set_block_dirty_fn(std::move(f));
                      }) {
            if (define_cache && define_cache->block_dirty_per_func) {
                const auto* cache = define_cache;
                const std::size_t func_idx = fi;
                pass.set_block_dirty_fn([cache, func_idx](std::uint32_t block_id) -> bool {
                    return cache->is_block_dirty(func_idx, block_id);
                });
            }
        }
        // Issue #2133: wire instruction dirty from ImpactScope / define cache
        // so DirtyAware passes can peel clean slots inside dirty blocks.
        if constexpr (requires(P& p, std::function<bool(std::uint32_t, std::uint32_t)> f) {
                          p.set_instruction_dirty_fn(std::move(f));
                      }) {
            if (define_cache && define_cache->instruction_dirty_per_func) {
                const auto* cache = define_cache;
                const std::size_t func_idx = fi;
                pass.set_instruction_dirty_fn(
                    [cache, func_idx](std::uint32_t block_id, std::uint32_t inst_id) -> bool {
                        return cache->is_instruction_dirty(func_idx, block_id, inst_id);
                    });
                instr_level_pass_runs_total.fetch_add(1, std::memory_order_relaxed);
            }
        }

        bool any_dirty = false;
        std::uint64_t dirty_blocks = 0;
        std::uint64_t clean_blocks = 0;
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            const auto bid = static_cast<std::uint32_t>(bi);
            // Prefer define-level mask when present; else pass probe.
            const bool block_dirty = (define_cache && define_cache->block_dirty_per_func)
                                         ? define_cache->is_block_dirty(fi, bid)
                                         : pass.is_block_dirty(bid);
            if (block_dirty) {
                any_dirty = true;
                ++dirty_blocks;
                // Phase 1 instruction probe: if the pass is
                // InstructionDirtyAwarePass, walk inst dirty bits for metrics.
                if constexpr (InstructionDirtyAwarePass<P>) {
                    // Issue #2109 / #2133: walk all instructions in the dirty
                    // block. Clean slots bump skip metrics so Agents can prove
                    // instruction-level partial wins (instr ImpactScope peel).
                    const std::uint32_t ninst =
                        bi < func.blocks.size()
                            ? static_cast<std::uint32_t>(func.blocks[bi].instructions.size())
                            : 8u;
                    for (std::uint32_t ii = 0; ii < ninst; ++ii) {
                        bool inst_dirty = true;
                        if (define_cache && define_cache->instruction_dirty_per_func)
                            inst_dirty = define_cache->is_instruction_dirty(fi, bid, ii);
                        else
                            inst_dirty = pass.is_instruction_dirty(bid, ii);
                        if (!inst_dirty) {
                            passes_skipped_instruction_dirty.fetch_add(1,
                                                                       std::memory_order_relaxed);
                            instr_level_pass_skipped_clean_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                    }
                }
            } else {
                ++clean_blocks;
                if (fn_shape_stable)
                    passes_skipped_shape_stable_blocks.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!any_dirty) {
            passes_skipped_dirty_pipeline.fetch_add(1, std::memory_order_relaxed);
            // Issue #1322: unified dirty short-circuit counter for Agent dashboards.
            pipeline_dirty_short_circuit_total.fetch_add(1, std::memory_order_relaxed);
            if (define_cache && define_cache->block_dirty_per_func)
                optimization_passes_skipped_by_define_dirty.fetch_add(1, std::memory_order_relaxed);
            if (fn_shape_stable)
                passes_skipped_shape_stable_blocks.fetch_add(
                    static_cast<std::uint64_t>(func.blocks.size()), std::memory_order_relaxed);
            if (clean_blocks > 0)
                dirty_only_blocks_skipped_total.fetch_add(clean_blocks, std::memory_order_relaxed);
            aura::core::cpp26::record_hotpath_invariant_hit();
            continue;
        }
        // Issue #2060: prefer explicit dirty-only / SoA-columnar entry.
        if constexpr (requires(P& p, aura::ir::IRFunction& f) { p.run_on_dirty_blocks_only(f); }) {
            dirty_only_entry_hits_total.fetch_add(1, std::memory_order_relaxed);
            dirty_only_blocks_run_total.fetch_add(dirty_blocks, std::memory_order_relaxed);
            if (clean_blocks > 0)
                dirty_only_blocks_skipped_total.fetch_add(clean_blocks, std::memory_order_relaxed);
            pass.run_on_dirty_blocks_only(func);
        } else {
            // DirtyAware + set_block_dirty_fn: pass peels clean blocks internally.
            // Still record #2060 skip metrics for Agent sparse-dirty dashboards.
            dirty_only_entry_hits_total.fetch_add(1, std::memory_order_relaxed);
            dirty_only_blocks_run_total.fetch_add(dirty_blocks, std::memory_order_relaxed);
            if (clean_blocks > 0)
                dirty_only_blocks_skipped_total.fetch_add(clean_blocks, std::memory_order_relaxed);
            pass.run(func);
        }
        if (pass.has_error())
            return false;
    }
    return true;
}

// Issue #2060: single-pass dirty-only entry. When define mask is fully
// clean, skip the entire pass; otherwise route through
// run_incremental_dirty_pipeline (DirtySoAEntry / block peel).
export template <IncrementalPass P>
    requires DirtyAwarePass<P>
bool run_one_dirty(aura::ir::IRModule& mod, P& pass,
                   const DefineDirtyMaskView* define_cache = nullptr) {
    run_one_dirty_calls_total.fetch_add(1, std::memory_order_relaxed);
    check_pipeline_dod_compliance<P>();
    if (define_cache && define_cache->block_dirty_per_func && !define_cache->any()) {
        note_define_dirty_mask_stats(*define_cache);
        optimization_passes_skipped_by_define_dirty.fetch_add(1, std::memory_order_relaxed);
        passes_skipped_dirty_pipeline.fetch_add(1, std::memory_order_relaxed);
        pipeline_dirty_short_circuit_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return run_incremental_dirty_pipeline(mod, pass, define_cache);
}

// ── Issue #2143: SoaDirtyAwarePass + run_dirty_pipeline (IRModuleV2) ──
//
// Historical DirtyAwarePass (concept_constraints) is the AoS block-dirty
// hook: bool is_block_dirty(block_id). Issue #2143 binds the SoA module
// surface sketched as "DirtyAwarePass + run_dirty(IRModuleV2&)" under a
// distinct name so existing DirtyAwarePass static_asserts stay green.
//
// Requirements (SoaDirtyAwarePass):
//   - void run_dirty(IRModuleV2&)  — dirty-only walk of SoA columns
//
// Preferred implementation:
//   for_each_block(..., dirty_only=true) / walk_soa_function_hotpath
// Migration off to_aos_view:
//   DirtyAware kinds should grow run_dirty and leave SoAtoAoSBridgePass
//   as a transitional bridge for legacy AoS-only stages (Phase 2 drops
//   hot-path to_aos for these kinds).
export template <typename P>
concept SoaDirtyAwarePass = requires(P& p, IRModuleV2& m) {
    { p.run_dirty(m) } -> std::same_as<void>;
};

// Fold-expression pure pipeline over SoaDirtyAwarePass stages.
// Short-circuits on has_error() when the pass exposes it.
// Metrics: run_dirty_pipeline_*_total (invocations / pass runs /
// clean_skips / dirty_runs from for_each_block helpers inside passes).
export template <typename... Passes>
    requires(SoaDirtyAwarePass<std::remove_cvref_t<Passes>> && ...)
bool run_dirty_pipeline(IRModuleV2& mod, Passes&... passes) {
    run_dirty_pipeline_invocations_total.fetch_add(1, std::memory_order_relaxed);
    bool ok = true;
    auto run_one_soa = [&](auto& pass) {
        if (!ok)
            return;
        // Capture ir_soa_migration dirty counters around the pass so
        // pipeline-level clean_skips / dirty_runs stay in sync even when
        // the pass records via record_dirty_block_skip/run.
        const auto mig_skips0 =
            ir_soa_migration::dirty_block_driven_skips.load(std::memory_order_relaxed);
        const auto mig_runs0 =
            ir_soa_migration::dirty_block_driven_runs.load(std::memory_order_relaxed);
        pass.run_dirty(mod);
        run_dirty_pipeline_pass_runs_total.fetch_add(1, std::memory_order_relaxed);
        const auto mig_skips1 =
            ir_soa_migration::dirty_block_driven_skips.load(std::memory_order_relaxed);
        const auto mig_runs1 =
            ir_soa_migration::dirty_block_driven_runs.load(std::memory_order_relaxed);
        if (mig_skips1 > mig_skips0)
            run_dirty_pipeline_clean_skips_total.fetch_add(mig_skips1 - mig_skips0,
                                                           std::memory_order_relaxed);
        if (mig_runs1 > mig_runs0)
            run_dirty_pipeline_dirty_runs_total.fetch_add(mig_runs1 - mig_runs0,
                                                          std::memory_order_relaxed);
        if constexpr (requires { pass.has_error(); }) {
            if (pass.has_error())
                ok = false;
        }
    };
    (run_one_soa(passes), ...);
    return ok;
}

// ── Issue #381: static_asserts documenting the new concepts ────
//
// These are documentation-as-tests: the static_asserts would
// fail at compile time if a refactor accidentally broke the
// concept satisfaction of a documented wrap. They're the
// canary for the "concepts work as advertised" promise.
//
// PureAnalysisPass satisfaction: requires const run().
// Issue #1204 Phase 1: ComputeKindWrap / ArityWrap already use
// const run() + mutable accumulators (#606). static_asserts live
// after the class definitions (see below).

// IncrementalPass satisfaction: requires run_function +
// run_block. Issue #606: ConstantFoldingWrap now exposes
// run_function + run_block aliases over its existing
// fold_function / fold_block implementations. The legacy
// names remain available for the 7 service.ixx + main.cpp
// call sites — no churn there.
//
// NOTE: this static_assert must live AFTER the
// ConstantFoldingWrap class definition (it's documented
// below next to its definition).

// DirtyAwarePass satisfaction: requires is_block_dirty.
// static_assert moved below ConstantFoldingWrap definition.

} // namespace aura::compiler
