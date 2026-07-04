// Issue #277: Hardware / Verilog backend mutation hook surface.
// Orthogonal to #182 backend implementation — provides the callback
// registration point for PPA-aware incremental RTL refresh.
module;

#include <cstdint>
#include <functional>

export module aura.compiler.hardware_backend;
import std;
import aura.core.ast;

namespace aura::compiler::hardware {

// Called after structural EDSL mutations when PPA bits are set.
// `dirty_reasons` is the FlatAST::DirtyReason mask; `ppa_reasons`
// is the FlatAST::PpaDirtyReason mask from the orthogonal column.
export using StructuralMutationHook =
    std::function<void(aura::ast::NodeId, std::uint8_t dirty_reasons, std::uint8_t ppa_reasons)>;

export void register_structural_mutation_hook(StructuralMutationHook hook);
export void clear_structural_mutation_hook();
export void on_structural_mutation(aura::ast::NodeId node, std::uint8_t dirty_reasons,
                                   std::uint8_t ppa_reasons);

// Parse optional mutate primitive ppa-hint integer into a bitmask.
export std::uint8_t parse_ppa_hint(std::int64_t hint) noexcept;

// Issue #693: SV-specific dirty reason bits passed to the structural hook.
export enum SvStructuralDirtyReason : std::uint8_t {
    kSvInterfaceDirty = 0x10,
    kSvModportDirty = 0x20,
    kSvSvaFeedbackDirty = 0x40,
};

export [[nodiscard]] bool is_sv_structural_node(const aura::ast::FlatAST& flat,
                                              aura::ast::NodeId id) noexcept;

export [[nodiscard]] std::uint8_t
sv_structural_dirty_reasons(const aura::ast::FlatAST& flat, aura::ast::NodeId id) noexcept;

export [[nodiscard]] bool should_invoke_sv_closedloop_hook(
    const aura::ast::FlatAST& flat, aura::ast::NodeId id) noexcept;

} // namespace aura::compiler::hardware
