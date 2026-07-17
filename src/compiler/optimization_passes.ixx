// optimization_passes.ixx — Issue #1201 Phase 1: typed pass registry / pipeline factory scaffold.

module;

export module aura.compiler.optimization_passes;

import std;

export namespace aura::compiler::opt_registry {

inline constexpr int kOptimizationPassesPhase = 1;

enum class PassKind : std::uint8_t {
    ConstantFold = 0,
    Inline = 1,
    TypeCheck = 2,
    Arity = 3,
    Shape = 4,
    LinearOwnership = 5,
    ComputeKind = 6,
    Render = 7,
    Count
};

struct PassDescriptor {
    PassKind kind = PassKind::ConstantFold;
    std::string_view name;
    bool dirty_aware = false;
    bool shape_stable = false;
};

// Phase 1 static table — factory hooks wire into pass_manager in follow-up.
// Issue #1574: Shape + ComputeKind + ConstantFold are DirtyAware and
// consume DefineDirtyMaskView via run_incremental_dirty_pipeline.
inline constexpr PassDescriptor kDefaultPassTable[] = {
    {PassKind::ConstantFold, "constant-fold", true, true},
    {PassKind::Inline, "inline", false, false},
    {PassKind::TypeCheck, "type-check", false, true},
    {PassKind::Arity, "arity", false, false},
    {PassKind::Shape, "shape", true, true},
    {PassKind::LinearOwnership, "linear-ownership", false, false},
    {PassKind::ComputeKind, "compute-kind", true, false}, // dirty-aware (#1574)
    {PassKind::Render, "render-present", true, true},
};

[[nodiscard]] inline std::size_t default_pass_count() noexcept {
    return sizeof(kDefaultPassTable) / sizeof(kDefaultPassTable[0]);
}

} // namespace aura::compiler::opt_registry
