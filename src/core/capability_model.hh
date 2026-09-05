// capability_model.hh — Issues #1180/#1187/#1192/#1565: Capability Effects SSOT.
// Module consumers: `import aura.core.capability_model;` re-exports this
// header. Non-module TUs: #include. Do not reintroduce a stub Effect enum
// in capability_model.ixx.

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
// Issue #3542: mutate MacroIntroduced opt-out requires MacroSelfEvo.
inline constexpr int kMacroMutateCapabilityFenceIssue = 3542;
// Issue #2055: grant/revoke bound to WorkspaceEpoch Mutation + fiber.
inline constexpr int kGrantEpochFiberBindIssue = 2055;
// Issue #2154: sliding grant_min_valid_epoch window on Mutation epoch bump.
inline constexpr int kGrantEpochRetainWindowIssue = 2154;
// Production multi-tenant / Strict default retain window (last K epochs).
inline constexpr std::uint64_t kDefaultGrantEpochRetainWindowMultiTenant = 64;
// Issue #2529: Restricted single-tenant production default (smaller than
// multi-tenant K=64) so privilege-sticky grants still slide off under long
// Mutation epoch advance. AURA_GRANT_EPOCH_RETAIN always overrides.
inline constexpr std::uint64_t kDefaultGrantEpochRetainWindowRestricted = 16;
// Issue #2529 stamp.
inline constexpr int kGrantEpochRetainRestrictedIssue = 2529;
// Issue #3207: dual-Evaluator cascade + consume linearizability on the
// process-global CapabilityRegistry (no per-Evaluator shard).
inline constexpr int kCapabilityDualEvaluatorCascadeIssue = 3207;
// Issue #3209: steal × nested abort × resume session-grant quiesce
// (mark_stolen → revoke_for_mid; mid clear is commutative no-op after).
inline constexpr int kCapabilitySessionQuiesceIssue = 3209;
// Issue #3241: concurrent outermost sharing process Mutation epoch as
// session_mid — revoke key is (mid, fiber); fiber=0 is legacy mid-only.
inline constexpr int kCapabilitySessionPeerFiberIssue = 3241;

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

// Issue #3304: parallel capability-deny reason family (analogous to
// kHygieneLimitReason* but for capability denials — MacroSelfEvo sits
// outside the hygiene taxonomy because it is a capability gate, not a
// hygiene ceiling/depth/pass. Agent tooling keys on these codes via
// capability_deny_last_reason_string() to distinguish "capability not
// granted" from "provenance fence" from "policy missing" from "limits
// are zero". Each code also stamps kHygieneLimitReasonCapabilityDeny (7)
// into the unified g_macro_hygiene_last_limit_reason atomic so the
// agent's existing last_limit_reason query routes correctly (the 7
// sentinel switches the agent into capability-replay mode).
//
// Order is stable — agents rely on the numeric code (do NOT renumber).
inline constexpr std::uint8_t kCapabilityDenyReasonNone = 0;
inline constexpr std::uint8_t kCapabilityDenyReasonNotGranted = 1;      // !has_bit && !wildcard_ok
inline constexpr std::uint8_t kCapabilityDenyReasonProvenanceFence = 2; // epoch/fiber/mid mismatch
inline constexpr std::uint8_t kCapabilityDenyReasonPolicyMissing = 3; // !has_policy && !wildcard_ok
inline constexpr std::uint8_t kCapabilityDenyReasonLimitsZero =
    4; // pol.max_depth==0 || pol.max_expansion_passes==0

// Issue #3304: per-fibre global atomic for the last capability-deny
// reason (parallel to g_macro_hygiene_last_limit_reason). Set on every
// deny site in check_macro_self_evo (and any future capability gate).
inline std::atomic<std::uint8_t> g_capability_deny_last_reason{0};

// Issue #3304 CI build fix: cannot extern-declare
// aura::compiler::macro_exp::g_macro_hygiene_last_limit_reason here —
// this header is #included inside the aura.compiler.macro_expansion module
// purview, where a non-module extern declaration conflicts with the module's
// export. Instead, route through an extern "C" bridge hook (defined in the
// macro_expansion.cpp global module fragment, alongside the existing
// aura_macro_* weak bridges) that performs the sentinel store.
extern "C" void aura_macro_hygiene_capability_deny_sentinel(void) noexcept;

// Issue #3304: public API mirroring note_hygiene_last_limit_reason.
// Stamps both g_capability_deny_last_reason (capability-side atomic)
// and g_macro_hygiene_last_limit_reason (with the kHygieneLimitReason
// CapabilityDeny=7 sentinel). The unified last_limit_reason API surface
// routes the agent to capability_deny_last_reason_string() when it
// sees the 7 sentinel.
inline void note_capability_deny_last_reason(std::uint8_t code) noexcept {
    g_capability_deny_last_reason.store(code, std::memory_order_relaxed);
    // Also bump the unified hygiene atomic so the agent's existing
    // last_limit_reason_string() reads "capability-deny" (case 7 in
    // macro_expansion.cpp). One unified surface; two parallel families.
    aura_macro_hygiene_capability_deny_sentinel();
}

// Issue #3304: stable string for the last capability-deny reason code.
// Order matches the kCapabilityDenyReason* constants above.
[[nodiscard]] inline const char* capability_deny_last_reason_string() noexcept {
    switch (g_capability_deny_last_reason.load(std::memory_order_relaxed)) {
        case 1:
            return "capability-not-granted";
        case 2:
            return "capability-provenance-fence";
        case 3:
            return "capability-policy-missing";
        case 4:
            return "capability-limits-zero";
        default:
            return "";
    }
}

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
    // Issue #2586: single-use / mutation-bound grant — auto-revoke after
    // first successful check_and_record_effect that this grant contributed
    // required bits to. Layout-additive at end of struct so existing
    // serialized / stable-layout consumers keep binary compat.
    // Deny path does NOT consume (retryable).
    bool single_use = false;
    // Issue #2944: mutation-session grant — valid only while call mid
    // equals bound_mutation_id under Restricted/Strict; auto-revoked on
    // outermost MutationBoundary exit for that mid (success or fail).
    // Complements single_use (first-success consume) for multi-step
    // self-evo under one mid without durable sticky grants. Soft/Off
    // ignores session_bound for enforcement (zero cost, AC3).
    bool session_bound = false;
    // Issue #3142: stolen flag — set by fiber steal path so caller-side
    // check_and_record_effect fails on a stolen SessionBound entry (no
    // double-consume). Layout-additive at END of struct per #2906.
    bool stolen = false;
};

// Sandbox mode mirror for effect checks (written only via sandbox::set_mode).
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
    // Issue #2707: zero mid on either grant.bound_mutation_id or
    // prov.mutation_id under Restricted/Strict (fail-closed join deny).
    // Subset of provenance_mismatch; Agents can chart zero-join holes
    // separately from strict-equality mismatches (mid N vs N+1).
    std::atomic<std::uint64_t> capability_mid_join_zero_deny_total{0};
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
    // Issue #2883: side-effect entry-point hard deny when the current
    // fiber resume had a hard principal mismatch under production.
    // Distinct from capability_fiber_hard_deny_total (which fires on
    // grant_fiber_id mismatch in provenance_ok) and from
    // tenant_scope_mismatch_hard_total (which fires on detection).
    // This counter fires only on actual side-effect denial at
    // require_effect* / check_and_record_effect / grant_effect_*.
    std::atomic<std::uint64_t> capability_fiber_principal_mismatch_hard_deny_total{0};
    // Issue #2154: sliding grant_min_valid window advanced on epoch bump.
    std::atomic<std::uint64_t> capability_grant_epoch_window_advance_total{0};
    // Issue #2688 / #2943: production-default hard_fiber_isolation + grant
    // epoch retain window. Wired in apply_production_security_defaults
    // (multi-tenant OR Strict → hard=true + K=64; pure Restricted → K=16
    // hard=false; Soft → K=0 hard=false). #2943 closes residual soft share
    // under pure Strict (was multi-tenant-only after #2835). Env overrides
    // AURA_HARD_FIBER_ISOLATION / AURA_GRANT_EPOCH_RETAIN in
    // security_defaults.hh. kCapabilityProductionDefaultIssue stays 2688
    // for lineage; schema-2943 is additive on posture / effect-stats.
    inline static constexpr int kCapabilityProductionDefaultIssue = 2688;
    // Issue #2943: production multi-tenant/Strict default hard fiber.
    inline static constexpr int kProductionHardFiberDefaultIssue = 2943;
    // Issue #2586: single-use grant consumption counter (parity
    // capability_revoke_total for histogram breakdown of revoke reasons —
    // auto-revoke vs operator-revoke).
    std::atomic<std::uint64_t> capability_single_use_consumed_total{0};
    // Issue #2882: production default single-use overrides for high-risk
    // (Mutate / MacroSelfEvo / TenantAdmin / Syscall) under Restricted/Strict.
    // Counts (a) forced single_use on the default `grant_effect_capability`
    // surface and (b) explicit `grant_effect_durable` admin overrides —
    // Agent dashboards chart privilege-sticky risk separately from the
    // common-case auto-revoke path (#2586).
    std::atomic<std::uint64_t> capability_high_risk_forced_single_use_total{0};
    std::atomic<std::uint64_t> capability_durable_high_risk_grant_total{0};
    // Issue #2944: mutation-session grant lifecycle.
    // session_grant_total: grants stamped session_bound=true.
    // session_revoke_total: auto-revokes on outermost boundary exit.
    // live_session_grants: residual live session-bound grants (zero-cost
    // early-out for Soft / no-session happy path, AC3).
    std::atomic<std::uint64_t> capability_session_grant_total{0};
    std::atomic<std::uint64_t> capability_session_revoke_total{0};
    std::atomic<std::uint64_t> capability_live_session_grants{0};
    // Issue #2967: durable high-risk grant denied at the call site — caller
    // lacks TenantAdmin (or equivalent meta-privilege) or the mandatory
    // agent-stable audit reason is empty. Appended at struct END (never
    // insert mid-struct: stale module BMIs writing at wrong offsets corrupt
    // neighboring heap — see #2906).
    std::atomic<std::uint64_t> capability_durable_grant_deny_total{0};
    // Issue #2969: registry write-fence deny — grant/revoke targeting a
    // foreign tenant id (tenant_id != 0 && tenant_id != capability_tenant_id_)
    // under production (Restricted/Strict) without TenantAdmin. Appended at
    // struct END (never insert mid-struct: stale module BMIs writing at wrong
    // offsets corrupt neighboring heap — see #2906).
    std::atomic<std::uint64_t> capability_grant_foreign_tenant_deny_total{0};
    // Issue #3029: grant_macro_self_evo denied under Restricted/Strict
    // without TenantAdmin. Appended at struct END (never insert mid-struct).
    std::atomic<std::uint64_t> capability_macro_self_evo_grant_deny_total{0};
    // Issue #3048: session-grant revoke via steal-complete vs force-cancel /
    // mark_outermost_failed (distinct from #2944 session-mid-exit). Appended
    // at struct END (never insert mid-struct: stale module BMIs writing at
    // wrong offsets corrupt neighboring heap — see #2906).
    std::atomic<std::uint64_t> capability_session_revoke_steal_total{0};
    // Issue #3141: kCapWildcard write-fence deny — production caller holds
    // kCapWildcard but NOT explicit TenantAdmin attempts privilege-bearing
    // string-path grant_capability("self-evo"|"tenant-admin"|...). Closes
    // privilege escalation via wildcard → full Effect mask → check passes →
    // grant succeeds. Appended at struct END (never insert mid-struct).
    std::atomic<std::uint64_t> capability_wildcard_write_fence_deny_total{0};
    // Issue #3144: kCapWildcard持卡但不显式 TenantAdmin → effects_for() 查询 path
    // strip TenantAdmin + MacroSelfEvo bits (caller cannot pass
    // require_effect(TenantAdmin) check; independent of #3141 grant_capability
    // string path). Soft/Off zero-cost (no strip; wildcard contract preserved).
    // Appended at struct END per #2906.
    std::atomic<std::uint64_t> wildcard_strip_tenant_admin_effect_total{0};
    // Issue #3142: SessionBound revoke cascade counters (additive; appended at
    // struct END per #2906). dtor_total: cascade revoke on nested TenantScope
    // dtor / abort. steal_total: stolen flag set on fiber steal path.
    // orphan_total: orphan grant caught at audit (live_session_grants > 0 but
    // no caller is tracked; defense-in-depth metric for long-run chaos).
    std::atomic<std::uint64_t> session_bound_revoked_on_scope_dtor_total{0};
    std::atomic<std::uint64_t> session_bound_revoked_on_steal_total{0};
    std::atomic<std::uint64_t> session_bound_orphan_detected_total{0};
    std::atomic<std::uint64_t> capability_session_revoke_abort_total{0};
    // Issue #3090: production (Restricted/Strict) grant refused when
    // prov.mutation_id == 0 — same refuse semantics as
    // resolve_audit_mutation_id (#2836). Grants no longer synthesize
    // bound_mid = epoch ?: 1 (which produced phantom mid=1 entries that
    // could not be joined with Typed trail / SE mid=0 refuse events).
    // Distinct from capability_mid_join_zero_deny_total (which fires at
    // provenance_ok check side, #2707): this counter fires at grant write
    // side. Appended at struct END (never insert mid-struct: #2906).
    std::atomic<std::uint64_t> capability_grant_mid_refused_total{0};
    // Issue #3177: production (Restricted/Strict) durable high-risk grants
    // (Mutate / MacroSelfEvo / TenantAdmin / Syscall) now stamp
    // session_bound=true by default so outermost MutationBoundaryGuard exit
    // / TenantScope dtor / steal-abort revokes them (#2944/#3048/#3142
    // path). The original single_use=false (privilege-sticky) is closed.
    // Appended at struct END per #2906 — never insert mid-struct (stale
    // module BMIs writing at wrong offsets corrupt neighboring heap).
    std::atomic<std::uint64_t> capability_durable_session_bound_total{0};
    // Issue #3542: mutate of MacroIntroduced with :allow-macro? / global
    // opt-out but no MacroSelfEvo (wildcard strip #3144). Append END.
    std::atomic<std::uint64_t> macro_mutate_capability_deny_total{0};
};

// Issue #2149: security provenance vocabulary — Mutation only.
inline constexpr int kEffectEpochUnifyIssue = 2149;
// Issue #3335: mutation audit ring.epoch uses Mutation (not Bridge).
inline constexpr int kMutationAuditEpochUnifyIssue = 3335;
// Issue #2151: optional hard-deny on grant_fiber_id mismatch.
inline constexpr int kHardFiberIsolationIssue = 2151;
// Issue #3333: provenance_ok mid/epoch/fiber join is per contributing grant
// (has_effect ∩ required), not every live grant on the tenant.
inline constexpr int kProvenanceContributingMidIssue = 3333;

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
    mutable std::mutex mtx;
    // tenant_id → grants (multiple named grants OR'd for checks)
    std::unordered_map<TenantId, std::vector<CapabilityGrant>> by_tenant;
    // Issue #2023: per-tenant MacroSelfEvo policy limits (paired with
    // Effect::MacroSelfEvo grant bit). Absent entry → no grant.
    std::unordered_map<TenantId, MacroSelfEvoPolicy> macro_self_evo_by_tenant;
    // Issue #2426 / #2427: atomic (was plain enum / TenantId — policy flip race).
    // Issue #2657: Process-wide authority is `aura::core::sandbox::set_mode`
    // (sandbox.hh SSOT). The field remains public for reads
    // (acquire-load via `reg.sandbox_mode.load()` or `==` comparison
    // are unchanged). Direct writes from outside the authority are
    // gated by the coverage linter
    // `scripts/check_sandbox_mode_authority.py` — the existing
    // #2427 AC3 assignment+compare signature is preserved for
    // backwards compatibility with the 14 existing test files.
    AtomicEffectSandboxMode sandbox_mode{};
    AtomicTenantId default_tenant{};
    // Issue #2530: 1024 slots — aligned with SecurityEvent ring (#2225)
    // so deny-storm Agent in-memory query retains a full SE-scale window
    // (earlier events remain on WAL). Was 128.
    static constexpr std::size_t kAuditRing = 1024;
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
    // Issue #2586: single_use=true marks this grant for auto-revoke after
    // first successful check_and_record_effect that uses its bits; re-grant
    // resets the flag (fresh grant semantics).
    // Issue #2944: session_bound=true marks a mutation-session grant —
    // mid-bound under Restricted/Strict; auto-revoked on outermost
    // MutationBoundary exit for bound_mutation_id (session-mid-exit).
    // Issue #3090: production (Restricted/Strict) refuses grants when
    // prov.mutation_id == 0 — same refuse semantics as
    // resolve_audit_mutation_id (#2836). Refuse is checked pre-lock so the
    // grant is rejected entirely (no effects OR, no bound_mid update).
    // Bumps capability_grant_mid_refused_total + emits SE
    // reason="grant-mid-refused" with mid=0 so Agent joins via reason +
    // mid=0 across grant ↔ trail ↔ effect-check surfaces. Soft/Off keeps
    // the legacy synthesis (zero-cost contract, AC5) for session_bound.
    bool grant(TenantId tenant, std::string_view name, Effect effects,
               const EffectProvenance& prov = {}, bool single_use = false,
               bool session_bound = false, TenantId caller_principal = 0) {
        // Pre-lock refuse (Issue #3090). Reads only sandbox_mode atomic +
        // prov.mutation_id (caller-owned); no registry state needed.
        {
            const auto mode_refuse = sandbox_mode.load(std::memory_order_acquire);
            const bool fail_closed = (mode_refuse == EffectSandboxMode::Restricted ||
                                      mode_refuse == EffectSandboxMode::Strict);
            if (fail_closed && prov.mutation_id == 0) {
                auto& met_refuse = g_capability_effect_metrics();
                met_refuse.capability_grant_mid_refused_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                using ::aura::core::security_event::SecurityEventKind;
                using ::aura::core::security_event_wal::emit_security_event_durable;
                emit_security_event_durable(
                    SecurityEventKind::EffectDeny, tenant,
                    /*mid=*/0, /*epoch=*/prov.epoch, static_cast<std::uint16_t>(effects), name,
                    "grant-mid-refused", /*denied=*/true, static_cast<std::int64_t>(prov.fiber_id));
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(mtx);
        return grant_locked(tenant, name, effects, prov, single_use, session_bound,
                            caller_principal);
    }

    // Issue #3126: caller MUST hold `mtx`. Body is the post-refuse
    // mutation logic of `grant()` (insert into by_tenant[name] + OR
    // effects, re-grant semantics). Used by security fence paths in
    // evaluator_security.cpp (grant_effect_* foreign-tenant +
    // TenantAdmin) so the admin re-check + act are atomic w.r.t.
    // concurrent revoke from another Evaluator / fiber. Pre-lock refuse
    // (Issue #3090) is the caller's responsibility (atomic only, OK to
    // run without the lock).
    bool grant_locked(TenantId tenant, std::string_view name, Effect effects,
                      const EffectProvenance& prov = {}, bool single_use = false,
                      bool session_bound = false, TenantId caller_principal = 0) {
        // Issue #3409: push the #3086/#3029 TenantAdmin fence shape into
        // grant_locked SSOT. Production Restricted/Strict requires TA on
        // the caller when (tenant is foreign) OR (effects contain high
        // bits {TA, MSE, Mutate, Syscall}). Soft/Off: zero-cost (no
        // fence). Evaluator path passes capability_tenant_id_ as
        // caller_principal; legacy direct callers default to caller=0
        // → default_tenant. Caller holds mtx so effects_for_locked is
        // TOCTOU-safe (same fence shape as grant_cross_tenant /
        // grant_macro_self_evo). Reuse existing deny counter
        // (capability_macro_self_evo_grant_deny_total — no new metrics
        // fields per issue AC4). New stable SE reason
        // `grant-ssot-needs-tenant-admin` (does not change old reason
        // strings).
        {
            const auto mode = sandbox_mode.load(std::memory_order_acquire);
            if (mode != EffectSandboxMode::Off) {
                // Session-bound Mutate is the Restricted mutation-session
                // path (#2944). A same-tenant session grant is not a
                // high-bits admin act — requiring TA here made every
                // Restricted session Mutate need TenantAdmin (steal-resume
                // tests and Agent mutate loops could not grant). Foreign
                // tenant still needs TA. caller=0 + session_bound uses
                // the grant tenant as self-principal.
                const auto caller = caller_principal != 0
                                        ? caller_principal
                                        : (session_bound && tenant != 0
                                               ? tenant
                                               : default_tenant.load(std::memory_order_acquire));
                constexpr std::uint16_t kHighBits =
                    static_cast<std::uint16_t>(Effect::TenantAdmin) |
                    static_cast<std::uint16_t>(Effect::MacroSelfEvo) |
                    static_cast<std::uint16_t>(Effect::Mutate) |
                    static_cast<std::uint16_t>(Effect::Syscall);
                const bool foreign_tenant = (tenant != 0 && tenant != caller);
                const bool high_bits = (static_cast<std::uint16_t>(effects) & kHighBits) != 0;
                if (foreign_tenant || (high_bits && !session_bound)) {
                    if (!has_effect(effects_for_locked(caller), Effect::TenantAdmin)) {
                        auto& met = g_capability_effect_metrics();
                        met.capability_macro_self_evo_grant_deny_total.fetch_add(
                            1, std::memory_order_relaxed);
                        using ::aura::core::security_event::SecurityEventKind;
                        using ::aura::core::security_event_wal::emit_security_event_durable;
                        emit_security_event_durable(
                            SecurityEventKind::EffectDeny, tenant, prov.mutation_id, prov.epoch,
                            static_cast<std::uint16_t>(effects), name,
                            "grant-ssot-needs-tenant-admin", /*denied=*/true,
                            static_cast<std::int64_t>(prov.fiber_id));
                        return false;
                    }
                }
            }
        }
        auto& vec = by_tenant[tenant];
        auto apply = [&](CapabilityGrant& g) {
            g.effects = g.effects | effects;
            // Issue #2586: re-grant resets single_use flag (fresh grant).
            const bool was_session = g.session_bound && !g.revoked;
            g.single_use = single_use;
            g.session_bound = session_bound; // #2944
            g.revoked = false;
            g.bound_mutation_id = prov.mutation_id;
            g.bound_node_id = prov.node_id;
            g.grant_epoch = prov.epoch;
            g.grant_fiber_id = prov.fiber_id;
            // Issue #2531 / #2944: Soft/Off (sandbox_mode==Off) keeps legacy
            // optional mid; session_bound always needs non-zero mid stamp.
            // After the pre-lock refuse (#3090), production with bound_mid==0
            // has already been refused above, so this synthesis is unreachable
            // under Restricted/Strict — it only fires for Soft/Off + session_bound.
            if ((sandbox_mode.load(std::memory_order_acquire) != EffectSandboxMode::Off ||
                 session_bound) &&
                g.bound_mutation_id == 0) {
                g.bound_mutation_id = g.grant_epoch != 0 ? g.grant_epoch : 1;
            }
            g.revoke_epoch = 0;
            auto& met = g_capability_effect_metrics();
            met.capability_grant_total.fetch_add(1, std::memory_order_relaxed);
            if (prov.epoch != 0)
                met.capability_grant_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
            if (prov.fiber_id != 0)
                met.capability_grant_fiber_bound_total.fetch_add(1, std::memory_order_relaxed);
            // Live session residual: new session grant that was not already live.
            if (session_bound && !was_session) {
                met.capability_session_grant_total.fetch_add(1, std::memory_order_relaxed);
                met.capability_live_session_grants.fetch_add(1, std::memory_order_relaxed);
            } else if (!session_bound && was_session) {
                // Re-grant cleared session_bound — drop live residual.
                auto cur = met.capability_live_session_grants.load(std::memory_order_relaxed);
                if (cur > 0)
                    met.capability_live_session_grants.fetch_sub(1, std::memory_order_relaxed);
            }
        };
        for (auto& g : vec) {
            if (g.name == name) {
                apply(g);
                return true;
            }
        }
        CapabilityGrant g;
        g.name = std::string(name);
        g.effects = effects;
        g.tenant_id = tenant;
        // single_use / session_bound left default false so apply() sees
        // was_session=false and correctly bumps live residual (#2944).
        apply(g);
        vec.push_back(std::move(g));
        return true;
    }

    // Issue #2586: single-use grant sugar — auto-revoke after first successful
    // check_and_record_effect that uses its bits. Equivalent to
    // grant(name, effects, prov, /*single_use=*/true).
    void grant_once(TenantId tenant, std::string_view name, Effect effects,
                    const EffectProvenance& prov = {}, TenantId caller_principal = 0) {
        grant(tenant, name, effects, prov, /*single_use=*/true, /*session_bound=*/false,
              caller_principal);
    }

    // Issue #2944: mutation-session grant sugar — mid-bound + session_bound.
    // Equivalent to grant(..., single_use, /*session_bound=*/true).
    // Prefer Evaluator::grant_effect_session for production high-risk force.
    void grant_session(TenantId tenant, std::string_view name, Effect effects,
                       const EffectProvenance& prov = {}, bool single_use = false,
                       TenantId caller_principal = 0) {
        grant(tenant, name, effects, prov, single_use, /*session_bound=*/true, caller_principal);
    }

    // Issue #3207: caller MUST hold `mtx`. Walk/revoke body of
    // `revoke_session_grants_for_mid` without the live==0 short-circuit
    // or lock_guard. Steal/abort paths that already hold mtx
    // (mark_outermost_mutation_failed) use this so stolen-mark + revoke
    // stay one critical section w.r.t. concurrent check_and_record_effect.
    std::size_t revoke_session_grants_for_mid_locked(std::uint64_t mid,
                                                     const char* reason = "session-mid-exit",
                                                     std::uint32_t fiber_id = 0) {
        if (mid == 0)
            return 0;
        if (!reason || reason[0] == '\0')
            reason = "session-mid-exit";
        auto& met = g_capability_effect_metrics();
        std::size_t n = 0;
        auto ep = ::aura::core::current_mutation_epoch();
        if (ep == 0)
            ep = 1;
        EffectProvenance audit_prov{};
        audit_prov.mutation_id = mid;
        audit_prov.epoch = ep;
        for (auto& [tenant, vec] : by_tenant) {
            for (auto& g : vec) {
                if (g.revoked || !g.session_bound)
                    continue;
                if (g.bound_mutation_id != mid)
                    continue;
                // Issue #3241: concurrent outermost sharing epoch mid.
                // Skip peer fiber when both ids are known and differ.
                // fiber_id=0 → legacy mid-only (unknown / single-fiber).
                // grant_fiber_id=0 is legacy and still matches.
                if (fiber_id != 0 && g.grant_fiber_id != 0 && g.grant_fiber_id != fiber_id)
                    continue;
                g.revoked = true;
                g.effects = Effect::None;
                g.revoke_epoch = ep;
                g.session_bound = false; // no longer live
                ++n;
                met.capability_session_revoke_total.fetch_add(1, std::memory_order_relaxed);
                met.capability_revoke_total.fetch_add(1, std::memory_order_relaxed);
                met.capability_revoke_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
                auto cur = met.capability_live_session_grants.load(std::memory_order_relaxed);
                if (cur > 0)
                    met.capability_live_session_grants.fetch_sub(1, std::memory_order_relaxed);
                record_audit(Effect::None, Effect::None, tenant, audit_prov,
                             /*denied=*/false, reason, reason);
            }
        }
        return n;
    }

    // Issue #2944: revoke live session_bound grants whose
    // bound_mutation_id matches mid. Called from outermost
    // MutationBoundaryGuard dtor (success or fail). Zero cost when
    // capability_live_session_grants == 0 (AC3 Soft happy path).
    // Returns number of grants revoked. Dual-writes audit reason
    // (default "session-mid-exit"). Issue #3048: steal/abort hooks
    // pass "session-mid-steal-exit" / "session-mid-abort-exit";
    // second call after first is a no-op (already revoked / not live).
    // Issue #3207: Soft/Off keeps the live==0 short-circuit (no lock).
    // Restricted/Strict always take mtx so a concurrent grant_session
    // cannot sneak a residual past this cascade (TOCTOU on the atomic).
    // Issue #3241: optional fiber_id narrows the sweep to (mid, fiber).
    // fiber_id=0 is the legacy mid-only key (unknown / single-fiber).
    std::size_t revoke_session_grants_for_mid(std::uint64_t mid,
                                              const char* reason = "session-mid-exit",
                                              std::uint32_t fiber_id = 0) {
        if (mid == 0)
            return 0;
        auto& met = g_capability_effect_metrics();
        const auto mode = sandbox_mode.load(std::memory_order_acquire);
        const bool production =
            (mode == EffectSandboxMode::Restricted || mode == EffectSandboxMode::Strict);
        if (!production && met.capability_live_session_grants.load(std::memory_order_relaxed) == 0)
            return 0; // AC3 / #3048 AC4: zero extra work when no session grants
        std::lock_guard<std::mutex> lock(mtx);
        return revoke_session_grants_for_mid_locked(mid, reason, fiber_id);
    }

    // Issue #3207: caller MUST hold `mtx`. Walk/revoke body of
    // `revoke_session_grants_for` without the live==0 short-circuit or
    // lock_guard. TenantScope::release holds mtx across this + principal
    // restore so a concurrent Evaluator cannot consume after restore.
    std::size_t revoke_session_grants_for_locked(TenantId tenant, std::uint64_t mid,
                                                 std::uint32_t fiber_id,
                                                 const char* reason = "scope-dtor-cascade") {
        if (mid == 0 || tenant == 0)
            return 0;
        if (!reason || reason[0] == '\0')
            reason = "scope-dtor-cascade";
        auto& met = g_capability_effect_metrics();
        std::size_t n = 0;
        auto ep = ::aura::core::current_mutation_epoch();
        if (ep == 0)
            ep = 1;
        EffectProvenance audit_prov{};
        audit_prov.mutation_id = mid;
        audit_prov.epoch = ep;
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return 0;
        for (auto& g : it->second) {
            if (g.revoked || !g.session_bound)
                continue;
            if (g.bound_mutation_id != mid)
                continue;
            if (fiber_id != 0 && g.grant_fiber_id != 0 && g.grant_fiber_id != fiber_id)
                continue;
            g.revoked = true;
            g.effects = Effect::None;
            g.revoke_epoch = ep;
            g.session_bound = false; // no longer live
            ++n;
            met.session_bound_revoked_on_scope_dtor_total.fetch_add(1, std::memory_order_relaxed);
            met.capability_session_revoke_total.fetch_add(1, std::memory_order_relaxed);
            met.capability_revoke_total.fetch_add(1, std::memory_order_relaxed);
            met.capability_revoke_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
            auto cur = met.capability_live_session_grants.load(std::memory_order_relaxed);
            if (cur > 0)
                met.capability_live_session_grants.fetch_sub(1, std::memory_order_relaxed);
            record_audit(Effect::None, Effect::None, tenant, audit_prov,
                         /*denied=*/false, reason, reason);
        }
        return n;
    }

    // Issue #3142: revoke all live session_bound grants matching
    // (tenant, mid, fiber_id) tuple. Used by TenantScope::release() to
    // cascade-revoke inner SessionBound grants on nested scope abort /
    // early-return. Public wrapper takes mtx (do not call while holding
    // mtx — use revoke_session_grants_for_locked). Zero-cost short-circuit
    // when capability_live_session_grants == 0 under Soft/Off (AC3).
    // Issue #3207: Restricted/Strict always take mtx (closes live==0
    // TOCTOU vs concurrent grant_session from another Evaluator).
    std::size_t revoke_session_grants_for(TenantId tenant, std::uint64_t mid,
                                          std::uint32_t fiber_id,
                                          const char* reason = "scope-dtor-cascade") {
        if (mid == 0 || tenant == 0)
            return 0;
        auto& met = g_capability_effect_metrics();
        const auto mode = sandbox_mode.load(std::memory_order_acquire);
        const bool production =
            (mode == EffectSandboxMode::Restricted || mode == EffectSandboxMode::Strict);
        if (!production && met.capability_live_session_grants.load(std::memory_order_relaxed) == 0)
            return 0; // AC3: zero extra work when no session grants
        std::lock_guard<std::mutex> lock(mtx);
        return revoke_session_grants_for_locked(tenant, mid, fiber_id, reason);
    }

    // Issue #3142: count live SessionBound entries for given tenant
    // (AC3 long-run chaos test: must == 0 after 1000 nested aborts).
    [[nodiscard]] std::size_t session_bound_entries_alive(TenantId tenant) const noexcept {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return 0;
        std::size_t n = 0;
        for (const auto& g : it->second) {
            if (!g.revoked && g.session_bound)
                ++n;
        }
        return n;
    }

    // Issue #3279: SSOT orphan detection — caller MUST hold `mtx`.
    // Walks by_tenant and counts session_bound && !revoked rows whose
    // bound_mutation_id is not in live_mids (the live mid set: current
    // fiber session mid, process hold mid, any live outermost Guard mid
    // set). Bumps session_bound_orphan_detected_total (existing counter,
    // #3142) per detected orphan — observe-only, no revoke here. Soft/Off
    // may call this freely (never revokes / denies solely due to sweep).
    // Issue #3241: fiber_id != 0 skips peer-fiber grants (both ids known
    // and differ → live on the peer, not an orphan); fiber_id=0 is legacy
    // mid-only (unknown / single-fiber). grant_fiber_id=0 is legacy and
    // still matches (same rule as revoke_session_grants_for_mid_locked).
    std::size_t count_session_bound_orphans_locked(const std::vector<std::uint64_t>& live_mids,
                                                   std::uint32_t fiber_id = 0) noexcept {
        std::size_t orphans = 0;
        for (const auto& [tenant, vec] : by_tenant) {
            (void)tenant;
            for (const auto& g : vec) {
                if (g.revoked || !g.session_bound)
                    continue;
                if (fiber_id != 0 && g.grant_fiber_id != 0 && g.grant_fiber_id != fiber_id)
                    continue; // peer-fiber grant — live elsewhere (#3241)
                bool live = false;
                for (const auto lm : live_mids) {
                    if (lm != 0 && lm == g.bound_mutation_id) {
                        live = true;
                        break;
                    }
                }
                if (!live) {
                    ++orphans;
                    g_capability_effect_metrics().session_bound_orphan_detected_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }
        return orphans;
    }

    // Issue #3279: production fail-closed orphan sweep (Option A).
    // When live_session_grants diverge from the tracked live mids (lost
    // Guard, abort without mid clear, dual-Evaluator race edge, sticky
    // escape misuse), the long-run grant table accumulates session_bound
    // rows whose mid is no longer live — privilege sticky relative to the
    // epoch model. Restricted/Strict: revoke orphaned rows with reason
    // "session-orphan-sweep" (SE + audit dual-write joinable by mid via
    // record_audit), clear the live counter. Soft/Off: observe-only —
    // detection may run (counter bumps) but never revokes/denies solely
    // due to the sweep (contract). fiber_id: current fiber (peer-fiber
    // grants skipped, #3241 — concurrent guards on other fibers stay live).
    std::size_t sweep_session_bound_orphans(const std::vector<std::uint64_t>& live_mids,
                                            std::uint32_t fiber_id = 0) {
        auto& met = g_capability_effect_metrics();
        const auto mode = sandbox_mode.load(std::memory_order_acquire);
        const bool production =
            (mode == EffectSandboxMode::Restricted || mode == EffectSandboxMode::Strict);
        std::lock_guard<std::mutex> lock(mtx);
        const auto orphans = count_session_bound_orphans_locked(live_mids, fiber_id);
        if (!production || orphans == 0)
            return orphans; // Soft/Off observe-only; production no-op on clean state
        std::size_t revoked = 0;
        auto ep = ::aura::core::current_mutation_epoch();
        if (ep == 0)
            ep = 1;
        for (auto& [tenant, vec] : by_tenant) {
            for (auto& g : vec) {
                if (g.revoked || !g.session_bound)
                    continue;
                if (fiber_id != 0 && g.grant_fiber_id != 0 && g.grant_fiber_id != fiber_id)
                    continue; // peer-fiber grant — live elsewhere (#3241)
                bool live = false;
                for (const auto lm : live_mids) {
                    if (lm != 0 && lm == g.bound_mutation_id) {
                        live = true;
                        break;
                    }
                }
                if (live)
                    continue;
                g.revoked = true;
                g.effects = Effect::None;
                g.revoke_epoch = ep;
                g.session_bound = false; // no longer live
                ++revoked;
                met.capability_session_revoke_total.fetch_add(1, std::memory_order_relaxed);
                met.capability_revoke_total.fetch_add(1, std::memory_order_relaxed);
                met.capability_revoke_epoch_bound_total.fetch_add(1, std::memory_order_relaxed);
                auto cur = met.capability_live_session_grants.load(std::memory_order_relaxed);
                if (cur > 0)
                    met.capability_live_session_grants.fetch_sub(1, std::memory_order_relaxed);
                EffectProvenance audit_prov{};
                audit_prov.mutation_id = g.bound_mutation_id;
                audit_prov.epoch = ep;
                record_audit(Effect::None, Effect::None, tenant, audit_prov,
                             /*denied=*/false, "session-orphan-sweep", "session-orphan-sweep");
            }
        }
        return revoked;
    }

    // Issue #3207: caller MUST hold `mtx`. Body of mark_session_bound_stolen
    // without the lock_guard — steal paths that already hold mtx must use
    // this (std::mutex is non-recursive).
    bool mark_session_bound_stolen_locked(TenantId tenant, std::uint64_t mid,
                                          std::uint32_t fiber_id) noexcept {
        if (mid == 0 || tenant == 0)
            return false;
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return false;
        bool found = false;
        for (auto& g : it->second) {
            if (g.revoked || !g.session_bound || g.stolen)
                continue;
            if (g.bound_mutation_id != mid)
                continue;
            if (fiber_id != 0 && g.grant_fiber_id != fiber_id)
                continue;
            g.stolen = true;
            found = true;
            g_capability_effect_metrics().session_bound_revoked_on_steal_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        return found;
    }

    // Issue #3142: mark SessionBound entry as stolen on fiber steal.
    // Public wrapper takes mtx (do not call while holding mtx — use
    // mark_session_bound_stolen_locked). Idempotent — second call on
    // same (tenant, mid, fiber_id) is no-op. Returns true if a matching
    // entry was found and marked stolen.
    bool mark_session_bound_stolen(TenantId tenant, std::uint64_t mid,
                                   std::uint32_t fiber_id) noexcept {
        if (mid == 0 || tenant == 0)
            return false;
        std::lock_guard<std::mutex> lock(mtx);
        return mark_session_bound_stolen_locked(tenant, mid, fiber_id);
    }

    // Issue #3209: mark stolen for every live session_bound grant bound
    // to mid (all tenants). Used by steal/abort when the caller has no
    // tenant (force-degrade tenant=0 was a no-op). Caller MUST hold mtx.
    bool mark_session_bound_stolen_for_mid_locked(std::uint64_t mid,
                                                  std::uint32_t fiber_id = 0) noexcept {
        if (mid == 0)
            return false;
        bool found = false;
        for (auto& [tenant, vec] : by_tenant) {
            (void)tenant;
            for (auto& g : vec) {
                if (g.revoked || !g.session_bound || g.stolen)
                    continue;
                if (g.bound_mutation_id != mid)
                    continue;
                if (fiber_id != 0 && g.grant_fiber_id != 0 && g.grant_fiber_id != fiber_id)
                    continue;
                g.stolen = true;
                found = true;
                g_capability_effect_metrics().session_bound_revoked_on_steal_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        return found;
    }

    // Issue #2055: revoke stamps revoke_epoch (WorkspaceEpoch Mutation) for audit.
    // If revoke_at_epoch == 0, callers should pass current_mutation_epoch() (or
    // the make_grant_provenance helper) so blame trails stay non-zero.
    void revoke(TenantId tenant, std::string_view name, std::uint64_t revoke_at_epoch = 0) {
        std::lock_guard<std::mutex> lock(mtx);
        revoke_locked(tenant, name, revoke_at_epoch);
    }

    // Issue #3126: caller MUST hold `mtx`. Body is the by_tenant
    // mutation logic of `revoke()` without the internal lock_guard.
    // Used by security fence paths in evaluator_security.cpp
    // (revoke_effect_capability foreign-tenant fence) so the admin
    // re-check + revoke are atomic w.r.t. concurrent grant / revoke
    // from another Evaluator / fiber.
    void revoke_locked(TenantId tenant, std::string_view name, std::uint64_t revoke_at_epoch = 0) {
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
    //
    // Issue #3126: Soft/observational only — unlocked read of `by_tenant`.
    // DO NOT use for security decisions under concurrent mutation: the
    // read can race with grant/revoke (data race UB on std::unordered_map,
    // and the result is stale w.r.t. concurrent admin revoke). Security
    // fence paths (grant_effect_* foreign-tenant + TenantAdmin fence)
    // must take the registry mtx and call `effects_for_locked` instead.
    [[nodiscard]] Effect effects_for(TenantId tenant) const {
        Effect acc = Effect::None;
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return acc;
        // Issue #3144 AC1: detect wildcard-only holder (kCapWildcard "*"
        // string grant but no explicit TenantAdmin) under production mode
        // (Restricted/Strict). If so, strip TenantAdmin + MacroSelfEvo
        // bits from the returned Effect (caller cannot pass
        // require_effect(TenantAdmin)). Soft/Off: zero-cost (no strip).
        const auto mode = sandbox_mode.load(std::memory_order_acquire);
        bool has_wildcard = false;
        bool has_explicit_TenantAdmin = false;
        for (const auto& g : it->second) {
            // Issue #3142: stolen entries do NOT contribute effects.
            if (!g.revoked && !g.stolen)
                acc = acc | g.effects;
            // Wildcard-only detection (production only).
            if (mode != EffectSandboxMode::Off && !g.revoked) {
                if (g.name == "*")
                    has_wildcard = true;
                else if ((static_cast<std::uint16_t>(g.effects) &
                          static_cast<std::uint16_t>(Effect::TenantAdmin)) != 0)
                    has_explicit_TenantAdmin = true;
            }
        }
        if (mode != EffectSandboxMode::Off && has_wildcard && !has_explicit_TenantAdmin) {
            constexpr std::uint16_t kStrip = static_cast<std::uint16_t>(Effect::TenantAdmin) |
                                             static_cast<std::uint16_t>(Effect::MacroSelfEvo);
            acc = static_cast<Effect>(static_cast<std::uint16_t>(acc) & ~kStrip);
            g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        return acc;
    }

    // Issue #3126: locked variant — caller MUST hold `mtx`. Body matches
    // `effects_for` but takes the registry lock so the read is atomic w.r.t.
    // concurrent grant/revoke. Used by `check_and_record_effect` (which
    // already holds `mtx`) and by security fence paths in
    // evaluator_security.cpp (grant_effect_* / revoke_effect_*) that take
    // the lock themselves to fold the admin re-check + act into one
    // critical section.
    [[nodiscard]] Effect effects_for_locked(TenantId tenant) const {
        Effect acc = Effect::None;
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return acc;
        // Issue #3144: same wildcard-only strip as effects_for above. Caller
        // MUST hold mtx (per the existing contract). Production fence strips
        // TenantAdmin + MacroSelfEvo from wildcard-only holders.
        const auto mode = sandbox_mode.load(std::memory_order_acquire);
        bool has_wildcard = false;
        bool has_explicit_TenantAdmin = false;
        for (const auto& g : it->second) {
            // Issue #3142: stolen entries excluded (see effects_for above).
            if (!g.revoked && !g.stolen)
                acc = acc | g.effects;
            if (mode != EffectSandboxMode::Off && !g.revoked) {
                if (g.name == "*")
                    has_wildcard = true;
                else if ((static_cast<std::uint16_t>(g.effects) &
                          static_cast<std::uint16_t>(Effect::TenantAdmin)) != 0)
                    has_explicit_TenantAdmin = true;
            }
        }
        if (mode != EffectSandboxMode::Off && has_wildcard && !has_explicit_TenantAdmin) {
            constexpr std::uint16_t kStrip = static_cast<std::uint16_t>(Effect::TenantAdmin) |
                                             static_cast<std::uint16_t>(Effect::MacroSelfEvo);
            acc = static_cast<Effect>(static_cast<std::uint16_t>(acc) & ~kStrip);
            g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        return acc;
    }

    // Issue #3141: wildcard-only detection. Returns true if tenant holds
    // kCapWildcard ("*") string grant AND no non-wildcard grant contributing
    // Effect::TenantAdmin. Caller MUST hold `mtx`. Distinguishes "TenantAdmin
    // via wildcard" from "explicit TenantAdmin via string grant" — the
    // former does NOT pass the production write fence.
    [[nodiscard]] bool holds_wildcard_only_locked(TenantId tenant) const noexcept {
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return false;
        bool has_wildcard = false;
        bool has_explicit_TenantAdmin = false;
        for (const auto& g : it->second) {
            if (g.revoked)
                continue;
            if (g.name == "*")
                has_wildcard = true;
            else if ((static_cast<std::uint16_t>(g.effects) &
                      static_cast<std::uint16_t>(Effect::TenantAdmin)) != 0)
                has_explicit_TenantAdmin = true;
        }
        return has_wildcard && !has_explicit_TenantAdmin;
    }

    // Optional provenance binding check: if grant has bound_mutation_id != 0
    // and caller's prov.mutation_id is non-zero and differs → mismatch.
    //
    // Issue #2707: under production sandbox (Restricted or Strict) the mid
    // join is **fail-closed**:
    //   - prov.mutation_id == 0 → deny (+ mid_join_zero_deny)
    //   - any non-revoked grant with bound_mutation_id == 0 → deny
    //   - else require bound_mutation_id == prov.mutation_id (strict eq)
    // Soft / Off keeps legacy skip-when-zero (zero-cost happy path, AC4).
    // Deny does not consume single-use (#2586) — caller only consumes on allow.
    //
    // Issue #2074: anti privilege-sticky — if grant has grant_epoch != 0
    // AND the registry's min_valid_epoch is set AND grant_epoch < min_valid_epoch,
    // the grant is expired (issued at a stale mutation epoch) → deny.
    // Issue #2055 / #2151 / #2536: grant_fiber_id mismatch policy:
    //   hard_fiber_isolation=false (Restricted default, #2536) → metric only,
    //     allow same-tenant multi-fiber share; TenantScope (#2491) is the
    //     principal boundary — fiber-level grant isolation is optional.
    //   hard_fiber_isolation=true → deny + capability_fiber_hard_deny_total
    //     (multi-tenant+Strict default, or AURA_HARD_FIBER_ISOLATION=1 even
    //     under pure Restricted — env never blocked by default branch).
    //
    // Issue #3126: Soft/observational only — unlocked read of `by_tenant`.
    // DO NOT use for security decisions under concurrent mutation: the
    // read can race with grant/revoke (data race UB on std::unordered_map,
    // and the result is stale w.r.t. concurrent admin revoke). Security
    // fence paths must take the registry mtx and call `provenance_ok_locked`
    // instead. `check_and_record_effect` (free function) already holds mtx
    // and uses `provenance_ok_locked`.
    [[nodiscard]] bool provenance_ok(TenantId tenant, const EffectProvenance& prov,
                                     Effect required = Effect::None) const {
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return true; // no grants → not a mismatch (denied separately)
        const bool hard_fiber = hard_fiber_isolation_.load(std::memory_order_acquire);
        // Issue #2707: fail-closed mid join under Restricted/Strict.
        const auto mode = sandbox_mode.load(std::memory_order_acquire);
        const bool fail_closed_mid =
            (mode == EffectSandboxMode::Restricted || mode == EffectSandboxMode::Strict);
        if (fail_closed_mid && prov.mutation_id == 0) {
            g_capability_effect_metrics().capability_mid_join_zero_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }
        for (const auto& g : it->second) {
            if (g.revoked || g.stolen)
                continue;
            // Issue #3333: only grants that contribute `required` bits join
            // mid / epoch / fiber. Unrelated live grants must not poison the
            // tenant. required==None → check all live grants (query path).
            if (required != Effect::None && !has_effect(g.effects, required))
                continue;
            if (fail_closed_mid) {
                // Production: zero bound mid is not a silent skip — refuse.
                if (g.bound_mutation_id == 0) {
                    g_capability_effect_metrics().capability_mid_join_zero_deny_total.fetch_add(
                        1, std::memory_order_relaxed);
                    return false;
                }
                if (g.bound_mutation_id != prov.mutation_id)
                    return false;
            } else {
                // Soft / Off: legacy skip-when-zero (AC4).
                if (g.bound_mutation_id != 0 && prov.mutation_id != 0 &&
                    g.bound_mutation_id != prov.mutation_id) {
                    return false;
                }
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
            // Issue #3076: capability_fiber_mismatch_total is Soft-observe
            // (not a Hard guarantee). Hard sibling is fiber_hard_deny
            // under hard_fiber_isolation (#2536 Restricted share stays Soft).
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

    // Issue #3126: locked variant — caller MUST hold `mtx`. Body matches
    // `provenance_ok` but takes the registry lock so the mid-join / epoch-
    // fence / fiber-mismatch checks are atomic w.r.t. concurrent
    // grant/revoke. Used by `check_and_record_effect` (which already holds
    // `mtx`) and by security fence paths.
    [[nodiscard]] bool provenance_ok_locked(TenantId tenant, const EffectProvenance& prov,
                                            Effect required = Effect::None) const {
        auto it = by_tenant.find(tenant);
        if (it == by_tenant.end())
            return true; // no grants → not a mismatch (denied separately)
        const bool hard_fiber = hard_fiber_isolation_.load(std::memory_order_acquire);
        const auto mode = sandbox_mode.load(std::memory_order_acquire);
        const bool fail_closed_mid =
            (mode == EffectSandboxMode::Restricted || mode == EffectSandboxMode::Strict);
        if (fail_closed_mid && prov.mutation_id == 0) {
            g_capability_effect_metrics().capability_mid_join_zero_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }
        for (const auto& g : it->second) {
            if (g.revoked || g.stolen)
                continue;
            // Issue #3333: contributing-grant mid join (see provenance_ok).
            if (required != Effect::None && !has_effect(g.effects, required))
                continue;
            if (fail_closed_mid) {
                if (g.bound_mutation_id == 0) {
                    g_capability_effect_metrics().capability_mid_join_zero_deny_total.fetch_add(
                        1, std::memory_order_relaxed);
                    return false;
                }
                if (g.bound_mutation_id != prov.mutation_id)
                    return false;
            } else {
                if (g.bound_mutation_id != 0 && prov.mutation_id != 0 &&
                    g.bound_mutation_id != prov.mutation_id) {
                    return false;
                }
            }
            const auto min_valid = grant_min_valid_epoch_.load(std::memory_order_acquire);
            if (g.grant_epoch != 0 && min_valid != 0 && g.grant_epoch < min_valid) {
                g_capability_effect_metrics().capability_epoch_fence_hit_total.fetch_add(
                    1, std::memory_order_relaxed);
                return false;
            }
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

    // Issue #2388 / #2530: private kAuditRing (1024) + dual-write SecurityEvent/WAL
    // so wrap under deny storms remains forensically recoverable (ring + WAL).
    // Earlier than ring-window events rely on WAL (documented: AC3).
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
        // Issue #3090: do NOT synthesize SE mid to `epoch ?: 1`. The old
        // `prov.mutation_id != 0 ? prov.mutation_id : (epoch ?: 1)` produced
        // phantom mid=1 entries that could not be joined with Typed trail /
        // grant refuse mid=0 events. Let mid=0 flow into the SE so Agent
        // joins via SE.reason (mid-fallback-refused / grant-mid-refused) +
        // mid=0 across trail ↔ grant ↔ effect-check surfaces (#3090 AC4).
        // Soft / quiet recording path may emit mid=0 here; SE.reason carries
        // the failure class — that is the canonical refuse shape.
        const auto mid = prov.mutation_id;
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
    //
    // Issue #3145 AC4: `caller_principal` is the explicit caller principal
    // (Evaluator::capability_tenant_id_). Default 0 falls back to the
    // process-global `default_tenant` for legacy direct callers without an
    // Evaluator context (tests / tooling); production Evaluator paths MUST
    // pass the explicit principal so the admin check resolves the real
    // per-Evaluator principal instead of the often-zero default_tenant.
    // The check already runs under `mtx`, so no additional lock is needed
    // (TOCTOU closure is implicit — the read is atomic w.r.t. concurrent
    // grant/revoke).
    bool grant_macro_self_evo(TenantId tenant, MacroSelfEvoPolicy policy = {},
                              const EffectProvenance& prov_in = {}, TenantId caller_principal = 0) {
        EffectProvenance prov = prov_in;
        // Always ensure non-zero epoch stamp (parity with make_grant_provenance).
        if (prov.epoch == 0) {
            const auto me = ::aura::core::current_mutation_epoch();
            prov.epoch = me != 0 ? me : 1;
        }
        // Issue #3459: phantom mid synthesis is Soft/Off-only contract
        // (#2531 parity). Production (Restricted/Strict) refuses mid==0
        // below — one refuse policy with grant() (#3090); no epoch|1
        // phantom rows (they cannot join the Typed trail while the SE
        // says mid=0 refused).
        {
            const auto mode_pre = sandbox_mode.load(std::memory_order_acquire);
            const bool fail_closed_pre = (mode_pre == EffectSandboxMode::Restricted ||
                                          mode_pre == EffectSandboxMode::Strict);
            if (!fail_closed_pre && prov.mutation_id == 0)
                prov.mutation_id = prov.epoch;
        }
        if (prov.fiber_id == 0)
            prov.fiber_id = effect_fiber_id_or(0);

        std::lock_guard<std::mutex> lock(mtx);
        // Issue #3029: production Restricted/Strict requires TenantAdmin
        // (or "tenant-admin" / "capability") on the caller or the target
        // tenant. Soft/Off (sandbox_mode==Off) is zero-cost.
        // Issue #3145 AC4: caller_principal is the explicit principal
        // (Evaluator::capability_tenant_id_); fallback to default_tenant
        // only for legacy direct callers.
        {
            const auto mode = sandbox_mode.load(std::memory_order_acquire);
            if (mode != EffectSandboxMode::Off) {
                auto has_admin = [&](TenantId t) -> bool {
                    // Issue #3362: use effects_for_locked (under mtx) which
                    // strips wildcard-only TenantAdmin / MacroSelfEvo bits
                    // (#3144) — closes the privilege-escalation path where a
                    // `*` grant alone could pass the admin check and then
                    // synthesize explicit TA via security:grant-effect!. Now
                    // aligns with try_grant_capability_string_path_privileged_locked
                    // / try_grant_cross_tenant_privileged (all use the same
                    // effects_for_locked face). Caller holds mtx so the
                    // _locked variant is safe.
                    return has_effect(effects_for_locked(t), Effect::TenantAdmin);
                };
                const auto caller = caller_principal != 0
                                        ? caller_principal
                                        : default_tenant.load(std::memory_order_acquire);
                if (!has_admin(caller) && !has_admin(tenant)) {
                    auto& met = g_capability_effect_metrics();
                    met.capability_macro_self_evo_grant_deny_total.fetch_add(
                        1, std::memory_order_relaxed);
                    using ::aura::core::security_event::SecurityEventKind;
                    using ::aura::core::security_event_wal::emit_security_event_durable;
                    emit_security_event_durable(
                        SecurityEventKind::EffectDeny, tenant, prov.mutation_id, prov.epoch,
                        static_cast<std::uint16_t>(Effect::MacroSelfEvo), "macro-self-evo",
                        "macro-self-evo-grant-needs-tenant-admin", /*denied=*/true,
                        static_cast<std::int64_t>(prov.fiber_id));
                    return false;
                }
            }
        }
        // Issue #3459: one refuse policy with grant() (#3090) — production
        // refuses prov.mutation_id == 0 BEFORE any write. The old front
        // synthesis (mid = epoch|1) made the in-apply refuse dead and
        // stamped phantom bound_mid rows that cannot join the Typed trail.
        {
            const auto mode_refuse = sandbox_mode.load(std::memory_order_acquire);
            const bool fail_closed = (mode_refuse == EffectSandboxMode::Restricted ||
                                      mode_refuse == EffectSandboxMode::Strict);
            if (fail_closed && prov.mutation_id == 0) {
                auto& met_refuse = g_capability_effect_metrics();
                met_refuse.capability_grant_mid_refused_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                using ::aura::core::security_event::SecurityEventKind;
                using ::aura::core::security_event_wal::emit_security_event_durable;
                emit_security_event_durable(SecurityEventKind::EffectDeny, tenant,
                                            /*mid=*/0, /*epoch=*/prov.epoch,
                                            static_cast<std::uint16_t>(Effect::MacroSelfEvo),
                                            "macro-self-evo", "grant-mid-refused",
                                            /*denied=*/true,
                                            static_cast<std::int64_t>(prov.fiber_id));
                return false;
            }
        }
        auto& vec = by_tenant[tenant];
        auto apply = [&](CapabilityGrant& g) {
            g.effects = g.effects | Effect::MacroSelfEvo;
            g.revoked = false;
            g.bound_mutation_id = prov.mutation_id;
            g.bound_node_id = prov.node_id;
            g.grant_epoch = prov.epoch;
            g.grant_fiber_id = prov.fiber_id;
            // Issue #3090: production refuse parity with grant(). Refuse when
            // Restricted/Strict and prov.mutation_id == 0 (same shape; macro
            // self-evo apply is gated by TenantAdmin fence #3029 above, but
            // this refuse is mid-specific and independent of TenantAdmin).
            {
                const auto mode_refuse = sandbox_mode.load(std::memory_order_acquire);
                const bool fail_closed = (mode_refuse == EffectSandboxMode::Restricted ||
                                          mode_refuse == EffectSandboxMode::Strict);
                if (fail_closed && g.bound_mutation_id == 0) {
                    auto& met_refuse = g_capability_effect_metrics();
                    met_refuse.capability_grant_mid_refused_total.fetch_add(
                        1, std::memory_order_relaxed);
                    using ::aura::core::security_event::SecurityEventKind;
                    using ::aura::core::security_event_wal::emit_security_event_durable;
                    emit_security_event_durable(SecurityEventKind::EffectDeny, tenant,
                                                /*mid=*/0, /*epoch=*/prov.epoch,
                                                static_cast<std::uint16_t>(Effect::MacroSelfEvo),
                                                "macro-self-evo", "grant-mid-refused",
                                                /*denied=*/true,
                                                static_cast<std::int64_t>(prov.fiber_id));
                    return;
                }
            }
            // Issue #2531: same non-zero mid force as grant().
            // After the refuse block above, this synthesis is dead under
            // production (bound_mid==0 has been refused above); it only
            // fires for Soft/Off when bound_mid is still zero.
            if (sandbox_mode.load(std::memory_order_acquire) != EffectSandboxMode::Off &&
                g.bound_mutation_id == 0) {
                g.bound_mutation_id = g.grant_epoch != 0 ? g.grant_epoch : 1;
            }
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
                return true;
            }
        }
        CapabilityGrant g;
        g.name = "macro-self-evo";
        g.effects = Effect::MacroSelfEvo;
        g.tenant_id = tenant;
        apply(g);
        vec.push_back(std::move(g));
        macro_self_evo_by_tenant[tenant] = policy;
        return true;
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

// Issue #3207: caller MUST hold registry mtx. Steal/abort counter bump
// + mid revoke without taking mtx again (std::mutex is non-recursive).
inline std::size_t
revoke_session_grants_on_steal_or_abort_locked(std::uint64_t session_mid, bool steal,
                                               std::uint32_t fiber_id = 0) noexcept {
    if (session_mid == 0)
        return 0;
    auto& met = g_capability_effect_metrics();
    auto& reg = g_capability_registry();
    // Issue #3209: mark_stolen before revoke so a resume consume that
    // interleaves after this lock still denies (stolen skip) even if a
    // later dtor revoke is the commutative no-op. fiber_id=0 marks every
    // live grant bound to this mid.
    (void)reg.mark_session_bound_stolen_for_mid_locked(session_mid, fiber_id);
    const char* reason = steal ? "session-mid-steal-exit" : "session-mid-abort-exit";
    const auto n = reg.revoke_session_grants_for_mid_locked(session_mid, reason, fiber_id);
    if (n > 0) {
        if (steal)
            met.capability_session_revoke_steal_total.fetch_add(n, std::memory_order_relaxed);
        else
            met.capability_session_revoke_abort_total.fetch_add(n, std::memory_order_relaxed);
    }
    return n;
}

// Issue #3048: single steal / force-cancel / non-Guard abort entry.
// Reuses revoke_session_grants_for_mid (no second revoke policy).
// steal=true → SE "session-mid-steal-exit"; else "session-mid-abort-exit".
// Zero cost when session_mid==0 or capability_live_session_grants==0 (AC4)
// under Soft/Off. Restricted/Strict take mtx even at live==0 (#3207).
// Second call is a no-op after the first (AC3, grants already revoked).
// Do not call while holding mtx — use the _locked sibling.
inline std::size_t revoke_session_grants_on_steal_or_abort(std::uint64_t session_mid, bool steal,
                                                           std::uint32_t fiber_id = 0) noexcept {
    if (session_mid == 0)
        return 0;
    auto& met = g_capability_effect_metrics();
    auto& reg = g_capability_registry();
    const auto mode = reg.sandbox_mode.load(std::memory_order_acquire);
    const bool production =
        (mode == EffectSandboxMode::Restricted || mode == EffectSandboxMode::Strict);
    if (!production && met.capability_live_session_grants.load(std::memory_order_relaxed) == 0)
        return 0; // AC4: Soft / empty live residual — no lock
    std::lock_guard<std::mutex> lock(reg.mtx);
    return revoke_session_grants_on_steal_or_abort_locked(session_mid, steal, fiber_id);
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
        // Issue #3363: unify the need_grant path — `wildcard_ok` no longer
        // skips the #3144 effects_for_locked strip. Wildcard-only TA/MSE
        // bits are stripped by effects_for_locked (per #3144), so a `*`
        // grant alone cannot satisfy require_effect(TenantAdmin |
        // MacroSelfEvo). Provenance fence stays. Soft/Off (`need_grant
        // ==false`): zero-cost short-circuit. `wildcard_ok` parameter
        // retained for backward compat (unused for allow).
        if (need_grant && required != Effect::None) {
            // Issue #3126: locked variant (caller already holds mtx).
            const Effect held = reg.effects_for_locked(tenant);
            // Require full coverage of required bits (not just any overlap).
            const auto req_u = static_cast<std::uint16_t>(required);
            const auto held_u = static_cast<std::uint16_t>(held);
            if ((held_u & req_u) != req_u)
                allowed = false;
            if (allowed && !reg.provenance_ok_locked(tenant, prov, required)) {
                allowed = false;
                met.capability_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const auto hard1 = met.capability_fiber_hard_deny_total.load(std::memory_order_relaxed);
        // Issue #2151 / #2388: Agent-stable reason when hard fiber isolation denies.
        // Issue #3363: stamp "via-wildcard" when caller passed wildcard_ok
        // but the bit check denied — observability only (no allow effect).
        const char* reason_hint = (!allowed && hard1 > hard0)               ? "fiber-grant-mismatch"
                                  : (wildcard_ok && need_grant && !allowed) ? "via-wildcard-denied"
                                                                            : nullptr;
        reg.record_audit(required, actual, tenant, prov, !allowed, op, reason_hint);

        // Issue #2586: single-use grant auto-revoke on successful allow.
        // Skip under wildcard_ok (wildcard grants all bits; single_use grant
        // was not necessary for allow and must stay intact). Skip when
        // required is None (no effect to match grants against).
        // Issue #3207: consume + live_session_grants fetch_sub stay under
        // this same lock (no lock-drop vs TenantScope cascade revoke).
        // Issue #3209: stolen skip below is the resume-after-steal deny
        // (no single-use-consumed) when mark_stolen happened-before this lock.
        if (allowed && required != Effect::None && !wildcard_ok) {
            auto it = reg.by_tenant.find(tenant);
            if (it != reg.by_tenant.end()) {
                for (auto& g : it->second) {
                    if (g.revoked || !g.single_use)
                        continue;
                    // Issue #3142 AC2: stolen entries are skipped — caller cannot
                    // consume a stolen SessionBound grant (no double-consume
                    // after fiber steal).
                    if (g.stolen)
                        continue;
                    if (!has_effect(g.effects, required))
                        continue;
                    const bool was_session = g.session_bound;
                    g.revoked = true;
                    g.effects = Effect::None;
                    g.session_bound = false; // #2944: no longer live session
                    auto ep = ::aura::core::current_mutation_epoch();
                    if (ep == 0)
                        ep = 1; // non-zero audit stamp at process origin
                    g.revoke_epoch = ep;
                    met.capability_single_use_consumed_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    met.capability_revoke_total.fetch_add(1, std::memory_order_relaxed);
                    if (was_session) {
                        auto cur =
                            met.capability_live_session_grants.load(std::memory_order_relaxed);
                        if (cur > 0)
                            met.capability_live_session_grants.fetch_sub(1,
                                                                         std::memory_order_relaxed);
                    }
                    // #2586 AC5: audit / SE dual-write reason "single-use-consumed"
                    // (record_audit pushes to SecurityEvent ring + WAL via
                    // emit_security_event_durable).
                    reg.record_audit(required, Effect::None, tenant, prov,
                                     /*denied=*/false, op, "single-use-consumed");
                }
            }
        }
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
        // Issue #2531: production (sandbox != Off) grant mid join must be
        // non-zero so provenance_ok mid compare cannot silently skip.
        // Fallback chain: caller mid → Mutation epoch → 0.
        // Issue #3090: do NOT fall through to `epoch ?: 1` — that synthesized
        // phantom mid=1 entries which broke mid-join alignment with
        // resolve_audit_mutation_id's refuse path (mid=0). Returning
        // mutation_id=0 lets the upper grant() apply refuse path bump
        // capability_grant_mid_refused_total + emit SE reason="grant-mid-refused"
        // instead of stamping a phantom mid=1 that cannot be joined with the
        // typed-trail / effect-check surfaces.
        prov.mutation_id = provenance_mutation_id != 0 ? provenance_mutation_id : me;
        // No `prov.epoch ?: 1` fallback: 0 is the canonical "no caller mid,
        // no Mutation epoch" signal; grant() apply will refuse under
        // Restricted/Strict per #3090 AC1.
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
    m.capability_mid_join_zero_deny_total.store(0, std::memory_order_relaxed); // #2707
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
    // Issue #2883: reset side-effect hard-deny counter for the
    // residual fiber-principal-mismatch hard face.
    m.capability_fiber_principal_mismatch_hard_deny_total.store(0, std::memory_order_relaxed);
    m.capability_grant_epoch_window_advance_total.store(0, std::memory_order_relaxed);
    m.capability_single_use_consumed_total.store(0, std::memory_order_relaxed); // #2586
    // Issue #2882: reset production-default single-use override counters
    // (forced-on + durable admin overrides).
    m.capability_high_risk_forced_single_use_total.store(0, std::memory_order_relaxed);
    m.capability_durable_high_risk_grant_total.store(0, std::memory_order_relaxed);
    // Issue #2944: session grant lifecycle.
    m.capability_session_grant_total.store(0, std::memory_order_relaxed);
    m.capability_session_revoke_total.store(0, std::memory_order_relaxed);
    m.capability_live_session_grants.store(0, std::memory_order_relaxed);
    // Issue #2967: durable high-risk call-site deny counter.
    m.capability_durable_grant_deny_total.store(0, std::memory_order_relaxed);
    // Issue #2969: registry write-fence foreign-tenant deny counter.
    m.capability_grant_foreign_tenant_deny_total.store(0, std::memory_order_relaxed);
    // Issue #3029: grant_macro_self_evo TenantAdmin fence deny counter.
    m.capability_macro_self_evo_grant_deny_total.store(0, std::memory_order_relaxed);
    // Issue #3048: steal / abort session-revoke breakdown.
    m.capability_session_revoke_steal_total.store(0, std::memory_order_relaxed);
    m.capability_session_revoke_abort_total.store(0, std::memory_order_relaxed);
    // Issue #3090: production grant refused when prov.mutation_id == 0
    // (Restricted/Strict). Reset alongside the rest of the grant counters.
    m.capability_grant_mid_refused_total.store(0, std::memory_order_relaxed);
    // Issue #3177: durable high-risk session_bound stamping counter
    // (Restricted/Strict). Reset alongside the rest of the durable / session
    // lifecycle counters.
    m.capability_durable_session_bound_total.store(0, std::memory_order_relaxed);
    m.macro_mutate_capability_deny_total.store(0, std::memory_order_relaxed);
}

struct CapabilityEffectStatsSnapshot {
    std::uint64_t enforced = 0;
    std::uint64_t denied = 0;
    std::uint64_t provenance_mismatch = 0;
    // Issue #2707: zero mid join denials under Restricted/Strict.
    std::uint64_t mid_join_zero_deny = 0;
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
    // Issue #2883: side-effect entry-point hard deny when current fiber
    // resume had a hard principal mismatch under production.
    std::uint64_t fiber_principal_mismatch_hard_deny = 0;
    int hard_fiber_isolation = 0;
    // Issue #2154
    std::uint64_t grant_min_valid_epoch = 0;
    std::uint64_t grant_epoch_retain_window = 0;
    std::uint64_t grant_epoch_window_advance = 0;
    // Issue #2586
    std::uint64_t capability_single_use_consumed = 0;
    // Issue #2882: production default single-use overrides for high-risk
    // (Mutate / MacroSelfEvo / TenantAdmin / Syscall) under Restricted/Strict.
    std::uint64_t capability_high_risk_forced_single_use = 0;
    std::uint64_t capability_durable_high_risk_grant = 0;
    // Issue #2944: mutation-session grant lifecycle.
    std::uint64_t capability_session_grant = 0;
    std::uint64_t capability_session_revoke = 0;
    std::uint64_t capability_live_session_grants = 0;
    // Issue #2967: durable high-risk call-site deny (missing TenantAdmin /
    // empty audit reason under production).
    std::uint64_t capability_durable_grant_deny = 0;
    // Issue #2969: registry write-fence foreign-tenant deny.
    std::uint64_t capability_grant_foreign_tenant_deny = 0;
    // Issue #3029: grant_macro_self_evo TenantAdmin fence deny.
    std::uint64_t capability_macro_self_evo_grant_deny = 0;
    // Issue #3048: steal / abort session-revoke breakdown (additive to
    // capability_session_revoke; Agent-readable vs normal session-mid-exit).
    std::uint64_t capability_session_revoke_steal = 0;
    std::uint64_t capability_session_revoke_abort = 0;
    // Issue #3090: production (Restricted/Strict) grant refused when
    // prov.mutation_id == 0. Distinct from mid_join_zero_deny (#2707,
    // check side): this is the grant-write-side refuse counter. Additive
    // on query:capability-effect-stats (key grant-mid-refused-total).
    std::uint64_t grant_mid_refused = 0;
    // Issue #3177: durable high-risk session_bound stamping counter
    // (Restricted/Strict production force). Reuses the existing
    // session_grant_total surface for live residual, but this separate
    // counter lets Agent dashboards chart the durable→session-bound force
    // separately from #2944 explicit session grants.
    std::uint64_t capability_durable_session_bound = 0;
    // Issue #3542: mutate MacroIntroduced opt-out without MacroSelfEvo.
    std::uint64_t macro_mutate_capability_deny = 0;
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
        s.mid_join_zero_deny =
            m.capability_mid_join_zero_deny_total.load(std::memory_order_acquire); // #2707
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
        // Issue #2883: hard-deny at side-effect entry on fiber principal
        // mismatch under production/Restricted.
        s.fiber_principal_mismatch_hard_deny =
            m.capability_fiber_principal_mismatch_hard_deny_total.load(std::memory_order_acquire);
        s.hard_fiber_isolation = reg.hard_fiber_isolation() ? 1 : 0;
        s.grant_min_valid_epoch = reg.grant_min_valid_epoch();
        s.grant_epoch_retain_window = reg.grant_epoch_retain_window();
        s.grant_epoch_window_advance =
            m.capability_grant_epoch_window_advance_total.load(std::memory_order_acquire);
        s.capability_single_use_consumed =
            m.capability_single_use_consumed_total.load(std::memory_order_acquire); // #2586
        // Issue #2882: production default single-use override counters.
        s.capability_high_risk_forced_single_use =
            m.capability_high_risk_forced_single_use_total.load(std::memory_order_acquire);
        s.capability_durable_high_risk_grant =
            m.capability_durable_high_risk_grant_total.load(std::memory_order_acquire);
        // Issue #2944: session grant lifecycle.
        s.capability_session_grant =
            m.capability_session_grant_total.load(std::memory_order_acquire);
        s.capability_session_revoke =
            m.capability_session_revoke_total.load(std::memory_order_acquire);
        s.capability_live_session_grants =
            m.capability_live_session_grants.load(std::memory_order_acquire);
        // Issue #2967: durable high-risk call-site deny.
        s.capability_durable_grant_deny =
            m.capability_durable_grant_deny_total.load(std::memory_order_acquire);
        // Issue #2969: registry write-fence foreign-tenant deny.
        s.capability_grant_foreign_tenant_deny =
            m.capability_grant_foreign_tenant_deny_total.load(std::memory_order_acquire);
        // Issue #3029: grant_macro_self_evo TenantAdmin fence.
        s.capability_macro_self_evo_grant_deny =
            m.capability_macro_self_evo_grant_deny_total.load(std::memory_order_acquire);
        // Issue #3048: steal / abort session-revoke breakdown.
        s.capability_session_revoke_steal =
            m.capability_session_revoke_steal_total.load(std::memory_order_acquire);
        s.capability_session_revoke_abort =
            m.capability_session_revoke_abort_total.load(std::memory_order_acquire);
        // Issue #3090: production grant refused when prov.mutation_id == 0.
        s.grant_mid_refused = m.capability_grant_mid_refused_total.load(std::memory_order_acquire);
        // Issue #3177: durable high-risk session_bound stamping counter.
        s.capability_durable_session_bound =
            m.capability_durable_session_bound_total.load(std::memory_order_acquire);
        // Issue #3542: mutate MacroIntroduced opt-out without MacroSelfEvo.
        s.macro_mutate_capability_deny =
            m.macro_mutate_capability_deny_total.load(std::memory_order_acquire);

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
        // Issue #3304: stamp parallel capability-deny code so Agent tooling
        // can key on it via capability_deny_last_reason_string().
        note_capability_deny_last_reason(kCapabilityDenyReasonNotGranted);
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, call_prov, true, "macro-self-evo");
        return out;
    }

    // Issue #2386: epoch fence / hard fiber isolation / bound mid (parity grant()).
    if (!reg.provenance_ok(tenant, call_prov, Effect::MacroSelfEvo)) {
        out.allowed = false;
        out.deny_reason = "MacroSelfEvo provenance fence (epoch/fiber/mid)";
        met.macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
        met.capability_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #3304: stamp parallel capability-deny code (epoch / fiber / mid).
        note_capability_deny_last_reason(kCapabilityDenyReasonProvenanceFence);
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
        // Issue #3304: stamp parallel capability-deny code (policy missing).
        note_capability_deny_last_reason(kCapabilityDenyReasonPolicyMissing);
        reg.record_audit(Effect::MacroSelfEvo, held, tenant, call_prov, true, "macro-self-evo");
        return out;
    }

    // Zero limits = explicit deny (AC: not granted or limits are zero).
    if (pol.max_depth == 0 || pol.max_expansion_passes == 0) {
        out.allowed = false;
        out.deny_reason = "MacroSelfEvo limits are zero";
        met.macro_self_evo_denied_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #3304: stamp parallel capability-deny code (limits zero).
        note_capability_deny_last_reason(kCapabilityDenyReasonLimitsZero);
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
// Issue #2532: write / policy-control caps join the matrix (epoch / fiber /
// retain-window via provenance_ok). SECURITY_EXEMPT read-only observability
// (effect_for_cap_name == None) remains string-list only:
//   compile, compile-stats, compile-dirty, compile-deopt, exception-control,
//   macro, query, sandbox
//   — comment token: SECURITY_EXEMPT: read-only observability
//
// Mapping table (write/control promoted):
//   workspace  → Mutate | TenantAdmin  (structural / policy write)
//   fiber      → TenantAdmin           (spawn / control plane)
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
    // Issue #2532: write / control plane (not pure query).
    if (name == "workspace")
        return Effect::Mutate | Effect::TenantAdmin;
    if (name == "fiber")
        return Effect::TenantAdmin;
    if (name == "*")
        return Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate | Effect::Network |
               Effect::Ffi | Effect::Render | Effect::MacroSelfEvo | Effect::TenantAdmin |
               Effect::Syscall;
    // SECURITY_EXEMPT: read-only observability (compile/query/sandbox/macro/…).
    return Effect::None;
}

// Issue #3141: kCapWildcard write-fence gate for string-path grant_capability.
// Under production mode (Restricted/Strict), a tenant holding kCapWildcard
// (which gives full Effect mask via `effect_for_cap_name("*")`) cannot
// write privilege-bearing cap names ("self-evo"/"synthesize"/"strategy"/
// "tenant-admin"/"capability"/"agent"/"workspace"/"fiber") without explicit
// TenantAdmin grant. Closes privilege escalation via wildcard → full mask →
// check passes → grant succeeds.
//
// Caller MUST hold registry `mtx`. AC3 Soft/Off zero-cost (one relaxed
// load + early-out before any scan).
//
// Return: true = allow; false = deny (counter bumped + SE emitted).
inline bool try_grant_capability_string_path_privileged_locked(TenantId caller,
                                                               std::string_view /*cap_name*/,
                                                               std::uint16_t eff_bits) noexcept {
    using namespace ::aura::core::capability;

    // AC3: Soft / sandbox=off zero-cost (no scan).
    const auto mode = g_capability_registry().sandbox_mode.load(std::memory_order_acquire);
    if (mode == EffectSandboxMode::Off)
        return true;

    // Only fence on privilege-bearing names (TenantAdmin / MacroSelfEvo).
    constexpr std::uint16_t kPrivilegeMask = static_cast<std::uint16_t>(Effect::TenantAdmin) |
                                             static_cast<std::uint16_t>(Effect::MacroSelfEvo);
    if ((eff_bits & kPrivilegeMask) == 0)
        return true;

    auto& reg = g_capability_registry();

    // AC2: caller has explicit TenantAdmin (not wildcard-only) → allow.
    // If caller is NOT a wildcard-only holder, the regular effects_for_locked
    // check applies: explicit TenantAdmin (via any non-wildcard string grant)
    // passes; no explicit TenantAdmin fails the standard check.
    if (!reg.holds_wildcard_only_locked(caller)) {
        const Effect held = reg.effects_for_locked(caller);
        return (static_cast<std::uint16_t>(held) &
                static_cast<std::uint16_t>(Effect::TenantAdmin)) != 0;
    }

    // AC1: wildcard-only holder attempting privilege-bearing grant → deny.
    g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.fetch_add(
        1, std::memory_order_relaxed);

    // Dual-write to SecurityEvent ring + WAL for forensic join (mid + fiber + epoch).
    using ::aura::core::current_mutation_epoch;
    using ::aura::core::security_event::SecurityEventKind;
    using ::aura::core::security_event_wal::emit_security_event_durable;
    const auto epoch = current_mutation_epoch();
    const auto mid = epoch != 0 ? epoch : static_cast<std::uint64_t>(1);
    const auto fid = static_cast<std::int64_t>(effect_fiber_id_or(0));
    emit_security_event_durable(SecurityEventKind::EffectDeny, caller, mid, epoch, eff_bits,
                                "grant_capability",
                                "wildcard-write-fence-needs-explicit-tenant-admin",
                                /*denied=*/true, fid);
    return false;
}

// Issue #3141: counter accessor for query surface.
[[nodiscard]] inline std::uint64_t capability_wildcard_write_fence_deny_total_v_read() noexcept {
    return g_capability_effect_metrics().capability_wildcard_write_fence_deny_total.load(
        std::memory_order_relaxed);
}

} // namespace aura::core::capability

#endif // AURA_CORE_CAPABILITY_MODEL_HH
