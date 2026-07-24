// provenance_tracker.hh — Issues #1180/#1500/#1564/#1630/#1877: full StableNodeRef
// provenance enforcement surface (header form for evaluator + tests).
// Complements FlatAST::StableNodeRef; does not replace it.
// Issue #1877: MacroIntroduced hygiene → provenance stamp + FailOnStale
// under sandbox Strict (no silent restamp in multi-tenant AI self-modify).

#ifndef AURA_CORE_PROVENANCE_TRACKER_HH
#define AURA_CORE_PROVENANCE_TRACKER_HH

#include <atomic>
#include <cstdint>

namespace aura::core::provenance {

inline constexpr int kProvenanceTrackerPhase = 3; // #1630 mandate full provenance
inline constexpr int kProvenanceTrackerIssue = 1630;

// Policy for ensure_valid_or_refresh (AC).
enum class AutoRefreshPolicy : std::uint8_t {
    Off = 0,
    AutoRefreshOnBoundary = 1, // default production: refresh when stale
    FailOnStale = 2,           // validate only; no restamp
};

// Process-wide enforcement counters (#1564 AC3).
struct ProvenanceEnforcementMetrics {
    std::atomic<std::uint64_t> stable_ref_auto_refresh_total{0};
    std::atomic<std::uint64_t> stable_ref_epoch_fence_hit_total{0};
    std::atomic<std::uint64_t> cross_layer_provenance_mismatch_total{0};
    std::atomic<std::uint64_t> ensure_valid_calls_total{0};
    std::atomic<std::uint64_t> ensure_valid_success_total{0};
    std::atomic<std::uint64_t> ensure_valid_fail_total{0};
    std::atomic<std::uint64_t> fiber_id_mismatch_total{0};
    std::atomic<std::uint64_t> policy_enforced_total{0};
    std::atomic<std::uint64_t> hot_path_auto_refresh_total{0};
    // Issue #1630 AC counters (aliases for Agent dashboards).
    std::atomic<std::uint64_t> boundary_pinned_auto_restamp_total{0};
    std::atomic<std::uint64_t> cross_cow_provenance_enforced_total{0};
    // Issue #1877: hygiene-protected / MacroIntroduced gates stamped into
    // provenance tracker (audit log + StableNodeRef-style record).
    std::atomic<std::uint64_t> macro_hygiene_provenance_hits_total{0};
    // Issue #1877: Strict sandbox engaged FailOnStale provenance policy.
    std::atomic<std::uint64_t> fail_on_stale_strict_sandbox_total{0};
    // Issue #2026: linear ownership × provenance consistency closed-loop.
    std::atomic<std::uint64_t> linear_provenance_checks_total{0};
    std::atomic<std::uint64_t> linear_provenance_ok_total{0};
    std::atomic<std::uint64_t> linear_provenance_mismatch_total{0};
    std::atomic<std::uint64_t> linear_provenance_moved_live_total{0};
    std::atomic<std::uint64_t> linear_provenance_incomplete_total{0};
    std::atomic<std::uint64_t> linear_provenance_deopt_total{0};
    std::atomic<std::uint64_t> linear_provenance_steal_checks_total{0};
    std::atomic<std::uint64_t> linear_provenance_gc_checks_total{0};
};

inline ProvenanceEnforcementMetrics& g_provenance_enforcement() noexcept {
    static ProvenanceEnforcementMetrics m;
    return m;
}

inline void record_auto_refresh(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_auto_refresh_total.fetch_add(n,
                                                                       std::memory_order_relaxed);
}
inline void record_epoch_fence_hit(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_epoch_fence_hit_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_cross_layer_mismatch(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().cross_layer_provenance_mismatch_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_ensure_valid_call() noexcept {
    g_provenance_enforcement().ensure_valid_calls_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_ensure_valid_success() noexcept {
    g_provenance_enforcement().ensure_valid_success_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_ensure_valid_fail() noexcept {
    g_provenance_enforcement().ensure_valid_fail_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_fiber_id_mismatch() noexcept {
    g_provenance_enforcement().fiber_id_mismatch_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_policy_enforced() noexcept {
    g_provenance_enforcement().policy_enforced_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_hot_path_auto_refresh(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().hot_path_auto_refresh_total.fetch_add(n, std::memory_order_relaxed);
}
inline void record_boundary_pinned_auto_restamp(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().boundary_pinned_auto_restamp_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_cross_cow_provenance_enforced(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().cross_cow_provenance_enforced_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_macro_hygiene_provenance_hit(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().macro_hygiene_provenance_hits_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_fail_on_stale_strict_sandbox(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().fail_on_stale_strict_sandbox_total.fetch_add(
        n, std::memory_order_relaxed);
}

// Issue #2026: linear ownership state codes (mirror linear_rt without
// depending on the evaluator module).
inline constexpr std::uint8_t kLinearUntracked = 0;
inline constexpr std::uint8_t kLinearOwned = 1;
inline constexpr std::uint8_t kLinearBorrowed = 2;
inline constexpr std::uint8_t kLinearMutBorrowed = 3;
inline constexpr std::uint8_t kLinearMoved = 4;

// Result of validate_linear_provenance (shared by steal / GC / IR / boundary).
struct LinearProvenanceResult {
    bool ok = true;
    bool force_deopt = false; // mismatch severe enough to drop/deopt
    const char* reason = nullptr;
};

// Issue #2026: unified linear ownership + provenance consistency check.
//
// Policy (shared across fiber-steal, GC safepoint, MutationBoundary failure,
// and IR executor linear ops):
//   - Untracked: always ok (no linear root)
//   - Moved as a live root: mismatch + force_deopt (use-after-move)
//   - Owned/Borrowed/MutBorrowed with stale frame_version: mismatch + deopt
//   - bridge_epoch != 0 and != current: mismatch + deopt (steal/GC domain)
//   - Tracked linear with both provenance_id==0 and mutation_id==0:
//     incomplete forensic trail → bump incomplete; force_deopt only when
//     require_complete=true (steal/GC enforce paths pass true)
//
// node_id is for audit/forensics (env_id or AST node); 0 when unavailable.
[[nodiscard]] inline LinearProvenanceResult
validate_linear_provenance(std::uint8_t linear_state, std::uint32_t node_id = 0,
                           std::uint32_t provenance_id = 0, std::uint64_t mutation_id = 0,
                           std::uint64_t frame_version = 0, std::uint64_t current_version = 0,
                           std::uint64_t bridge_epoch = 0, std::uint64_t current_bridge_epoch = 0,
                           bool require_complete = false) noexcept {
    (void)node_id;
    auto& m = g_provenance_enforcement();
    m.linear_provenance_checks_total.fetch_add(1, std::memory_order_relaxed);
    LinearProvenanceResult r;

    if (linear_state == kLinearUntracked) {
        m.linear_provenance_ok_total.fetch_add(1, std::memory_order_relaxed);
        return r;
    }

    // Moved must never remain a live GC/steal root.
    if (linear_state == kLinearMoved) {
        r.ok = false;
        r.force_deopt = true;
        r.reason = "Moved linear live root";
        m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_moved_live_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
        m.cross_layer_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        return r;
    }

    // EnvFrame version drift (steal / mutate concurrent with GC).
    if (current_version != 0 && frame_version != 0 && frame_version < current_version) {
        r.ok = false;
        r.force_deopt = true;
        r.reason = "linear EnvFrame version stale";
        m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
        return r;
    }

    // Bridge epoch drift across COW / compact / steal.
    if (bridge_epoch != 0 && current_bridge_epoch != 0 && bridge_epoch != current_bridge_epoch) {
        r.ok = false;
        r.force_deopt = true;
        r.reason = "linear bridge_epoch mismatch";
        m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
        return r;
    }

    // Tracked linear without forensic provenance (incomplete chain).
    if (provenance_id == 0 && mutation_id == 0) {
        m.linear_provenance_incomplete_total.fetch_add(1, std::memory_order_relaxed);
        if (require_complete) {
            r.ok = false;
            r.force_deopt = true;
            r.reason = "linear provenance incomplete";
            m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
            m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
            return r;
        }
        // Soft: incomplete but not force-deopt on hot IR path.
    }

    m.linear_provenance_ok_total.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// Completeness ratio in basis points (0–10000): ok / checks.
[[nodiscard]] inline std::uint64_t linear_provenance_consistency_bp() noexcept {
    auto& m = g_provenance_enforcement();
    const auto c = m.linear_provenance_checks_total.load(std::memory_order_relaxed);
    const auto ok = m.linear_provenance_ok_total.load(std::memory_order_relaxed);
    return c > 0 ? (ok * 10000u) / c : 10000u;
}

// Issue #1877: last MacroIntroduced hygiene → provenance stamp so truncated
// blame chains can append a hygiene frame (traceable under AI self-modify).
struct HygieneProvenanceStamp {
    std::uint32_t node_id = 0;
    std::uint64_t tenant_id = 0;
    std::uint64_t source_mutation_id = 0;
    std::uint32_t fiber_id = 0;
    std::uint64_t seq = 0;
};

// Validate tenant_id against current principal (hot-path helper for #1877).
// Zero on either side is treated as "unset" (compatible with legacy refs).
[[nodiscard]] inline bool tenant_ids_compatible(std::uint64_t ref_tenant,
                                                std::uint64_t current_tenant) noexcept {
    if (ref_tenant == 0 || current_tenant == 0)
        return true;
    return ref_tenant == current_tenant;
}

// Forward decl so reset can clear last_hygiene on the process-wide tracker.
struct ProvenanceTracker;
inline ProvenanceTracker& g_provenance_tracker() noexcept;

struct ProvenanceStatsSnapshot {
    std::uint64_t auto_refresh = 0;
    std::uint64_t epoch_fence_hit = 0;
    std::uint64_t cross_layer_mismatch = 0;
    std::uint64_t ensure_calls = 0;
    std::uint64_t ensure_success = 0;
    std::uint64_t ensure_fail = 0;
    std::uint64_t fiber_mismatch = 0;
    std::uint64_t policy_enforced = 0;
    std::uint64_t hot_path_refresh = 0;
    std::uint64_t boundary_pinned_auto_restamp = 0;
    std::uint64_t cross_cow_provenance_enforced = 0;
    std::uint64_t macro_hygiene_provenance_hits = 0;
    std::uint64_t fail_on_stale_strict_sandbox = 0;
    int phase = kProvenanceTrackerPhase;
    int issue = kProvenanceTrackerIssue;
};

[[nodiscard]] inline ProvenanceStatsSnapshot snapshot_provenance_enforcement() noexcept {
    auto& m = g_provenance_enforcement();
    return ProvenanceStatsSnapshot{
        m.stable_ref_auto_refresh_total.load(std::memory_order_relaxed),
        m.stable_ref_epoch_fence_hit_total.load(std::memory_order_relaxed),
        m.cross_layer_provenance_mismatch_total.load(std::memory_order_relaxed),
        m.ensure_valid_calls_total.load(std::memory_order_relaxed),
        m.ensure_valid_success_total.load(std::memory_order_relaxed),
        m.ensure_valid_fail_total.load(std::memory_order_relaxed),
        m.fiber_id_mismatch_total.load(std::memory_order_relaxed),
        m.policy_enforced_total.load(std::memory_order_relaxed),
        m.hot_path_auto_refresh_total.load(std::memory_order_relaxed),
        m.boundary_pinned_auto_restamp_total.load(std::memory_order_relaxed),
        m.cross_cow_provenance_enforced_total.load(std::memory_order_relaxed),
        m.macro_hygiene_provenance_hits_total.load(std::memory_order_relaxed),
        m.fail_on_stale_strict_sandbox_total.load(std::memory_order_relaxed),
        kProvenanceTrackerPhase,
        kProvenanceTrackerIssue,
    };
}

// Lightweight tracker (Phase 2): mutation_id + epoch fence bookkeeping.
// FlatAST::StableNodeRef remains the production handle; this tracks policy metrics.
struct ProvenanceTracker {
    std::uint64_t records = 0;
    std::uint64_t validations = 0;
    std::uint64_t dirty_propagations = 0;
    AutoRefreshPolicy policy = AutoRefreshPolicy::AutoRefreshOnBoundary;
    // Issue #1877: last hygiene stamp lives on the process-wide tracker so
    // module TUs (type_checker) and non-module TUs (tests/audit) share it.
    HygieneProvenanceStamp last_hygiene{};

    void record_mutation() noexcept { ++records; }
    void set_policy(AutoRefreshPolicy p) noexcept { policy = p; }
    [[nodiscard]] AutoRefreshPolicy get_policy() const noexcept { return policy; }

    // mutation_id check (legacy scaffold API, still used by demos).
    [[nodiscard]] bool validate_mutation_id(std::uint64_t captured,
                                            std::uint64_t current_source) const noexcept {
        ++const_cast<ProvenanceTracker*>(this)->validations;
        return captured == 0 || captured == current_source;
    }

    // Epoch fence: true if still fresh (captured == current or captured==0 legacy).
    [[nodiscard]] bool epoch_fence_ok(std::uint64_t captured_epoch,
                                      std::uint64_t current_epoch) const noexcept {
        if (captured_epoch == 0)
            return true; // unstamped / legacy
        return captured_epoch == current_epoch;
    }
};

// Namespace-scope inline (not function-local static) so module TUs that
// include this header in the global module fragment share one instance
// with non-module TUs (tests / typed_mutation_audit). Issue #1877.
inline ProvenanceTracker g_provenance_tracker_storage{};

inline ProvenanceTracker& g_provenance_tracker() noexcept {
    return g_provenance_tracker_storage;
}

// Alias onto process-wide tracker (module-safe shared state).
inline HygieneProvenanceStamp& g_last_hygiene_provenance_stamp() noexcept {
    return g_provenance_tracker().last_hygiene;
}

// Stamp hygiene violation into process-wide provenance tracker + last stamp.
// tenant_id from workspace_isolation / CapabilityGrant principal.
// Issue #1877: called from TypedMutationAudit hygiene gate so both audit
// trail and provenance tracker see MacroIntroduced blocks.
inline void record_macro_hygiene_provenance(std::uint32_t node_id, std::uint64_t tenant_id = 0,
                                            std::uint64_t mutation_id = 0,
                                            std::uint32_t fiber_id = 0) noexcept {
    auto& tr = g_provenance_tracker();
    tr.record_mutation();
    record_macro_hygiene_provenance_hit();
    auto& s = tr.last_hygiene;
    s.node_id = node_id;
    s.tenant_id = tenant_id;
    s.source_mutation_id = mutation_id;
    s.fiber_id = fiber_id;
    ++s.seq;
}

inline void reset_provenance_enforcement_for_test() noexcept {
    auto& m = g_provenance_enforcement();
    m.stable_ref_auto_refresh_total.store(0, std::memory_order_relaxed);
    m.stable_ref_epoch_fence_hit_total.store(0, std::memory_order_relaxed);
    m.cross_layer_provenance_mismatch_total.store(0, std::memory_order_relaxed);
    m.ensure_valid_calls_total.store(0, std::memory_order_relaxed);
    m.ensure_valid_success_total.store(0, std::memory_order_relaxed);
    m.ensure_valid_fail_total.store(0, std::memory_order_relaxed);
    m.fiber_id_mismatch_total.store(0, std::memory_order_relaxed);
    m.policy_enforced_total.store(0, std::memory_order_relaxed);
    m.hot_path_auto_refresh_total.store(0, std::memory_order_relaxed);
    m.boundary_pinned_auto_restamp_total.store(0, std::memory_order_relaxed);
    m.cross_cow_provenance_enforced_total.store(0, std::memory_order_relaxed);
    m.macro_hygiene_provenance_hits_total.store(0, std::memory_order_relaxed);
    m.fail_on_stale_strict_sandbox_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_checks_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_ok_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_mismatch_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_moved_live_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_incomplete_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_deopt_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_steal_checks_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_gc_checks_total.store(0, std::memory_order_relaxed);
    g_provenance_tracker().last_hygiene = {};
}

} // namespace aura::core::provenance

#endif // AURA_CORE_PROVENANCE_TRACKER_HH
