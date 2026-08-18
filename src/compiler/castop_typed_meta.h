// castop_typed_meta.h — Issue #2624 Phase A + #3140 Phase C.
// Process-local side table of CastOp type metadata:
//   {src_type_id, dst_type_id, narrow_evidence, type_tag, epoch_or_mid} keyed by site.
// Prefer side table over SoA/ABI layout change (AC4: not persisted in IR cache).
// Soft: stamp only when non-elided CastOp is emitted with type ids/tags.
// Missing meta on legacy IR → lookup returns nullopt + missing_total++ (AC3).
// Phase B (executor Strict) explicitly out of scope; Phase C (JIT deopt on
// typed-meta missing / epoch lag under Production only) wired here.
// See castop_density_policy.hh for the hot-entry gate.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace aura::compiler::castop_meta {

// ── Site key: block | instr | result_slot (matches dce_deopt #2611 shape) ──
[[nodiscard]] inline std::uint64_t make_site_key(std::uint32_t block_id, std::uint32_t instr_idx,
                                                 std::uint32_t result_slot) noexcept {
    return (static_cast<std::uint64_t>(block_id) << 40) |
           (static_cast<std::uint64_t>(instr_idx & 0x00FFFFFFu) << 16) |
           static_cast<std::uint64_t>(result_slot & 0xFFFFu);
}

struct CastOpTypedMeta {
    std::uint64_t site_key = 0;
    std::uint32_t src_type_id = 0;
    std::uint32_t dst_type_id = 0;
    std::uint32_t narrow_evidence = 0;
    std::uint32_t type_tag = 0;
    // Issue #3140 Phase C: stamped at lower time as the current
    // TypeLinearCommitProof stamp (last_type_linear_commit_proof_stamp
    // or occurrence_stability_epoch). JIT hot entry compares
    // meta.epoch_or_mid vs current_stamp; lag or missing → deopt /
    // force-relower under Production. 0 = un-stamped legacy → always
    // missing on lookup → AC1 lag path.
    std::uint64_t epoch_or_mid = 0;
};

// Bounded ring — Agents join recent non-elided CastOp proofs; no cache ABI.
inline constexpr std::size_t kCastOpTypedMetaCap = 256;

// ── Counters (query:dead-coercion-layered-stats schema-2624) ──
inline std::atomic<std::uint64_t> castop_typed_meta_stamped_total{0};
inline std::atomic<std::uint64_t> castop_typed_meta_missing_total{0};
inline std::atomic<std::uint64_t> castop_typed_meta_map_size{0};
inline std::atomic<std::uint64_t> castop_typed_meta_lookup_hits{0};
inline std::atomic<std::uint64_t> castop_typed_meta_identity_elide_total{0};
inline std::atomic<std::uint32_t> castop_typed_meta_wired{1};
inline std::atomic<std::uint32_t> castop_typed_meta_phase_a{1}; // Phase A only
// Issue #3140 Phase C: JIT deopt on missing/aging typed-meta under
// Production. Additive counter (AC4); no permanent dirty bits on Quiet
// (AC3, current_stamp==0 or meta.epoch_or_mid >= current_stamp).
inline constexpr int kCastOpTypedMetaPhaseCIssue = 3140;
inline std::atomic<std::uint64_t> castop_typed_meta_phase_c_deopt_total{0};
inline std::atomic<std::uint32_t> castop_typed_meta_phase_c_wired{1};
// Last stamp tracked (Agent join; mirrors the Phase A `last_*` set).
inline std::atomic<std::uint64_t> castop_typed_meta_last_epoch_or_mid{0};
// Last stamp (Agent join)
inline std::atomic<std::uint64_t> castop_typed_meta_last_site_key{0};
inline std::atomic<std::uint64_t> castop_typed_meta_last_src{0};
inline std::atomic<std::uint64_t> castop_typed_meta_last_dst{0};
inline std::atomic<std::uint64_t> castop_typed_meta_last_evidence{0};

namespace detail {
    inline std::mutex g_meta_mu;
    inline std::array<CastOpTypedMeta, kCastOpTypedMetaCap> g_meta_ring{};
    inline std::size_t g_meta_head = 0;
    inline std::size_t g_meta_count = 0;
} // namespace detail

// Purpose: stamp typed meta for a non-elided CastOp at lower time
// Pre: CastOp just emitted; site_key from block/instr/result
// Post: ring entry when (src|dst|tag|evidence) non-zero; else no stamp
// Safety Class: P2 (bounded ring + mutex; no throw)
// Issue: #2624 Phase A
// AI-Native Rationale: Agents observe AST→IR type inflow without full typed IR
// Issue #3140 Phase C: epoch_or_mid parameter (default 0 for legacy call
// sites). 0 = un-stamped legacy → AC1 lag path on JIT hot entry lookup.
// lowering_impl.cpp passes last_type_linear_commit_proof_stamp_v_read()
// so a fresh stamp matches current_stamp on the next call (AC3 Quiet).
inline void stamp_castop_typed_meta(std::uint64_t site_key, std::uint32_t src_type_id,
                                    std::uint32_t dst_type_id, std::uint32_t narrow_evidence,
                                    std::uint32_t type_tag,
                                    std::uint64_t epoch_or_mid = 0) noexcept {
    // Soft zero-cost when completely untyped: do not grow map.
    if (src_type_id == 0 && dst_type_id == 0 && narrow_evidence == 0 && type_tag == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(detail::g_meta_mu);
        detail::g_meta_ring[detail::g_meta_head] = CastOpTypedMeta{
            site_key, src_type_id, dst_type_id, narrow_evidence, type_tag, epoch_or_mid};
        detail::g_meta_head = (detail::g_meta_head + 1) % kCastOpTypedMetaCap;
        if (detail::g_meta_count < kCastOpTypedMetaCap)
            ++detail::g_meta_count;
        castop_typed_meta_map_size.store(static_cast<std::uint64_t>(detail::g_meta_count),
                                         std::memory_order_relaxed);
    }
    castop_typed_meta_stamped_total.fetch_add(1, std::memory_order_relaxed);
    castop_typed_meta_last_site_key.store(site_key, std::memory_order_relaxed);
    castop_typed_meta_last_src.store(src_type_id, std::memory_order_relaxed);
    castop_typed_meta_last_dst.store(dst_type_id, std::memory_order_relaxed);
    castop_typed_meta_last_evidence.store(narrow_evidence, std::memory_order_relaxed);
    castop_typed_meta_last_epoch_or_mid.store(epoch_or_mid, std::memory_order_relaxed);
}

// Purpose: lookup meta by site_key (DCE / tests / Phase B later)
// Pre: site_key from make_site_key at lower or DCE time
// Post: optional meta; missing_total++ on miss; lookup_hits on hit
// AC3: missing on old IR is non-fatal
[[nodiscard]] inline std::optional<CastOpTypedMeta>
lookup_castop_typed_meta(std::uint64_t site_key) noexcept {
    std::lock_guard<std::mutex> lock(detail::g_meta_mu);
    if (detail::g_meta_count == 0) {
        castop_typed_meta_missing_total.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    for (std::size_t n = 0; n < detail::g_meta_count; ++n) {
        const std::size_t idx =
            (detail::g_meta_head + kCastOpTypedMetaCap - 1 - n) % kCastOpTypedMetaCap;
        if (detail::g_meta_ring[idx].site_key == site_key) {
            castop_typed_meta_lookup_hits.fetch_add(1, std::memory_order_relaxed);
            return detail::g_meta_ring[idx];
        }
    }
    castop_typed_meta_missing_total.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

// Purpose: test isolation — clear ring (not lifetime stamped_total)
inline void clear_castop_typed_meta_for_test() noexcept {
    std::lock_guard<std::mutex> lock(detail::g_meta_mu);
    detail::g_meta_ring.fill(CastOpTypedMeta{});
    detail::g_meta_head = 0;
    detail::g_meta_count = 0;
    castop_typed_meta_map_size.store(0, std::memory_order_relaxed);
    castop_typed_meta_last_site_key.store(0, std::memory_order_relaxed);
    castop_typed_meta_last_src.store(0, std::memory_order_relaxed);
    castop_typed_meta_last_dst.store(0, std::memory_order_relaxed);
    castop_typed_meta_last_evidence.store(0, std::memory_order_relaxed);
    castop_typed_meta_last_epoch_or_mid.store(0, std::memory_order_relaxed);
}

// True when side table has any entry — Soft DCE can skip meta consult (zero cost).
[[nodiscard]] inline bool castop_typed_meta_present() noexcept {
    return castop_typed_meta_map_size.load(std::memory_order_relaxed) != 0;
}

// Issue #3140 Phase C: per-site lag check at JIT hot entry. Production
// only — caller gates via castop_density::production_path_enabled(). Does
// NOT bump Soft observe counters (missing_total, lookup_hits); separate
// Production-side path.
//
//   AC1: missing meta → lag (legacy un-stamped OR ring evict)
//   AC1: meta.epoch_or_mid < current_stamp → lag (AI mutate advanced proof)
//   AC3: meta.epoch_or_mid >= current_stamp OR current_stamp == 0 → no lag
//   AC3: Quiet → zero extra atomic RMW (no counter bump, no deopt branch)
//   AC2: Soft path never calls this (caller gates on production_override)
[[nodiscard]] inline bool castop_typed_meta_phase_c_lags(std::uint64_t site_key,
                                                         std::uint64_t current_stamp) noexcept {
    std::lock_guard<std::mutex> lock(detail::g_meta_mu);
    if (detail::g_meta_count == 0)
        return true; // AC1: empty ring → all sites lag
    const CastOpTypedMeta* hit = nullptr;
    for (std::size_t n = 0; n < detail::g_meta_count; ++n) {
        const std::size_t idx =
            (detail::g_meta_head + kCastOpTypedMetaCap - 1 - n) % kCastOpTypedMetaCap;
        if (detail::g_meta_ring[idx].site_key == site_key) {
            hit = &detail::g_meta_ring[idx];
            break;
        }
    }
    if (!hit)
        return true; // AC1: missing meta → lag
    if (current_stamp == 0)
        return false;                         // AC3 Quiet: no proofs stamped yet → no lag
    return hit->epoch_or_mid < current_stamp; // AC1 lag vs AC3 Quiet
}

// Issue #3140 Phase C: accessor for the additive counter (AC4 additive).
[[nodiscard]] inline std::uint64_t castop_typed_meta_phase_c_deopt_total_v_read() noexcept {
    return castop_typed_meta_phase_c_deopt_total.load(std::memory_order_relaxed);
}

// Issue #3140 Phase C: test helper — flip wired flag (AC2 AC3).
inline void set_castop_typed_meta_phase_c_wired_for_test(bool wired) noexcept {
    castop_typed_meta_phase_c_wired.store(wired ? 1u : 0u, std::memory_order_relaxed);
}

// Issue #3140 Phase C: reset counter for test isolation.
inline void reset_castop_typed_meta_phase_c_for_test() noexcept {
    castop_typed_meta_phase_c_deopt_total.store(0, std::memory_order_relaxed);
    castop_typed_meta_last_epoch_or_mid.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler::castop_meta
