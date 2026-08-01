// capability_model.hh — Issues #1180/#1187/#1192/#1565: Capability Effects enforcement.
// Header form for evaluator TUs + tests. Keep in sync with capability_model.ixx.

#ifndef AURA_CORE_CAPABILITY_MODEL_HH
#define AURA_CORE_CAPABILITY_MODEL_HH

#include "core/workspace_epoch.hh"
#include "core/security_event.hh"     // #2388 dual-write kinds
#include "core/security_event_wal.hh" // #2388 emit_security_event_durable

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace aura::core::capability {

inline constexpr int kCapabilityModelPhase = 2; // #1565 enforcement
inline constexpr int kCapabilityModelIssue = 1565;
// Issue #2055: grant/revoke bound to WorkspaceEpoch Mutation + fiber.
inline constexpr int kGrantEpochFiberBindIssue = 2055;
// Issue #2154: sliding grant_min_valid_epoch window on Mutation epoch bump.
inline constexpr int kGrantEpochRetainWindowIssue = 2154;
// Production multi-tenant / Strict default retain window (last K epochs).
inline constexpr std::uint64_t kDefaultGrantEpochRetainWindowMultiTenant = 64;

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
    // Issue #2387: previously string-only sensitive caps promoted into the
    // Effect matrix so has_capability / grant_epoch / fiber bind share one
    // authority with Mutate/FFI. Non-security compile-stats / fiber / query
    // display names stay string-list only (staged).
    TenantAdmin = 1 << 8, // tenant-admin
    Syscall = 1 << 9,     // syscall (high-risk arbitrary syscall)
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
    // Issue #2243: enforce strict-mode hygiene defaults under multi-tenant.
    // Default-ON for granted policies; Off-mode policy overrides to false below.
    bool force_hygienic = true;
    // 0 = unlimited. Hard ceiling on name_map size during expand; prevents
    // memory amplification from adversarial macros.
    std::uint32_t max_gensym_map_size = 0;
    // 0 = unlimited. Maps into the C-linkage per-fiber violation budget
    // helper at expand entry, complementing #2241's #2097 violation counter.
    std::uint32_t max_violations_per_fiber = 0;
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
// Issue #2149: `epoch` is WorkspaceEpoch **Mutation** only (same counter
// as make_grant_provenance / CapabilityGrant::grant_epoch). Bridge is
// AOT/JIT/closure freshness — not a security fence key.
struct EffectProvenance {
    std::uint32_t node_id = 0;
    std::uint16_t gen = 0;
    std::uint64_t mutation_id = 0;
    std::uint32_t workspace_id = 0;
    std::uint32_t fiber_id = 0;
    std::uint64_t epoch = 0; // Mutation epoch (never Bridge as primary)
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
    std::uint64_t grant_epoch = 0; // WorkspaceEpoch Mutation at grant (#2055)
    // Issue #2055: fiber that issued the grant (audit / cross-fiber blame).
    std::uint32_t grant_fiber_id = 0;
    // Issue #2055: mutation epoch at revoke time (0 = never revoked / legacy).
    std::uint64_t revoke_epoch = 0;
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
    // Issue #2427 AC4: sandbox_mode observed at record time (policy flip audit).
    EffectSandboxMode sandbox_mode = EffectSandboxMode::Off;
    char op[40]{};
};

// Issue #2425: per-slot atomic publish so readers never observe a torn
// multi-field EffectAuditEntry. Writer fills `data` then stores
// publish_seq = entry.seq + 1 with release. Reader acquire-loads
// publish_seq, copies data under audit_ring_mtx_, re-checks publish_seq
// (double-check). 0 = never published / cleared.
struct PublishedAuditSlot {
    std::atomic<std::uint64_t> publish_seq{0};
    EffectAuditEntry data{};
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
    // Issue #2055: WorkspaceEpoch + fiber bind observability
    std::atomic<std::uint64_t> capability_grant_epoch_bound_total{0};
    std::atomic<std::uint64_t> capability_revoke_epoch_bound_total{0};
    std::atomic<std::uint64_t> capability_grant_fiber_bound_total{0};
    std::atomic<std::uint64_t> capability_fiber_mismatch_total{0};
    std::atomic<std::uint64_t> capability_epoch_fence_hit_total{0};
    // Issue #2149: Mutation vs Bridge diverge on effect-check path
    // (observability only; Bridge is never the security fence key).
    std::atomic<std::uint64_t> capability_mutation_bridge_split_total{0};
    // Issue #2151: hard-deny on grant_fiber_id mismatch (when policy on).
    std::atomic<std::uint64_t> capability_fiber_hard_deny_total{0};
    // Issue #2154: sliding grant_min_valid window advanced on epoch bump.
    std::atomic<std::uint64_t> capability_grant_epoch_window_advance_total{0};
};

// Issue #2149: security provenance vocabulary — Mutation only.
inline constexpr int kEffectEpochUnifyIssue = 2149;
// Issue #2151: optional hard-deny on grant_fiber_id mismatch.
inline constexpr int kHardFiberIsolationIssue = 2151;

// Test/production optional override for EffectProvenance fiber stamping.
// Non-zero → use this id instead of aura_fiber_current_id() (0 = use TLS fiber).
// Production always leaves at 0; tests simulate fiber A vs B without a scheduler.
[[nodiscard]] inline std::atomic<std::uint32_t>& g_effect_fiber_id_override() noexcept {
    static std::atomic<std::uint32_t> o{0};
    return o;
}
inline void set_effect_fiber_id_override(std::uint32_t id) noexcept {
    g_effect_fiber_id_override().store(id, std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint32_t effect_fiber_id_or(std::uint32_t live_fiber_id) noexcept {
    const auto o = g_effect_fiber_id_override().load(std::memory_order_relaxed);
    return o != 0 ? o : live_fiber_id;
}

inline CapabilityEffectMetrics& g_capability_effect_metrics() noexcept {
    static CapabilityEffectMetrics m;
    return m;
}

// Issue #2426 / #2427: atomic-backed fields with assignment + conversion so
// existing `reg.sandbox_mode = M` / `reg.sandbox_mode == M` keep working
// (AC3 signature preservation) while lock-free readers use acquire loads.
// #2427 F3: sandbox_mode was plain EffectSandboxMode (uint8) — policy flip race.
// #2427 F4: default_tenant was plain TenantId — same pattern; fixed together.
static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
              "Issue #2427: uint8 atomic for sandbox_mode must be lock-free");
static_assert(std::atomic<TenantId>::is_always_lock_free,
              "Issue #2427: TenantId atomic for default_tenant must be lock-free");
struct AtomicEffectSandboxMode {
    std::atomic<std::uint8_t> v{static_cast<std::uint8_t>(EffectSandboxMode::Off)};
    AtomicEffectSandboxMode() noexcept = default;
    AtomicEffectSandboxMode(EffectSandboxMode m) noexcept {
        v.store(static_cast<std::uint8_t>(m), std::memory_order_relaxed);
    }
    AtomicEffectSandboxMode(const AtomicEffectSandboxMode&) = delete;
    AtomicEffectSandboxMode& operator=(const AtomicEffectSandboxMode&) = delete;
    // Writers: release store (Issue #2427 AC1/AC3).
    AtomicEffectSandboxMode& operator=(EffectSandboxMode m) noexcept {
        v.store(static_cast<std::uint8_t>(m), std::memory_order_release);
        return *this;
    }
    // Readers: acquire load.
    [[nodiscard]] operator EffectSandboxMode() const noexcept {
        return static_cast<EffectSandboxMode>(v.load(std::memory_order_acquire));
    }
    [[nodiscard]] EffectSandboxMode
    load(std::memory_order o = std::memory_order_acquire) const noexcept {
        return static_cast<EffectSandboxMode>(v.load(o));
    }
    void store(EffectSandboxMode m, std::memory_order o = std::memory_order_release) noexcept {
        v.store(static_cast<std::uint8_t>(m), o);
    }
};

struct AtomicTenantId {
    std::atomic<TenantId> v{0};
    AtomicTenantId() noexcept = default;
    explicit AtomicTenantId(TenantId t) noexcept { v.store(t, std::memory_order_relaxed); }
    AtomicTenantId(const AtomicTenantId&) = delete;
    AtomicTenantId& operator=(const AtomicTenantId&) = delete;
    AtomicTenantId& operator=(TenantId t) noexcept {
        v.store(t, std::memory_order_release);
        return *this;
    }
    [[nodiscard]] operator TenantId() const noexcept { return v.load(std::memory_order_acquire); }
    [[nodiscard]] TenantId load(std::memory_order o = std::memory_order_acquire) const noexcept {
        return v.load(o);
    }
    void store(TenantId t, std::memory_order o = std::memory_order_release) noexcept {
        v.store(t, o);
    }
};

// Issue #2426: consistent multi-field view (#1840 snapshot_verify_dirty_totals).
struct RegistryStateSnapshot {
    EffectSandboxMode sandbox_mode = EffectSandboxMode::Off;
    TenantId default_tenant = 0;
    std::uint64_t grant_min_valid_epoch = 0;
    std::uint64_t grant_epoch_retain_window = 0;
    bool hard_fiber_isolation = false;
    std::uint64_t audit_seq = 0;
};

// Process-wide grant registry + audit ring.
struct CapabilityRegistry {
    std::mutex mtx;
    // tenant_id → grants (multiple named grants OR'd for checks)
    std::unordered_map<TenantId, std::vector<CapabilityGrant>> by_tenant;
    // Issue #2023: per-tenant MacroSelfEvo policy limits (paired with
    // Effect::MacroSelfEvo grant bit). Absent entry → no grant.
    std::unordered_map<TenantId, MacroSelfEvoPolicy> macro_self_evo_by_tenant;
    // Issue #2426 / #2427: atomic (was plain enum / TenantId — policy flip race).
    AtomicEffectSandboxMode sandbox_mode{};
    AtomicTenantId default_tenant{};
    static constexpr std::size_t kAuditRing = 128;
    // Issue #2425: published slots (data + atomic publish_seq).
    // audit_ring_mtx_: exclusive for slot write / clear; shared for
    // try_load_* so concurrent readers are TSAN-clean with writers.
    // Global audit_seq is independent (fetch_add only); slot body uses
    // the mutex + publish_seq release/acquire pair.
    mutable std::shared_mutex audit_ring_mtx_;
    PublishedAuditSlot audit_ring[kAuditRing]{};
    std::atomic<std::uint64_t> audit_seq{0};
    // Issue #2074: anti privilege-sticky — min mutation epoch a grant
    // must have been issued at to be considered valid. Grants with
    // grant_epoch < grant_min_valid_epoch_ are denied in provenance_ok().
    // 0 = disabled (legacy behavior). Set via set_grant_min_valid_epoch.
    std::atomic<std::uint64_t> grant_min_valid_epoch_{0};
    // Issue #2154: retain last K mutation epochs of grants (0 = no auto
    // advance; default 0 for compat). On Mutation epoch bump to new_ep,
    // when K>0 && new_ep>K, min_valid advances to new_ep-K (forward only).
    std::atomic<std::uint64_t> grant_epoch_retain_window_{0};
    // Issue #2151: when true, grant_fiber_id mismatch is a hard deny
    // (not observability-only). Default false preserves #2055 soft share.
    std::atomic<bool> hard_fiber_isolation_{false};

    // Issue #2074: anti privilege-sticky accessors.
    void set_grant_min_valid_epoch(std::uint64_t epoch) noexcept {
        grant_min_valid_epoch_.store(epoch, std::memory_order_release);
    }
    [[nodiscard]] std::uint64_t grant_min_valid_epoch() const noexcept {
        return grant_min_valid_epoch_.load(std::memory_order_acquire);
    }

    // Issue #2154: sliding retain-window policy (0 disables auto fence).
    void set_grant_epoch_retain_window(std::uint64_t k) noexcept {
        grant_epoch_retain_window_.store(k, std::memory_order_release);
        // Apply immediately against the current Mutation epoch so operators
        // enabling K mid-process get a fence without waiting for a bump.
        if (k > 0)
            on_mutation_epoch_bump(::aura::core::current_mutation_epoch());
    }
    [[nodiscard]] std::uint64_t grant_epoch_retain_window() const noexcept {
        return grant_epoch_retain_window_.load(std::memory_order_acquire);
    }

    // Issue #2154: called from bump_mutation_epoch via process hook.
    // When retain window K is set and new_ep > K, raise min_valid to
    // new_ep - K (never lowers an existing higher manual fence).
    void on_mutation_epoch_bump(std::uint64_t new_ep) noexcept {
        const auto k = grant_epoch_retain_window_.load(std::memory_order_acquire);
        if (k == 0 || new_ep <= k)
            return;
        const auto next_min = new_ep - k;
        auto cur = grant_min_valid_epoch_.load(std::memory_order_acquire);
        while (next_min > cur) {
            if (grant_min_valid_epoch_.compare_exchange_weak(
                    cur, next_min, std::memory_order_acq_rel, std::memory_order_acquire)) {
                g_capability_effect_metrics().capability_grant_epoch_window_advance_total.fetch_add(
                    1, std::memory_order_relaxed);
                break;
            }
        }
    }

    // Issue #2151: hard fiber isolation policy.
    void set_hard_fiber_isolation(bool on) noexcept {
        hard_fiber_isolation_.store(on, std::memory_order_release);
    }
    [[nodiscard]] bool hard_fiber_isolation() const noexcept {
        return hard_fiber_isolation_.load(std::memory_order_acquire);
    }

    // Issue #2426: multi-field consistent snapshot (#1840 pattern).
    // Double-check acquire on all atomic policy fields so concurrent
    // set_hard_fiber / set_grant_min_valid / sandbox_mode writes cannot
    // yield a torn mix (e.g. new min_valid with old hard_fiber flag).
    [[nodiscard]] RegistryStateSnapshot snapshot_registry_state() const noexcept {
        RegistryStateSnapshot s;
        for (int attempt = 0; attempt < 16; ++attempt) {
            s.sandbox_mode = sandbox_mode.load(std::memory_order_acquire);
            s.default_tenant = default_tenant.load(std::memory_order_acquire);
            s.grant_min_valid_epoch = grant_min_valid_epoch_.load(std::memory_order_acquire);
            s.grant_epoch_retain_window =
                grant_epoch_retain_window_.load(std::memory_order_acquire);
            s.hard_fiber_isolation = hard_fiber_isolation_.load(std::memory_order_acquire);
            s.audit_seq = audit_seq.load(std::memory_order_acquire);
            if (sandbox_mode.load(std::memory_order_acquire) == s.sandbox_mode &&
                default_tenant.load(std::memory_order_acquire) == s.default_tenant &&
                grant_min_valid_epoch_.load(std::memory_order_acquire) == s.grant_min_valid_epoch &&
                grant_epoch_retain_window_.load(std::memory_order_acquire) ==
                    s.grant_epoch_retain_window &&
                hard_fiber_isolation_.load(std::memory_order_acquire) == s.hard_fiber_isolation &&
                audit_seq.load(std::memory_order_acquire) == s.audit_seq) {
                return s;
            }
        }
        return s; // best-effort after retries
    }

    // Grant effects to a tenant (OR into named grant).
    // Issue #2055: stamps grant_epoch (WorkspaceEpoch Mutation) + grant_fiber_id
    // from EffectProvenance so long-running multi-tenant blame stays consistent.
    void grant(TenantId tenant, std::string_view name, Effect effects,
               const EffectProvenance& prov = {}) {
        std::lock_guard<std::mutex> lock(mtx);
        auto& vec = by_tenant[tenant];
        auto apply = [&](CapabilityGrant& g) {
            g.effects = g.effects | effects;
            g.revoked = false;
            g.bound_mutation_id = prov.mutation_id;
            g.bound_node_id = prov.node_id;
            g.grant_epoch = prov.epoch;
            g.grant_fiber_id = prov.fiber_id;
            g.revoke_epoch = 0;
            auto& met = g_capability_effect_metrics();
            met.capability_grant_total.fetch_add(1, std::memory_order_relaxed);
            if (prov.epoch != 0)
                met.capability_grant_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
            if (prov.fiber_id != 0)
                met.capability_grant_fiber_bound_total.fetch_add(1, std::memory_order_relaxed);
        };
        for (auto& g : vec) {
            if (g.name == name) {
                apply(g);
                return;
            }
        }
        CapabilityGrant g;
        g.name = std::string(name);
        g.effects = effects;
        g.tenant_id = tenant;
        apply(g);
        vec.push_back(std::move(g));
    }

    // Issue #2055: revoke stamps revoke_epoch (WorkspaceEpoch Mutation) for audit.
    // If revoke_at_epoch == 0, callers should pass current_mutation_epoch() (or
    // the make_grant_provenance helper) so blame trails stay non-zero.
    void revoke(TenantId tenant, std::string_view name, std::uint64_t revoke_at_epoch = 0) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return;
        for (auto& g : it->second) {
            if (g.name == name) {
                g.revoked = true;
                g.effects = Effect::None;
                // Prefer explicit epoch; else stamp current Mutation epoch.
                auto ep = revoke_at_epoch;
                if (ep == 0)
                    ep = ::aura::core::current_mutation_epoch();
                if (ep == 0)
                    ep = 1; // non-zero audit stamp at process origin
                g.revoke_epoch = ep;
                auto& met = g_capability_effect_metrics();
                met.capability_revoke_total.fetch_add(1, std::memory_order_relaxed);
                met.capability_revoke_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
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
    // Issue #2074: anti privilege-sticky — if grant has grant_epoch != 0
    // AND the registry's min_valid_epoch is set AND grant_epoch < min_valid_epoch,
    // the grant is expired (issued at a stale mutation epoch) → deny.
    // Issue #2055 / #2151: grant_fiber_id mismatch:
    //   hard_fiber_isolation=false (default) → metric only, allow (same-tenant
    //     multi-fiber share grants; TenantScope remains principal boundary).
    //   hard_fiber_isolation=true → deny + capability_fiber_hard_deny_total
    //     (commercial multi-tenant Strict / AURA_HARD_FIBER_ISOLATION=1).
    [[nodiscard]] bool provenance_ok(TenantId tenant, const EffectProvenance& prov) const {
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return true; // no grants → not a mismatch (denied separately)
        const bool hard_fiber = hard_fiber_isolation_.load(std::memory_order_acquire);
        for (const auto& g : it->second) {
            if (g.revoked)
                continue;
            if (g.bound_mutation_id != 0 && prov.mutation_id != 0 &&
                g.bound_mutation_id != prov.mutation_id) {
                return false;
            }
            // Issue #2074 / #2055 / #2154: expired grant — grant_epoch behind
            // min_valid (manual set or sliding retain window).
            const auto min_valid = grant_min_valid_epoch_.load(std::memory_order_acquire);
            if (g.grant_epoch != 0 && min_valid != 0 && g.grant_epoch < min_valid) {
                g_capability_effect_metrics().capability_epoch_fence_hit_total.fetch_add(
                    1, std::memory_order_relaxed);
                return false;
            }
            // Fiber mismatch: soft (metric) or hard deny (#2151).
            if (g.grant_fiber_id != 0 && prov.fiber_id != 0 && g.grant_fiber_id != prov.fiber_id) {
                g_capability_effect_metrics().capability_fiber_mismatch_total.fetch_add(
                    1, std::memory_order_relaxed);
                if (hard_fiber) {
                    g_capability_effect_metrics().capability_fiber_hard_deny_total.fetch_add(
                        1, std::memory_order_relaxed);
                    return false;
                }
            }
        }
        return true;
    }

    // Issue #2055: lookup a grant for tests / audit (newest name match).
    [[nodiscard]] bool find_grant(TenantId tenant, std::string_view name, CapabilityGrant& out) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return false;
        for (const auto& g : it->second) {
            if (g.name == name) {
                out = g;
                return true;
            }
        }
        return false;
    }

    // Issue #2388: private 128-slot ring + dual-write SecurityEvent/WAL so
    // wrap under deny storms remains forensically recoverable (ring 1024 + WAL).
    // reason_hint: optional Agent-stable reason (e.g. fiber-grant-mismatch).
    //
    // Issue #2425: build a full EffectAuditEntry locally, then under
    // exclusive audit_ring_mtx_ store data and publish_seq (release).
    // Readers use try_load_audit_* (shared lock + acquire double-check).
    void record_audit(Effect required, Effect actual, TenantId tenant, const EffectProvenance& prov,
                      bool denied, std::string_view op, const char* reason_hint = nullptr) {
        // AC3: release on audit_seq increment pairs with acquire loads.
        const auto seq = audit_seq.fetch_add(1, std::memory_order_release);
        EffectAuditEntry entry{};
        entry.seq = seq;
        entry.timestamp_ms = 0; // filled by caller if needed
        entry.required = required;
        entry.actual = actual;
        entry.tenant_id = tenant;
        entry.prov = prov;
        entry.denied = denied;
        // Issue #2427 AC4: stamp policy mode observed at audit time.
        entry.sandbox_mode = sandbox_mode.load(std::memory_order_acquire);
        const auto n = std::min(op.size(), sizeof(entry.op) - 1);
        std::memcpy(entry.op, op.data(), n);
        entry.op[n] = '\0';
        {
            std::unique_lock<std::shared_mutex> wlock(audit_ring_mtx_);
            auto& slot = audit_ring[seq % kAuditRing];
            slot.data = entry;
            // Publish after full write: readers that observe this value
            // (acquire) see a complete entry, never a mid-field mix.
            slot.publish_seq.store(seq + 1, std::memory_order_release);
        }
        g_capability_effect_metrics().capability_audit_total.fetch_add(1,
                                                                       std::memory_order_relaxed);

        // Dual-write unified SecurityEvent surface (#2075/#2054/#2225/#2388).
        using ::aura::core::security_event::kSecurityAuditFoldIssue;
        using ::aura::core::security_event::SecurityEventKind;
        using ::aura::core::security_event_wal::emit_security_event_durable;
        (void)kSecurityAuditFoldIssue;
        const auto mid = prov.mutation_id != 0
                             ? prov.mutation_id
                             : (prov.epoch != 0 ? prov.epoch : static_cast<std::uint64_t>(1));
        const auto epoch = prov.epoch != 0 ? prov.epoch : mid;
        const auto kind = denied ? SecurityEventKind::EffectDeny : SecurityEventKind::EffectAllow;
        const char* reason = reason_hint;
        if (reason == nullptr) {
            reason = denied ? "capability-effect-deny" : "effect-allow";
            if (denied) {
                const auto bits = static_cast<std::uint16_t>(required);
                if (has_effect(required, Effect::Mutate))
                    reason = "mutate-deny";
                else if (has_effect(required, Effect::Ffi))
                    reason = "ffi-deny";
                else if (has_effect(required, Effect::Render))
                    reason = "render-deny";
                (void)bits;
            }
        }
        emit_security_event_durable(kind, tenant, mid, epoch, static_cast<std::uint16_t>(required),
                                    op, reason, denied, static_cast<std::int64_t>(prov.fiber_id));
    }

    // Issue #2425: acquire load of global audit_seq.
    [[nodiscard]] std::uint64_t load_audit_seq() const noexcept {
        return audit_seq.load(std::memory_order_acquire);
    }

    // Issue #2425: TSAN-clean snapshot of the slot that held `seq`
    // (seq % kAuditRing). Returns false if never published, overwritten
    // by a newer wrap, or publish_seq double-check fails.
    [[nodiscard]] bool try_load_audit_seq(std::uint64_t seq, EffectAuditEntry& out) const noexcept {
        const auto idx = seq % kAuditRing;
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::shared_lock<std::shared_mutex> rlock(audit_ring_mtx_);
            const auto& slot = audit_ring[idx];
            const auto pub = slot.publish_seq.load(std::memory_order_acquire);
            if (pub == 0)
                return false;
            // Fully published marker is entry.seq + 1.
            if (pub != seq + 1) {
                // Slot reused by a different generation (ring wrap) or
                // concurrent publish still settling — retry once more.
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

    // Issue #2425: load the most recently published entry (best-effort
    // under concurrency / wrap). False when ring is empty.
    [[nodiscard]] bool try_load_latest_audit(EffectAuditEntry& out) const noexcept {
        const auto seq = load_audit_seq();
        if (seq == 0)
            return false;
        // audit_seq is next-to-allocate; latest published is seq - 1.
        return try_load_audit_seq(seq - 1, out);
    }

    // Issue #2023 / #2386: grant MacroSelfEvo effect + store policy limits.
    // Issue #2386: stamp grant_epoch / grant_fiber_id / bound_mutation_id
    // with the same apply discipline as grant() (#2055) so retain-window
    // (#2154) and hard fiber isolation (#2151) apply to macro expand grants.
    // Single lock scope (grant/revoke also lock — do not nest).
    // Callers should pass make_grant_provenance(...) when available; empty
    // prov is filled with non-zero Mutation epoch (force-bind style).
    void grant_macro_self_evo(TenantId tenant, MacroSelfEvoPolicy policy = {},
                              const EffectProvenance& prov_in = {}) {
        EffectProvenance prov = prov_in;
        // Always ensure non-zero epoch stamp (parity with make_grant_provenance).
        if (prov.epoch == 0) {
            const auto me = ::aura::core::current_mutation_epoch();
            prov.epoch = me != 0 ? me : 1;
        }
        if (prov.mutation_id == 0)
            prov.mutation_id = prov.epoch;
        if (prov.fiber_id == 0)
            prov.fiber_id = effect_fiber_id_or(0);

        std::lock_guard<std::mutex> lock(mtx);
        auto& vec = by_tenant[tenant];
        auto apply = [&](CapabilityGrant& g) {
            g.effects = g.effects | Effect::MacroSelfEvo;
            g.revoked = false;
            g.bound_mutation_id = prov.mutation_id;
            g.bound_node_id = prov.node_id;
            g.grant_epoch = prov.epoch;
            g.grant_fiber_id = prov.fiber_id;
            g.revoke_epoch = 0;
            auto& met = g_capability_effect_metrics();
            met.capability_grant_total.fetch_add(1, std::memory_order_relaxed);
            if (prov.epoch != 0)
                met.capability_grant_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
            if (prov.fiber_id != 0)
                met.capability_grant_fiber_bound_total.fetch_add(1, std::memory_order_relaxed);
        };
        for (auto& g : vec) {
            if (g.name == "macro-self-evo") {
                apply(g);
                macro_self_evo_by_tenant[tenant] = policy;
                return;
            }
        }
        CapabilityGrant g;
        g.name = "macro-self-evo";
        g.effects = Effect::MacroSelfEvo;
        g.tenant_id = tenant;
        apply(g);
        vec.push_back(std::move(g));
        macro_self_evo_by_tenant[tenant] = policy;
    }

    // Issue #2023 / #2386: revoke MacroSelfEvo + stamp revoke_epoch (#2055).
    void revoke_macro_self_evo(TenantId tenant, std::uint64_t revoke_at_epoch = 0) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_tenant.find(tenant);
        if (it != by_tenant.end()) {
            for (auto& g : it->second) {
                if (g.name == "macro-self-evo") {
                    g.revoked = true;
                    g.effects = Effect::None;
                    auto ep = revoke_at_epoch;
                    if (ep == 0)
                        ep = ::aura::core::current_mutation_epoch();
                    if (ep == 0)
                        ep = 1;
                    g.revoke_epoch = ep;
                    auto& met = g_capability_effect_metrics();
                    met.capability_revoke_total.fetch_add(1, std::memory_order_relaxed);
                    met.capability_revoke_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
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
        {
            // Issue #2425: clear published slots under ring mutex.
            std::unique_lock<std::shared_mutex> wlock(audit_ring_mtx_);
            for (auto& slot : audit_ring) {
                slot.data = EffectAuditEntry{};
                slot.publish_seq.store(0, std::memory_order_relaxed);
            }
            audit_seq.store(0, std::memory_order_relaxed);
        }
        grant_min_valid_epoch_.store(0, std::memory_order_relaxed);
        grant_epoch_retain_window_.store(0, std::memory_order_relaxed);
        hard_fiber_isolation_.store(false, std::memory_order_relaxed);
    }
};

// Forward decl — trampoline body needs the registry accessor.
[[nodiscard]] inline CapabilityRegistry& g_capability_registry() noexcept;

// Issue #2154: trampoline installed once so Mutation epoch bumps advance
// the sliding grant fence without workspace_epoch → capability include cycle.
inline void grant_epoch_window_bump_trampoline(std::uint64_t new_ep) noexcept {
    g_capability_registry().on_mutation_epoch_bump(new_ep);
}

inline void install_grant_epoch_window_hook() noexcept {
    static std::atomic<bool> done{false};
    bool expected = false;
    if (done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ::aura::core::set_mutation_epoch_bump_hook(&grant_epoch_window_bump_trampoline);
    }
}

inline CapabilityRegistry& g_capability_registry() noexcept {
    static CapabilityRegistry r;
    install_grant_epoch_window_hook();
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
    // Issue #2426: acquire load of atomic sandbox mode.
    const auto mode = reg.sandbox_mode.load(std::memory_order_acquire);
    const bool need_grant = (mode == EffectSandboxMode::Strict) ||
                            (mode == EffectSandboxMode::Restricted && sandbox_active);

    {
        std::lock_guard<std::mutex> lock(reg.mtx);
        const auto hard0 = met.capability_fiber_hard_deny_total.load(std::memory_order_relaxed);
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
            // Issue #2055: wildcard must still honor epoch fence / provenance
            // binding — otherwise privilege-sticky grants pass under "*".
            if (!reg.provenance_ok(tenant, prov)) {
                allowed = false;
                met.capability_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const auto hard1 = met.capability_fiber_hard_deny_total.load(std::memory_order_relaxed);
        // Issue #2151 / #2388: Agent-stable reason when hard fiber isolation denies.
        const char* reason_hint = (!allowed && hard1 > hard0) ? "fiber-grant-mismatch" : nullptr;
        reg.record_audit(required, actual, tenant, prov, !allowed, op, reason_hint);
    }

    if (allowed)
        met.capability_effect_enforced_total.fetch_add(1, std::memory_order_relaxed);
    else
        met.capability_effect_denied_total.fetch_add(1, std::memory_order_relaxed);
    return allowed;
}

// Issue #2055: build EffectProvenance stamped with WorkspaceEpoch Mutation
// + fiber. Always produces non-zero epoch (AC: grant carries non-zero
// epoch matching mutation epoch at grant time). When force_mutation_bind
// is true (sandbox != Off), mutation_id defaults to the mutation epoch if
// the caller left it zero (#2074 anti-sticky). Pass fiber_id from
// aura_fiber_current_id() at the Evaluator boundary (core header stays
// free of fiber TLS).
[[nodiscard]] inline EffectProvenance
make_grant_provenance(std::uint64_t provenance_mutation_id = 0, bool force_mutation_bind = true,
                      std::uint32_t node_id = 0, std::uint32_t fiber_id = 0) noexcept {
    EffectProvenance prov;
    prov.node_id = node_id;
    prov.fiber_id = fiber_id;
    const auto me = ::aura::core::current_mutation_epoch();
    // Non-zero WorkspaceEpoch Mutation stamp (0 is "unset / legacy").
    prov.epoch = me != 0 ? me : 1;
    if (force_mutation_bind) {
        prov.mutation_id = provenance_mutation_id != 0 ? provenance_mutation_id : me;
        if (prov.mutation_id == 0)
            prov.mutation_id = prov.epoch;
    } else {
        prov.mutation_id = provenance_mutation_id;
    }
    return prov;
}

inline void reset_capability_effects_for_test() noexcept {
    g_capability_registry().clear_for_test();
    g_capability_registry().set_grant_min_valid_epoch(0);
    // set_grant_epoch_retain_window(0) only stores; clear_for_test already zeroed K.
    g_capability_registry().set_grant_epoch_retain_window(0);
    g_capability_registry().set_hard_fiber_isolation(false);
    set_effect_fiber_id_override(0);
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
    m.capability_grant_epoch_bound_total.store(0, std::memory_order_relaxed);
    m.capability_revoke_epoch_bound_total.store(0, std::memory_order_relaxed);
    m.capability_grant_fiber_bound_total.store(0, std::memory_order_relaxed);
    m.capability_fiber_mismatch_total.store(0, std::memory_order_relaxed);
    m.capability_epoch_fence_hit_total.store(0, std::memory_order_relaxed);
    m.capability_mutation_bridge_split_total.store(0, std::memory_order_relaxed);
    m.capability_fiber_hard_deny_total.store(0, std::memory_order_relaxed);
    m.capability_grant_epoch_window_advance_total.store(0, std::memory_order_relaxed);
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
    // Issue #2055
    std::uint64_t grant_epoch_bound = 0;
    std::uint64_t revoke_epoch_bound = 0;
    std::uint64_t grant_fiber_bound = 0;
    std::uint64_t fiber_mismatch = 0;
    std::uint64_t epoch_fence_hits = 0;
    // Issue #2149
    std::uint64_t mutation_bridge_split = 0;
    // Issue #2151
    std::uint64_t fiber_hard_deny = 0;
    int hard_fiber_isolation = 0;
    // Issue #2154
    std::uint64_t grant_min_valid_epoch = 0;
    std::uint64_t grant_epoch_retain_window = 0;
    std::uint64_t grant_epoch_window_advance = 0;
};

// Issue #2430: multi-field consistent snapshot (#1840 / #2426 pattern).
// 22+ atomic loads without double-check can tear under concurrent
// grant/revoke/check_and_record_effect writers. Retry up to 16 times
// verifying the hottest counters (enforced/denied/grants/checks) did
// not move mid-snapshot; best-effort after retries.
[[nodiscard]] inline CapabilityEffectStatsSnapshot snapshot_capability_effect_stats() noexcept {
    auto& m = g_capability_effect_metrics();
    auto& reg = g_capability_registry();
    CapabilityEffectStatsSnapshot s;
    s.phase = kCapabilityModelPhase;
    s.issue = kCapabilityModelIssue;
    for (int attempt = 0; attempt < 16; ++attempt) {
        s.enforced = m.capability_effect_enforced_total.load(std::memory_order_acquire);
        s.denied = m.capability_effect_denied_total.load(std::memory_order_acquire);
        s.provenance_mismatch =
            m.capability_provenance_mismatch_total.load(std::memory_order_acquire);
        s.grants = m.capability_grant_total.load(std::memory_order_acquire);
        s.revokes = m.capability_revoke_total.load(std::memory_order_acquire);
        s.checks = m.capability_check_total.load(std::memory_order_acquire);
        s.audits = m.capability_audit_total.load(std::memory_order_acquire);
        s.sandbox_mode = static_cast<int>(reg.sandbox_mode.load(std::memory_order_acquire));
        s.macro_self_evo_checks = m.macro_self_evo_check_total.load(std::memory_order_acquire);
        s.macro_self_evo_allowed = m.macro_self_evo_allowed_total.load(std::memory_order_acquire);
        s.macro_self_evo_denied = m.macro_self_evo_denied_total.load(std::memory_order_acquire);
        s.macro_self_evo_depth_clamps =
            m.macro_self_evo_depth_clamp_total.load(std::memory_order_acquire);
        s.macro_self_evo_pass_clamps =
            m.macro_self_evo_pass_clamp_total.load(std::memory_order_acquire);
        s.grant_epoch_bound = m.capability_grant_epoch_bound_total.load(std::memory_order_acquire);
        s.revoke_epoch_bound =
            m.capability_revoke_epoch_bound_total.load(std::memory_order_acquire);
        s.grant_fiber_bound = m.capability_grant_fiber_bound_total.load(std::memory_order_acquire);
        s.fiber_mismatch = m.capability_fiber_mismatch_total.load(std::memory_order_acquire);
        s.epoch_fence_hits = m.capability_epoch_fence_hit_total.load(std::memory_order_acquire);
        s.mutation_bridge_split =
            m.capability_mutation_bridge_split_total.load(std::memory_order_acquire);
        s.fiber_hard_deny = m.capability_fiber_hard_deny_total.load(std::memory_order_acquire);
        s.hard_fiber_isolation = reg.hard_fiber_isolation() ? 1 : 0;
        s.grant_min_valid_epoch = reg.grant_min_valid_epoch();
        s.grant_epoch_retain_window = reg.grant_epoch_retain_window();
        s.grant_epoch_window_advance =
            m.capability_grant_epoch_window_advance_total.load(std::memory_order_acquire);

        // Double-check most-bumped counters for torn multi-field view.
        if (m.capability_effect_enforced_total.load(std::memory_order_acquire) == s.enforced &&
            m.capability_effect_denied_total.load(std::memory_order_acquire) == s.denied &&
            m.capability_grant_total.load(std::memory_order_acquire) == s.grants &&
            m.capability_check_total.load(std::memory_order_acquire) == s.checks &&
            m.capability_audit_total.load(std::memory_order_acquire) == s.audits &&
            m.capability_revoke_total.load(std::memory_order_acquire) == s.revokes) {
            return s;
        }
    }
    return s; // best-effort after retries
}

// Issue #2023 / #2386: consult MacroSelfEvo capability at macro expand entry.
//
// Policy:
//   Sandbox Off: always allow; effective.max_* = 0 means "no clamp"
//                (caller max_passes + internal MAX_HYGIENE_DEPTH).
//   Strict / Restricted+active without MacroSelfEvo grant: deny.
//   Granted with max_depth==0 or max_expansion_passes==0: deny (zero limits).
//   Granted with positive limits: allow + return policy for clamping.
//   Issue #2386: provenance_ok (epoch fence / hard fiber) after bit check —
//   same fence as Mutate grants (#2055/#2151/#2154).
//
// wildcard_ok: kCapWildcard holders inherit default permissive MacroSelfEvo
// with default policy (32 passes / 256 depth) when no explicit grant.
// call_fiber_id: live fiber for hard isolation / audit (0 → effect override only).
[[nodiscard]] inline MacroSelfEvoCheck
check_macro_self_evo(TenantId tenant, bool sandbox_active = false, bool wildcard_ok = false,
                     std::uint32_t call_fiber_id = 0) noexcept {
    auto& reg = g_capability_registry();
    auto& met = g_capability_effect_metrics();
    met.macro_self_evo_check_total.fetch_add(1, std::memory_order_relaxed);

    MacroSelfEvoCheck out;
    // Issue #2426: acquire load of atomic sandbox mode.
    const auto mode = reg.sandbox_mode.load(std::memory_order_acquire);
    const bool need_grant = (mode == EffectSandboxMode::Strict) ||
                            (mode == EffectSandboxMode::Restricted && sandbox_active);

    // Issue #2386: non-empty EffectProvenance for audit + provenance_ok.
    EffectProvenance call_prov{};
    {
        const auto me = ::aura::core::current_mutation_epoch();
        call_prov.epoch = me != 0 ? me : 1;
        call_prov.mutation_id = call_prov.epoch;
        call_prov.fiber_id = effect_fiber_id_or(call_fiber_id);
    }

    if (!need_grant) {
        // Off / Restricted-inactive: preserve historical unconstrained behaviour.
        out.allowed = true;
        out.effective.max_expansion_passes = 0; // 0 = no pass clamp
        out.effective.max_depth = 0;            // 0 = use MAX_HYGIENE_DEPTH
        out.effective.allow_rest_hygiene = true;
        out.effective.allow_concurrent_fiber = true;
        // Issue #2243: Off / Restricted-inactive keeps historical unconstrained
        // behavior — force_hygienic OFF, no gensym-map-size ceiling, no
        // per-fiber violation budget. Multi-tenant enforcement comes from
        // the granted-policy path (MacroSelfEvoPolicy{} defaults are ON).
        out.effective.force_hygienic = false;
        out.effective.max_gensym_map_size = 0;
        out.effective.max_violations_per_fiber = 0;
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
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, call_prov, true, "macro-self-evo");
        return out;
    }

    // Issue #2386: epoch fence / hard fiber isolation / bound mid (parity grant()).
    if (!reg.provenance_ok(tenant, call_prov)) {
        out.allowed = false;
        out.deny_reason = "MacroSelfEvo provenance fence (epoch/fiber/mid)";
        met.macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
        met.capability_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, call_prov, true, "macro-self-evo");
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
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, call_prov, true, "macro-self-evo");
        return out;
    }

    // Zero limits = explicit deny (AC: not granted or limits are zero).
    if (pol.max_depth == 0 || pol.max_expansion_passes == 0) {
        out.allowed = false;
        out.deny_reason = "MacroSelfEvo limits are zero";
        met.macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, call_prov, true, "macro-self-evo");
        return out;
    }

    out.allowed = true;
    out.effective = pol;
    met.macro_self_evo_allowed_total.fetch_add(1, std::memory_order_relaxed);
    reg.record_audit(Effect::MacroSelfEvo, held | Effect::MacroSelfEvo, tenant, call_prov, false,
                     "macro-self-evo");
    return out;
}

// Map security cap name → Effect bit.
//
// Issue #2387: tenant-admin + syscall join the matrix (epoch / fiber bind).
// Issue #2489: remaining high-risk sensitive caps promoted —
//   - self-evo / synthesize / strategy → MacroSelfEvo (AI self-modify)
//   - sys-open / sys-write → Syscall | Write (raw syscall + write)
//   - sys-read → Syscall | Read
//   - agent → TenantAdmin (cross-tenant agent spawn adjacency)
//   - capability → TenantAdmin (meta-privilege: cap:grant / cap:revoke)
//
// SECURITY_EXEMPT (staged, effect_for_cap_name == None) — non-security
// display / low-risk paths intentionally left on the string list:
//   compile, compile-stats, compile-dirty, compile-deopt, fiber, workspace,
//   exception-control, macro, query, sandbox.
//
// has_capability(needed) consults this map first; effect-mapped names hit
// g_capability_registry().effects_for(tenant) + provenance_ok (epoch fence /
// hard fiber / sliding retain window) so grant_capability / grant_effect_-
// capability / revoke_effect_capability are single-authority.
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
    if (name == "tenant-admin")
        return Effect::TenantAdmin;
    if (name == "syscall")
        return Effect::Syscall;
    // Issue #2489: high-risk residual promoted (see block comment above).
    if (name == "self-evo" || name == "synthesize" || name == "strategy")
        return Effect::MacroSelfEvo;
    if (name == "sys-open" || name == "sys-write")
        return Effect::Syscall | Effect::Write;
    if (name == "sys-read")
        return Effect::Syscall | Effect::Read;
    if (name == "agent")
        return Effect::TenantAdmin;
    if (name == "capability")
        return Effect::TenantAdmin;
    if (name == "*")
        return Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate | Effect::Network |
               Effect::Ffi | Effect::Render | Effect::MacroSelfEvo | Effect::TenantAdmin |
               Effect::Syscall;
    return Effect::None;
}

} // namespace aura::core::capability

#endif // AURA_CORE_CAPABILITY_MODEL_HH
