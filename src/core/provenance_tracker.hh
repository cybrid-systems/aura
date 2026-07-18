// provenance_tracker.hh — Issues #1180/#1500/#1564/#1630: full StableNodeRef
// provenance enforcement surface (header form for evaluator + tests).
// Complements FlatAST::StableNodeRef; does not replace it.

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
}

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

inline ProvenanceTracker& g_provenance_tracker() noexcept {
    static ProvenanceTracker t;
    return t;
}

} // namespace aura::core::provenance

#endif // AURA_CORE_PROVENANCE_TRACKER_HH
