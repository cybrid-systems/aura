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
#include "core/resource_quota.hh" // #2384: host provenance_mutation_id for require_effect
#include "core/sandbox.hh"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"
#include "core/mutation_audit_wal.hh"
#include "core/provenance_tracker.hh"
#include "core/security_event.hh"     // #2075: shared SecurityEvent surface
#include "core/security_event_wal.hh" // #2225: durable SecurityEvent side-car WAL
#include "observability_metrics.h"
// Issue #2883: resume_had_mismatch / g_current_fiber for hard principal deny.
#include "serve/fiber.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;

namespace aura::compiler {

// Issue #918: explicit using-declarations (no using-namespace).
using security::kCapWildcard;

// Issue #2077 / #2387: unify has_capability string path with Effect matrix
// (single source of truth). When `effect_for_cap_name(needed) != None`
// the Effect bit in `g_capability_registry().effects_for(tenant)` is
// consulted — grant_capability / grant_effect_capability already mirror
// these names into the registry, and now has_capability reads them
// from there too. Wildcard "*" still grants everything via the
// explicit string-grant path; it also maps to the full effect mask
// so an effect-only grant (without pushing "*" as a string) can
// satisfy wildcard queries if every bit is held.
// Issue #2387: tenant-admin + syscall promoted into the matrix (epoch /
// fiber bind).
// Issue #2489: remaining high-risk sensitive caps promoted —
//   self-evo / synthesize / strategy / sys-open / sys-write / sys-read /
//   agent / capability now also map to matrix bits (MacroSelfEvo / Syscall
//   / TenantAdmin). Caps that remain intentionally string-only
//   (effect_for_cap_name == None, SECURITY_EXEMPT staged):
//   compile, compile-stats, compile-dirty, compile-deopt, fiber, workspace,
//   exception-control, macro, query, sandbox.
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
    using namespace ::aura::core::capability;
    const auto eff = effect_for_cap_name(cap);

    // Issue #3141: production fence — wildcard-only holder cannot write
    // privilege-bearing cap names without explicit TenantAdmin. Gate BEFORE
    // pushing to granted_capabilities_ so dedup state stays consistent on
    // deny. AC3 Soft/Off zero-cost (sandbox_mode atomic relaxed load).
    if (eff != Effect::None) {
        auto& reg = g_capability_registry();
        std::lock_guard<std::mutex> lock(reg.mtx);
        if (!try_grant_capability_string_path_privileged_locked(capability_tenant_id_, cap,
                                                                static_cast<std::uint16_t>(eff))) {
            return; // AC1 deny: skip both push and effect-grant
        }
    }

    granted_capabilities_.push_back(std::move(cap));
    // #1565 / #2055: mirror named grant into effect matrix for current tenant.
    // Stamp WorkspaceEpoch Mutation + fiber (not Bridge) so grant_epoch matches
    // the mutation epoch at grant time and long-running blame stays consistent.
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
    // Issue #3056: append stays fail-open — disk error does not abort
    // the mutation commit (AC3). Posture arm is observe-only.
    if (g_mutation_audit_wal().is_enabled()) {
        const auto rec = make_record(slot.seq, slot.timestamp_ms, slot.fiber_id, slot.nodes_changed,
                                     slot.epoch_delta, slot.target_node, slot.op, slot.effect_bits,
                                     slot.tenant_id, slot.provenance_mutation_id, slot.epoch,
                                     slot.effect_denied);
        (void)g_mutation_audit_wal().append(rec);
    }
}

// Issue #1565 / #1876 / #2706: private capability effect check + audit.
// Production entry is require_effect / require_effect_on_ref only (#2706).
// #1876: under sandbox, also validate/record StableNodeRef provenance and
// bump sandbox_violations_total + capability_denials_by_effect metrics.
bool Evaluator::check_and_record_effect(std::uint16_t required_effect_bits,
                                        std::uint16_t actual_effect_bits, std::string_view op,
                                        ast::NodeId target_node, std::uint64_t tenant_id,
                                        std::uint64_t provenance_mutation_id) noexcept {
    using namespace ::aura::core::capability;
    using namespace ::aura::core::sandbox;
    using namespace ::aura::core::provenance;

    // Issue #2657: removed inline direct writes to
    // g_capability_registry().sandbox_mode. The process-wide authority
    // aura::core::sandbox::set_mode is the SOLE writer (atomic + plain
    // enum + registry + workspace_isolation + provenance_tracker).
    // Direct writes from per-call sites leaked drift (Strict in name
    // but Restricted/Off in registers) — the historical hot-fix loop
    // is now unnecessary because the authority keeps the stores in
    // agreement at every set.

    EffectProvenance prov;
    prov.node_id = static_cast<std::uint32_t>(target_node);
    prov.mutation_id = provenance_mutation_id;
    // Issue #2151: effect_fiber_id_or lets tests simulate fiber A vs B;
    // production leaves override at 0 → live TLS fiber id.
    prov.fiber_id = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));

    // Issue #2883: production hard principal check on side-effect entry.
    // If the current fiber resume had a hard principal mismatch under
    // production/Restricted (ambient worker principal diverged from
    // fiber `assigned_tenant_id`), deny the side-effect at entry with SE
    // reason `fiber-principal-mismatch` rather than silently letting an
    // ambient principal escape through. Soft / Off path: the flag stays
    // false (set only by hard path), so this check is a no-op. Off path
    // never flips the flag so the existing soft metric-only behaviour
    // is preserved.
    if (sandbox_mode_ != 0 || effect_sandbox_mode() != 0) {
        // Issue #2883: live TLS fiber (serve::g_current_fiber). Metric lives on
        // CapabilityEffectMetrics (capability_model.hh), not CompilerMetrics.
        if (auto* f = ::aura::serve::g_current_fiber) {
            if (f->resume_had_mismatch()) {
                // Bump hard-deny counter distinct from mismatch-detected.
                f->bump_fiber_principal_mismatch_hard_deny();
                g_capability_effect_metrics()
                    .capability_fiber_principal_mismatch_hard_deny_total.fetch_add(
                        1, std::memory_order_relaxed);
                using ::aura::core::security_event::SecurityEventKind;
                using ::aura::core::security_event_wal::emit_security_event_durable;
                const auto epoch = ::aura::core::current_mutation_epoch();
                const auto mid = provenance_mutation_id != 0
                                     ? provenance_mutation_id
                                     : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
                const auto fid = static_cast<std::int64_t>(f->id());
                // Tenant: prefer caller's tenant_id; fall back to evaluator ambient.
                const auto tenant =
                    tenant_id != 0 ? tenant_id : static_cast<std::uint64_t>(capability_tenant_id_);
                // SE reason: 'fiber-principal-mismatch' (same shape as the
                // resume-time SE so Agent dashboards chart a single reason).
                emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                            /*effect_bits=*/required_effect_bits,
                                            /*op=*/op,
                                            /*reason=*/"fiber-principal-mismatch",
                                            /*denied=*/true, fid);
                return false; // hard deny — no partial mutate, no mid auto-bump.
            }
        }
    }
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

    // Issue #2388: CapabilityRegistry::record_audit dual-writes SecurityEvent
    // + WAL (single path). fiber-grant-mismatch reason is stamped there via
    // hard-deny counter delta (#2151). Do not append SE again below.
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
            // Issue #3056: fail-open (AC3) — same as emit_mutation_audit.
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

    // Issue #2388: SecurityEvent ring + WAL come from capability::record_audit
    // (emit_security_event_durable). Keep TypedMutationAudit correlation only
    // here so Agents still join by mutation_id without double-counting SE.
    // mid matches capability dual-write (prov.mutation_id / epoch, not tenant).
    {
        const auto mid = provenance_mutation_id != 0
                             ? provenance_mutation_id
                             : (prov.epoch != 0 ? prov.epoch : static_cast<std::uint64_t>(1));
        typed_audit::capture_security_correlated_audit(mid, op, prov.epoch, /*denied=*/!ok,
                                                       static_cast<std::uint32_t>(target_node),
                                                       slot.fiber_id);
    }
    g_sandbox_state().effect_checks++;
    return ok;
}

// Issue #2072 / #2384: single production entry for new side-effect paths.
// Wraps check_and_record_effect with the standard arguments (required =
// actual = req_bits, tenant = capability_tenant_id_).
//
// Issue #2384: NEVER hardcode provenance_mutation_id=0. Live mid is
// required so CapabilityGrant::bound_mutation_id / provenance_ok can
// fire on FFI/network/exec/render/hotpath, and so SecurityEvent +
// TypedMutationAudit join by mutation_id (not seq-only fallback).
// Stamp order (WorkspaceEpoch Mutation vocabulary, #2149):
//   1. process ResourceQuota host provenance_mutation_id when set
//   2. current_mutation_epoch() when non-zero
//   3. non-zero join stamp 1 (process origin)
// Soft / Off paths unchanged beyond filling mid (still allow when no
// grant required). Callers that already pass a real mid keep using
// check_and_record_effect directly.
// All new FFI / network / exec / render / hotpath entry points MUST
// go through require_effect (not call check_and_record_effect
// directly) so the audit ring + capability metrics surface stays
// consistent. Returns true on allow, false on deny.
//
// Issue #2490: require_effect is the single side-effect entry —
// auto-enforce workspace isolation when req_bits != 0 so callers cannot
// skip isolation by only calling require_effect. Pure / zero-bits callers
// are unchanged. Single SE IsolationDeny count is preserved via #2388
// (check_workspace_isolation emits at most one IsolationDeny; callers
// that already pair the checks short-circuit on the first deny).
// Issue #2658: optional `ref_tenant` carries StableNodeRef provenance so
// cross-tenant refs deny under Restricted/Strict BEFORE the effect runs
// (previously the auto-isolation check hardcoded ref_tenant=0 and the
// foreign-tenant gate was deferred to resolve_stamped / StableNodeRef
// access sites — a window where the effect could partially run before
// the late isolation deny). Default ref_tenant=0 keeps the legacy
// three-arg call shape unchanged; new code paths that hold a stamped
// ref should pass `ref.tenant_id` (or use require_effect_on_ref below).
//
// Issue #2689: mandate the on_ref overload on every StableNodeRef side-effect
// path. Coverage linter `check_side_effect_security.py` flags `require_effect(`
// inside a function/lambda that also names a `StableNodeRef` parameter/local
// without `ref_tenant` / `require_effect_on_ref` (allowlist only documented
// NodeId-only paths). Test extension per #81967 in
// `test_require_effect_auto_isolation.cpp` covers the foreign-tenant
// matrix under Restricted + Strict. Closes the residual late-isolation
// window after #2658 for paths outside `mutate:force`.
bool Evaluator::require_effect(std::uint16_t req_bits, std::string_view op, ast::NodeId target_node,
                               std::uint64_t ref_tenant) noexcept {
    // #3109: fail-closed deny at entry (Strict + overflow ring full).
    // #3302: fail-closed is force_wal-defaulted or AURA_WAL_APPEND_FAIL_CLOSED.
    if (req_bits != 0 && ::aura::core::wal_slo::wal_append_fail_closed_active() &&
        ::aura::core::security_event_wal::wal_overflow_ring_full() &&
        ::aura::core::sandbox::is_strict())
        return false;
    if (req_bits != 0) {
        if (!check_workspace_isolation(/*target=*/capability_tenant_id_,
                                       /*ref_tenant=*/ref_tenant, req_bits, op))
            return false; // IsolationDeny emitted (single-count, #2388)
    }
    // Issue #3296 AC1: SSOT order under Restricted/Strict is TypedMid
    // (boundary-stamped) first, then the process-global epoch fallback,
    // then 1 for Soft / standalone. Drop the host-quota mid from the
    // production cascade: quota can drift / lag relative to the
    // TypedMid that was live when the grant was issued (steal × abort
    // × boundary enter clears quota + epoch while TypedMid remains).
    // Soft / Off contract unchanged: TypedMid == 0 falls through to 1
    // with zero extra atomics.
    std::uint64_t mid = typed_audit::last_type_linear_commit_proof_stamp_v_read();
    if (mid == 0)
        mid = ::aura::core::current_mutation_epoch();
    if (mid == 0)
        mid = 1; // Soft / standalone: non-zero join stamp (process origin)
    return check_and_record_effect(req_bits, req_bits, op, target_node, capability_tenant_id_, mid);
}

// Issue #2658: thin helper for call sites that already hold a stamped
// ast::FlatAST::StableNodeRef. Extracts ref.tenant_id + ref.id and routes
// through require_effect so the cross-tenant isolation deny fires at the
// same point as the capability check (no late-isolation-deny window).
// Use this when a side-effect path holds a stamped handle (mutate prims,
// fiber mutation, render batch with cross-tenant handles, FFI on stamped
// node, etc.) — callers that don't have a ref in hand keep using the
// positional require_effect(req_bits, op, target_node) form.
bool Evaluator::require_effect_on_ref(std::uint16_t req_bits, std::string_view op,
                                      const ast::FlatAST::StableNodeRef& ref) noexcept {
    return require_effect(req_bits, op, ref.id, ref.tenant_id);
}

// Issue #2839: NodeId-only side-effect entry. Force-construct stamped ref
// from the current principal then require_effect_on_ref so Restricted /
// Strict isolation + capability deny before body (no partial mutate on
// foreign-tenant NodeId under principal-unset or wrong principal). Prefer
// this over 2-arg require_effect when the op mutates a concrete NodeId.
// Exempt: non-workspace side effects (file/io/network) remain 2-arg with
// documented rationale in the coverage linter inventory.
//
// Issue #2881: residual NodeId-only workspace side-effect coverage. Every
// prim that mutates a concrete NodeId must go through this helper (or
// require_effect_on_ref with a stamped ref). Bare 2-arg require_effect is
// only allowed for non-workspace ops (file/io/network/exec with no NodeId
// target). The coverage linter inventory lives in
// scripts/coverage/checks/check_side_effect_fiber_principal_2839.py and
// tracks both exempt ops and per-file scope. New mutating prims MUST use
// this helper; refactor sites that hit the late-isolation-deny window are
// not allowed after #2881.
//
// Issue #2942: mandate this helper (or require_effect_on_ref) on ALL
// workspace NodeId side-effect prims — closes residual late-isolation
// window after #2881 (add_mutate + any new NodeId path). Bare 2-arg
// require_effect remains only for documented non-workspace ops
// (EXEMPT_2ARG_OPS). Coverage linter:
// scripts/coverage/checks/check_side_effect_node_id_mandate_2942.py.
//
// Issue #3040: residual compile:/verify:/syntax: NodeId writers must
// call this (or require_effect_on_ref for a stamped foreign tenant)
// BEFORE Guard / topology write — no 2-arg default ref_tenant=0
// overload (that re-opens the late-isolation window). Deny bumps
// nodeid_only_entry_prevented_total (Soft/Off allow path does not store).
bool Evaluator::require_effect_for_node_id(std::uint16_t req_bits, std::string_view op,
                                           ast::NodeId node_id) noexcept {
    // make_stamped_ref stamps capability_tenant_id_ + fiber so ref_tenant
    // matches principal — isolation auto-gate (#2490) then runs with a
    // non-zero ref_tenant (closes 3-arg default ref_tenant=0 window).
    const auto ref = make_stamped_ref(node_id);
    const bool ok = require_effect_on_ref(req_bits, op, ref);
    if (!ok) {
        using ::aura::core::workspace_isolation::g_tenant_isolation_metrics;
        g_tenant_isolation_metrics().nodeid_only_entry_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    return ok;
}

// Issue #2706: test-only public surface — forwards to private
// check_and_record_effect. Unit Soft paths that need explicit mid or
// required≠actual bits use this; production prims must not.
bool Evaluator::check_and_record_effect_for_test(std::uint16_t required_effect_bits,
                                                 std::uint16_t actual_effect_bits,
                                                 std::string_view op, ast::NodeId target_node,
                                                 std::uint64_t tenant_id,
                                                 std::uint64_t provenance_mutation_id) noexcept {
    return check_and_record_effect(required_effect_bits, actual_effect_bits, op, target_node,
                                   tenant_id, provenance_mutation_id);
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
                                        std::uint64_t provenance_mutation_id,
                                        bool single_use) noexcept {
    using namespace ::aura::core::capability;
    // Issue #2074 / #2055: anti privilege-sticky + WorkspaceEpoch Mutation bind.
    // force_mutation_bind when sandbox active (Restricted/Strict or evaluator
    // sandbox_mode_). Always stamps non-zero grant_epoch + fiber_id.
    const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
    // Issue #2151: stamp grant with effect_fiber_id_or (override for tests).
    const auto fiber = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));
    auto prov = make_grant_provenance(provenance_mutation_id, force_bind, /*node_id=*/0, fiber);
    // Issue #2882: production default single-use override. Under production
    // defaults (sandbox_mode_ != 0 || effect_sandbox_mode() != 0) any grant
    // touching a high-risk effect (Mutate | MacroSelfEvo | TenantAdmin |
    // Syscall) is force-promoted to single_use=true regardless of caller
    // intent. Privilege-sticky Mutate / MacroSelfEvo / TenantAdmin grants
    // are the practical bypass vector for epoch-fenced self-modify in
    // commercial multi-tenant loads after #2586; the production surface
    // must close this. Explicit durable admin path goes through
    // grant_effect_durable() which is auditable and bumps a separate
    // counter (capability_durable_high_risk_grant_total).
    using aura::compiler::security::kEffectMacroSelfEvo;
    using aura::compiler::security::kEffectMutate;
    using aura::compiler::security::kEffectSyscall;
    using aura::compiler::security::kEffectTenantAdmin;
    constexpr std::uint16_t kHighRiskMask = static_cast<std::uint16_t>(
        kEffectMutate | kEffectMacroSelfEvo | kEffectTenantAdmin | kEffectSyscall);
    const bool production_defaults = force_bind;
    const bool is_high_risk = (effect_bits & kHighRiskMask) != 0;
    if (production_defaults && is_high_risk && !single_use) {
        single_use = true;
        g_capability_effect_metrics().capability_high_risk_forced_single_use_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    // Issue #2586: single_use flag forwarded to registry grant (auto-revoke
    // after first successful check_and_record_effect that uses the bits).
    // Issue #2968 AC2: granting effects onto a foreign tenant id under
    // production (Restricted/Strict) requires TenantAdmin — without the
    // meta-privilege an Agent holding a weaker path could seed foreign-
    // tenant grants in the process-global by_tenant table (multi-tenant
    // isolation then depends on grant writers being gated). Same-tenant
    // self-grant (tenant_id == capability_tenant_id_ or tenant_id == 0)
    // stays on the existing Mutate/capability policy. AC3: Off/Soft path
    // short-circuits before any privilege lookup.
    const auto self_tenant = static_cast<std::uint64_t>(capability_tenant_id_);
    const bool foreign_target = tenant_id != 0 && tenant_id != self_tenant;
    auto& reg = g_capability_registry();
    if (force_bind && foreign_target) {
        using ::aura::core::capability::Effect;
        // Issue #3126: take the registry mtx + use effects_for_locked so a
        // concurrent revoke of TenantAdmin from another Evaluator / fiber
        // cannot race past this fence. The previous has_capability() call
        // (unlocked read of by_tenant via effects_for) was a TOCTOU window
        // that allowed a momentarily-held TenantAdmin to land a foreign
        // grant before revocation.
        std::lock_guard<std::mutex> lock(reg.mtx);
        const auto held = reg.effects_for_locked(self_tenant);
        const bool is_admin = has_effect(held, Effect::TenantAdmin);
        if (!is_admin) {
            using ::aura::core::security_event::SecurityEventKind;
            using ::aura::core::security_event_wal::emit_security_event_durable;
            const auto epoch = ::aura::core::current_mutation_epoch();
            const auto mid = provenance_mutation_id != 0
                                 ? provenance_mutation_id
                                 : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
            const auto fid = static_cast<std::int64_t>(fiber);
            using ::aura::core::workspace_isolation::g_tenant_isolation_metrics;
            g_tenant_isolation_metrics().cross_tenant_grant_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant_id, mid, epoch,
                                        effect_bits, name, "cross-tenant-grant-needs-tenant-admin",
                                        /*denied=*/true, fid);
            return; // deny — no registry grant, no allow-counter bump
        }
        // Issue #3126: act via grant_locked (caller holds mtx); no nested lock.
        reg.grant_locked(tenant_id, name, static_cast<Effect>(effect_bits), prov, single_use);
    } else {
        // Issue #3362: same-tenant self-grant still requires an admin fence
        // for high-risk bits (TenantAdmin | MacroSelfEvo | Syscall | Mutate)
        // under production. Without this fence, a wildcard-only holder can
        // use security:grant-effect! to write explicit TA into the registry,
        // then bypass the #3141 string fence (which denies but does not roll
        // back the registry write) and the #3144 effects_for strip — the
        // residual privilege-escalation path this issue closes.
        if (force_bind && is_high_risk) {
            std::lock_guard<std::mutex> lock(reg.mtx);
            const auto held = reg.effects_for_locked(self_tenant);
            if (!has_effect(held, Effect::TenantAdmin)) {
                using ::aura::core::security_event::SecurityEventKind;
                using ::aura::core::security_event_wal::emit_security_event_durable;
                const auto epoch = ::aura::core::current_mutation_epoch();
                const auto mid = provenance_mutation_id != 0
                                     ? provenance_mutation_id
                                     : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
                const auto fid = static_cast<std::int64_t>(fiber);
                // Reuse the #3141 wildcard-write-fence counter (additive, no
                // new metric per non-goals).
                g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.fetch_add(
                    1, std::memory_order_relaxed);
                emit_security_event_durable(SecurityEventKind::EffectDeny, tenant_id, mid, epoch,
                                            effect_bits, name,
                                            "grant-effect-needs-explicit-tenant-admin",
                                            /*denied=*/true, fid);
                return; // deny — no registry write, no allow-counter bump
            }
            // String fence MUST run BEFORE the registry write for high-risk
            // bits so a deny at the #3141 string-path layer does not leave
            // TA/MSE rows behind in by_tenant (the issue's "fence → string →
            // registry" ordering).
            if (!try_grant_capability_string_path_privileged_locked(
                    self_tenant, name, static_cast<std::uint16_t>(effect_bits))) {
                return; // string fence denied — no registry write
            }
            reg.grant_locked(tenant_id, name, static_cast<Effect>(effect_bits), prov, single_use);
        } else {
            // Same-tenant low-risk (or Soft/Off): no fence, plain grant().
            reg.grant(tenant_id, name, static_cast<Effect>(effect_bits), prov, single_use);
        }
    }
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

// Issue #2882: explicit durable admin path for high-risk grants. Bypasses
// the production-default single-use override (capability_high_risk_
// forced_single_use_total stays at 0) and bumps the durable override
// counter so Agent dashboards can chart privilege-sticky admin grants
// separately from the auto-revoke common-case. Use only when audit
// rationale is established — long-lived Mutate / MacroSelfEvo /
// TenantAdmin / Syscall grants remain sticky for the lifetime of the
// grant even with epoch binding (#2074) and retain windows.
//
// Issue #2967: under production (Restricted/Strict) the durable surface is
// gated at the call site — the caller principal must hold
// Effect::TenantAdmin (or the string caps "tenant-admin" / "capability"
// that map to it) AND pass a non-empty agent-stable audit reason.
// Missing privilege → deny + SE reason 'durable-grant-needs-tenant-admin';
// empty reason → deny + SE reason 'durable-grant-reason-required'. Both
// deny paths bump capability_durable_grant_deny_total and do NOT bump the
// allow counter (AC4: durable counter only when allow). Soft/Off path has
// zero added cost (AC3: gate short-circuits before any privilege lookup).
void Evaluator::grant_effect_durable(std::uint64_t tenant_id, std::string_view name,
                                     std::uint16_t effect_bits,
                                     std::uint64_t provenance_mutation_id,
                                     std::string_view reason) noexcept {
    using namespace ::aura::core::capability;
    const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
    const auto fiber = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));
    auto prov = make_grant_provenance(provenance_mutation_id, force_bind, /*node_id=*/0, fiber);
    // Bump the durable high-risk counter when this durable override touches
    // a high-risk effect bit (Mutate / MacroSelfEvo / TenantAdmin / Syscall).
    // Non-high-risk durable grants are tracked only via capability_grant_total.
    using aura::compiler::security::kCapCapability;
    using aura::compiler::security::kCapTenantAdmin;
    using aura::compiler::security::kEffectMacroSelfEvo;
    using aura::compiler::security::kEffectMutate;
    using aura::compiler::security::kEffectSyscall;
    using aura::compiler::security::kEffectTenantAdmin;
    constexpr std::uint16_t kHighRiskMask = static_cast<std::uint16_t>(
        kEffectMutate | kEffectMacroSelfEvo | kEffectTenantAdmin | kEffectSyscall);
    const bool is_high_risk = (effect_bits & kHighRiskMask) != 0;
    // Issue #2969 AC1: registry write-fence — under production
    // (Restricted/Strict), writing a grant for a FOREIGN tenant id
    // (tenant_id != 0 && tenant_id != capability_tenant_id_) requires
    // TenantAdmin (or the "capability" meta-cap). Without the fence a
    // weaker caller could seed foreign-tenant grants in the process-global
    // by_tenant table. AC3: Soft/Off short-circuits before any privilege
    // lookup; AC2: same-tenant self-grant keeps existing policy.
    // Issue #3126: take the registry mtx + use effects_for_locked (single
    // computation shared with the high-risk TenantAdmin + reason gate
    // #2967 below) so a concurrent revoke of TenantAdmin from another
    // Evaluator / fiber cannot race past this fence. Previous
    // has_capability() (unlocked read of by_tenant via effects_for) was
    // a TOCTOU window.
    const auto self_tenant = static_cast<std::uint64_t>(capability_tenant_id_);
    const bool foreign_target = tenant_id != 0 && tenant_id != self_tenant;
    auto& reg_durable = g_capability_registry();
    std::lock_guard<std::mutex> lock(reg_durable.mtx);
    const auto held_durable = reg_durable.effects_for_locked(self_tenant);
    const bool is_admin = has_effect(held_durable, Effect::TenantAdmin);
    if (force_bind && foreign_target && !is_admin) {
        using ::aura::core::security_event::SecurityEventKind;
        using ::aura::core::security_event_wal::emit_security_event_durable;
        const auto epoch = ::aura::core::current_mutation_epoch();
        const auto mid = provenance_mutation_id != 0
                             ? provenance_mutation_id
                             : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
        const auto tenant = tenant_id != 0 ? tenant_id : self_tenant;
        const auto fid = static_cast<std::int64_t>(fiber);
        aura::core::capability::g_capability_effect_metrics()
            .capability_grant_foreign_tenant_deny_total.fetch_add(1, std::memory_order_relaxed);
        emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch, effect_bits,
                                    name, "grant-foreign-tenant-needs-tenant-admin",
                                    /*denied=*/true, fid);
        return; // deny — no registry write, no allow-counter bump (AC4)
    }
    // Issue #2967: production gate — TenantAdmin + mandatory reason.
    // AC3: Soft / Off (sandbox_mode_ == 0 && effect_sandbox_mode() == 0)
    // short-circuits here: zero-cost legacy path, no privilege lookup.
    if (force_bind && is_high_risk) {
        using ::aura::core::security_event::SecurityEventKind;
        using ::aura::core::security_event_wal::emit_security_event_durable;
        const auto epoch = ::aura::core::current_mutation_epoch();
        const auto mid = provenance_mutation_id != 0
                             ? provenance_mutation_id
                             : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
        const auto tenant =
            tenant_id != 0 ? tenant_id : static_cast<std::uint64_t>(capability_tenant_id_);
        const auto fid = static_cast<std::int64_t>(fiber);
        // AC1: caller principal must hold TenantAdmin (string caps
        // "tenant-admin" / "capability" map to the same Effect bit via
        // effect_for_cap_name → has_effect on capability_tenant_id_).
        // Issue #3126: use the already-computed `is_admin` from
        // effects_for_locked under the registry mtx above (no re-read).
        if (!is_admin) {
            g_capability_effect_metrics().capability_durable_grant_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                        effect_bits, name, "durable-grant-needs-tenant-admin",
                                        /*denied=*/true, fid);
            return; // deny — no grant, no allow-counter bump (AC4)
        }
        // AC2: mandatory agent-stable audit reason under production.
        if (reason.empty()) {
            g_capability_effect_metrics().capability_durable_grant_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                        effect_bits, name, "durable-grant-reason-required",
                                        /*denied=*/true, fid);
            return; // deny — no grant, no allow-counter bump (AC4)
        }
    }
    if (is_high_risk) {
        g_capability_effect_metrics().capability_durable_high_risk_grant_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    // Issue #3177: production high-risk durable grants now stamp
    // session_bound=true by default so outermost MutationBoundaryGuard exit
    // / TenantScope dtor / steal-abort revokes them (#2944/#3048/#3142
    // path). Closes the last privilege-sticky surface for self-modifying
    // Agents under long-running multi-tenant loads. Soft/Off zero-cost (no
    // session force, AC2). Sticky escape (true privilege-sticky) is opt-in
    // via grant_effect_durable_sticky + AURA_ALLOW_DURABLE_STICKY env gate.
    const bool force_session = force_bind && is_high_risk;
    reg_durable.grant_locked(tenant_id, name, static_cast<Effect>(effect_bits), prov,
                             /*single_use=*/false, /*session_bound=*/force_session);
    if (is_high_risk && force_session) {
        g_capability_effect_metrics().capability_durable_session_bound_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    if ((effect_bits & static_cast<std::uint16_t>(Effect::Render)) != 0 && name.empty()) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->render_effect_granted_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!name.empty())
        grant_capability(std::string(name));
}

// Issue #3177: explicit privilege-sticky escape for durable high-risk
// grants under production (Restricted/Strict). Stamps single_use=false
// AND session_bound=false (true sticky — survives outermost mid exit),
// but ONLY when (a) caller holds TenantAdmin (or "tenant-admin" /
// "capability") AND (b) the audit reason is non-empty AND (c) the
// AURA_ALLOW_DURABLE_STICKY env var is set to a truthy value. Missing
// any of the three gates denies with the same SE reasons as
// grant_effect_durable (#2967) plus a new
// 'durable-sticky-needs-env-allow' reason for the env gate. Off / Soft
// path: no env gate, plain sticky (zero-cost contract, AC2). Use only
// for operator-supervised long-lived admin paths — the production-default
// surface (grant_effect_durable) now force-binds session_bound under
// production (#3177), so sticky is opt-in for callers that explicitly
// need it.
void Evaluator::grant_effect_durable_sticky(std::uint64_t tenant_id, std::string_view name,
                                            std::uint16_t effect_bits,
                                            std::uint64_t provenance_mutation_id,
                                            std::string_view reason) noexcept {
    using namespace ::aura::core::capability;
    const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
    const auto fiber = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));
    auto prov = make_grant_provenance(provenance_mutation_id, force_bind, /*node_id=*/0, fiber);
    using aura::compiler::security::kEffectMacroSelfEvo;
    using aura::compiler::security::kEffectMutate;
    using aura::compiler::security::kEffectSyscall;
    using aura::compiler::security::kEffectTenantAdmin;
    constexpr std::uint16_t kHighRiskMask = static_cast<std::uint16_t>(
        kEffectMutate | kEffectMacroSelfEvo | kEffectTenantAdmin | kEffectSyscall);
    const bool is_high_risk = (effect_bits & kHighRiskMask) != 0;
    const auto self_tenant = static_cast<std::uint64_t>(capability_tenant_id_);
    const bool foreign_target = tenant_id != 0 && tenant_id != self_tenant;
    auto& reg_sticky = g_capability_registry();
    std::lock_guard<std::mutex> lock(reg_sticky.mtx);
    const auto held_sticky = reg_sticky.effects_for_locked(self_tenant);
    const bool is_admin = has_effect(held_sticky, Effect::TenantAdmin);
    // Issue #2969 AC1: foreign-tenant write-fence — same as #2967.
    if (force_bind && foreign_target && !is_admin) {
        using ::aura::core::security_event::SecurityEventKind;
        using ::aura::core::security_event_wal::emit_security_event_durable;
        const auto epoch = ::aura::core::current_mutation_epoch();
        const auto mid = provenance_mutation_id != 0
                             ? provenance_mutation_id
                             : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
        const auto tenant = tenant_id != 0 ? tenant_id : self_tenant;
        const auto fid = static_cast<std::int64_t>(fiber);
        aura::core::capability::g_capability_effect_metrics()
            .capability_grant_foreign_tenant_deny_total.fetch_add(1, std::memory_order_relaxed);
        emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch, effect_bits,
                                    name, "grant-foreign-tenant-needs-tenant-admin",
                                    /*denied=*/true, fid);
        return; // deny — no registry write, no allow-counter bump (AC4)
    }
    // Issue #2967: production gate — TenantAdmin + mandatory reason.
    // Issue #3177: adds the env gate (AURA_ALLOW_DURABLE_STICKY=1) on
    // top — explicit opt-in for true privilege-sticky escapes.
    if (force_bind && is_high_risk) {
        using ::aura::core::security_event::SecurityEventKind;
        using ::aura::core::security_event_wal::emit_security_event_durable;
        const auto epoch = ::aura::core::current_mutation_epoch();
        const auto mid = provenance_mutation_id != 0
                             ? provenance_mutation_id
                             : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
        const auto tenant =
            tenant_id != 0 ? tenant_id : static_cast<std::uint64_t>(capability_tenant_id_);
        const auto fid = static_cast<std::int64_t>(fiber);
        if (!is_admin) {
            g_capability_effect_metrics().capability_durable_grant_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                        effect_bits, name, "durable-grant-needs-tenant-admin",
                                        /*denied=*/true, fid);
            return; // deny — no grant, no allow-counter bump (AC4)
        }
        if (reason.empty()) {
            g_capability_effect_metrics().capability_durable_grant_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                        effect_bits, name, "durable-grant-reason-required",
                                        /*denied=*/true, fid);
            return; // deny — no grant, no allow-counter bump (AC4)
        }
        // Issue #3177: sticky requires env gate AURA_ALLOW_DURABLE_STICKY=1
        // under production (Restricted/Strict). Deny otherwise with the
        // same deny counter + a dedicated SE reason so dashboards can
        // distinguish sticky-needs-env from sticky-granted-with-env.
        const char* env = std::getenv("AURA_ALLOW_DURABLE_STICKY");
        const bool env_ok = env != nullptr && env[0] == '1';
        if (!env_ok) {
            g_capability_effect_metrics().capability_durable_grant_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                        effect_bits, name, "durable-sticky-needs-env-allow",
                                        /*denied=*/true, fid);
            return; // deny — no grant, no allow-counter bump (AC4)
        }
    }
    if (is_high_risk) {
        g_capability_effect_metrics().capability_durable_high_risk_grant_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    // Issue #3177: NO session_bound force — this is the explicit sticky
    // escape. Operator-supervised long-lived admin grants survive
    // outermost MutationBoundary exit (true privilege-sticky).
    reg_sticky.grant_locked(tenant_id, name, static_cast<Effect>(effect_bits), prov,
                            /*single_use=*/false, /*session_bound=*/false);
    if ((effect_bits & static_cast<std::uint16_t>(Effect::Render)) != 0 && name.empty()) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->render_effect_granted_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!name.empty())
        grant_capability(std::string(name));
}

// Issue #2944: mutation-session grant — mid-bound + session_bound=true.
// Outermost MutationBoundary exit revokes matching session grants
// (capability_session_revoke_total + SE reason session-mid-exit).
// Issue #3048: steal-complete / force-cancel / mark_outermost_failed
// also revoke via revoke_session_grants_on_steal_or_abort (SE reason
// session-mid-steal-exit / session-mid-abort-exit) when Guard dtor
// does not run.
// High-risk production force (#2882) still applies single_use unless
// already requested; durable sticky remains grant_effect_durable.
void Evaluator::grant_effect_session(std::uint64_t tenant_id, std::string_view name,
                                     std::uint16_t effect_bits,
                                     std::uint64_t provenance_mutation_id,
                                     bool single_use) noexcept {
    using namespace ::aura::core::capability;
    const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
    const auto fiber = effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id()));
    auto prov = make_grant_provenance(provenance_mutation_id, force_bind, /*node_id=*/0, fiber);
    // Ensure non-zero mid for session binding (Soft may leave zero → force 1).
    if (prov.mutation_id == 0)
        prov.mutation_id = prov.epoch != 0 ? prov.epoch : 1;
    using aura::compiler::security::kEffectMacroSelfEvo;
    using aura::compiler::security::kEffectMutate;
    using aura::compiler::security::kEffectSyscall;
    using aura::compiler::security::kEffectTenantAdmin;
    constexpr std::uint16_t kHighRiskMask = static_cast<std::uint16_t>(
        kEffectMutate | kEffectMacroSelfEvo | kEffectTenantAdmin | kEffectSyscall);
    const bool production_defaults = force_bind;
    const bool is_high_risk = (effect_bits & kHighRiskMask) != 0;
    if (production_defaults && is_high_risk && !single_use) {
        single_use = true;
        g_capability_effect_metrics().capability_high_risk_forced_single_use_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    // Issue #2969 AC1: registry write-fence — under production
    // (Restricted/Strict), writing a session grant for a FOREIGN tenant id
    // requires TenantAdmin. AC3: Soft/Off short-circuits; AC2: same-tenant
    // self-grant keeps existing policy.
    // Issue #3126: take the registry mtx + use effects_for_locked so a
    // concurrent revoke of TenantAdmin from another Evaluator / fiber
    // cannot race past this fence. Previous has_capability() (unlocked
    // read of by_tenant via effects_for) was a TOCTOU window.
    const auto self_tenant = static_cast<std::uint64_t>(capability_tenant_id_);
    const bool foreign_target = tenant_id != 0 && tenant_id != self_tenant;
    auto& reg_session = g_capability_registry();
    if (force_bind && foreign_target) {
        std::lock_guard<std::mutex> lock(reg_session.mtx);
        const auto held = reg_session.effects_for_locked(self_tenant);
        const bool is_admin = has_effect(held, Effect::TenantAdmin);
        if (!is_admin) {
            using ::aura::core::security_event::SecurityEventKind;
            using ::aura::core::security_event_wal::emit_security_event_durable;
            const auto epoch = ::aura::core::current_mutation_epoch();
            const auto mid = provenance_mutation_id != 0
                                 ? provenance_mutation_id
                                 : (epoch != 0 ? epoch : static_cast<std::uint64_t>(1));
            const auto tenant = tenant_id != 0 ? tenant_id : self_tenant;
            const auto fid = static_cast<std::int64_t>(fiber);
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.fetch_add(1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                        effect_bits, name,
                                        "grant-foreign-tenant-needs-tenant-admin",
                                        /*denied=*/true, fid);
            return; // deny — no registry write, no allow-counter bump (AC4)
        }
        reg_session.grant_locked(tenant_id, name, static_cast<Effect>(effect_bits), prov,
                                 single_use, /*session_bound=*/true);
    } else {
        reg_session.grant(tenant_id, name, static_cast<Effect>(effect_bits), prov, single_use,
                          /*session_bound=*/true);
    }
    if ((effect_bits & static_cast<std::uint16_t>(Effect::Render)) != 0 && name.empty()) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->render_effect_granted_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (!name.empty())
        grant_capability(std::string(name));
}

// Issue #2055: revoke with WorkspaceEpoch Mutation stamp for audit trail.
void Evaluator::revoke_effect_capability(std::uint64_t tenant_id, std::string_view name) noexcept {
    using namespace ::aura::core::capability;
    // Issue #2969 AC1: registry write-fence — under production
    // (Restricted/Strict), revoking a FOREIGN tenant's grant requires
    // TenantAdmin. AC3: Soft/Off short-circuits; AC2: same-tenant revoke
    // keeps existing policy.
    // Issue #3126: take the registry mtx + use effects_for_locked so a
    // concurrent revoke of TenantAdmin from another Evaluator / fiber
    // cannot race past this fence. Previous has_capability() (unlocked
    // read of by_tenant via effects_for) was a TOCTOU window.
    const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
    const auto self_tenant = static_cast<std::uint64_t>(capability_tenant_id_);
    const bool foreign_target = tenant_id != 0 && tenant_id != self_tenant;
    auto ep = aura::core::current_mutation_epoch();
    if (ep == 0)
        ep = 1;
    auto& reg_revoke = g_capability_registry();
    if (force_bind && foreign_target) {
        std::lock_guard<std::mutex> lock(reg_revoke.mtx);
        const auto held = reg_revoke.effects_for_locked(self_tenant);
        const bool is_admin = has_effect(held, Effect::TenantAdmin);
        if (!is_admin) {
            using ::aura::core::security_event::SecurityEventKind;
            using ::aura::core::security_event_wal::emit_security_event_durable;
            const auto epoch = aura::core::current_mutation_epoch();
            const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);
            const auto tenant = tenant_id != 0 ? tenant_id : self_tenant;
            const auto fid = static_cast<std::int64_t>(
                effect_fiber_id_or(static_cast<std::uint32_t>(aura_fiber_current_id())));
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.fetch_add(1, std::memory_order_relaxed);
            emit_security_event_durable(SecurityEventKind::EffectDeny, tenant, mid, epoch,
                                        /*effect_bits=*/0, name,
                                        "grant-foreign-tenant-needs-tenant-admin",
                                        /*denied=*/true, fid);
            return; // deny — no revoke, no allow-counter bump (AC4)
        }
        reg_revoke.revoke_locked(tenant_id, name, ep);
    } else {
        reg_revoke.revoke(tenant_id, name, ep);
    }
    // Drop matching string grant so has_capability stays consistent.
    if (!name.empty()) {
        granted_capabilities_.erase(std::remove(granted_capabilities_.begin(),
                                                granted_capabilities_.end(), std::string(name)),
                                    granted_capabilities_.end());
    }
}

void Evaluator::set_effect_sandbox_mode(std::uint8_t mode) noexcept {
    using namespace ::aura::core::sandbox;
    using namespace ::aura::core::provenance;
    if (mode > 2)
        mode = 2;
    // Issue #2657: route through the process-wide authority
    // aura::core::sandbox::set_mode (the SOLE writer of sandbox mode —
    // atomic + plain enum + CapabilityRegistry + workspace_isolation
    // strict link + provenance_tracker policy). After this call all
    // four stores agree; the per-Evaluator `sandbox_mode_` field and
    // `set_stable_ref_auto_refresh_policy` are per-instance legacy
    // gates that remain below.
    set_mode(static_cast<SandboxMode>(mode));
    // Strict/Restricted also set evaluator sandbox_mode_ so legacy gates engage.
    sandbox_mode_ = (mode != 0);
    // Issue #1877: under sandbox Strict, no silent restamp of
    // gen-stale StableNodeRef (multi-tenant AI self-modify must fail
    // closed rather than auto-refresh). The provenance_tracker policy
    // was already set by the authority (FailOnStale / AutoRefreshOnBoundary);
    // here we only mirror the legacy gate.
    if (mode == 2) {
        set_stable_ref_auto_refresh_policy(false);
        record_fail_on_stale_strict_sandbox();
        record_policy_enforced();
    } else {
        set_stable_ref_auto_refresh_policy(true);
    }
}

std::uint8_t Evaluator::effect_sandbox_mode() const noexcept {
    return static_cast<std::uint8_t>(
        aura::core::capability::g_capability_registry().sandbox_mode.load());
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
    if (effect_bits & aura::compiler::security::kEffectTenantAdmin)
        return "tenant-admin";
    if (effect_bits & aura::compiler::security::kEffectSyscall)
        return "syscall";
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
    const auto mode = static_cast<std::uint8_t>(
        ::aura::core::capability::g_capability_registry().sandbox_mode.load());
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
// Issue #2659: ONLY the Evaluator-local principal is set — the process-global
// WorkspaceIsolationPolicy::current is no longer written, so multiple
// Evaluators / concurrent fibers in one process no longer race on a
// single "current tenant" (see #2659 audit). Cross-grant table remains
// process-global (shared policy).
void Evaluator::set_tenant_principal(std::uint64_t tenant_id, std::string_view /*name*/,
                                     bool allow_cross) noexcept {
    // Issue #3010: production same-tenant self-grant of the isolation
    // bypass flag requires TenantAdmin / wildcard / capability. Soft/Off
    // (sandbox_mode_ == 0 && effect_sandbox_mode() == 0) short-circuits
    // before any privilege lookup (AC3: zero extra cost).
    if (allow_cross) {
        const bool force_bind = sandbox_mode_ != 0 || effect_sandbox_mode() != 0;
        if (force_bind) {
            using aura::compiler::security::kCapCapability;
            using aura::compiler::security::kCapTenantAdmin;
            using aura::compiler::security::kCapWildcard;
            const bool privileged = has_capability(kCapTenantAdmin) ||
                                    has_capability(kCapWildcard) || has_capability(kCapCapability);
            if (!privileged) {
                using ::aura::core::security_event::SecurityEventKind;
                using ::aura::core::security_event_wal::emit_security_event_durable;
                using ::aura::core::workspace_isolation::g_tenant_isolation_metrics;
                const auto epoch = ::aura::core::current_mutation_epoch();
                const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);
                const auto fid =
                    static_cast<std::int64_t>(::aura::core::capability::effect_fiber_id_or(
                        static_cast<std::uint32_t>(aura_fiber_current_id())));
                g_tenant_isolation_metrics().allow_cross_tenant_deny_total.fetch_add(
                    1, std::memory_order_relaxed);
                bump_capability_denial();
                emit_security_event_durable(SecurityEventKind::EffectDeny, tenant_id, mid, epoch,
                                            /*effect_bits=*/0, "set-tenant-principal",
                                            "allow-cross-needs-tenant-admin",
                                            /*denied=*/true, fid);
                capability_tenant_id_ = tenant_id;
                return; // refuse the flag — leave allow_cross_tenant_ unchanged
            }
        }
    }
    capability_tenant_id_ = tenant_id;
    allow_cross_tenant_ = allow_cross;
}

// Issue #2055: RAII TenantScope — snapshot principal at fiber entry so a
// stolen / resumed fiber cannot silently keep another tenant's principal.
// Issue #2659: snapshot/restore the Evaluator-local fields only — no
// process-global writes (multi-Evaluator race).
Evaluator::TenantScope::TenantScope(Evaluator& ev, std::uint64_t tenant_id, std::string_view name,
                                    bool allow_cross) noexcept
    : ev_(&ev)
    , prev_tenant_(ev.capability_tenant_id())
    , prev_allow_cross_(ev.allow_cross_tenant_)
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
    // Issue #3142 AC1: cascade-revoke SessionBound grants bound to
    // (prev_tenant_, current mid, this fiber) before restoring prior
    // principal. Prevents orphan SessionBound grants on nested scope
    // abort / early-return. fiber_id_ captured at ctor.
    // Issue #3207: Restricted/Strict always take registry mtx and restore
    // principal under that same lock so a concurrent Evaluator cannot
    // consume a grant after this scope's principal is visible again.
    // Soft/Off + capability_live_session_grants == 0: zero-cost (no lock).
    // Use _locked sibling — std::mutex is non-recursive (nested lock_guard
    // on the public wrapper deadlocked the live-residual path).
    using namespace ::aura::core::capability;
    const auto mid = ::aura::core::current_mutation_epoch();
    auto& reg = g_capability_registry();
    auto& met = g_capability_effect_metrics();
    const auto mode = reg.sandbox_mode.load(std::memory_order_acquire);
    const bool production =
        mode == EffectSandboxMode::Restricted || mode == EffectSandboxMode::Strict;
    const bool have_live = met.capability_live_session_grants.load(std::memory_order_relaxed) != 0;
    if (mid != 0 && (production || have_live)) {
        std::lock_guard<std::mutex> lock(reg.mtx);
        (void)reg.revoke_session_grants_for_locked(prev_tenant_, mid, fiber_id_,
                                                   "scope-dtor-cascade");
        // Restore prior principal under the same lock (#3207 happens-before).
        ev_->set_capability_tenant_id(prev_tenant_);
        ev_->allow_cross_tenant_ = prev_allow_cross_;
        active_ = false;
        return;
    }
    // Restore prior principal (name empty → keep id only).
    // Issue #2659: Evaluator-local only — no global write.
    ev_->set_capability_tenant_id(prev_tenant_);
    ev_->allow_cross_tenant_ = prev_allow_cross_;
    active_ = false;
}

void Evaluator::grant_cross_tenant_access(std::uint64_t from_tenant, std::uint64_t to_tenant,
                                          std::uint16_t effect_bits) noexcept {
    using namespace ::aura::core::workspace_isolation;
    // Issue #3086: fence moved into WorkspaceIsolationPolicy::grant_cross_tenant
    // (the SSOT method). Evaluator wrapper is now a thin stamp+call —
    // no second policy, no double-count of deny metrics.
    // Issue #3145 AC2: forward this Evaluator's `capability_tenant_id_`
    // (the per-Evaluator principal restored by TenantScope) as the explicit
    // caller principal to the SSOT helper. The SSOT helper
    // `try_grant_cross_tenant_privileged` previously resolved the caller
    // via the process-global `default_tenant`, which is almost always 0
    // under multi-Evaluator production. The helper takes the registry mtx
    // and reads `effects_for_locked(caller_principal)` so a concurrent
    // revoke of TenantAdmin from another Evaluator / fiber cannot race
    // past the fence (TOCTOU closure — #3126). Soft/Off stays zero-cost
    // (the SSOT helper short-circuits before any lock or principal load).
    g_workspace_isolation().grant_cross_tenant(from_tenant, to_tenant, effect_bits,
                                               capability_tenant_id_);
}

bool Evaluator::check_workspace_isolation(std::uint64_t target_tenant, std::uint64_t ref_tenant,
                                          std::uint16_t required_effects,
                                          std::string_view op) noexcept {
    // Issue #1566: workspace isolation bridge. Use explicit `using`
    // declarations with `::aura::core::` (absolute path) because we're
    // inside namespace `aura::compiler` and `aura::core::` would
    // otherwise resolve as nested (`aura::compiler::aura::core::`)
    // which doesn't exist.
    using ::aura::core::capability::EffectSandboxMode;
    using ::aura::core::capability::g_capability_registry;
    using ::aura::core::sandbox::is_strict;
    using ::aura::core::workspace_isolation::check_boundary;
    using ::aura::core::workspace_isolation::g_workspace_isolation;
    using ::aura::core::workspace_isolation::IsolationRefProvenance;
    const auto target = target_tenant != 0 ? target_tenant : capability_tenant_id_;
    const auto mode = effect_sandbox_mode();
    const bool strict = mode == 2 || is_strict() || g_workspace_isolation().strict_sandbox_linked;
    // Issue #2385: Restricted (mode 1 or registry Restricted) must deny
    // side-effects when principal is unset — production default footgun.
    const bool restricted =
        mode == 1 || g_capability_registry().sandbox_mode == EffectSandboxMode::Restricted;
    IsolationRefProvenance prov{};
    prov.tenant_id = ref_tenant;
    // Issue #2659: caller_principal + allow_cross_tenant come from this
    // Evaluator (per-instance) — the process-global `current` is no
    // longer read by check_boundary_ex. Multi-Evaluator co-location no
    // longer races on a single principal.
    const bool ok = check_boundary(capability_tenant_id_, target, &prov, allow_cross_tenant_,
                                   required_effects, strict, op, restricted);
    if (!ok) {
        bump_capability_denial();
        // Issue #2388: IsolationDeny SecurityEvent + WAL dual-written from
        // WorkspaceIsolationPolicy::record_audit (single path — AC2 no
        // double-count). Reasons (unset-principal / ref-tenant) stamped
        // there; mid = Mutation epoch (#2156). Keep TypedMutationAudit only.
        using ::aura::core::security_event::kIsolationAuditMidIssue;
        using ::aura::core::security_event::kSecurityAuditFoldIssue;
        (void)kIsolationAuditMidIssue;
        (void)kSecurityAuditFoldIssue;
        const auto fiber = static_cast<std::int64_t>(aura_fiber_current_id());
        const auto epoch = ::aura::core::current_mutation_epoch();
        const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);
        typed_audit::capture_security_correlated_audit(mid, op, mid, /*denied=*/true,
                                                       /*target_node=*/0, fiber);
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
    // Issue #2687 / #2759: bump local capture counter — sole production
    // multi-tenant path (per-Evaluator authority via capability_tenant_id_).
    // Distinct from maybe_stamp_stable_ref_isolation_tenant which bumps
    // g_isolation_capture_stamp_global_fallback_total_atomic when it uses
    // the process-global g_isolation_capture_tenant atomic (Soft only under
    // hard-close; #2705 refuses that write path).
    ::aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().fetch_add(
        1, std::memory_order_relaxed);
    ::aura::core::provenance::stamp_stable_ref_fields(ref, capability_tenant_id_, fiber);
}

// Issue #3000 / #3037 / #3121: production query:*-stable must not export a
// pre-mutate generation when the last outermost restamp hit
// AURA_RESTAMP_BUDGET_NODES. Soft / sandbox=off: observe only (stamp
// proceeds). Quiet path: last-exceeded false → one relaxed load,
// no new atomics. Peek eager bit *before* make_ref_layout so
// lazy-align cannot hide lag.
// Issue #3037: do not treat lazy-align (node_gen_==generation_) as
// eager restamp. Over-budget marks generation torn; only nodes
// restamp_all actually wrote may export. Peek eager bit *before*
// make_ref_layout so is_valid cannot hide lag.
bool Evaluator::allow_query_stable_ref_export(ast::NodeId id) const noexcept {
    auto* ws = workspace_flat_;
    if (!ws || id == ast::NULL_NODE)
        return true;
    // #3230: !ws->restamp_last_budget_exceeded() && !torn.
    if (!ws->nested_authority_gap() && !ws->restamp_over_budget_torn())
        return true;
    if (!ws->nested_authority_gap() && ws->node_eagerly_restamped(id))
        return true;
    // Issue #3076: production Hard sibling — Soft observe must not rise.
    if (typed_audit::should_hard_reject_soft_sibling()) {
        ::aura::core::provenance::record_query_stable_ref_restamp_lag_prevented();
        ::aura::core::provenance::record_query_stable_ref_restamp_torn_reject();
        return false;
    }
    ::aura::core::provenance::record_query_stable_ref_restamp_lag_soft_observe();
    ::aura::core::provenance::record_query_stable_ref_restamp_torn_soft_observe();
    return true;
}

// Issue #3100: shared restamp-status probe (the issue body's
// 'shared restamp-status probe used by query:*-stable'). Returns
// true under production_defaults_active + last restamp over budget
// for this workspace/evaluator. Soft/Off/sandbox=off returns false
// (metric/soft observe allowed per AC2). Reuses the existing
// restamp_last_budget_exceeded_ flag maintained by
// unified_restamp_after_boundary + ast_impl.cpp
// restamp_eager_after_boundary_locked (already set when the budget
// is exceeded). One cheap production_defaults load + one flag load
// on quiet path (under-budget → false early); over-budget path is a
// single positive load + early-return. Exposed for query:*-stable
// sites that previously bypassed allow_query_stable_ref_export
// (e.g., internal restamp consumers reading the gate directly).
bool Evaluator::query_stable_hard_reject_torn() const noexcept {
    if (!typed_audit::should_hard_reject_soft_sibling())
        return false;
    auto* ws = workspace_flat_;
    if (!ws)
        return false;
    return ws->restamp_last_budget_exceeded() || ws->nested_authority_gap();
}

// Issue #2960: query Agent export — remake brace-init residuals, stamp tenant+fiber,
// count stamped / unstamped_prevented (target 0 residual under production).
// Issue #3000: production restamp-lag reject (null ref; do not stamp-green).
void Evaluator::stamp_query_stable_ref_export(ast::FlatAST::StableNodeRef& ref) const noexcept {
    if (workspace_flat_ && ref.id != ast::NULL_NODE) {
        // Issue #3230: consult torn/budget *before* make_ref_layout so
        // lazy-align cannot hide a pre-mutate gen. Soft allow proceeds.
        // Issue #3259: hot-cone eager bit is accepted by allow; lagging
        // remainder still nulls (never green a pre-mutate gen).
        if (!allow_query_stable_ref_export(ref.id)) {
            ref = {};
            return;
        }
        const auto we = workspace_flat_->wrap_epoch();
        const auto ce = workspace_flat_->workspace_cow_epoch();
        // Brace-init {id, gen} leaves wrap/cow at 0 while advanced workspaces
        // have non-zero state; layout-only capture always matches FlatAST.
        const bool layout_missing =
            (we != 0 && ref.wrap_epoch == 0) || (ce != 0 && ref.cow_epoch_at_capture == 0);
        if (layout_missing) {
            const auto id = ref.id;
            ref = workspace_flat_->make_ref_layout(id);
            // Issue #3230: layout gen is post-mutate authority. Do not
            // paint a pre-mutate gen onto a remade layout.
            ::aura::core::provenance::record_query_stable_ref_unstamped_prevented();
        }
    }
    if (ref.id == ast::NULL_NODE)
        return;
    stamp_stable_ref(ref);
    ::aura::core::provenance::record_query_stable_ref_stamped();
}

ast::FlatAST::StableNodeRef Evaluator::make_stamped_ref(ast::NodeId id) const noexcept {
    // Issue #2759: layout-only capture then Evaluator stamp (sole production
    // authority). Avoid make_ref → maybe_stamp under hard-close, which would
    // false-count evaluator_miss while still overwriting via stamp_stable_ref.
    ast::FlatAST::StableNodeRef ref{};
    if (workspace_flat_)
        ref = workspace_flat_->make_ref_layout(id);
    else
        ref.id = id;
    stamp_stable_ref(ref);
    return ref;
}

ast::FlatAST::StableNodeRef
Evaluator::make_stamped_safe_ref(ast::NodeId id, std::uint32_t workspace_id,
                                 std::uint32_t fiber_id) const noexcept {
    // Issue #2759: same layout-only + stamp path as make_stamped_ref.
    ast::FlatAST::StableNodeRef ref{};
    const auto fiber =
        fiber_id != 0 ? fiber_id : static_cast<std::uint32_t>(aura_fiber_current_id());
    if (workspace_flat_)
        ref = workspace_flat_->make_safe_ref_layout(id, workspace_id, fiber);
    else {
        ref.id = id;
        ref.fiber_id = fiber;
    }
    stamp_stable_ref(ref);
    return ref;
}

// Issue #2224 / #2404: sole public outbound helper — every StableNodeRef
// handed to Agent / user code MUST go through export_ref / export_ref_safe
// so the tenant + fiber stamp is guaranteed AND validate_or_refresh runs
// before return (Agent export contract). Parity with #2152 dispatch
// required_effects: side effects are non-bypassable; isolation should match.
ast::FlatAST::StableNodeRef Evaluator::export_ref(ast::NodeId id) const noexcept {
    // Issue #3198: Agent export must not bypass the torn/budget gate.
    if (!allow_query_stable_ref_export(id))
        return {};
    // const surface kept for #2224 call sites; finalize mutates only the
    // returned ref + process-wide atomics (via non-const ensure helper).
    return const_cast<Evaluator*>(this)->finalize_agent_export(make_stamped_ref(id));
}

ast::FlatAST::StableNodeRef Evaluator::export_ref_safe(ast::NodeId id, std::uint32_t workspace_id,
                                                       std::uint32_t fiber_id) const noexcept {
    // Issue #3198: same gate as export_ref (query:*-stable / ensure-ref).
    if (!allow_query_stable_ref_export(id))
        return {};
    return const_cast<Evaluator*>(this)->finalize_agent_export(
        make_stamped_safe_ref(id, workspace_id, fiber_id));
}

// Issue #2404: stamp is already applied; run ensure_valid_or_refresh and
// classify export metrics (valid soft / refresh / stale-reject).
ast::FlatAST::StableNodeRef
Evaluator::finalize_agent_export(ast::FlatAST::StableNodeRef ref) noexcept {
    using aura::core::provenance::hard_capture_tenant_active;
    using aura::core::provenance::record_stable_ref_export_refresh;
    using aura::core::provenance::record_stable_ref_export_stale_reject;
    using aura::core::provenance::record_stable_ref_export_valid;
    using aura::core::provenance::record_stable_ref_tenant_stamp_zero_rejected;
    using aura::core::provenance::stable_ref_export_hard_reject;

    auto* ws = workspace_flat_;
    if (!ws || ref.id == aura::ast::NULL_NODE) {
        record_stable_ref_export_stale_reject();
        if (stable_ref_export_hard_reject())
            return {};
        return ref;
    }
    // Issue #3204: production layout-only / mailbox re-export must pass
    // Evaluator stamp (sole production authority #2759) before Agent
    // delivery. Quiet: tenant already non-zero → skip. Soft/Off: no
    // stamp (existing tenant_id==0 contract; zero extra beyond one
    // acquire/env read when tenant_id==0).
    if (ref.tenant_id == 0 && stable_ref_export_hard_reject()) {
        stamp_stable_ref(ref);
        if (ref.tenant_id == 0 && (capability_tenant_id_ != 0 || hard_capture_tenant_active())) {
            record_stable_ref_export_stale_reject();
            record_stable_ref_tenant_stamp_zero_rejected();
            return {};
        }
    }
    // AC3: already-valid → no restamp work beyond lock-free validate in
    // ensure_valid_or_refresh / validate_or_refresh (refresh_if_stale early-outs).
    const bool already_valid = ref.is_valid_in(*ws);
    auto view = ensure_valid_or_refresh(ref, /*auto_refresh=*/true);
    if (!view) {
        record_stable_ref_export_stale_reject();
        if (stable_ref_export_hard_reject())
            return {};
        return ref; // soft: return unrefreshable stamped handle for Agent error path
    }
    if (already_valid)
        record_stable_ref_export_valid();
    else
        record_stable_ref_export_refresh();
    return ref;
}

std::optional<ast::FlatAST::StableNodeRef>
Evaluator::export_held_ref(ast::FlatAST::StableNodeRef ref) noexcept {
    // Issue #3198: Agent re-export must not bypass the torn/budget gate.
    if (!allow_query_stable_ref_export(ref.id))
        return std::nullopt;
    // Re-export long-held handle (mailbox / cross-fiber). finalize_agent_export
    // owns export metrics; callers get nullopt on unrefreshable.
    auto out = finalize_agent_export(std::move(ref));
    auto* ws = workspace_flat_;
    if (!ws || out.id == aura::ast::NULL_NODE || !out.is_valid_in(*ws))
        return std::nullopt;
    return out;
}

// Issue #2632: single internal handoff helper for cross-fiber / mailbox /
// orch result packaging. Wraps export_held_ref and bumps a dedicated
// stable_ref_handoff_reject_total counter so we can distinguish handoff
// rejections from query-time rejections (export-stale-reject). Callers
// that hand a StableNodeRef across a fiber / mailbox / orch boundary
// MUST call this rather than touching the raw export_held_ref path:
// the helper ensures the dedicated counter increments and the
// stamp-stale trace carries a "handoff" tag for the dashboard.
std::optional<ast::FlatAST::StableNodeRef>
Evaluator::handoff_ref(ast::FlatAST::StableNodeRef ref) noexcept {
    auto out = export_held_ref(std::move(ref));
    if (!out) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->stable_ref_handoff_reject_total.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
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
