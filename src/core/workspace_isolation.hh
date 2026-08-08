// workspace_isolation.hh — Issues #1180/#1183/#1566: WorkspaceIsolationPolicy SSOT.
// Module consumers: `import aura.core.workspace_isolation;` re-exports this
// header. Non-module TUs: #include. TenantPrincipal layout stable
// (id, name, allow_cross_tenant).

#ifndef AURA_CORE_WORKSPACE_ISOLATION_HH
#define AURA_CORE_WORKSPACE_ISOLATION_HH

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/provenance_tracker.hh" // #2125: set_isolation_capture_tenant
#include "core/security_event.hh"     // #2388 dual-write kinds
#include "core/security_event_wal.hh" // #2388 emit_security_event_durable
#include "core/workspace_epoch.hh"    // #2388 / #2156 isolation mid = Mutation epoch

namespace aura::core::workspace_isolation {

inline constexpr int kWorkspaceIsolationPhase = 2; // #1566 enforcement
inline constexpr int kWorkspaceIsolationIssue = 1566;

using TenantId = std::uint64_t;

// Layout-stable principal (do not reorder/remove core fields).
struct TenantPrincipal {
    TenantId id = 0;
    std::string_view name;
    bool allow_cross_tenant = false;
};

// Optional ref provenance snapshot for isolation (does not require
// FlatAST::StableNodeRef layout change for the check itself).
struct IsolationRefProvenance {
    TenantId tenant_id = 0;
    std::uint32_t node_id = 0;
    std::uint64_t mutation_id = 0;
    std::uint32_t workspace_id = 0;
    std::uint32_t fiber_id = 0;
};

struct IsolationAuditEntry {
    std::uint64_t seq = 0;
    TenantId current = 0;
    TenantId target = 0;
    TenantId ref_tenant = 0;
    bool denied = false;
    bool provenance_deny = false;
    bool capability_deny = false;
    // Issue #2530 / #2534: Mutation epoch mid for correlated-trail join.
    std::uint64_t mutation_id = 0;
    char op[40]{};
};

// Issue #2530: published isolation slot (mirrors Capability PublishedAuditSlot
// / #2425). Writer: exclusive lock → data → release store publish_seq.
// Reader: shared lock + acquire double-check. 0 = never published / cleared.
struct PublishedIsolationSlot {
    std::atomic<std::uint64_t> publish_seq{0};
    IsolationAuditEntry data{};
};

struct TenantIsolationMetrics {
    std::atomic<std::uint64_t> tenant_boundary_checks_total{0};
    std::atomic<std::uint64_t> tenant_boundary_violation_prevented_total{0};
    std::atomic<std::uint64_t> cross_tenant_provenance_deny_total{0};
    std::atomic<std::uint64_t> cross_tenant_capability_grant_total{0};
    std::atomic<std::uint64_t> cross_tenant_capability_deny_total{0};
    std::atomic<std::uint64_t> isolation_audit_total{0};
    std::atomic<std::uint64_t> strict_sandbox_isolation_denials{0};
};

inline TenantIsolationMetrics& g_tenant_isolation_metrics() noexcept {
    static TenantIsolationMetrics m;
    return m;
}

// Pair key: from → to cross-tenant grant.
struct CrossTenantKey {
    TenantId from = 0;
    TenantId to = 0;
    bool operator==(const CrossTenantKey& o) const noexcept { return from == o.from && to == o.to; }
};

struct CrossTenantKeyHash {
    std::size_t operator()(const CrossTenantKey& k) const noexcept {
        return static_cast<std::size_t>(k.from * 1315423911u) ^ static_cast<std::size_t>(k.to);
    }
};

struct WorkspaceIsolationPolicy {
    std::mutex mtx;
    TenantPrincipal current{};
    // Owned name storage so string_view in principal stays valid.
    std::string current_name_owned;
    // from_tenant → (to_tenant → effect bit OR-mask allowed)
    std::unordered_map<CrossTenantKey, std::uint16_t, CrossTenantKeyHash> cross_grants;
    // Legacy counters (also mirrored to atomics)
    std::uint64_t boundary_checks = 0;
    std::uint64_t denials = 0;
    bool isolation_enabled = false; // false = permissive (tenant id 0 / unset)
    bool strict_sandbox_linked = false;
    // Issue #2530: 1024 slots — aligned with Capability / SecurityEvent rings.
    // Soft / Off paths pay nothing when record_audit is never called.
    // Earlier-than-window events rely on WAL (AC3).
    static constexpr std::size_t kAuditRing = 1024;
    // Issue #2530: shared_mutex + published slots (was bare array — torn reads).
    mutable std::shared_mutex audit_ring_mtx_;
    PublishedIsolationSlot audit_ring[kAuditRing]{};
    std::atomic<std::uint64_t> audit_seq{0};

    void set_current_tenant(TenantId id, std::string_view name = {},
                            bool allow_cross = false) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        current.id = id;
        current.allow_cross_tenant = allow_cross;
        if (!name.empty()) {
            current_name_owned.assign(name);
            current.name = current_name_owned;
        } else if (id == 0) {
            current_name_owned.clear();
            current.name = {};
        }
        isolation_enabled = (id != 0);
        // Issue #2125 / #2687 / #2759: hot-path capture principal for FlatAST
        // make_ref family (best-effort process-global mirror for legacy
        // single-tenant / Soft). Zero when isolation off so raw make_ref stays
        // unstamped (AC5 / #2056). Production multi-tenant authority is
        // per-Evaluator (Evaluator::stamp_stable_ref / capability_tenant_id_).
        // Under #2705/#2759 hard-close: set_isolation_capture_tenant suppresses
        // non-zero writes + maybe_stamp refuses residual global stamp.
        ::aura::core::provenance::set_isolation_capture_tenant(id);
    }

    void set_allow_cross_tenant(bool v) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        current.allow_cross_tenant = v;
    }

    void set_strict_sandbox_linked(bool v) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        strict_sandbox_linked = v;
    }

    // Grant cross-tenant access: `from` may touch resources owned by `to`
    // for the given effect bits (OR into existing).
    void grant_cross_tenant(TenantId from, TenantId to, std::uint16_t effect_bits) noexcept {
        if (from == 0 || to == 0)
            return;
        std::lock_guard<std::mutex> lock(mtx);
        CrossTenantKey key{from, to};
        cross_grants[key] = static_cast<std::uint16_t>(cross_grants[key] | effect_bits);
        g_tenant_isolation_metrics().cross_tenant_capability_grant_total.fetch_add(
            1, std::memory_order_relaxed);
    }

    void revoke_cross_tenant(TenantId from, TenantId to) noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        cross_grants.erase(CrossTenantKey{from, to});
    }

    [[nodiscard]] std::uint16_t cross_grant_bits(TenantId from, TenantId to) const noexcept {
        auto it = cross_grants.find(CrossTenantKey{from, to});
        if (it == cross_grants.end())
            return 0;
        return it->second;
    }

    // Issue #2388 / #2530: private kAuditRing (1024) + dual-write IsolationDeny
    // into SecurityEvent/WAL when denied. Publish_seq + shared_mutex double-check
    // mirrors Capability ring (#2425). Allows stay private-ring only.
    // Soft/Off: zero extra cost when this is never called.
    void record_audit(TenantId target, TenantId ref_tenant, bool denied, bool prov_deny,
                      bool cap_deny, std::string_view op,
                      std::uint16_t required_effects = 0) noexcept {
        using ::aura::core::current_mutation_epoch;
        const auto epoch = current_mutation_epoch();
        const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);

        const auto seq = audit_seq.fetch_add(1, std::memory_order_release);
        IsolationAuditEntry entry{};
        entry.seq = seq;
        entry.current = current.id;
        entry.target = target;
        entry.ref_tenant = ref_tenant;
        entry.denied = denied;
        entry.provenance_deny = prov_deny;
        entry.capability_deny = cap_deny;
        entry.mutation_id = mid;
        const auto n = std::min(op.size(), sizeof(entry.op) - 1);
        std::memcpy(entry.op, op.data(), n);
        entry.op[n] = '\0';
        {
            // Issue #2530: exclusive → data → release publish_seq.
            std::unique_lock<std::shared_mutex> wlock(audit_ring_mtx_);
            auto& slot = audit_ring[seq % kAuditRing];
            slot.data = entry;
            slot.publish_seq.store(seq + 1, std::memory_order_release);
        }
        g_tenant_isolation_metrics().isolation_audit_total.fetch_add(1, std::memory_order_relaxed);

        if (!denied)
            return; // AC2: one SE only on deny (not on allow)

        using ::aura::core::security_event::kIsolationAuditMidIssue;
        using ::aura::core::security_event::kSecurityAuditFoldIssue;
        using ::aura::core::security_event::SecurityEventKind;
        using ::aura::core::security_event_wal::emit_security_event_durable;
        (void)kIsolationAuditMidIssue;
        (void)kSecurityAuditFoldIssue;
        char reason_buf[64];
        const char* reason = "isolation-deny";
        // Issue #2385: unset principal under Restricted/Strict side-effect path.
        if (current.id == 0) {
            reason = "isolation-deny:unset-principal";
        } else if (ref_tenant != 0) {
            std::snprintf(reason_buf, sizeof(reason_buf), "isolation-deny:ref-tenant=%llu",
                          static_cast<unsigned long long>(ref_tenant));
            reason = reason_buf;
        }
        // Tenant stays in tenant_id field; mid is Mutation epoch (#2156).
        // effect_bits preserves required side-effect mask for forensic join.
        emit_security_event_durable(SecurityEventKind::IsolationDeny, target, mid, mid,
                                    required_effects, op, reason, /*denied=*/true,
                                    /*fiber_id=*/0);
    }

    // Issue #2530: TSAN-clean load of slot for `seq` (seq % kAuditRing).
    [[nodiscard]] bool try_load_audit_seq(std::uint64_t seq,
                                          IsolationAuditEntry& out) const noexcept {
        const auto idx = seq % kAuditRing;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::shared_lock<std::shared_mutex> rlock(audit_ring_mtx_);
            const auto& slot = audit_ring[idx];
            const auto pub = slot.publish_seq.load(std::memory_order_acquire);
            if (pub == 0)
                return false;
            if (pub != seq + 1) {
                if (attempt + 1 < 8)
                    continue;
                return false;
            }
            out = slot.data;
            const auto pub2 = slot.publish_seq.load(std::memory_order_acquire);
            if (pub2 == pub && out.seq == seq)
                return true;
        }
        return false;
    }

    [[nodiscard]] std::uint64_t load_audit_seq() const noexcept {
        return audit_seq.load(std::memory_order_acquire);
    }

    // AC1 legacy: simple ID boundary check.
    // Issue #2659: caller_principal + allow_cross_tenant supplied by caller
    // (defaults to 0 / false for legacy 1-arg callers).
    [[nodiscard]] bool check_boundary(TenantId caller_principal, TenantId target,
                                      bool allow_cross_tenant = false) noexcept {
        return check_boundary_ex(caller_principal, target, /*ref_tenant=*/0, allow_cross_tenant,
                                 /*required_effects=*/0,
                                 /*sandbox_strict=*/false, "boundary",
                                 /*sandbox_restricted=*/false);
    }

    // AC1–4 enhanced: capability propagation + provenance + Strict sandbox.
    // Issue #2385: Restricted + unset principal + side-effect deny.
    //
    // Policy:
    //   - current.id==0 + Off (neither strict nor restricted side-effect):
    //       allow (pure read / unit Soft path)
    //   - current.id==0 + Strict → deny (side-effect and pure: principal required)
    //   - current.id==0 + Restricted + required_effects!=0 → deny
    //       (production default footgun closed — #2385)
    //   - current.id==0 + Restricted + required_effects==0 → allow (query-only)
    //   - current.allow_cross_tenant → allow
    //   - current.id == target (or target==0 meaning "same workspace") → allow
    //   - ref_tenant != 0 && ref_tenant != current.id && != target → provenance deny
    //   - cross_grants[current→target] covers required_effects → allow
    //   - sandbox_strict → no soft fallback; deny without grant
    //   - else deny (boundary violation prevented)
    //
    // sandbox_restricted: EffectSandboxMode::Restricted (mode==1). Wire from
    // Evaluator::check_workspace_isolation via g_capability_registry /
    // effect_sandbox_mode — do not invent a second mode enum.
    // Issue #2659: caller_principal + allow_cross_tenant come from the
    // Evaluator (per-instance), NOT from the process-global `current` field.
    // The `current` global is no longer written by set_tenant_principal /
    // TenantScope (multi-Evaluator race — see #2659 audit). Cross-grant
    // table remains process-global (shared policy).
    [[nodiscard]] bool check_boundary_ex(TenantId caller_principal, TenantId target,
                                         TenantId ref_tenant, bool allow_cross_tenant,
                                         std::uint16_t required_effects, bool sandbox_strict,
                                         std::string_view op = "workspace",
                                         bool sandbox_restricted = false) noexcept {
        auto& met = g_tenant_isolation_metrics();
        met.tenant_boundary_checks_total.fetch_add(1, std::memory_order_relaxed);

        bool allowed = true;
        bool prov_deny = false;
        bool cap_deny = false;

        {
            std::lock_guard<std::mutex> lock(mtx);
            ++boundary_checks;

            const bool strict = sandbox_strict || strict_sandbox_linked;
            // Issue #2659: caller_principal replaces the process-global read.
            const TenantId cur = caller_principal;

            // Issue #2385: unset principal (tenant=0).
            // Strict always requires a principal. Restricted requires a
            // principal when the call carries side-effect bits (Mutate/FFI/…).
            // Pure reads under Restricted stay permissive (legacy query paths).
            // Off stays fully permissive when principal unset.
            if (cur == 0) {
                const bool need_principal = strict || (sandbox_restricted && required_effects != 0);
                if (!need_principal) {
                    record_audit(target, ref_tenant, false, false, false, op, required_effects);
                    return true;
                }
                // Deny: isolation-deny:unset-principal (reason dual-written
                // into SecurityEvent from record_audit — #2388 single path).
                allowed = false;
                ++denials;
                met.tenant_boundary_violation_prevented_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                if (strict)
                    met.strict_sandbox_isolation_denials.fetch_add(1, std::memory_order_relaxed);
                record_audit(target, ref_tenant, true, false, false, op, required_effects);
                return false;
            }
            if (allow_cross_tenant) {
                record_audit(target, ref_tenant, false, false, false, op, required_effects);
                return true;
            }
            // Same tenant or unscoped target → ok (still check ref provenance).
            if (target == 0 || cur == target) {
                // fall through to provenance
            } else {
                // Cross-tenant path: need grant covering required effects
                // (or any grant when required_effects == 0).
                const auto held = cross_grant_bits(cur, target);
                if (required_effects == 0) {
                    if (held == 0) {
                        allowed = false;
                        cap_deny = true;
                    }
                } else if ((held & required_effects) != required_effects) {
                    allowed = false;
                    cap_deny = true;
                }
            }

            // Provenance: ref stamped for another tenant — need grant
            // current → ref_tenant (or same as target path already covered).
            if (allowed && ref_tenant != 0 && cur != 0 && ref_tenant != cur) {
                const auto held = cross_grant_bits(cur, ref_tenant);
                if (held == 0) {
                    allowed = false;
                    prov_deny = true;
                }
            }

            if (!allowed) {
                ++denials;
                met.tenant_boundary_violation_prevented_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                if (prov_deny)
                    met.cross_tenant_provenance_deny_total.fetch_add(1, std::memory_order_relaxed);
                if (cap_deny)
                    met.cross_tenant_capability_deny_total.fetch_add(1, std::memory_order_relaxed);
                if (strict)
                    met.strict_sandbox_isolation_denials.fetch_add(1, std::memory_order_relaxed);
            }
            record_audit(target, ref_tenant, !allowed, prov_deny, cap_deny, op, required_effects);
        }
        return allowed;
    }

    void clear_for_test() noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        current = {};
        current_name_owned.clear();
        cross_grants.clear();
        boundary_checks = 0;
        denials = 0;
        isolation_enabled = false;
        strict_sandbox_linked = false;
        audit_seq.store(0, std::memory_order_relaxed);
        ::aura::core::provenance::set_isolation_capture_tenant(0);
    }
};

inline WorkspaceIsolationPolicy& g_workspace_isolation() noexcept {
    static WorkspaceIsolationPolicy p;
    return p;
}

// Free-function convenience (matches issue pseudo-code).
// Issue #2385: sandbox_restricted wires EffectSandboxMode::Restricted.
// Issue #2659: caller_principal + allow_cross_tenant supplied by the caller
// (typically Evaluator::capability_tenant_id_ + Evaluator::allow_cross_tenant_).
// Defaults to 0 / false so legacy / off-path callers still compile.
[[nodiscard]] inline bool
check_boundary(TenantId caller_principal, TenantId target,
               const IsolationRefProvenance* ref = nullptr, bool allow_cross_tenant = false,
               std::uint16_t required_effects = 0, bool sandbox_strict = false,
               std::string_view op = "workspace", bool sandbox_restricted = false) noexcept {
    TenantId ref_t = ref ? ref->tenant_id : 0;
    return g_workspace_isolation().check_boundary_ex(caller_principal, target, ref_t,
                                                     allow_cross_tenant, required_effects,
                                                     sandbox_strict, op, sandbox_restricted);
}

inline void reset_tenant_isolation_for_test() noexcept {
    g_workspace_isolation().clear_for_test();
    auto& m = g_tenant_isolation_metrics();
    m.tenant_boundary_checks_total.store(0, std::memory_order_relaxed);
    m.tenant_boundary_violation_prevented_total.store(0, std::memory_order_relaxed);
    m.cross_tenant_provenance_deny_total.store(0, std::memory_order_relaxed);
    m.cross_tenant_capability_grant_total.store(0, std::memory_order_relaxed);
    m.cross_tenant_capability_deny_total.store(0, std::memory_order_relaxed);
    m.isolation_audit_total.store(0, std::memory_order_relaxed);
    m.strict_sandbox_isolation_denials.store(0, std::memory_order_relaxed);
}

struct TenantIsolationStatsSnapshot {
    std::uint64_t checks = 0;
    std::uint64_t boundary_violations_prevented = 0;
    std::uint64_t cross_tenant_provenance_deny = 0;
    std::uint64_t cross_tenant_capability_grants = 0;
    std::uint64_t cross_tenant_capability_deny = 0;
    std::uint64_t audits = 0;
    std::uint64_t strict_denials = 0;
    std::uint64_t current_tenant = 0;
    int phase = kWorkspaceIsolationPhase;
    int issue = kWorkspaceIsolationIssue;
    int isolation_enabled = 0;
    int allow_cross = 0;
    int strict_linked = 0;
};

[[nodiscard]] inline TenantIsolationStatsSnapshot snapshot_tenant_isolation_stats() noexcept {
    auto& m = g_tenant_isolation_metrics();
    auto& p = g_workspace_isolation();
    return TenantIsolationStatsSnapshot{
        m.tenant_boundary_checks_total.load(std::memory_order_relaxed),
        m.tenant_boundary_violation_prevented_total.load(std::memory_order_relaxed),
        m.cross_tenant_provenance_deny_total.load(std::memory_order_relaxed),
        m.cross_tenant_capability_grant_total.load(std::memory_order_relaxed),
        m.cross_tenant_capability_deny_total.load(std::memory_order_relaxed),
        m.isolation_audit_total.load(std::memory_order_relaxed),
        m.strict_sandbox_isolation_denials.load(std::memory_order_relaxed),
        p.current.id,
        kWorkspaceIsolationPhase,
        kWorkspaceIsolationIssue,
        p.isolation_enabled ? 1 : 0,
        p.current.allow_cross_tenant ? 1 : 0,
        p.strict_sandbox_linked ? 1 : 0,
    };
}

} // namespace aura::core::workspace_isolation

#endif // AURA_CORE_WORKSPACE_ISOLATION_HH
