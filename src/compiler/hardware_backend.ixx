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

} // namespace aura::compiler::hardware
