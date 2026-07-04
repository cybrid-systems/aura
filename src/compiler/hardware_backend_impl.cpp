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

bool is_sv_structural_node(const aura::ast::FlatAST& flat,
                           const aura::ast::NodeId id) noexcept {
    if (id == aura::ast::NULL_NODE || id >= flat.size())
        return false;
    const auto tag = flat.get(id).tag;
    if (tag == aura::ast::NodeTag::Interface || tag == aura::ast::NodeTag::Modport ||
        tag == aura::ast::NodeTag::Property || tag == aura::ast::NodeTag::Sequence ||
        tag == aura::ast::NodeTag::Assert || tag == aura::ast::NodeTag::Covergroup ||
        tag == aura::ast::NodeTag::Coverpoint)
        return true;
    return (flat.verify_dirty(id) & aura::ast::FlatAST::kSvaDirty) != 0;
}

std::uint8_t sv_structural_dirty_reasons(const aura::ast::FlatAST& flat,
                                         const aura::ast::NodeId id) noexcept {
    if (id == aura::ast::NULL_NODE || id >= flat.size())
        return 0;
    std::uint8_t mask = 0;
    const auto tag = flat.get(id).tag;
    if (tag == aura::ast::NodeTag::Interface)
        mask = static_cast<std::uint8_t>(mask | SvStructuralDirtyReason::kSvInterfaceDirty);
    if (tag == aura::ast::NodeTag::Modport)
        mask = static_cast<std::uint8_t>(mask | SvStructuralDirtyReason::kSvModportDirty);
    if ((flat.verify_dirty(id) & aura::ast::FlatAST::kSvaDirty) != 0)
        mask = static_cast<std::uint8_t>(mask | SvStructuralDirtyReason::kSvSvaFeedbackDirty);
    if (flat.verification_dirty(id) != 0)
        mask = static_cast<std::uint8_t>(mask | SvStructuralDirtyReason::kSvSvaFeedbackDirty);
    return mask;
}

bool should_invoke_sv_closedloop_hook(const aura::ast::FlatAST& flat,
                                      const aura::ast::NodeId id) noexcept {
    if (!is_sv_structural_node(flat, id))
        return false;
    return flat.verification_dirty(id) != 0 || flat.ppa_dirty_reasons(id) != 0;
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
