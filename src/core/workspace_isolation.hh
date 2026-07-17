// workspace_isolation.hh — Issues #1180/#1183/#1566: WorkspaceIsolationPolicy
// enforcement (header form for evaluator TUs + tests).
// TenantPrincipal layout stable (id, name, allow_cross_tenant).

#ifndef AURA_CORE_WORKSPACE_ISOLATION_HH
#define AURA_CORE_WORKSPACE_ISOLATION_HH

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
    char op[40]{};
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
    static constexpr std::size_t kAuditRing = 128;
    IsolationAuditEntry audit_ring[kAuditRing]{};
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

    void record_audit(TenantId target, TenantId ref_tenant, bool denied, bool prov_deny,
                      bool cap_deny, std::string_view op) noexcept {
        const auto seq = audit_seq.fetch_add(1, std::memory_order_relaxed);
        auto& slot = audit_ring[seq % kAuditRing];
        slot.seq = seq;
        slot.current = current.id;
        slot.target = target;
        slot.ref_tenant = ref_tenant;
        slot.denied = denied;
        slot.provenance_deny = prov_deny;
        slot.capability_deny = cap_deny;
        const auto n = std::min(op.size(), sizeof(slot.op) - 1);
        std::memcpy(slot.op, op.data(), n);
        slot.op[n] = '\0';
        g_tenant_isolation_metrics().isolation_audit_total.fetch_add(1, std::memory_order_relaxed);
    }

    // AC1 legacy: simple ID boundary check.
    [[nodiscard]] bool check_boundary(TenantId target) noexcept {
        return check_boundary_ex(target, /*ref_tenant=*/0, /*required_effects=*/0,
                                 /*sandbox_strict=*/false, "boundary");
    }

    // AC1–4 enhanced: capability propagation + provenance + Strict sandbox.
    //
    // Policy:
    //   - current.id==0 or isolation disabled → allow (still count check)
    //   - current.allow_cross_tenant → allow
    //   - current.id == target (or target==0 meaning "same workspace") → allow
    //   - ref_tenant != 0 && ref_tenant != current.id && != target → provenance deny
    //   - cross_grants[current→target] covers required_effects → allow
    //   - sandbox_strict → no soft fallback; deny without grant
    //   - else deny (boundary violation prevented)
    [[nodiscard]] bool check_boundary_ex(TenantId target, TenantId ref_tenant,
                                         std::uint16_t required_effects, bool sandbox_strict,
                                         std::string_view op = "workspace") noexcept {
        auto& met = g_tenant_isolation_metrics();
        met.tenant_boundary_checks_total.fetch_add(1, std::memory_order_relaxed);

        bool allowed = true;
        bool prov_deny = false;
        bool cap_deny = false;

        {
            std::lock_guard<std::mutex> lock(mtx);
            ++boundary_checks;

            const bool strict = sandbox_strict || strict_sandbox_linked;
            const TenantId cur = current.id;

            // Disabled / unset tenant: permissive unless Strict forces a tenant.
            if (cur == 0 && !strict) {
                record_audit(target, ref_tenant, false, false, false, op);
                return true;
            }
            if (current.allow_cross_tenant) {
                record_audit(target, ref_tenant, false, false, false, op);
                return true;
            }
            // Same tenant or unscoped target → ok (still check ref provenance).
            if (target == 0 || cur == 0 || cur == target) {
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
            record_audit(target, ref_tenant, !allowed, prov_deny, cap_deny, op);
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
    }
};

inline WorkspaceIsolationPolicy& g_workspace_isolation() noexcept {
    static WorkspaceIsolationPolicy p;
    return p;
}

// Free-function convenience (matches issue pseudo-code).
[[nodiscard]] inline bool check_boundary(TenantId target,
                                         const IsolationRefProvenance* ref = nullptr,
                                         std::uint16_t required_effects = 0,
                                         bool sandbox_strict = false,
                                         std::string_view op = "workspace") noexcept {
    TenantId ref_t = ref ? ref->tenant_id : 0;
    return g_workspace_isolation().check_boundary_ex(target, ref_t, required_effects,
                                                     sandbox_strict, op);
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
