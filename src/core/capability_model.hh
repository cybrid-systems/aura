// capability_model.hh — Issues #1180/#1187/#1192/#1565: Capability Effects enforcement.
// Header form for evaluator TUs + tests. Keep in sync with capability_model.ixx.

#ifndef AURA_CORE_CAPABILITY_MODEL_HH
#define AURA_CORE_CAPABILITY_MODEL_HH

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
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
    // Issue #2023: Agent / multi-tenant macro self-evolution policy gate.
    // Distinct from Mutate — expand can run without mutate, but still needs
    // MacroSelfEvo when sandbox is Strict / Restricted+active.
    MacroSelfEvo = 1 << 7,
};

// Issue #2023: policy limits for macro expansion (capability layer).
// Internal hard depth (MAX_HYGIENE_DEPTH=1024) remains last-resort safety;
// these bounds are the supervisor-tunable policy. Zero max_depth or
// max_expansion_passes means "deny" when the capability check is active.
struct MacroSelfEvoPolicy {
    std::uint32_t max_expansion_passes = 32;
    std::uint32_t max_depth = 256; // tighter than internal hard limit 1024
    bool allow_rest_hygiene = true;
    bool allow_concurrent_fiber = true;
};

// Result of check_macro_self_evo (expand entry gate).
struct MacroSelfEvoCheck {
    bool allowed = true;
    MacroSelfEvoPolicy effective{};
    const char* deny_reason = nullptr; // stable string literal when !allowed
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
    // Issue #2023: MacroSelfEvo expand gate
    std::atomic<std::uint64_t> macro_self_evo_check_total{0};
    std::atomic<std::uint64_t> macro_self_evo_allowed_total{0};
    std::atomic<std::uint64_t> macro_self_evo_denied_total{0};
    std::atomic<std::uint64_t> macro_self_evo_depth_clamp_total{0};
    std::atomic<std::uint64_t> macro_self_evo_pass_clamp_total{0};
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
    // Issue #2023: per-tenant MacroSelfEvo policy limits (paired with
    // Effect::MacroSelfEvo grant bit). Absent entry → no grant.
    std::unordered_map<TenantId, MacroSelfEvoPolicy> macro_self_evo_by_tenant;
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

    // Issue #2023: grant MacroSelfEvo effect + store policy limits.
    // Single lock scope (grant/revoke also lock — do not nest).
    void grant_macro_self_evo(TenantId tenant, MacroSelfEvoPolicy policy = {}) {
        std::lock_guard<std::mutex> lock(mtx);
        auto& vec = by_tenant[tenant];
        bool found = false;
        for (auto& g : vec) {
            if (g.name == "macro-self-evo") {
                g.effects = g.effects | Effect::MacroSelfEvo;
                g.revoked = false;
                found = true;
                break;
            }
        }
        if (!found) {
            CapabilityGrant g;
            g.name = "macro-self-evo";
            g.effects = Effect::MacroSelfEvo;
            g.tenant_id = tenant;
            vec.push_back(std::move(g));
        }
        macro_self_evo_by_tenant[tenant] = policy;
        g_capability_effect_metrics().capability_grant_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
    }

    void revoke_macro_self_evo(TenantId tenant) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_tenant.find(tenant);
        if (it != by_tenant.end()) {
            for (auto& g : it->second) {
                if (g.name == "macro-self-evo") {
                    g.revoked = true;
                    g.effects = Effect::None;
                    g_capability_effect_metrics().capability_revoke_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }
        macro_self_evo_by_tenant.erase(tenant);
    }

    [[nodiscard]] std::optional<MacroSelfEvoPolicy> macro_self_evo_policy(TenantId tenant) const {
        // Caller should hold mtx for a strict snapshot; unlocked read is
        // best-effort for observability (map may race with grant/revoke).
        auto it = macro_self_evo_by_tenant.find(tenant);
        if (it == macro_self_evo_by_tenant.end())
            return std::nullopt;
        return it->second;
    }

    void clear_for_test() {
        std::lock_guard<std::mutex> lock(mtx);
        by_tenant.clear();
        macro_self_evo_by_tenant.clear();
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
    m.macro_self_evo_check_total.store(0, std::memory_order_relaxed);
    m.macro_self_evo_allowed_total.store(0, std::memory_order_relaxed);
    m.macro_self_evo_denied_total.store(0, std::memory_order_relaxed);
    m.macro_self_evo_depth_clamp_total.store(0, std::memory_order_relaxed);
    m.macro_self_evo_pass_clamp_total.store(0, std::memory_order_relaxed);
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
    // Issue #2023
    std::uint64_t macro_self_evo_checks = 0;
    std::uint64_t macro_self_evo_allowed = 0;
    std::uint64_t macro_self_evo_denied = 0;
    std::uint64_t macro_self_evo_depth_clamps = 0;
    std::uint64_t macro_self_evo_pass_clamps = 0;
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
        m.macro_self_evo_check_total.load(std::memory_order_relaxed),
        m.macro_self_evo_allowed_total.load(std::memory_order_relaxed),
        m.macro_self_evo_denied_total.load(std::memory_order_relaxed),
        m.macro_self_evo_depth_clamp_total.load(std::memory_order_relaxed),
        m.macro_self_evo_pass_clamp_total.load(std::memory_order_relaxed),
    };
}

// Issue #2023: consult MacroSelfEvo capability at macro expand entry.
//
// Policy:
//   Sandbox Off: always allow; effective.max_* = 0 means "no clamp"
//                (caller max_passes + internal MAX_HYGIENE_DEPTH).
//   Strict / Restricted+active without MacroSelfEvo grant: deny.
//   Granted with max_depth==0 or max_expansion_passes==0: deny (zero limits).
//   Granted with positive limits: allow + return policy for clamping.
//
// wildcard_ok: kCapWildcard holders inherit default permissive MacroSelfEvo
// with default policy (32 passes / 256 depth) when no explicit grant.
[[nodiscard]] inline MacroSelfEvoCheck check_macro_self_evo(TenantId tenant,
                                                            bool sandbox_active = false,
                                                            bool wildcard_ok = false) noexcept {
    auto& reg = g_capability_registry();
    auto& met = g_capability_effect_metrics();
    met.macro_self_evo_check_total.fetch_add(1, std::memory_order_relaxed);

    MacroSelfEvoCheck out;
    const auto mode = reg.sandbox_mode;
    const bool need_grant = (mode == EffectSandboxMode::Strict) ||
                            (mode == EffectSandboxMode::Restricted && sandbox_active);

    if (!need_grant) {
        // Off / Restricted-inactive: preserve historical unconstrained behaviour.
        out.allowed = true;
        out.effective.max_expansion_passes = 0; // 0 = no pass clamp
        out.effective.max_depth = 0;            // 0 = use MAX_HYGIENE_DEPTH
        out.effective.allow_rest_hygiene = true;
        out.effective.allow_concurrent_fiber = true;
        met.macro_self_evo_allowed_total.fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    std::lock_guard<std::mutex> lock(reg.mtx);
    const Effect held = reg.effects_for(tenant);
    const bool has_bit = has_effect(held, Effect::MacroSelfEvo);
    auto pol_it = reg.macro_self_evo_by_tenant.find(tenant);
    const bool has_policy = pol_it != reg.macro_self_evo_by_tenant.end();

    if (!has_bit && !wildcard_ok) {
        out.allowed = false;
        out.deny_reason = "MacroSelfEvo capability not granted";
        met.macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, {}, true, "macro-self-evo");
        return out;
    }

    MacroSelfEvoPolicy pol{};
    if (has_policy)
        pol = pol_it->second;
    else if (wildcard_ok) {
        // Wildcard → default policy (still bounded vs unconstrained Off mode).
        pol = MacroSelfEvoPolicy{};
    } else {
        out.allowed = false;
        out.deny_reason = "MacroSelfEvo policy missing";
        met.macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, {}, true, "macro-self-evo");
        return out;
    }

    // Zero limits = explicit deny (AC: not granted or limits are zero).
    if (pol.max_depth == 0 || pol.max_expansion_passes == 0) {
        out.allowed = false;
        out.deny_reason = "MacroSelfEvo limits are zero";
        met.macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, {}, true, "macro-self-evo");
        return out;
    }

    out.allowed = true;
    out.effective = pol;
    met.macro_self_evo_allowed_total.fetch_add(1, std::memory_order_relaxed);
    reg.record_audit(Effect::MacroSelfEvo, held | Effect::MacroSelfEvo, tenant, {}, false,
                     "macro-self-evo");
    return out;
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
    if (name == "macro-self-evo" || name == "macro_self_evo" || name == "MacroSelfEvo")
        return Effect::MacroSelfEvo;
    if (name == "*")
        return Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate | Effect::Network |
               Effect::Ffi | Effect::Render | Effect::MacroSelfEvo;
    return Effect::None;
}

} // namespace aura::core::capability

#endif // AURA_CORE_CAPABILITY_MODEL_HH
