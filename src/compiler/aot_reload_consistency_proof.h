// Issue #2753: AotReloadConsistencyProof single Agent-holdable facade
// (symmetric to TypeLinearCommitProof #2697/#2717). Thin header so module
// partitions and light tests can include accessors without the full
// aura_jit_bridge.h surface.
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

inline std::atomic<std::uint64_t> g_aot_reload_proof_stamp_epoch{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_table_epoch{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_bridge_epoch{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_defuse_version{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_region_mask{0};
inline std::atomic<std::uint8_t> g_aot_reload_proof_last_fail{0};
inline std::atomic<std::uint64_t> g_aot_reload_proof_force_jit_mask{0};
inline std::atomic<int> g_aot_reload_proof_would_allow_native{1};
inline std::atomic<std::uint64_t> g_aot_reload_proof_stamped_total{0};

inline AotReloadConsistencyProof
build_aot_reload_consistency_proof_from_live(bool would_allow_native = true) noexcept {
    AotReloadConsistencyProof p{};
    p.table_epoch = g_aot_reload_proof_table_epoch.load(std::memory_order_relaxed);
    p.bridge_epoch = g_aot_reload_proof_bridge_epoch.load(std::memory_order_relaxed);
    p.defuse_version = g_aot_reload_proof_defuse_version.load(std::memory_order_relaxed);
    p.region_mask = g_aot_reload_proof_region_mask.load(std::memory_order_relaxed);
    p.last_fail_reason = g_aot_reload_proof_last_fail.load(std::memory_order_relaxed);
    p.force_jit_regions_mask = g_aot_reload_proof_force_jit_mask.load(std::memory_order_relaxed);
    p.would_allow_native =
        would_allow_native && p.last_fail_reason == 0 && p.force_jit_regions_mask == 0;
    if (!would_allow_native)
        p.would_allow_native = false;
    p.stamp_epoch = g_aot_reload_proof_stamp_epoch.load(std::memory_order_relaxed) + 1;
    p.schema = kAotReloadConsistencyProofIssue;
    return p;
}

inline void stamp_aot_reload_consistency_proof(const AotReloadConsistencyProof& p) noexcept {
    g_aot_reload_proof_table_epoch.store(p.table_epoch, std::memory_order_relaxed);
    g_aot_reload_proof_bridge_epoch.store(p.bridge_epoch, std::memory_order_relaxed);
    g_aot_reload_proof_defuse_version.store(p.defuse_version, std::memory_order_relaxed);
    g_aot_reload_proof_region_mask.store(p.region_mask, std::memory_order_relaxed);
    g_aot_reload_proof_last_fail.store(p.last_fail_reason, std::memory_order_relaxed);
    g_aot_reload_proof_force_jit_mask.store(p.force_jit_regions_mask, std::memory_order_relaxed);
    g_aot_reload_proof_would_allow_native.store(p.would_allow_native ? 1 : 0,
                                                std::memory_order_relaxed);
    g_aot_reload_proof_stamp_epoch.store(p.stamp_epoch, std::memory_order_relaxed);
    g_aot_reload_proof_stamped_total.fetch_add(1, std::memory_order_relaxed);
}

inline std::uint64_t aura_last_aot_reload_consistency_stamp_epoch(void) noexcept {
    return g_aot_reload_proof_stamp_epoch.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_last_aot_reload_consistency_table_epoch(void) noexcept {
    return g_aot_reload_proof_table_epoch.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_last_aot_reload_consistency_bridge_epoch(void) noexcept {
    return g_aot_reload_proof_bridge_epoch.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_last_aot_reload_consistency_defuse_version(void) noexcept {
    return g_aot_reload_proof_defuse_version.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_last_aot_reload_consistency_region_mask(void) noexcept {
    return g_aot_reload_proof_region_mask.load(std::memory_order_relaxed);
}
inline std::uint8_t aura_last_aot_reload_consistency_last_fail_reason(void) noexcept {
    return g_aot_reload_proof_last_fail.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_last_aot_reload_consistency_force_jit_mask(void) noexcept {
    return g_aot_reload_proof_force_jit_mask.load(std::memory_order_relaxed);
}
inline int aura_last_aot_reload_consistency_would_allow_native(void) noexcept {
    return g_aot_reload_proof_would_allow_native.load(std::memory_order_relaxed);
}
inline std::uint64_t aura_aot_reload_consistency_proof_stamped_total(void) noexcept {
    return g_aot_reload_proof_stamped_total.load(std::memory_order_relaxed);
}
inline int aura_aot_reload_consistency_proof_wired(void) noexcept {
    return 1;
}

#endif // AURA_COMPILER_AOT_RELOAD_CONSISTENCY_PROOF_H
