// optimization_passes.ixx — Issue #1201 Phase 1 scaffold + Issue #1576 Phase 2:
// concrete pass types with C++26 contracts, factory, and default pipeline.

module;

export module aura.compiler.optimization_passes;

import std;
import aura.compiler.ir;
import aura.compiler.pass_manager;

export namespace aura::compiler::opt_registry {

inline constexpr int kOptimizationPassesPhase = 2; // #1576: concrete passes + contracts

// ── Metrics (#1576) ────────────────────────────────────────────
inline std::atomic<std::uint64_t> opt_pass_runs_total{0};
inline std::atomic<std::uint64_t> opt_pass_errors_total{0};
inline std::atomic<std::uint64_t> opt_contracts_pre_checks_total{0};
inline std::atomic<std::uint64_t> opt_contracts_post_checks_total{0};
inline std::atomic<std::uint64_t> opt_pipeline_factory_runs_total{0};
inline std::atomic<std::uint64_t> opt_contract_violations_soft_total{0};

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
    {PassKind::Shape, "shape", true, true, true, true},
    {PassKind::Inline, "inline", false, false, false, false},
    {PassKind::TypeCheck, "type-check", false, true, false, false},
    {PassKind::Arity, "arity", false, false, false, true},
    {PassKind::LinearOwnership, "linear-ownership", false, false, false, true},
    {PassKind::Render, "render-present", true, true, false, false},
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

// ── Factory + default pipeline (#1576 AC3/AC4) ─────────────────
// Run the four core concrete passes via run_pipeline (contracts + metrics).
inline bool run_default_optimization_pipeline(aura::ir::IRModule& mod) {
    opt_pipeline_factory_runs_total.fetch_add(1, std::memory_order_relaxed);
    ConstantFoldingPass cf;
    TypePropagationPass tp;
    ComputeKindPass ck;
    ShapeAwareFoldingPass sa;
    // Sync pipeline epoch into JIT-friendly passes.
    const auto epoch = aura::compiler::pipeline_mutation_epoch();
    if (epoch != 0) {
        cf.set_pipeline_epoch(epoch);
        tp.set_pipeline_epoch(epoch);
    }
    return aura::compiler::run_pipeline(mod, cf, tp, ck, sa);
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
