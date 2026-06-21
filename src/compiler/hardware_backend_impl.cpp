module;

module aura.compiler.hardware_backend;

import std;
import aura.core.ast;

namespace aura::compiler::hardware {

namespace {

StructuralMutationHook g_structural_mutation_hook;

} // namespace

void register_structural_mutation_hook(StructuralMutationHook hook) {
    g_structural_mutation_hook = std::move(hook);
}

void clear_structural_mutation_hook() { g_structural_mutation_hook = nullptr; }

void on_structural_mutation(const aura::ast::NodeId node, const std::uint8_t dirty_reasons,
                            const std::uint8_t ppa_reasons) {
    if (g_structural_mutation_hook)
        g_structural_mutation_hook(node, dirty_reasons, ppa_reasons);
}

std::uint8_t parse_ppa_hint(const std::int64_t hint) noexcept {
    using P = aura::ast::FlatAST::PpaDirtyReason;
    std::uint8_t mask = 0;
    if (hint & 0x01)
        mask = static_cast<std::uint8_t>(mask | P::kTimingDirty);
    if (hint & 0x02)
        mask = static_cast<std::uint8_t>(mask | P::kPowerDirty);
    if (hint & 0x04)
        mask = static_cast<std::uint8_t>(mask | P::kAreaDirty);
    if (hint & 0x08)
        mask = static_cast<std::uint8_t>(mask | P::kBackendHint);
    return mask;
}

} // namespace aura::compiler::hardware