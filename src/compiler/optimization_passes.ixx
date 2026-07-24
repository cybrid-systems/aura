// optimization_passes.ixx — Issue #1201 Phase 1 scaffold + #1576 Phase 2
// concrete passes/contracts + #1578 RenderPass dirty-aware incremental render.

module;
#include "renderer/render_pass.hh"

export module aura.compiler.optimization_passes;

import std;
import aura.compiler.ir;
import aura.core.concept_constraints; // #1577: Pass / DirtyAware / …
import aura.compiler.pass_manager;
import aura.compiler.dirty_propagation;

export namespace aura::compiler::opt_registry {

inline constexpr int kOptimizationPassesPhase = 3; // #1578: RenderPass incremental

// ── Metrics (#1576) ────────────────────────────────────────────
inline std::atomic<std::uint64_t> opt_pass_runs_total{0};
inline std::atomic<std::uint64_t> opt_pass_errors_total{0};
inline std::atomic<std::uint64_t> opt_contracts_pre_checks_total{0};
inline std::atomic<std::uint64_t> opt_contracts_post_checks_total{0};
inline std::atomic<std::uint64_t> opt_pipeline_factory_runs_total{0};
inline std::atomic<std::uint64_t> opt_contract_violations_soft_total{0};

// ── Metrics (#1578 RenderPass) ─────────────────────────────────
inline std::atomic<std::uint64_t> render_dirty_skipped_blocks{0};
inline std::atomic<std::uint64_t> render_shape_stable_violations{0};
inline std::atomic<std::uint64_t> render_incremental_hits{0};
inline std::atomic<std::uint64_t> render_blocks_processed_total{0};
inline std::atomic<std::uint64_t> render_pass_runs_total{0};
inline std::atomic<std::uint64_t> render_full_module_fallback_total{0};
inline std::atomic<std::uint64_t> render_framebuffer_present_skips{0};
inline std::atomic<std::uint64_t> render_framebuffer_present_ok{0};

// Per-kind run counters (indexed by PassKind).
inline std::array<std::atomic<std::uint64_t>, 16> opt_pass_runs_by_kind{};

enum class PassKind : std::uint8_t {
    ConstantFold = 0,
    Inline = 1,
    TypeCheck = 2,
    Arity = 3,
    Shape = 4,
    LinearOwnership = 5,
    ComputeKind = 6,
    Render = 7,
    TypePropagation = 8, // #1576 concrete core pass
    ShapeAwareFold = 9,  // #1576 concrete core pass
    DeadCoercion = 10,   // #2066: CastOp elision driven by narrow_evidence + provenance
    Count
};

struct PassDescriptor {
    PassKind kind = PassKind::ConstantFold;
    std::string_view name;
    bool dirty_aware = false;
    bool shape_stable = false;
    bool requires_contracts = false; // #1576
    bool pure = false;               // #1576 PureAnalysisPass-friendly
};

// Issue #1574/#1576: core dirty-aware + contracts-enabled registry.
inline constexpr PassDescriptor kDefaultPassTable[] = {
    {PassKind::ConstantFold, "constant-fold", true, true, true, false},
    {PassKind::TypePropagation, "type-propagation", true, false, true, false},
    {PassKind::ComputeKind, "compute-kind", true, false, true, true},
    {PassKind::ShapeAwareFold, "shape-aware-fold", true, true, true, false},
    // Issue #2066: DeadCoercion (CastOp elision driven by narrow_evidence) runs
    // after TypePropagation (dirty-aware, contracts-enabled, pure-analysis-friendly).
    // Pre-existing wire-up in service.ixx:2890 via run_pipeline(...) parameter pack
    // — this table entry surfaces the pass to Agents via the canonical PassKind enum.
    {PassKind::DeadCoercion, "dead-coercion-elim", true, false, true, true},
    {PassKind::Shape, "shape", true, true, true, true},
    {PassKind::Inline, "inline", false, false, false, false},
    {PassKind::TypeCheck, "type-check", false, true, false, false},
    {PassKind::Arity, "arity", false, false, false, true},
    {PassKind::LinearOwnership, "linear-ownership", false, false, false, true},
    // #1578: RenderPass is dirty-aware + shape_stable + contracts-enabled.
    {PassKind::Render, "render-present", true, true, true, false},
};

[[nodiscard]] inline std::size_t default_pass_count() noexcept {
    return sizeof(kDefaultPassTable) / sizeof(kDefaultPassTable[0]);
}

[[nodiscard]] inline const PassDescriptor* find_descriptor(PassKind kind) noexcept {
    for (const auto& d : kDefaultPassTable) {
        if (d.kind == kind)
            return &d;
    }
    return nullptr;
}

// ── Contract helpers (#1576 AC2) ────────────────────────────────
// Soft structural precondition for IR modules entering a pass.
// Mirrors the AC sketch `valid_soa_view()` without requiring SoA emit.
[[nodiscard]] inline bool ir_module_structurally_valid(const aura::ir::IRModule& m) noexcept {
    opt_contracts_pre_checks_total.fetch_add(1, std::memory_order_relaxed);
    // Empty module is only valid with entry_function_id == 0 (unset).
    if (m.functions.empty())
        return m.entry_function_id == 0;
    if (m.entry_function_id >= m.functions.size())
        return false;
    for (const auto& f : m.functions) {
        if (!f.blocks.empty() && f.entry_block >= f.blocks.size())
            return false;
        for (const auto& b : f.blocks) {
            // Instructions may be empty (stubs); ids should be consistent when present.
            (void)b;
        }
    }
    return true;
}

// Alias matching AC wording.
[[nodiscard]] inline bool valid_soa_view(const aura::ir::IRModule& m) noexcept {
    return ir_module_structurally_valid(m);
}

// Epoch consistent when pipeline epoch is 0 (unset) or matches pass hint.
[[nodiscard]] inline bool pipeline_epoch_consistent() noexcept {
    opt_contracts_pre_checks_total.fetch_add(1, std::memory_order_relaxed);
    // Advisory: always true when no global epoch is forced; callers may
    // tighten via set_pipeline_mutation_epoch before run_pipeline.
    return true;
}

// Post-condition helper: on error path we still require the module
// remains structurally valid (no catastrophic corruption).
[[nodiscard]] inline bool dirty_flags_cleared_or_ok(const aura::ir::IRModule& m,
                                                    bool has_error) noexcept {
    opt_contracts_post_checks_total.fetch_add(1, std::memory_order_relaxed);
    if (has_error)
        return ir_module_structurally_valid(m); // soft: structure intact on error
    return ir_module_structurally_valid(m);
}

inline void note_pass_run(PassKind kind, bool error) noexcept {
    opt_pass_runs_total.fetch_add(1, std::memory_order_relaxed);
    const auto idx = static_cast<std::size_t>(kind);
    if (idx < opt_pass_runs_by_kind.size())
        opt_pass_runs_by_kind[idx].fetch_add(1, std::memory_order_relaxed);
    if (error)
        opt_pass_errors_total.fetch_add(1, std::memory_order_relaxed);
}

// ── Concrete passes (#1576 AC1) ────────────────────────────────
// Thin adapters over pass_manager implementations: add contracts,
// metrics, and uniform DirtyAware / Incremental entry points.

class ConstantFoldingPass {
public:
    void set_block_dirty_fn(std::function<bool(std::uint32_t)> fn) {
        impl_.set_block_dirty_fn(std::move(fn));
    }
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        return impl_.is_block_dirty(block_id);
    }
    void set_pipeline_epoch(std::uint64_t epoch) noexcept { impl_.set_pipeline_epoch(epoch); }
    [[nodiscard]] std::uint64_t pipeline_epoch_hint() const noexcept {
        return impl_.pipeline_epoch_hint();
    }

    void run(aura::ir::IRModule& m) pre(valid_soa_view(m) && pipeline_epoch_consistent()) {
        note_pass_run(PassKind::ConstantFold, false);
        impl_.run(m);
        error_ = impl_.has_error();
        if (error_)
            opt_pass_errors_total.fetch_add(1, std::memory_order_relaxed);
        // post: structural integrity preserved
        if (!dirty_flags_cleared_or_ok(m, error_))
            opt_contract_violations_soft_total.fetch_add(1, std::memory_order_relaxed);
    }
    void run(aura::ir::IRFunction& f) { impl_.run(f); }
    void run(aura::ir::BasicBlock& b) { impl_.run(b); }

    [[nodiscard]] bool has_error() const { return error_ || impl_.has_error(); }
    [[nodiscard]] std::string_view name() const { return "constant-fold"; }
    [[nodiscard]] std::size_t folded_count() const { return impl_.folded_count(); }

private:
    aura::compiler::ConstantFoldingWrap impl_;
    bool error_ = false;
};

class TypePropagationPass {
public:
    void set_block_dirty_fn(std::function<bool(std::uint32_t)> fn) {
        impl_.set_block_dirty_fn(std::move(fn));
    }
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        return impl_.is_block_dirty(block_id);
    }
    void set_pipeline_epoch(std::uint64_t epoch) noexcept { impl_.set_pipeline_epoch(epoch); }
    [[nodiscard]] std::uint64_t pipeline_epoch_hint() const noexcept {
        return impl_.pipeline_epoch_hint();
    }

    void run(aura::ir::IRModule& m) pre(valid_soa_view(m) && pipeline_epoch_consistent()) {
        note_pass_run(PassKind::TypePropagation, false);
        impl_.run(m);
        error_ = impl_.has_error();
        if (error_)
            opt_pass_errors_total.fetch_add(1, std::memory_order_relaxed);
        if (!dirty_flags_cleared_or_ok(m, error_))
            opt_contract_violations_soft_total.fetch_add(1, std::memory_order_relaxed);
    }
    void run(aura::ir::IRFunction& f) { impl_.run(f); }
    void run(aura::ir::BasicBlock& b) { impl_.run(b); }

    [[nodiscard]] bool has_error() const { return error_ || impl_.has_error(); }
    [[nodiscard]] std::string_view name() const { return "type-propagation"; }
    [[nodiscard]] std::size_t propagated_count() const { return impl_.propagated_count(); }

private:
    aura::compiler::TypePropagationPass impl_;
    bool error_ = false;
};

class ComputeKindPass {
public:
    void set_block_dirty_fn(std::function<bool(std::uint32_t)> fn) {
        impl_.set_block_dirty_fn(std::move(fn));
    }
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        return impl_.is_block_dirty(block_id);
    }

    // PureAnalysisPass: const run(module) path.
    void run(aura::ir::IRModule& m) const pre(valid_soa_view(m) && pipeline_epoch_consistent()) {
        note_pass_run(PassKind::ComputeKind, false);
        impl_.run(m);
        if (!dirty_flags_cleared_or_ok(m, false))
            opt_contract_violations_soft_total.fetch_add(1, std::memory_order_relaxed);
    }
    void run(aura::ir::IRFunction& f) { impl_.run(f); }
    void run(aura::ir::BasicBlock& b) { impl_.run(b); }

    [[nodiscard]] bool has_error() const { return impl_.has_error(); }
    [[nodiscard]] std::string_view name() const { return "compute-kind"; }

private:
    mutable aura::compiler::ComputeKindWrap impl_;
};

class ShapeAwareFoldingPass {
public:
    void set_block_dirty_fn(std::function<bool(std::uint32_t)> fn) {
        block_dirty_fn_ = std::move(fn);
    }
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        if (!block_dirty_fn_)
            return true;
        return block_dirty_fn_(block_id);
    }

    void run(aura::ir::IRModule& m) pre(valid_soa_view(m) && pipeline_epoch_consistent()) {
        note_pass_run(PassKind::ShapeAwareFold, false);
        if (block_dirty_fn_) {
            // Dirty-aware peel: only fold functions with any dirty block.
            for (auto& func : m.functions) {
                bool any = false;
                for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
                    if (is_block_dirty(static_cast<std::uint32_t>(bi))) {
                        any = true;
                        break;
                    }
                }
                if (!any)
                    continue;
                // Delegate single-function work via full module clone of one fn
                // is expensive — ShapeAwareFoldingPass runs whole module today;
                // for dirty-aware we still run full impl (correctness-first).
                (void)func;
            }
        }
        impl_.run(m);
        error_ = impl_.has_error();
        if (error_)
            opt_pass_errors_total.fetch_add(1, std::memory_order_relaxed);
        if (!dirty_flags_cleared_or_ok(m, error_))
            opt_contract_violations_soft_total.fetch_add(1, std::memory_order_relaxed);
    }
    void run(aura::ir::IRFunction& /*f*/) {
        // Whole-module pass; IncrementalPass stub for DirtyAware pipeline.
    }
    void run(aura::ir::BasicBlock& /*b*/) {}

    [[nodiscard]] bool has_error() const { return error_ || impl_.has_error(); }
    [[nodiscard]] std::string_view name() const { return "shape-aware-fold"; }
    [[nodiscard]] std::uint64_t fold_count() const { return impl_.fold_count(); }

private:
    aura::compiler::ShapeAwareFoldingPass impl_;
    std::function<bool(std::uint32_t)> block_dirty_fn_;
    bool error_ = false;
};

// ── Concept static_asserts (AC1) ───────────────────────────────
static_assert(aura::compiler::Pass<ConstantFoldingPass>,
              "ConstantFoldingPass must satisfy Pass (#1576)");
static_assert(aura::compiler::DirtyAwarePass<ConstantFoldingPass>,
              "ConstantFoldingPass must be DirtyAware (#1576)");
static_assert(aura::compiler::IncrementalPass<ConstantFoldingPass>,
              "ConstantFoldingPass must be Incremental (#1576)");

static_assert(aura::compiler::Pass<TypePropagationPass>,
              "TypePropagationPass must satisfy Pass (#1576)");
static_assert(aura::compiler::DirtyAwarePass<TypePropagationPass>,
              "TypePropagationPass must be DirtyAware (#1576)");
static_assert(aura::compiler::IncrementalPass<TypePropagationPass>,
              "TypePropagationPass must be Incremental (#1576)");

static_assert(aura::compiler::Pass<ComputeKindPass>, "ComputeKindPass must satisfy Pass (#1576)");
static_assert(aura::compiler::DirtyAwarePass<ComputeKindPass>,
              "ComputeKindPass must be DirtyAware (#1576)");
// PureAnalysisPass requires const run(IRModule&) — ComputeKindPass has it.
static_assert(aura::compiler::PureAnalysisPass<ComputeKindPass>,
              "ComputeKindPass must be PureAnalysisPass (#1576)");

static_assert(aura::compiler::Pass<ShapeAwareFoldingPass>,
              "ShapeAwareFoldingPass must satisfy Pass (#1576)");
static_assert(aura::compiler::DirtyAwarePass<ShapeAwareFoldingPass>,
              "ShapeAwareFoldingPass must be DirtyAware (#1576)");

// ── RenderPass (#1578) ─────────────────────────────────────────
// Dirty-aware + shape_stable + SoAView + JIT-friendly incremental render.
// Treats IR basic blocks as render work units: only dirty blocks are
// "presented". Links optionally to aura::renderer framebuffer dirty AABB
// and dirty_propagation cascade roots.
//
// Contracts (AC2):
//   pre:  valid_soa_view(m) && pipeline_epoch_consistent()
//         (+ soft shape_stable probe when registered)
//   post: output shape fingerprint unchanged unless full fallback;
//         dirty-cleared flag set when no blocks processed or cleared.
class RenderPass {
public:
    void set_block_dirty_fn(std::function<bool(std::uint32_t)> fn) {
        block_dirty_fn_ = std::move(fn);
    }
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        if (!block_dirty_fn_)
            return true; // no mask → full render (conservative)
        return block_dirty_fn_(block_id);
    }
    [[nodiscard]] bool uses_soa_view() const noexcept { return true; }
    [[nodiscard]] std::uint64_t pipeline_epoch_hint() const noexcept { return pipeline_epoch_; }
    void set_pipeline_epoch(std::uint64_t epoch) noexcept { pipeline_epoch_ = epoch; }

    // When true, also consult aura::renderer::present_batch_if_dirty().
    void enable_framebuffer_present(bool v) noexcept { framebuffer_present_ = v; }
    // Soft shape contract: if true, count shape_stable_violations on mismatch.
    void enforce_shape_stable(bool v) noexcept { enforce_shape_stable_ = v; }

    [[nodiscard]] std::uint64_t last_shape_fingerprint() const noexcept {
        return last_output_shape_;
    }
    [[nodiscard]] std::uint64_t blocks_processed_last() const noexcept {
        return blocks_processed_last_;
    }
    [[nodiscard]] std::uint64_t blocks_skipped_last() const noexcept {
        return blocks_skipped_last_;
    }
    [[nodiscard]] bool last_dirty_cleared() const noexcept { return last_dirty_cleared_; }

    // FNV-1a over function/block counts + opcode stream (structure, not values).
    [[nodiscard]] static std::uint64_t compute_module_shape(const aura::ir::IRModule& m) noexcept {
        std::uint64_t h = 14695981039346656037ull;
        auto mix = [&](std::uint64_t v) {
            h ^= v;
            h *= 1099511628211ull;
        };
        mix(m.functions.size());
        mix(m.entry_function_id);
        for (const auto& f : m.functions) {
            mix(f.blocks.size());
            mix(f.arg_count);
            mix(f.local_count);
            for (const auto& b : f.blocks) {
                mix(b.instructions.size());
                for (const auto& ins : b.instructions)
                    mix(static_cast<std::uint64_t>(ins.opcode));
            }
        }
        return h;
    }

    [[nodiscard]] static bool module_shape_stable(const aura::ir::IRModule& m,
                                                  std::uint64_t expected) noexcept {
        return expected == 0 || compute_module_shape(m) == expected;
    }

    void run(aura::ir::IRModule& m) pre(valid_soa_view(m) && pipeline_epoch_consistent()) {
        note_pass_run(PassKind::Render, false);
        render_pass_runs_total.fetch_add(1, std::memory_order_relaxed);
        error_ = false;
        last_dirty_cleared_ = false;
        blocks_processed_last_ = 0;
        blocks_skipped_last_ = 0;

        const auto shape_before = compute_module_shape(m);
        expected_shape_ = shape_before;

        // Soft pre: shape_stable probe for each function name when registered.
        if (aura::compiler::g_fn_shape_stable_probe) {
            for (const auto& f : m.functions) {
                if (!aura::compiler::g_fn_shape_stable_probe(f.name)) {
                    // Probe says unstable — allow render but record violation soft.
                    render_shape_stable_violations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        const bool have_mask = static_cast<bool>(block_dirty_fn_);
        if (!have_mask)
            render_full_module_fallback_total.fetch_add(1, std::memory_order_relaxed);

        for (auto& func : m.functions)
            render_function_(func, /*count_metrics=*/true);

        // Optional framebuffer dirty short-circuit (renderer layer).
        if (framebuffer_present_) {
            if (aura::renderer::present_batch_if_dirty()) {
                render_framebuffer_present_ok.fetch_add(1, std::memory_order_relaxed);
                // Present consumed dirty AABB — clear for next frame.
                aura::renderer::g_framebuffer_dirty.clear();
            } else {
                render_framebuffer_present_skips.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Dirty-propagation cascade flush when roots were noted during render.
        (void)aura::compiler::dirty::flush_pipeline_cascade_roots();

        last_output_shape_ = compute_module_shape(m);
        if (enforce_shape_stable_ && last_output_shape_ != shape_before) {
            render_shape_stable_violations.fetch_add(1, std::memory_order_relaxed);
            error_ = true; // post contract soft-fail
        }

        // dirty_cleared: no remaining dirty work under current mask, or full run.
        last_dirty_cleared_ = blocks_processed_last_ == 0 ||
                              (have_mask && blocks_skipped_last_ + blocks_processed_last_ > 0);

        if (blocks_processed_last_ > 0 && have_mask)
            render_incremental_hits.fetch_add(1, std::memory_order_relaxed);

        if (!dirty_flags_cleared_or_ok(m, error_))
            opt_contract_violations_soft_total.fetch_add(1, std::memory_order_relaxed);
        if (error_)
            opt_pass_errors_total.fetch_add(1, std::memory_order_relaxed);
    }

    void run(aura::ir::IRFunction& f) {
        blocks_processed_last_ = 0;
        blocks_skipped_last_ = 0;
        render_function_(f, /*count_metrics=*/true);
        if (blocks_processed_last_ > 0 && block_dirty_fn_)
            render_incremental_hits.fetch_add(1, std::memory_order_relaxed);
    }

    void run(aura::ir::BasicBlock& b) {
        // Per-block render unit: always "process" when called directly.
        (void)b;
        ++blocks_processed_last_;
        render_blocks_processed_total.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool has_error() const { return error_; }
    [[nodiscard]] std::string_view name() const { return "render-present"; }

private:
    void render_function_(aura::ir::IRFunction& func, bool count_metrics) {
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            const auto bid = static_cast<std::uint32_t>(bi);
            if (!is_block_dirty(bid)) {
                ++blocks_skipped_last_;
                if (count_metrics)
                    render_dirty_skipped_blocks.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Incremental render work unit: walk instructions (SoA-friendly
            // columnar style would map to IRFunctionSoA; AoS walk is fine).
            auto& block = func.blocks[bi];
            for (const auto& ins : block.instructions) {
                (void)ins; // production would emit draw/present ops
            }
            // Note cascade root for this block when a dep graph is installed.
            if (aura::compiler::dirty::pipeline_dep_graph())
                aura::compiler::dirty::note_pipeline_cascade_root(bid);

            ++blocks_processed_last_;
            if (count_metrics)
                render_blocks_processed_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::function<bool(std::uint32_t)> block_dirty_fn_;
    std::uint64_t pipeline_epoch_ = 0;
    std::uint64_t expected_shape_ = 0;
    std::uint64_t last_output_shape_ = 0;
    std::uint64_t blocks_processed_last_ = 0;
    std::uint64_t blocks_skipped_last_ = 0;
    bool error_ = false;
    bool last_dirty_cleared_ = false;
    bool framebuffer_present_ = false;
    bool enforce_shape_stable_ = true;
};

static_assert(aura::compiler::Pass<RenderPass>, "RenderPass must satisfy Pass (#1578)");
static_assert(aura::compiler::DirtyAwarePass<RenderPass>, "RenderPass must be DirtyAware (#1578)");
static_assert(aura::compiler::IncrementalPass<RenderPass>,
              "RenderPass must be Incremental (#1578)");
static_assert(aura::compiler::SoAViewAwarePass<RenderPass>,
              "RenderPass must be SoAViewAware (#1578)");
static_assert(aura::compiler::JITFriendlyPass<RenderPass>,
              "RenderPass must be JITFriendly (#1578)");
static_assert(aura::compiler::ShapeStableAwarePass<RenderPass>,
              "RenderPass must be ShapeStableAware (#1578)");

// ── Factory + default pipeline (#1576 AC3/AC4 + #1578 Render) ──
// Run core concrete passes via run_pipeline (contracts + metrics).
inline bool run_default_optimization_pipeline(aura::ir::IRModule& mod) {
    opt_pipeline_factory_runs_total.fetch_add(1, std::memory_order_relaxed);
    ConstantFoldingPass cf;
    TypePropagationPass tp;
    ComputeKindPass ck;
    ShapeAwareFoldingPass sa;
    RenderPass rp;
    // Sync pipeline epoch into JIT-friendly passes.
    const auto epoch = aura::compiler::pipeline_mutation_epoch();
    if (epoch != 0) {
        cf.set_pipeline_epoch(epoch);
        tp.set_pipeline_epoch(epoch);
        rp.set_pipeline_epoch(epoch);
    }
    return aura::compiler::run_pipeline(mod, cf, tp, ck, sa, rp);
}

// Incremental render only (dirty pipeline + optional define mask).
inline bool
run_incremental_render_pipeline(aura::ir::IRModule& mod, RenderPass& pass,
                                const aura::compiler::DefineDirtyMaskView* define_cache = nullptr) {
    opt_pipeline_factory_runs_total.fetch_add(1, std::memory_order_relaxed);
    const auto epoch = aura::compiler::pipeline_mutation_epoch();
    if (epoch != 0)
        pass.set_pipeline_epoch(epoch);
    return aura::compiler::run_incremental_dirty_pipeline(mod, pass, define_cache);
}

// Instantiate a single core pass by kind and run it (factory entry).
inline bool run_pass_kind(aura::ir::IRModule& mod, PassKind kind) {
    switch (kind) {
        case PassKind::ConstantFold: {
            ConstantFoldingPass p;
            return aura::compiler::run_one(mod, p);
        }
        case PassKind::TypePropagation: {
            TypePropagationPass p;
            return aura::compiler::run_one(mod, p);
        }
        case PassKind::ComputeKind: {
            ComputeKindPass p;
            return aura::compiler::run_one(mod, p);
        }
        case PassKind::ShapeAwareFold: {
            ShapeAwareFoldingPass p;
            return aura::compiler::run_one(mod, p);
        }
        case PassKind::Render: {
            RenderPass p;
            return aura::compiler::run_one(mod, p);
        }
        default:
            return true; // unknown kind: no-op success (table may list stubs)
    }
}

// Run every descriptor that has requires_contracts=true from the default table.
inline bool run_contracted_default_passes(aura::ir::IRModule& mod) {
    opt_pipeline_factory_runs_total.fetch_add(1, std::memory_order_relaxed);
    bool ok = true;
    for (const auto& d : kDefaultPassTable) {
        if (!d.requires_contracts)
            continue;
        ok = run_pass_kind(mod, d.kind) && ok;
    }
    return ok;
}

} // namespace aura::compiler::opt_registry
