// castop_typed_meta.h — Issue #2624 Phase A
// Process-local side table of CastOp type metadata:
//   {src_type_id, dst_type_id, narrow_evidence, type_tag} keyed by site.
// Prefer side table over SoA/ABI layout change (AC4: not persisted in IR cache).
// Soft: stamp only when non-elided CastOp is emitted with type ids/tags.
// Missing meta on legacy IR → lookup returns nullopt + missing_total++ (AC3).
// Phase B (executor Strict) / Phase C (JIT deopt) explicitly out of scope.
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
inline void stamp_castop_typed_meta(std::uint64_t site_key, std::uint32_t src_type_id,
                                    std::uint32_t dst_type_id, std::uint32_t narrow_evidence,
                                    std::uint32_t type_tag) noexcept {
    // Soft zero-cost when completely untyped: do not grow map.
    if (src_type_id == 0 && dst_type_id == 0 && narrow_evidence == 0 && type_tag == 0)
        return;
    {
        std::lock_guard<std::mutex> lock(detail::g_meta_mu);
        detail::g_meta_ring[detail::g_meta_head] =
            CastOpTypedMeta{site_key, src_type_id, dst_type_id, narrow_evidence, type_tag};
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
}

// True when side table has any entry — Soft DCE can skip meta consult (zero cost).
[[nodiscard]] inline bool castop_typed_meta_present() noexcept {
    return castop_typed_meta_map_size.load(std::memory_order_relaxed) != 0;
}

} // namespace aura::compiler::castop_meta
