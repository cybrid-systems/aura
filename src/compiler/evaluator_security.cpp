// evaluator_security.cpp — Issue #676 sandbox + audit; #1565 capability effects.

module;

#include <chrono>
#include <cstring>

#include "security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"

module aura.compiler.evaluator;

import std;
import aura.core.ast;

namespace aura::compiler {

// Issue #918: explicit using-declarations (no using-namespace).
using security::kCapWildcard;

bool Evaluator::has_capability(std::string_view needed) const noexcept {
    if (!sandbox_mode_)
        return true;
    if (needed.empty())
        return false;
    const auto matches = [&](const std::string& held) {
        return held == needed || held == kCapWildcard;
    };
    for (const auto& cap : granted_capabilities_) {
        if (matches(cap))
            return true;
    }
    for (const auto& layer : capability_stack_) {
        for (const auto& cap : layer) {
            if (matches(cap))
                return true;
        }
    }
    return false;
}

void Evaluator::grant_capability(std::string cap) {
    for (const auto& existing : granted_capabilities_) {
        if (existing == cap)
            return;
    }
    granted_capabilities_.push_back(std::move(cap));
    // #1565: mirror named grant into effect matrix for current tenant.
    using namespace aura::core::capability;
    const auto eff = effect_for_cap_name(granted_capabilities_.back());
    if (eff != Effect::None) {
        EffectProvenance prov;
        prov.epoch = current_bridge_epoch();
        g_capability_registry().grant(capability_tenant_id_, granted_capabilities_.back(), eff,
                                      prov);
    }
}

void Evaluator::emit_mutation_audit(std::uint32_t nodes_changed, std::uint32_t epoch_delta,
                                    std::string_view op, ast::NodeId target_node) noexcept {
    const auto seq = mutation_audit_seq_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = mutation_audit_ring_[seq % kMutationAuditRingSize];
    slot.seq = seq;
    slot.timestamp_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    slot.fiber_id = static_cast<std::int64_t>(aura_fiber_current_id());
    slot.nodes_changed = nodes_changed;
    slot.epoch_delta = epoch_delta;
    slot.target_node = static_cast<std::uint32_t>(target_node);
    const auto n = std::min(op.size(), sizeof(slot.op) - 1);
    std::memcpy(slot.op, op.data(), n);
    slot.op[n] = '\0';
    // #1565: default mutate effect stamp on structural audits
    slot.effect_bits = static_cast<std::uint16_t>(aura::core::capability::Effect::Mutate);
    slot.tenant_id = capability_tenant_id_;
    slot.provenance_mutation_id = 0;
    slot.effect_denied = false;
    mutation_audit_total_.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1565: force side-effect paths through capability effect check.
bool Evaluator::check_and_record_effect(std::uint16_t required_effect_bits,
                                        std::uint16_t actual_effect_bits, std::string_view op,
                                        ast::NodeId target_node, std::uint64_t tenant_id,
                                        std::uint64_t provenance_mutation_id) noexcept {
    using namespace aura::core::capability;
    using namespace aura::core::sandbox;

    // Keep sandbox.hh mode in sync with evaluator sandbox_mode_ + Strict.
    if (sandbox_mode_ && g_capability_registry().sandbox_mode == EffectSandboxMode::Off)
        g_capability_registry().sandbox_mode = EffectSandboxMode::Restricted;
    if (is_strict())
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;

    EffectProvenance prov;
    prov.node_id = static_cast<std::uint32_t>(target_node);
    prov.mutation_id = provenance_mutation_id;
    prov.fiber_id = static_cast<std::uint32_t>(aura_fiber_current_id());
    prov.epoch = current_bridge_epoch();

    const auto tenant = tenant_id != 0 ? tenant_id : capability_tenant_id_;
    const bool wildcard = has_capability(kCapWildcard);
    // When evaluator sandbox is off and effect mode is Off, still record.
    const bool sb_active = sandbox_mode_ || is_strict() || is_sandbox_active();

    const bool ok = aura::core::capability::check_and_record_effect(
        static_cast<Effect>(required_effect_bits), static_cast<Effect>(actual_effect_bits), prov,
        tenant, op, wildcard, sb_active);

    // Mirror into mutation audit ring for unified Agent visibility.
    const auto seq = mutation_audit_seq_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = mutation_audit_ring_[seq % kMutationAuditRingSize];
    slot.seq = seq;
    slot.timestamp_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    slot.fiber_id = static_cast<std::int64_t>(aura_fiber_current_id());
    slot.nodes_changed = 0;
    slot.epoch_delta = 0;
    slot.target_node = static_cast<std::uint32_t>(target_node);
    const auto n = std::min(op.size(), sizeof(slot.op) - 1);
    std::memcpy(slot.op, op.data(), n);
    slot.op[n] = '\0';
    slot.effect_bits = actual_effect_bits;
    slot.tenant_id = tenant;
    slot.provenance_mutation_id = provenance_mutation_id;
    slot.effect_denied = !ok;
    mutation_audit_total_.fetch_add(1, std::memory_order_relaxed);

    if (!ok) {
        bump_capability_denial();
        g_sandbox_state().effect_denials++;
    }
    g_sandbox_state().effect_checks++;
    return ok;
}

void Evaluator::grant_effect_capability(std::uint64_t tenant_id, std::string_view name,
                                        std::uint16_t effect_bits,
                                        std::uint64_t provenance_mutation_id) noexcept {
    using namespace aura::core::capability;
    EffectProvenance prov;
    prov.mutation_id = provenance_mutation_id;
    prov.epoch = current_bridge_epoch();
    prov.fiber_id = static_cast<std::uint32_t>(aura_fiber_current_id());
    g_capability_registry().grant(tenant_id, name, static_cast<Effect>(effect_bits), prov);
    // Also string-grant for legacy has_capability path.
    if (!name.empty())
        grant_capability(std::string(name));
}

void Evaluator::set_effect_sandbox_mode(std::uint8_t mode) noexcept {
    using namespace aura::core::capability;
    using namespace aura::core::sandbox;
    if (mode > 2)
        mode = 2;
    g_capability_registry().sandbox_mode = static_cast<EffectSandboxMode>(mode);
    set_mode(static_cast<SandboxMode>(mode));
    // Strict/Restricted also set evaluator sandbox_mode_ so legacy gates engage.
    sandbox_mode_ = (mode != 0);
}

std::uint8_t Evaluator::effect_sandbox_mode() const noexcept {
    return static_cast<std::uint8_t>(aura::core::capability::g_capability_registry().sandbox_mode);
}

} // namespace aura::compiler
