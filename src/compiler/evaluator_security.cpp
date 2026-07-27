// evaluator_security.cpp — Issue #676/#1565/#1566/#1567 security + audit WAL.

module;

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "security_capabilities.h"
#include "security_defaults.hh" // #2076/#2053 production defaults (header-inline)
#include "typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"
#include "core/mutation_audit_wal.hh"
#include "core/provenance_tracker.hh"
#include "core/security_event.hh"     // #2075: shared SecurityEvent surface
#include "core/security_event_wal.hh" // #2225: durable SecurityEvent side-car WAL
#include "observability_metrics.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;

namespace aura::compiler {

// Issue #918: explicit using-declarations (no using-namespace).
using security::kCapWildcard;

// Issue #2077: unify has_capability string path with Effect matrix
// (single source of truth). When `effect_for_cap_name(needed) != None`
// the Effect bit in `g_capability_registry().effects_for(tenant)` is
// consulted — grant_capability / grant_effect_capability already mirror
// these names into the registry, and now has_capability reads them
// from there too. Wildcard "*" still grants everything via the
// explicit string-grant path; it also maps to the full effect mask
// so an effect-only grant (without pushing "*" as a string) can
// satisfy wildcard queries if every bit is held. Caps with
// `effect_for_cap_name == None` (tenant-admin, compile-stats, agent,
// workspace, fiber, exception-control, macro, query, capability,
// sys-read/write/open/syscall, self-evo, synthesize, strategy,
// sandbox) keep the legacy string-list path.
bool Evaluator::has_capability(std::string_view needed) const noexcept {
    // Sandbox fully off (Evaluator sandbox + global registry effect mode)
    // preserves legacy "always allow" semantics — matches check_and_record_effect.
    if (!sandbox_mode_ && effect_sandbox_mode() == 0)
        return true;
    if (needed.empty())
        return false;
    using namespace ::aura::core::capability;
    // Explicit "*" in any layer keeps legacy wildcard-grants-all behavior.
    const auto wildcard_held = [&]() noexcept {
        for (const auto& cap : granted_capabilities_) {
            if (cap == kCapWildcard)
                return true;
        }
        for (const auto& layer : capability_stack_) {
            for (const auto& cap : layer) {
                if (cap == kCapWildcard)
                    return true;
            }
        }
        return false;
    }();
    if (wildcard_held)
        return true;
    // Delegate to effect matrix when name maps to a known Effect bit.
    // effect_for_cap_name returns the full mask for "*" so an effect-only
    // full-grant (registry has every bit) satisfies individual effect-mapped
    // cap queries even without an explicit "*" string grant.
    const Effect eff = effect_for_cap_name(needed);
    if (eff != Effect::None) {
        return has_effect(g_capability_registry().effects_for(capability_tenant_id_), eff);
    }
    // Legacy string-only caps keep the list path.
    const auto matches = [&](const std::string& held) { return held == needed; };
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
    // #1565 / #2055: mirror named grant into effect matrix for current tenant.
    // Stamp WorkspaceEpoch Mutation + fiber (not Bridge) so grant_epoch matches
    // the mutation epoch at grant time and long-running blame stays consistent.
    using namespace ::aura::core::capability;
    const auto eff = effect_for_cap_name(granted_capabilities_.back());
    if (eff != Effect::None) {
        const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
        // Issue #2151: honor effect_fiber_id_or so tests can stamp fiber A/B
        // without a real scheduler; production override stays 0.
        const auto fiber = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));
        auto prov = make_grant_provenance(/*mutation_id=*/0, force_bind, /*node_id=*/0, fiber);
        g_capability_registry().grant(capability_tenant_id_, granted_capabilities_.back(), eff,
                                      prov);
        // Issue #2136: count Render effect grants for Agent dashboards.
        if (has_effect(eff, Effect::Render)) {
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->render_effect_granted_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Evaluator::emit_mutation_audit(std::uint32_t nodes_changed, std::uint32_t epoch_delta,
                                    std::string_view op, ast::NodeId target_node) noexcept {
    using namespace ::aura::core::audit_wal;
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
    using namespace ::aura::core::capability;
    using namespace ::aura::core::sandbox;
    using namespace ::aura::core::provenance;

    // Keep sandbox.hh mode in sync with evaluator sandbox_mode_ + Strict.
    if (sandbox_mode_ && g_capability_registry().sandbox_mode == EffectSandboxMode::Off)
        g_capability_registry().sandbox_mode = EffectSandboxMode::Restricted;
    if (is_strict())
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;

    EffectProvenance prov;
    prov.node_id = static_cast<std::uint32_t>(target_node);
    prov.mutation_id = provenance_mutation_id;
    // Issue #2151: effect_fiber_id_or lets tests simulate fiber A vs B;
    // production leaves override at 0 → live TLS fiber id.
    prov.fiber_id = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));
    // Issue #2149: security provenance uses WorkspaceEpoch Mutation only
    // (same vocabulary as make_grant_provenance / grant_epoch). Bridge is
    // AOT/JIT/closure — never the capability fence key. Pre-#2149 this
    // path used Evaluator::current_bridge_epoch(), which can diverge from
    // Mutation under independent bumps and misalign audit / grant epochs.
    {
        const auto me = ::aura::core::current_mutation_epoch();
        prov.epoch = me != 0 ? me : 1;
        // Optional observability: Mutation vs Bridge split under Strict
        // (does not deny — Agent sees capability_mutation_bridge_split_total).
        if (is_strict()) {
            const auto be = ::aura::core::current_bridge_epoch();
            if (be != 0 && me != 0 && be != me) {
                ::aura::core::capability::g_capability_effect_metrics()
                    .capability_mutation_bridge_split_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const auto tenant = tenant_id != 0 ? tenant_id : capability_tenant_id_;
    const bool wildcard = has_capability(kCapWildcard);
    // When evaluator sandbox is off and effect mode is Off, still record.
    const bool sb_active = sandbox_mode_ || is_strict() || is_sandbox_active();

    // Issue #2151: snapshot hard-deny counter so SecurityEvent can emit
    // the stable reason "fiber-grant-mismatch" (Agent-recoverable).
    const auto hard_deny_before =
        g_capability_effect_metrics().capability_fiber_hard_deny_total.load(
            std::memory_order_relaxed);

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
    // Prefer effect-check fiber stamp (includes #2151 override) for audit.
    slot.fiber_id = static_cast<std::int64_t>(prov.fiber_id);
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
        using namespace ::aura::core::audit_wal;
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
            using aura::compiler::security::kEffectRender;
            if (required_effect_bits & kEffectMutate)
                m->capability_denial_mutate_total.fetch_add(1, std::memory_order_relaxed);
            if (required_effect_bits & kEffectFfi)
                m->capability_denial_ffi_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2136: first-class Render deny counter (batch/FFI matrix).
            if (required_effect_bits & kEffectRender)
                m->effect_denied_render_total.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (sb_active) {
        // Issue #1876: all allowed effects under sandbox record provenance.
        g_provenance_tracker().record_mutation();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->sandbox_provenance_records_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #2075 / #2054: always emit correlated SecurityEvent (allow + deny)
    // so Agents get a complete forensic trail. Use ::aura::core:: (absolute)
    // because we're inside namespace aura::compiler.
    {
        using aura::compiler::security::kEffectFfi;
        using aura::compiler::security::kEffectMutate;
        using aura::compiler::security::kEffectRender;
        using ::aura::core::security_event::append_security_event;
        using ::aura::core::security_event::g_security_event_ring;
        using ::aura::core::security_event::SecurityEventKind;
        const auto mid =
            provenance_mutation_id != 0 ? provenance_mutation_id : static_cast<std::uint64_t>(seq);
        const auto kind = ok ? SecurityEventKind::EffectAllow : SecurityEventKind::EffectDeny;
        const char* reason_str = "effect-allow";
        if (!ok) {
            // Issue #2151: hard fiber isolation deny is Agent-stable.
            const auto hard_deny_after =
                g_capability_effect_metrics().capability_fiber_hard_deny_total.load(
                    std::memory_order_relaxed);
            if (hard_deny_after > hard_deny_before) {
                reason_str = "fiber-grant-mismatch";
            } else {
                reason_str = "capability-effect-deny";
                if (required_effect_bits & kEffectMutate)
                    reason_str = "mutate-deny";
                else if (required_effect_bits & kEffectFfi)
                    reason_str = "ffi-deny";
                else if (required_effect_bits & kEffectRender)
                    reason_str = "render-deny";
            }
        }
        append_security_event(g_security_event_ring(), kind, tenant, mid, prov.epoch,
                              required_effect_bits, op, reason_str, /*denied=*/!ok, slot.fiber_id);
        // #2225: durable mirror — short-circuits when WAL off (~1 ns).
        ::aura::core::security_event_wal::persist_security_event(
            kind, tenant, mid, prov.epoch, required_effect_bits, op, reason_str,
            /*denied=*/!ok, slot.fiber_id,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count()));
        // #2054: always-on TypedMutationAudit correlation (bypasses Sampled).
        typed_audit::capture_security_correlated_audit(mid, op, prov.epoch, /*denied=*/!ok,
                                                       static_cast<std::uint32_t>(target_node),
                                                       slot.fiber_id);
    }
    g_sandbox_state().effect_checks++;
    return ok;
}

// Issue #2072: single production entry for new side-effect paths.
// Wraps check_and_record_effect with the standard arguments (required =
// actual = req_bits, tenant = capability_tenant_id_, provenance = 0
// when no active mutation). All new FFI / network / exec / render /
// hotpath entry points MUST go through require_effect (not call
// check_and_record_effect directly) so the audit ring + capability
// metrics surface stays consistent. Returns true on allow, false on deny.
bool Evaluator::require_effect(std::uint16_t req_bits, std::string_view op,
                               ast::NodeId target_node) noexcept {
    return check_and_record_effect(req_bits, req_bits, op, target_node, capability_tenant_id_,
                                   /*provenance_mutation_id=*/0);
}

// Issue #1567: enable WAL under persist_dir; replay prior records into ring.
bool Evaluator::enable_mutation_audit_wal(std::string_view persist_dir) noexcept {
    using namespace ::aura::core::audit_wal;
    std::vector<AuditWalRecord> replayed;
    if (!g_mutation_audit_wal().enable(persist_dir, &replayed, kMutationAuditRingSize))
        return false;
    // Install recovered records into ring (preserve seq continuity).
    // Issue #2054: do NOT skip rec.seq==0 — the first emit uses seq 0
    // (fetch_add starts at 0); skipping dropped the first allow on replay.
    std::uint64_t max_seq = 0;
    for (const auto& rec : replayed) {
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
    if (!replayed.empty()) {
        // max_seq is exclusive next; if only seq=0 was present, max_seq=1.
        if (max_seq == 0)
            max_seq = 1;
        mutation_audit_seq_.store(max_seq, std::memory_order_relaxed);
        // total reflects recovered + future; seed with recovered count.
        mutation_audit_total_.store(replayed.size(), std::memory_order_relaxed);
    }
    // Issue #2054: rebuild unified SecurityEvent ring from WAL so Agents
    // see the same chronological trail after restart (durable backend =
    // mutation_audit_wal). Clear then re-append in seq order.
    {
        using ::aura::core::security_event::append_security_event;
        using ::aura::core::security_event::g_security_event_ring;
        using ::aura::core::security_event::reset_security_event_ring_for_test;
        using ::aura::core::security_event::SecurityEventKind;
        reset_security_event_ring_for_test();
        for (const auto& rec : replayed) {
            const bool denied = rec.effect_denied != 0;
            const auto kind =
                denied ? SecurityEventKind::EffectDeny : SecurityEventKind::EffectAllow;
            const char* reason = denied ? "wal-replay-deny" : "wal-replay-allow";
            const auto mid = rec.provenance_mutation_id != 0 ? rec.provenance_mutation_id
                                                             : (rec.seq == 0 ? 1 : rec.seq);
            append_security_event(g_security_event_ring(), kind, rec.tenant_id, mid, rec.epoch,
                                  rec.effect_bits,
                                  std::string_view(rec.op, strnlen(rec.op, sizeof(rec.op))), reason,
                                  denied, rec.fiber_id);
        }
    }
    // Issue #2225: auto-pair with the side-car SecurityEventWAL so
    // production defaults under multi-tenant / Strict / #2150 cover
    // both durable paths. The side-car has full SecurityEvent fidelity
    // (all 5 kinds: EffectDeny/Allow + IsolationDeny + InvariantFail +
    // MacroHygiene); the mutation-derived rebuild above is the fallback
    // for old WAL files that pre-date the side-car. Side-car enable
    // failure is non-fatal: the mutation WAL stays enabled, hot-path
    // persist_security_event short-circuits when the side-car is off.
    (void)enable_security_event_wal(persist_dir);
    return true;
}

void Evaluator::disable_mutation_audit_wal() noexcept {
    aura::core::audit_wal::g_mutation_audit_wal().disable();
    // #2225: also disable the side-car WAL so disable_mutation_audit_wal
    // remains a single switch for the production audit surface.
    aura::core::security_event_wal::g_security_event_wal().disable();
}

bool Evaluator::mutation_audit_wal_enabled() const noexcept {
    return aura::core::audit_wal::g_mutation_audit_wal().is_enabled();
}

// Issue #2225: enable the durable side-car WAL for the unified
// SecurityEvent surface. Independent of mutation_audit_wal (file
// format, segment rotation, replay path are all separate), but
// enable_mutation_audit_wal auto-pairs this so production defaults
// under multi-tenant / Strict / #2150 cover both.
//
// Replay: when the side-car has records, reset the live ring and
// repopulate in disk-seq order. ring.seq is set to max+1 so
// subsequent appends do not collide with the replayed range —
// monotonic seq across restart is preserved. When the side-car is
// empty (fresh dir or old-format-only WAL), the existing mutation-
// derived rebuild (if mutation_audit_wal was also enabled) stays in
// the ring as the source of truth (backward-compat fallback).
bool Evaluator::enable_security_event_wal(std::string_view persist_dir) noexcept {
    using namespace ::aura::core::security_event_wal;
    std::vector<SecurityEventWalRecord> replayed;
    if (!g_security_event_wal().enable(persist_dir, &replayed, kSecurityEventRingSize))
        return false;
    if (replayed.empty())
        return true; // no durable records; live ring stays as caller left it
    using ::aura::core::security_event::append_security_event;
    using ::aura::core::security_event::g_security_event_ring;
    using ::aura::core::security_event::reset_security_event_ring_for_test;
    using ::aura::core::security_event::SecurityEvent;
    using ::aura::core::security_event::SecurityEventKind;
    reset_security_event_ring_for_test();
    std::uint64_t max_seq = 0;
    for (const auto& rec : replayed) {
        const auto kind = static_cast<SecurityEventKind>(rec.kind);
        const bool denied = rec.denied != 0;
        const std::string_view op_sv(rec.op, strnlen(rec.op, sizeof(rec.op)));
        const std::string_view reason_sv(rec.reason, strnlen(rec.reason, sizeof(rec.reason)));
        append_security_event(g_security_event_ring(), kind, rec.tenant_id, rec.mutation_id,
                              rec.epoch, rec.effect_bits, op_sv, reason_sv, denied, rec.fiber_id);
        if (rec.seq + 1 > max_seq)
            max_seq = rec.seq + 1;
    }
    if (max_seq == 0)
        max_seq = 1;
    g_security_event_ring().seq.store(max_seq, std::memory_order_relaxed);
    return true;
}

void Evaluator::disable_security_event_wal() noexcept {
    aura::core::security_event_wal::g_security_event_wal().disable();
}

bool Evaluator::security_event_wal_enabled() const noexcept {
    return aura::core::security_event_wal::g_security_event_wal().is_enabled();
}

void Evaluator::grant_effect_capability(std::uint64_t tenant_id, std::string_view name,
                                        std::uint16_t effect_bits,
                                        std::uint64_t provenance_mutation_id) noexcept {
    using namespace ::aura::core::capability;
    // Issue #2074 / #2055: anti privilege-sticky + WorkspaceEpoch Mutation bind.
    // force_mutation_bind when sandbox active (Restricted/Strict or evaluator
    // sandbox_mode_). Always stamps non-zero grant_epoch + fiber_id.
    const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
    // Issue #2151: stamp grant with effect_fiber_id_or (override for tests).
    const auto fiber = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));
    auto prov = make_grant_provenance(provenance_mutation_id, force_bind, /*node_id=*/0, fiber);
    g_capability_registry().grant(tenant_id, name, static_cast<Effect>(effect_bits), prov);
    // Issue #2136: count Render grants (effect-only path when name empty;
    // named "render" also bumps via grant_capability below).
    if ((effect_bits & static_cast<std::uint16_t>(Effect::Render)) != 0 && name.empty()) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->render_effect_granted_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Also string-grant for legacy has_capability path.
    if (!name.empty())
        grant_capability(std::string(name));
}

// Issue #2055: revoke with WorkspaceEpoch Mutation stamp for audit trail.
void Evaluator::revoke_effect_capability(std::uint64_t tenant_id, std::string_view name) noexcept {
    using namespace ::aura::core::capability;
    auto ep = aura::core::current_mutation_epoch();
    if (ep == 0)
        ep = 1;
    g_capability_registry().revoke(tenant_id, name, ep);
    // Drop matching string grant so has_capability stays consistent.
    if (!name.empty()) {
        granted_capabilities_.erase(std::remove(granted_capabilities_.begin(),
                                                granted_capabilities_.end(), std::string(name)),
                                    granted_capabilities_.end());
    }
}

void Evaluator::set_effect_sandbox_mode(std::uint8_t mode) noexcept {
    using namespace ::aura::core::capability;
    using namespace ::aura::core::sandbox;
    using namespace ::aura::core::workspace_isolation;
    using namespace ::aura::core::provenance;
    if (mode > 2)
        mode = 2;
    g_capability_registry().sandbox_mode = static_cast<EffectSandboxMode>(mode);
    set_mode(static_cast<SandboxMode>(mode));
    // Strict/Restricted also set evaluator sandbox_mode_ so legacy gates engage.
    sandbox_mode_ = (mode != 0);
    // #1566: Strict sandbox links isolation enforcement.
    g_workspace_isolation().set_strict_sandbox_linked(mode == 2);
    // Issue #1877: under sandbox Strict, FailOnStale provenance policy —
    // no silent restamp of gen-stale StableNodeRef (multi-tenant AI
    // self-modify must fail closed rather than auto-refresh).
    if (mode == 2) {
        g_provenance_tracker().set_policy(AutoRefreshPolicy::FailOnStale);
        set_stable_ref_auto_refresh_policy(false);
        record_fail_on_stale_strict_sandbox();
        record_policy_enforced();
    } else if (g_provenance_tracker().get_policy() == AutoRefreshPolicy::FailOnStale) {
        // Leaving Strict: restore production default AutoRefreshOnBoundary.
        g_provenance_tracker().set_policy(AutoRefreshPolicy::AutoRefreshOnBoundary);
        set_stable_ref_auto_refresh_policy(true);
    }
}

std::uint8_t Evaluator::effect_sandbox_mode() const noexcept {
    return static_cast<std::uint8_t>(aura::core::capability::g_capability_registry().sandbox_mode);
}

// Issue #2076: unified Agent-readable deny reason formatter.
// Shape: "effect-denied: <EffectName> not granted tenant=<id> op=<op>"
// Stable string literal so Agents can parse / grep / dashboard.
static const char* effect_name_str(std::uint16_t effect_bits) noexcept {
    if (effect_bits & aura::compiler::security::kEffectMutate)
        return "mutate";
    if (effect_bits & aura::compiler::security::kEffectFfi)
        return "ffi";
    if (effect_bits & aura::compiler::security::kEffectNetwork)
        return "network";
    if (effect_bits & aura::compiler::security::kEffectExec)
        return "exec";
    if (effect_bits & aura::compiler::security::kEffectRender)
        return "render";
    if (effect_bits & aura::compiler::security::kEffectWrite)
        return "write";
    if (effect_bits & aura::compiler::security::kEffectRead)
        return "read";
    if (effect_bits & aura::compiler::security::kEffectMacroSelfEvo)
        return "macro-self-evo";
    return "unknown";
}

// format_deny_reason is header-inline in security_capabilities.h (#2076).
// Do NOT reopen `namespace aura::compiler::security` here while inside
// `namespace aura::compiler` — that creates the bogus nested path
// aura::compiler::aura::compiler::security (module ODR / link break).

// Issue #2076: production default Restricted sandbox + env override.
// Reads AURA_SANDBOX env var:
//   "off"        → set_effect_sandbox_mode(0) (legacy Off behavior)
//   "strict"     → set_effect_sandbox_mode(2)
//   "restricted" / unset / other → set_effect_sandbox_mode(1) (default Restricted)
//
// Call this early in main() / service startup so production deploys
// default to Restricted (the open-by-default gap closed by #2076).
// Dev/test fixtures call set_effect_sandbox_mode(0) explicitly to
// restore Off behavior.
void Evaluator::apply_env_sandbox() noexcept {
    // Issue #2053: full production security bundle (sandbox + audit + WAL).
    // Relative security:: is correct inside namespace aura::compiler.
    security::apply_production_security_defaults();
    // Mirror process-wide mode onto this Evaluator (sandbox_mode_, Strict link).
    const auto mode =
        static_cast<std::uint8_t>(::aura::core::capability::g_capability_registry().sandbox_mode);
    set_effect_sandbox_mode(mode);
    // Issue #2150: if production enabled WAL (env path OR forced multi-tenant
    // / Strict default dir), attach this Evaluator's ring + replay SecurityEvent.
    if (::aura::core::audit_wal::g_mutation_audit_wal().is_enabled()) {
        auto dir = ::aura::core::audit_wal::g_mutation_audit_wal().directory();
        if (dir.empty()) {
            // Fallback: re-resolve from env / default (same as force policy).
            dir = ::aura::core::audit_wal::resolve_mutation_audit_wal_dir();
        }
        if (!dir.empty())
            (void)enable_mutation_audit_wal(dir);
    }
}

// Issue #1566: multi-tenant workspace isolation bridge.
void Evaluator::set_tenant_principal(std::uint64_t tenant_id, std::string_view name,
                                     bool allow_cross) noexcept {
    using namespace ::aura::core::workspace_isolation;
    capability_tenant_id_ = tenant_id;
    g_workspace_isolation().set_current_tenant(tenant_id, name, allow_cross);
}

// Issue #2055: RAII TenantScope — snapshot principal at fiber entry so a
// stolen / resumed fiber cannot silently keep another tenant's principal.
Evaluator::TenantScope::TenantScope(Evaluator& ev, std::uint64_t tenant_id, std::string_view name,
                                    bool allow_cross) noexcept
    : ev_(&ev)
    , prev_tenant_(ev.capability_tenant_id())
    , fiber_id_(static_cast<std::uint32_t>(aura_fiber_current_id()))
    , active_(true) {
    ev.set_tenant_principal(tenant_id, name, allow_cross);
}

Evaluator::TenantScope::~TenantScope() noexcept {
    release();
}

void Evaluator::TenantScope::release() noexcept {
    if (!active_ || !ev_)
        return;
    // Restore prior principal (name empty → keep id only).
    ev_->set_capability_tenant_id(prev_tenant_);
    using namespace ::aura::core::workspace_isolation;
    g_workspace_isolation().set_current_tenant(prev_tenant_, {}, false);
    active_ = false;
}

void Evaluator::grant_cross_tenant_access(std::uint64_t from_tenant, std::uint64_t to_tenant,
                                          std::uint16_t effect_bits) noexcept {
    using namespace ::aura::core::workspace_isolation;
    g_workspace_isolation().grant_cross_tenant(from_tenant, to_tenant, effect_bits);
}

bool Evaluator::check_workspace_isolation(std::uint64_t target_tenant, std::uint64_t ref_tenant,
                                          std::uint16_t required_effects,
                                          std::string_view op) noexcept {
    // Issue #1566: workspace isolation bridge. Use explicit `using`
    // declarations with `::aura::core::` (absolute path) because we're
    // inside namespace `aura::compiler` and `aura::core::` would
    // otherwise resolve as nested (`aura::compiler::aura::core::`)
    // which doesn't exist.
    using ::aura::core::sandbox::is_strict;
    using ::aura::core::workspace_isolation::check_boundary;
    using ::aura::core::workspace_isolation::g_workspace_isolation;
    using ::aura::core::workspace_isolation::IsolationRefProvenance;
    const auto target = target_tenant != 0 ? target_tenant : capability_tenant_id_;
    const bool strict =
        effect_sandbox_mode() == 2 || is_strict() || g_workspace_isolation().strict_sandbox_linked;
    IsolationRefProvenance prov{};
    prov.tenant_id = ref_tenant;
    const bool ok = check_boundary(target, &prov, required_effects, strict, op);
    if (!ok) {
        bump_capability_denial();
        // Issue #2075: shared SecurityEvent surface — also append to the
        // unified audit ring so query:security-audit-trail covers
        // isolation denies alongside effect denies. Use ::aura::core::
        // (absolute path) because we're inside namespace aura::compiler
        // and `aura::core::` would otherwise resolve as nested
        // (aura::compiler::aura::core::) which doesn't exist.
        //
        // Issue #2156: mutation_id must be real Mutation epoch / audit join
        // space — NEVER a tenant id. Pre-#2156 this path wrote
        // ref_tenant/target into mutation_id, polluting trail_find_by_mutation_id
        // and query:security-audit forensic joins. Tenant stays in tenant_id;
        // foreign ref principal is encoded in the reason string when present.
        using ::aura::core::security_event::append_security_event;
        using ::aura::core::security_event::g_security_event_ring;
        using ::aura::core::security_event::kIsolationAuditMidIssue;
        using ::aura::core::security_event::SecurityEventKind;
        const auto fiber = static_cast<std::int64_t>(aura_fiber_current_id());
        const auto epoch = ::aura::core::current_mutation_epoch();
        // Prefer Mutation epoch as mid (join with effect denials on same attempt).
        // Non-zero stamp: epoch==0 is "unset" — use 1 for joinability (grant paths).
        const auto mid = epoch != 0 ? epoch : 1;
        char reason_buf[64];
        const char* reason_str = "isolation-deny";
        if (ref_tenant != 0) {
            // Keep foreign principal for Agents without polluting mutation_id.
            std::snprintf(reason_buf, sizeof(reason_buf), "isolation-deny:ref-tenant=%llu",
                          static_cast<unsigned long long>(ref_tenant));
            reason_str = reason_buf;
        }
        (void)kIsolationAuditMidIssue; // stamp for Agent / docs grep
        append_security_event(g_security_event_ring(), SecurityEventKind::IsolationDeny, target,
                              mid, epoch != 0 ? epoch : mid,
                              static_cast<std::uint16_t>(required_effects), op, reason_str,
                              /*denied=*/true, fiber);
        // #2225: durable mirror of the isolation-deny — #2156 mid
        // (epoch, never tenant) is what the WAL persists so replay
        // restores the Agent's joinable mutation_id.
        ::aura::core::security_event_wal::persist_security_event(
            SecurityEventKind::IsolationDeny, target, mid, epoch != 0 ? epoch : mid,
            static_cast<std::uint16_t>(required_effects), op, reason_str,
            /*denied=*/true, fiber,
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count()));
        // #2054 / #2156: correlate isolation deny into TypedMutation trail
        // with the same mid as SecurityEvent (never tenant id).
        typed_audit::capture_security_correlated_audit(mid, op, epoch != 0 ? epoch : mid,
                                                       /*denied=*/true, /*target_node=*/0, fiber);
    }
    return ok;
}

void Evaluator::stamp_ref_tenant(ast::FlatAST::StableNodeRef& ref) const noexcept {
    // Issue #2056: full stamp (tenant + fiber) — alias of stamp_stable_ref.
    stamp_stable_ref(ref);
}

// Issue #2056: mandate tenant_id + fiber provenance on every StableNodeRef
// handed to Agent / user code. Central create/rebind helper.
void Evaluator::stamp_stable_ref(ast::FlatAST::StableNodeRef& ref) const noexcept {
    const auto fiber = static_cast<std::uint32_t>(aura_fiber_current_id());
    ::aura::core::provenance::stamp_stable_ref_fields(ref, capability_tenant_id_, fiber);
}

ast::FlatAST::StableNodeRef Evaluator::make_stamped_ref(ast::NodeId id) const noexcept {
    ast::FlatAST::StableNodeRef ref{};
    if (workspace_flat_)
        ref = workspace_flat_->make_ref(id);
    else
        ref.id = id;
    stamp_stable_ref(ref);
    return ref;
}

ast::FlatAST::StableNodeRef
Evaluator::make_stamped_safe_ref(ast::NodeId id, std::uint32_t workspace_id,
                                 std::uint32_t fiber_id) const noexcept {
    ast::FlatAST::StableNodeRef ref{};
    const auto fiber =
        fiber_id != 0 ? fiber_id : static_cast<std::uint32_t>(aura_fiber_current_id());
    if (workspace_flat_)
        ref = workspace_flat_->make_safe_ref(id, workspace_id, fiber);
    else {
        ref.id = id;
        ref.fiber_id = fiber;
    }
    stamp_stable_ref(ref);
    return ref;
}

// Issue #2224: sole public outbound helper — every StableNodeRef handed to
// Agent / user code MUST go through export_ref / export_ref_safe so the
// tenant + fiber stamp is guaranteed. Parity with #2152 dispatch
// required_effects: side effects are non-bypassable; isolation should match.
// Underlying call still routes through make_stamped_* so semantics are
// identical; export_ref just locks the Agent-facing surface.
ast::FlatAST::StableNodeRef Evaluator::export_ref(ast::NodeId id) const noexcept {
    return make_stamped_ref(id);
}

ast::FlatAST::StableNodeRef Evaluator::export_ref_safe(ast::NodeId id, std::uint32_t workspace_id,
                                                       std::uint32_t fiber_id) const noexcept {
    return make_stamped_safe_ref(id, workspace_id, fiber_id);
}

// Issue #2224: shared resolve entry — query / mutate / ast-walk paths
// must call resolve_stamped before touching the slot. Order of checks:
//   1. workspace_flat_ present? (no → nullopt)
//   2. isolation check: ref.tenant_id against current principal; under
//      Strict / Restricted + cross-tenant ref → deny (last_mutate_error_
//      populated with "isolation-deny: ref-tenant=N"; metrics:
//      tenant_boundary_violation_prevented_total /
//      cross_tenant_provenance_deny_total bumped inside
//      check_workspace_isolation).
//   3. FlatAST validity: get_safe(ref) (gen + COW epoch check inside
//      FlatAST) — stale / free-slot / cross-tenant ref → nullopt with
//      reason "resolve-stamped: stale-ref" so callers can tell apart
//      isolation deny from gen-mismatch.
//   4. optional required_effects flow (per #2152 dispatch parity: when
//      ref carries tenant we still allow req_effects=0 for pure reads,
//      but query:mutate-style paths pass nonzero to ensure capability
//      gate is consulted — out of scope for #2224 AC2-AC4, wired
//      here for the next audit's coverage).
std::optional<ast::NodeView> Evaluator::resolve_stamped(const ast::FlatAST::StableNodeRef& ref,
                                                        std::uint16_t required_effects,
                                                        std::string_view op) noexcept {
    if (!workspace_flat_) {
        last_mutate_error_ = std::string(op) + ": no workspace";
        return std::nullopt;
    }
    // Stage 1: isolation. ref.tenant_id == 0 (legacy / unstamped) is
    // allowed only when isolation is off OR principal is unset (tenant
    // 0 + sandbox off → legacy permissive, see AC4 / #2056). Under
    // Strict / Restricted with isolation on, tenant 0 is denied.
    if (!check_workspace_isolation(/*target=*/ref.tenant_id, /*ref_tenant=*/ref.tenant_id,
                                   required_effects, op)) {
        // last_mutate_error_ already set by check_workspace_isolation.
        // Augment with ref context for Agent-readable trail.
        if (last_mutate_error_.empty()) {
            last_mutate_error_ =
                std::string(op) + ": isolation-deny: ref-tenant=" + std::to_string(ref.tenant_id);
        }
        return std::nullopt;
    }
    // Stage 2: FlatAST validity (gen + COW + workspace_id match).
    // get_safe already returns nullopt for stale / out-of-bounds / free
    // slots; distinguish from isolation deny with a clear reason.
    auto opt = workspace_flat_->get_safe(ref);
    if (!opt.has_value()) {
        last_mutate_error_ = std::string(op) + ": stale-ref id=" + std::to_string(ref.id) +
                             " gen=" + std::to_string(ref.gen);
        return std::nullopt;
    }
    last_mutate_error_.clear();
    return opt;
}

} // namespace aura::compiler

// apply_aura_sandbox_env / apply_production_security_defaults:
// header-inline in security_defaults.hh (included via security_capabilities.h).
