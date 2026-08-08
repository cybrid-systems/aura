// Issue #2753: AotReloadConsistencyProof single Agent-holdable facade
// (symmetric to TypeLinearCommitProof #2697/#2717). Thin header so module
// partitions and light tests can include accessors without the full
// aura_jit_bridge.h surface.
//
// Issue #2776: stamp is concurrent-safe —
//   - stamp_epoch via fetch_add (no lost-update RMW)
//   - multi-writer seqlock: CAS even→odd claim, release odd→even;
//     readers spin until even and re-check so they never observe a torn
//     {table_epoch, bridge_epoch, defuse, masks, …} set.
#ifndef AURA_COMPILER_AOT_RELOAD_CONSISTENCY_PROOF_H
#define AURA_COMPILER_AOT_RELOAD_CONSISTENCY_PROOF_H

#include <atomic>
#include <cstdint>

// Keep AotReloadFail values as raw uint8_t here to avoid including the
// full bridge header (Ok=0, Version=2, …).

struct AotReloadConsistencyProof {
    std::uint64_t table_epoch = 0;
    std::uint64_t bridge_epoch = 0;
    std::uint64_t defuse_version = 0;
    std::uint64_t region_mask = 0;
    std::uint8_t last_fail_reason = 0;
    std::uint64_t force_jit_regions_mask = 0;
    bool would_allow_native = true;
    std::uint64_t stamp_epoch = 0;
    int schema = 2753;
};

inline constexpr int kAotReloadConsistencyProofIssue = 2753;
// Issue #2776: concurrent stamp discipline.
inline constexpr int kAotReloadConsistencyStampConcurrentIssue = 2776;

// Process-wide last stamped proof (seqlock-protected writers; readers use
// load_aot_reload_consistency_proof_snapshot / build_*_from_live).
inline std::atomic<std::uint64_t> g_aot_reload_proof_stamp_epoch{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_table_epoch{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_bridge_epoch{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_defuse_version{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_region_mask{0};
inline std::atomic<std::uint8_t> g_aot_reload_proof_last_fail{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_force_jit_mask{0};
inline std::atomic<int> g_aot_reload_proof_would_allow_native{1};
inline std::atomic<std::uint64_t> g_aot_reload_proof_stamped_total{0};
// Issue #2776: multi-writer seqlock — even = stable, odd = writer holds claim.
inline std::atomic<std::uint64_t> g_aot_reload_proof_seq{0};
// Soft metric: reader retried due to concurrent stamp (observability only).
inline std::atomic<std::uint64_t> g_aot_reload_proof_seqlock_retry_total{0};

// Seqlock-consistent snapshot of the last stamped proof.
// Spins while a writer holds the odd phase; never returns a torn multi-field set.
[[nodiscard]] inline AotReloadConsistencyProof
load_aot_reload_consistency_proof_snapshot() noexcept {
    AotReloadConsistencyProof p{};
    for (;;) {
        const auto s1 = g_aot_reload_proof_seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0) {
            g_aot_reload_proof_seqlock_retry_total.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        // Fence so field loads cannot hoist before s1 / sink after s2.
        std::atomic_thread_fence(std::memory_order_acquire);
        p.table_epoch = g_aot_reload_proof_table_epoch.load(std::memory_order_relaxed);
        p.bridge_epoch = g_aot_reload_proof_bridge_epoch.load(std::memory_order_relaxed);
        p.defuse_version = g_aot_reload_proof_defuse_version.load(std::memory_order_relaxed);
        p.region_mask = g_aot_reload_proof_region_mask.load(std::memory_order_relaxed);
        p.last_fail_reason = g_aot_reload_proof_last_fail.load(std::memory_order_relaxed);
        p.force_jit_regions_mask =
            g_aot_reload_proof_force_jit_mask.load(std::memory_order_relaxed);
        p.would_allow_native =
            g_aot_reload_proof_would_allow_native.load(std::memory_order_relaxed) != 0;
        p.stamp_epoch = g_aot_reload_proof_stamp_epoch.load(std::memory_order_relaxed);
        p.schema = kAotReloadConsistencyProofIssue;
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto s2 = g_aot_reload_proof_seq.load(std::memory_order_relaxed);
        if (s1 == s2 && (s2 & 1u) == 0)
            return p;
        g_aot_reload_proof_seqlock_retry_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Build a local proof from the last stable stamped snapshot.
// stamp_epoch is set to last+1 for callers that still fill a prep field before
// stamp(); stamp_aot_reload_consistency_proof ignores p.stamp_epoch and uses
// fetch_add (#2776 — no lost-update RMW).
[[nodiscard]] inline AotReloadConsistencyProof
build_aot_reload_consistency_proof_from_live(bool would_allow_native = true) noexcept {
    AotReloadConsistencyProof p = load_aot_reload_consistency_proof_snapshot();
    // Prep convenience for stamp sites that still assign p.stamp_epoch (ignored).
    p.stamp_epoch = p.stamp_epoch + 1;
    if (!would_allow_native) {
        p.would_allow_native = false;
    } else {
        p.would_allow_native =
            would_allow_native && p.last_fail_reason == 0 && p.force_jit_regions_mask == 0;
    }
    p.schema = kAotReloadConsistencyProofIssue;
    return p;
}

// Publish a multi-field proof under multi-writer seqlock.
// stamp_epoch is always assigned via fetch_add(1) — p.stamp_epoch is
// intentionally ignored (Issue #2776 lost-update ban).
inline void stamp_aot_reload_consistency_proof(const AotReloadConsistencyProof& p) noexcept {
    // Claim write phase: CAS even → odd (multi-writer safe).
    std::uint64_t s = g_aot_reload_proof_seq.load(std::memory_order_relaxed);
    for (;;) {
        if ((s & 1u) != 0) {
            s = g_aot_reload_proof_seq.load(std::memory_order_relaxed);
            continue;
        }
        if (g_aot_reload_proof_seq.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed))
            break;
    }
    // Monotonic stamp_epoch (lost-update RMW ban — one atomic, unique values).
    (void)g_aot_reload_proof_stamp_epoch.fetch_add(1, std::memory_order_relaxed);
    g_aot_reload_proof_table_epoch.store(p.table_epoch, std::memory_order_relaxed);
    g_aot_reload_proof_bridge_epoch.store(p.bridge_epoch, std::memory_order_relaxed);
    g_aot_reload_proof_defuse_version.store(p.defuse_version, std::memory_order_relaxed);
    g_aot_reload_proof_region_mask.store(p.region_mask, std::memory_order_relaxed);
    g_aot_reload_proof_last_fail.store(p.last_fail_reason, std::memory_order_relaxed);
    g_aot_reload_proof_force_jit_mask.store(p.force_jit_regions_mask, std::memory_order_relaxed);
    g_aot_reload_proof_would_allow_native.store(p.would_allow_native ? 1 : 0,
                                                std::memory_order_relaxed);
    g_aot_reload_proof_stamped_total.fetch_add(1, std::memory_order_relaxed);
    // End write: odd → even (release so readers see all field stores).
    g_aot_reload_proof_seq.fetch_add(1, std::memory_order_release);
}

// Individual field accessors — for multi-field decisions prefer
// load_aot_reload_consistency_proof_snapshot() (#2776).
inline std::uint64_t aura_last_aot_reload_consistency_stamp_epoch(void) noexcept {
    return g_aot_reload_proof_stamp_epoch.load(std::memory_order_acquire);
}
inline std::uint64_t aura_last_aot_reload_consistency_table_epoch(void) noexcept {
    return g_aot_reload_proof_table_epoch.load(std::memory_order_acquire);
}
inline std::uint64_t aura_last_aot_reload_consistency_bridge_epoch(void) noexcept {
    return g_aot_reload_proof_bridge_epoch.load(std::memory_order_acquire);
}
inline std::uint64_t aura_last_aot_reload_consistency_defuse_version(void) noexcept {
    return g_aot_reload_proof_defuse_version.load(std::memory_order_acquire);
}
inline std::uint64_t aura_last_aot_reload_consistency_region_mask(void) noexcept {
    return g_aot_reload_proof_region_mask.load(std::memory_order_acquire);
}
inline std::uint8_t aura_last_aot_reload_consistency_last_fail_reason(void) noexcept {
    return g_aot_reload_proof_last_fail.load(std::memory_order_acquire);
}
inline std::uint64_t aura_last_aot_reload_consistency_force_jit_mask(void) noexcept {
    return g_aot_reload_proof_force_jit_mask.load(std::memory_order_acquire);
}
inline int aura_last_aot_reload_consistency_would_allow_native(void) noexcept {
    return g_aot_reload_proof_would_allow_native.load(std::memory_order_acquire);
}
inline std::uint64_t aura_aot_reload_consistency_proof_stamped_total(void) noexcept {
    return g_aot_reload_proof_stamped_total.load(std::memory_order_relaxed);
}
inline int aura_aot_reload_consistency_proof_wired(void) noexcept {
    return 1;
}
inline std::uint64_t aura_aot_reload_proof_seqlock_retry_total(void) noexcept {
    return g_aot_reload_proof_seqlock_retry_total.load(std::memory_order_relaxed);
}

#endif // AURA_COMPILER_AOT_RELOAD_CONSISTENCY_PROOF_H
