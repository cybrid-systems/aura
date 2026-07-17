// capability_model.hh — Issues #1180/#1187/#1192/#1565: Capability Effects enforcement.
// Header form for evaluator TUs + tests. Keep in sync with capability_model.ixx.

#ifndef AURA_CORE_CAPABILITY_MODEL_HH
#define AURA_CORE_CAPABILITY_MODEL_HH

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aura::core::capability {

inline constexpr int kCapabilityModelPhase = 2; // #1565 enforcement
inline constexpr int kCapabilityModelIssue = 1565;

// First-class effects (layout-stable uint16_t bitflags).
enum class Effect : std::uint16_t {
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    Exec = 1 << 2,
    Mutate = 1 << 3,
    Network = 1 << 4,
    Ffi = 1 << 5,
    Render = 1 << 6,
};

[[nodiscard]] constexpr Effect operator|(Effect a, Effect b) noexcept {
    return static_cast<Effect>(static_cast<std::uint16_t>(a) | static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr Effect operator&(Effect a, Effect b) noexcept {
    return static_cast<Effect>(static_cast<std::uint16_t>(a) & static_cast<std::uint16_t>(b));
}
[[nodiscard]] constexpr bool has_effect(Effect set, Effect bit) noexcept {
    return (static_cast<std::uint16_t>(set) & static_cast<std::uint16_t>(bit)) != 0;
}

using TenantId = std::uint64_t;

// Provenance snapshot for audit (does not change StableNodeRef layout).
struct EffectProvenance {
    std::uint32_t node_id = 0;
    std::uint16_t gen = 0;
    std::uint64_t mutation_id = 0;
    std::uint32_t workspace_id = 0;
    std::uint32_t fiber_id = 0;
    std::uint64_t epoch = 0;
};

// CapabilityGrant — keep field set stable (name, effects, tenant_id).
// Optional provenance binding lives alongside, not inside layout-critical core.
struct CapabilityGrant {
    std::string name; // owned for registry
    Effect effects = Effect::None;
    TenantId tenant_id = 0;
    // #1565: provenance binding + delegation audit (additive fields ok)
    std::uint64_t bound_mutation_id = 0;
    std::uint32_t bound_node_id = 0;
    std::uint64_t grant_epoch = 0;
    bool revoked = false;
};

// Sandbox mode mirror for effect checks (also in sandbox.ixx).
enum class EffectSandboxMode : std::uint8_t {
    Off = 0,
    Restricted = 1, // sandbox_mode_ style: need grant when active
    Strict = 2,     // always require grant for side effects
};

struct EffectAuditEntry {
    std::uint64_t seq = 0;
    std::uint64_t timestamp_ms = 0;
    Effect required = Effect::None;
    Effect actual = Effect::None;
    TenantId tenant_id = 0;
    EffectProvenance prov{};
    bool denied = false;
    char op[40]{};
};

struct CapabilityEffectMetrics {
    std::atomic<std::uint64_t> capability_effect_enforced_total{0};
    std::atomic<std::uint64_t> capability_effect_denied_total{0};
    std::atomic<std::uint64_t> capability_provenance_mismatch_total{0};
    std::atomic<std::uint64_t> capability_grant_total{0};
    std::atomic<std::uint64_t> capability_revoke_total{0};
    std::atomic<std::uint64_t> capability_check_total{0};
    std::atomic<std::uint64_t> capability_audit_total{0};
};

inline CapabilityEffectMetrics& g_capability_effect_metrics() noexcept {
    static CapabilityEffectMetrics m;
    return m;
}

// Process-wide grant registry + audit ring.
struct CapabilityRegistry {
    std::mutex mtx;
    // tenant_id → grants (multiple named grants OR'd for checks)
    std::unordered_map<TenantId, std::vector<CapabilityGrant>> by_tenant;
    EffectSandboxMode sandbox_mode = EffectSandboxMode::Off;
    TenantId default_tenant = 0;
    static constexpr std::size_t kAuditRing = 128;
    EffectAuditEntry audit_ring[kAuditRing]{};
    std::atomic<std::uint64_t> audit_seq{0};

    // Grant effects to a tenant (OR into named grant).
    void grant(TenantId tenant, std::string_view name, Effect effects,
               const EffectProvenance& prov = {}) {
        std::lock_guard<std::mutex> lock(mtx);
        auto& vec = by_tenant[tenant];
        for (auto& g : vec) {
            if (g.name == name) {
                g.effects = g.effects | effects;
                g.revoked = false;
                g.bound_mutation_id = prov.mutation_id;
                g.bound_node_id = prov.node_id;
                g.grant_epoch = prov.epoch;
                g_capability_effect_metrics().capability_grant_total.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
        }
        CapabilityGrant g;
        g.name = std::string(name);
        g.effects = effects;
        g.tenant_id = tenant;
        g.bound_mutation_id = prov.mutation_id;
        g.bound_node_id = prov.node_id;
        g.grant_epoch = prov.epoch;
        vec.push_back(std::move(g));
        g_capability_effect_metrics().capability_grant_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
    }

    void revoke(TenantId tenant, std::string_view name) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return;
        for (auto& g : it->second) {
            if (g.name == name) {
                g.revoked = true;
                g.effects = Effect::None;
                g_capability_effect_metrics().capability_revoke_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
    }

    // OR all non-revoked grants for tenant.
    [[nodiscard]] Effect effects_for(TenantId tenant) const {
        Effect acc = Effect::None;
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return acc;
        for (const auto& g : it->second) {
            if (!g.revoked)
                acc = acc | g.effects;
        }
        return acc;
    }

    // Optional provenance binding check: if grant has bound_mutation_id != 0
    // and caller's prov.mutation_id is non-zero and differs → mismatch.
    [[nodiscard]] bool provenance_ok(TenantId tenant, const EffectProvenance& prov) const {
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return true; // no grants → not a mismatch (denied separately)
        for (const auto& g : it->second) {
            if (g.revoked)
                continue;
            if (g.bound_mutation_id != 0 && prov.mutation_id != 0 &&
                g.bound_mutation_id != prov.mutation_id) {
                return false;
            }
        }
        return true;
    }

    void record_audit(Effect required, Effect actual, TenantId tenant, const EffectProvenance& prov,
                      bool denied, std::string_view op) {
        const auto seq = audit_seq.fetch_add(1, std::memory_order_relaxed);
        auto& slot = audit_ring[seq % kAuditRing];
        slot.seq = seq;
        slot.timestamp_ms = 0; // filled by caller if needed
        slot.required = required;
        slot.actual = actual;
        slot.tenant_id = tenant;
        slot.prov = prov;
        slot.denied = denied;
        const auto n = std::min(op.size(), sizeof(slot.op) - 1);
        std::memcpy(slot.op, op.data(), n);
        slot.op[n] = '\0';
        g_capability_effect_metrics().capability_audit_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
    }

    void clear_for_test() {
        std::lock_guard<std::mutex> lock(mtx);
        by_tenant.clear();
        sandbox_mode = EffectSandboxMode::Off;
        audit_seq.store(0, std::memory_order_relaxed);
    }
};

inline CapabilityRegistry& g_capability_registry() noexcept {
    static CapabilityRegistry r;
    return r;
}

// AC1: check_and_record_effect — core enforcement entry.
// Returns true if allowed. Always records audit + metrics.
//
// Policy:
//   Off: always allow (still audit as enforced)
//   Restricted: if sandbox_active, require grant bits
//   Strict: always require grant bits for non-None required effects
//
// wildcard_ok: caller may pass true if kCapWildcard held (Evaluator bridge).
inline bool check_and_record_effect(Effect required, Effect actual, const EffectProvenance& prov,
                                    TenantId tenant, std::string_view op = "effect",
                                    bool wildcard_ok = false, bool sandbox_active = false) {
    auto& reg = g_capability_registry();
    auto& met = g_capability_effect_metrics();
    met.capability_check_total.fetch_add(1, std::memory_order_relaxed);

    bool allowed = true;
    const auto mode = reg.sandbox_mode;
    const bool need_grant = (mode == EffectSandboxMode::Strict) ||
                            (mode == EffectSandboxMode::Restricted && sandbox_active);

    {
        std::lock_guard<std::mutex> lock(reg.mtx);
        if (need_grant && required != Effect::None && !wildcard_ok) {
            const Effect held = reg.effects_for(tenant);
            // Require full coverage of required bits (not just any overlap).
            const auto req_u = static_cast<std::uint16_t>(required);
            const auto held_u = static_cast<std::uint16_t>(held);
            if ((held_u & req_u) != req_u)
                allowed = false;
            if (allowed && !reg.provenance_ok(tenant, prov)) {
                allowed = false;
                met.capability_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (wildcard_ok) {
            if (!reg.provenance_ok(tenant, prov)) {
                met.capability_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        reg.record_audit(required, actual, tenant, prov, !allowed, op);
    }

    if (allowed)
        met.capability_effect_enforced_total.fetch_add(1, std::memory_order_relaxed);
    else
        met.capability_effect_denied_total.fetch_add(1, std::memory_order_relaxed);
    return allowed;
}

inline void reset_capability_effects_for_test() noexcept {
    g_capability_registry().clear_for_test();
    auto& m = g_capability_effect_metrics();
    m.capability_effect_enforced_total.store(0, std::memory_order_relaxed);
    m.capability_effect_denied_total.store(0, std::memory_order_relaxed);
    m.capability_provenance_mismatch_total.store(0, std::memory_order_relaxed);
    m.capability_grant_total.store(0, std::memory_order_relaxed);
    m.capability_revoke_total.store(0, std::memory_order_relaxed);
    m.capability_check_total.store(0, std::memory_order_relaxed);
    m.capability_audit_total.store(0, std::memory_order_relaxed);
}

struct CapabilityEffectStatsSnapshot {
    std::uint64_t enforced = 0;
    std::uint64_t denied = 0;
    std::uint64_t provenance_mismatch = 0;
    std::uint64_t grants = 0;
    std::uint64_t revokes = 0;
    std::uint64_t checks = 0;
    std::uint64_t audits = 0;
    int phase = kCapabilityModelPhase;
    int issue = kCapabilityModelIssue;
    int sandbox_mode = 0;
};

[[nodiscard]] inline CapabilityEffectStatsSnapshot snapshot_capability_effect_stats() noexcept {
    auto& m = g_capability_effect_metrics();
    return CapabilityEffectStatsSnapshot{
        m.capability_effect_enforced_total.load(std::memory_order_relaxed),
        m.capability_effect_denied_total.load(std::memory_order_relaxed),
        m.capability_provenance_mismatch_total.load(std::memory_order_relaxed),
        m.capability_grant_total.load(std::memory_order_relaxed),
        m.capability_revoke_total.load(std::memory_order_relaxed),
        m.capability_check_total.load(std::memory_order_relaxed),
        m.capability_audit_total.load(std::memory_order_relaxed),
        kCapabilityModelPhase,
        kCapabilityModelIssue,
        static_cast<int>(g_capability_registry().sandbox_mode),
    };
}

// Map security cap name → Effect bit.
[[nodiscard]] inline Effect effect_for_cap_name(std::string_view name) noexcept {
    if (name == "mutate")
        return Effect::Mutate;
    if (name == "io" || name == "io-read")
        return Effect::Read;
    if (name == "io-write")
        return Effect::Write;
    if (name == "exec")
        return Effect::Exec;
    if (name == "ffi")
        return Effect::Ffi;
    if (name == "network")
        return Effect::Network;
    if (name == "render")
        return Effect::Render;
    if (name == "*")
        return Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate | Effect::Network |
               Effect::Ffi | Effect::Render;
    return Effect::None;
}

} // namespace aura::core::capability

#endif // AURA_CORE_CAPABILITY_MODEL_HH
