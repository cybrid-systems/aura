// evaluator_security.cpp — Issue #676/#1565/#1566/#1567 security + audit WAL.

module;

#include <chrono>
#include <cstring>

#include "security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"
#include "core/mutation_audit_wal.hh"
#include "core/provenance_tracker.hh"
#include "observability_metrics.h"

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
    using namespace aura::core::audit_wal;
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
    // #1567: full epoch stamp
    slot.epoch = current_bridge_epoch();
    mutation_audit_total_.fetch_add(1, std::memory_order_relaxed);
    // #1567: WAL append (optional; no-op when disabled)
    if (g_mutation_audit_wal().is_enabled()) {
        const auto rec = make_record(slot.seq, slot.timestamp_ms, slot.fiber_id, slot.nodes_changed,
                                     slot.epoch_delta, slot.target_node, slot.op, slot.effect_bits,
                                     slot.tenant_id, slot.provenance_mutation_id, slot.epoch,
                                     slot.effect_denied);
        (void)g_mutation_audit_wal().append(rec);
    }
}

// Issue #1565 / #1876: force side-effect paths through capability effect check.
// #1876: under sandbox, also validate/record StableNodeRef provenance and
// bump sandbox_violations_total + capability_denials_by_effect metrics.
bool Evaluator::check_and_record_effect(std::uint16_t required_effect_bits,
                                        std::uint16_t actual_effect_bits, std::string_view op,
                                        ast::NodeId target_node, std::uint64_t tenant_id,
                                        std::uint64_t provenance_mutation_id) noexcept {
    using namespace aura::core::capability;
    using namespace aura::core::sandbox;
    using namespace aura::core::provenance;

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

    // Issue #1876: under sandbox, deny when explicit tenant arg mismatches
    // the evaluator's capability_tenant_id_ (cross-tenant effect).
    if (sb_active && !wildcard) {
        const auto cur_tenant = capability_tenant_id_;
        if (tenant_id != 0 && cur_tenant != 0 && tenant_id != cur_tenant) {
            bump_capability_denial();
            g_sandbox_state().effect_denials++;
            g_sandbox_state().effect_checks++;
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
                m->sandbox_violations_total.fetch_add(1, std::memory_order_relaxed);
                m->sandbox_provenance_invalid_total.fetch_add(1, std::memory_order_relaxed);
                m->capability_denials_by_effect.fetch_or(required_effect_bits,
                                                         std::memory_order_relaxed);
                using aura::compiler::security::kEffectMutate;
                if (required_effect_bits & kEffectMutate)
                    m->capability_denial_mutate_total.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
    }

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
    // #1567: full epoch + WAL persist on effect path too
    slot.epoch = prov.epoch;
    mutation_audit_total_.fetch_add(1, std::memory_order_relaxed);
    {
        using namespace aura::core::audit_wal;
        if (g_mutation_audit_wal().is_enabled()) {
            const auto rec = make_record(
                slot.seq, slot.timestamp_ms, slot.fiber_id, slot.nodes_changed, slot.epoch_delta,
                slot.target_node, slot.op, slot.effect_bits, slot.tenant_id,
                slot.provenance_mutation_id, slot.epoch, slot.effect_denied);
            (void)g_mutation_audit_wal().append(rec);
        }
    }

    if (!ok) {
        bump_capability_denial();
        g_sandbox_state().effect_denials++;
        // Issue #1876: CompilerMetrics sandbox violation + per-effect denials.
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            if (sb_active || is_strict() || is_sandbox_active())
                m->sandbox_violations_total.fetch_add(1, std::memory_order_relaxed);
            m->capability_denials_by_effect.fetch_or(required_effect_bits,
                                                     std::memory_order_relaxed);
            using aura::compiler::security::kEffectFfi;
            using aura::compiler::security::kEffectMutate;
            if (required_effect_bits & kEffectMutate)
                m->capability_denial_mutate_total.fetch_add(1, std::memory_order_relaxed);
            if (required_effect_bits & kEffectFfi)
                m->capability_denial_ffi_total.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (sb_active) {
        // Issue #1876: all allowed effects under sandbox record provenance.
        g_provenance_tracker().record_mutation();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->sandbox_provenance_records_total.fetch_add(1, std::memory_order_relaxed);
    }
    g_sandbox_state().effect_checks++;
    return ok;
}

// Issue #1567: enable WAL under persist_dir; replay prior records into ring.
bool Evaluator::enable_mutation_audit_wal(std::string_view persist_dir) noexcept {
    using namespace aura::core::audit_wal;
    std::vector<AuditWalRecord> replayed;
    if (!g_mutation_audit_wal().enable(persist_dir, &replayed, kMutationAuditRingSize))
        return false;
    // Install recovered records into ring (preserve seq continuity).
    std::uint64_t max_seq = 0;
    for (const auto& rec : replayed) {
        if (rec.seq == 0)
            continue;
        auto& slot = mutation_audit_ring_[rec.seq % kMutationAuditRingSize];
        slot.seq = rec.seq;
        slot.timestamp_ms = rec.timestamp_ms;
        slot.fiber_id = rec.fiber_id;
        slot.nodes_changed = rec.nodes_changed;
        slot.epoch_delta = rec.epoch_delta;
        slot.target_node = rec.target_node;
        std::memcpy(slot.op, rec.op, sizeof(slot.op));
        slot.effect_bits = rec.effect_bits;
        slot.tenant_id = rec.tenant_id;
        slot.provenance_mutation_id = rec.provenance_mutation_id;
        slot.effect_denied = rec.effect_denied != 0;
        slot.epoch = rec.epoch;
        if (rec.seq + 1 > max_seq)
            max_seq = rec.seq + 1;
    }
    if (max_seq > 0) {
        mutation_audit_seq_.store(max_seq, std::memory_order_relaxed);
        // total reflects recovered + future; seed with recovered count.
        mutation_audit_total_.store(replayed.size(), std::memory_order_relaxed);
    }
    return true;
}

void Evaluator::disable_mutation_audit_wal() noexcept {
    aura::core::audit_wal::g_mutation_audit_wal().disable();
}

bool Evaluator::mutation_audit_wal_enabled() const noexcept {
    return aura::core::audit_wal::g_mutation_audit_wal().is_enabled();
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
    using namespace aura::core::workspace_isolation;
    if (mode > 2)
        mode = 2;
    g_capability_registry().sandbox_mode = static_cast<EffectSandboxMode>(mode);
    set_mode(static_cast<SandboxMode>(mode));
    // Strict/Restricted also set evaluator sandbox_mode_ so legacy gates engage.
    sandbox_mode_ = (mode != 0);
    // #1566: Strict sandbox links isolation enforcement.
    g_workspace_isolation().set_strict_sandbox_linked(mode == 2);
}

std::uint8_t Evaluator::effect_sandbox_mode() const noexcept {
    return static_cast<std::uint8_t>(aura::core::capability::g_capability_registry().sandbox_mode);
}

// Issue #1566: multi-tenant workspace isolation bridge.
void Evaluator::set_tenant_principal(std::uint64_t tenant_id, std::string_view name,
                                     bool allow_cross) noexcept {
    using namespace aura::core::workspace_isolation;
    capability_tenant_id_ = tenant_id;
    g_workspace_isolation().set_current_tenant(tenant_id, name, allow_cross);
}

void Evaluator::grant_cross_tenant_access(std::uint64_t from_tenant, std::uint64_t to_tenant,
                                          std::uint16_t effect_bits) noexcept {
    using namespace aura::core::workspace_isolation;
    g_workspace_isolation().grant_cross_tenant(from_tenant, to_tenant, effect_bits);
}

bool Evaluator::check_workspace_isolation(std::uint64_t target_tenant, std::uint64_t ref_tenant,
                                          std::uint16_t required_effects,
                                          std::string_view op) noexcept {
    using namespace aura::core::workspace_isolation;
    using namespace aura::core::sandbox;
    const auto target = target_tenant != 0 ? target_tenant : capability_tenant_id_;
    const bool strict =
        effect_sandbox_mode() == 2 || is_strict() || g_workspace_isolation().strict_sandbox_linked;
    IsolationRefProvenance prov{};
    prov.tenant_id = ref_tenant;
    const bool ok = check_boundary(target, &prov, required_effects, strict, op);
    if (!ok)
        bump_capability_denial();
    return ok;
}

void Evaluator::stamp_ref_tenant(ast::FlatAST::StableNodeRef& ref) const noexcept {
    ref.tenant_id = capability_tenant_id_;
}

} // namespace aura::compiler
