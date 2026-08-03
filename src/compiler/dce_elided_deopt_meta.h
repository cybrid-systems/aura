// dce_elided_deopt_meta.h — Issue #2611
// Bounded side map of elided CastOp deopt meta: {mutation_id, narrow_evidence,
// original_type_tag} keyed by site. Zero cost when no evidence-backed elision.
// Soft: stamp when evidence non-zero (optional path). Production: same stamp
// rule when evidence non-zero (bounded ring). No stamp without evidence (AC2).
#pragma once

#include "typed_mutation_audit.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace aura::compiler::dce_deopt {

// ── Site key: block | instr | result_slot (compact, stable for AoS tests) ──
[[nodiscard]] inline std::uint64_t make_site_key(std::uint32_t block_id, std::uint32_t instr_idx,
                                                 std::uint32_t result_slot) noexcept {
    return (static_cast<std::uint64_t>(block_id) << 40) |
           (static_cast<std::uint64_t>(instr_idx & 0x00FFFFFFu) << 16) |
           static_cast<std::uint64_t>(result_slot & 0xFFFFu);
}

struct ElidedCastDeoptMeta {
    std::uint64_t site_key = 0;
    std::uint64_t mutation_id = 0;
    std::uint32_t narrow_evidence = 0;
    std::uint32_t original_type_tag = 0;
};

// Bounded ring capacity — Agent dashboards only need recent elide→deopt joins.
inline constexpr std::size_t kElidedCastDeoptMetaCap = 256;

// ── Counters (Agent / query:dead-coercion-layered-stats schema-2611) ──
inline std::atomic<std::uint64_t> dce_deopt_meta_stamped_total{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_skipped_no_evidence{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_map_size{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_lookup_hits{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_deopt_expose_total{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_last_mid{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_last_evidence{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_last_type_tag{0};
inline std::atomic<std::uint64_t> dce_deopt_meta_last_site_key{0};

namespace detail {
    inline std::mutex g_meta_mu;
    inline std::array<ElidedCastDeoptMeta, kElidedCastDeoptMetaCap> g_meta_ring{};
    inline std::size_t g_meta_head = 0; // next write index
    inline std::size_t g_meta_count = 0;
} // namespace detail

// Purpose: stamp deopt meta for an evidence-backed CastOp elision
// Pre: elision about to replace CastOp with Local (or AST identity skip)
// Post: ring entry + last_* atomics when narrow_evidence != 0; else skip counter
// Safety Class: P2 (bounded ring + mutex; no throw; zero cost when evidence==0)
// Issue: #2611
// AI-Native Rationale: Agents join post-mutate deopt spikes to MutationLog mid
//   + narrow_evidence without replaying the DCE pass
inline void stamp_elided_cast_deopt_meta(std::uint64_t site_key, std::uint64_t mutation_id,
                                         std::uint32_t narrow_evidence,
                                         std::uint32_t original_type_tag) noexcept {
    // AC2: elide without evidence → no meta stamp; zero extra map growth.
    if (narrow_evidence == 0) {
        dce_deopt_meta_skipped_no_evidence.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Soft + production: stamp when evidence non-zero (bounded). Soft "optional"
    // means absence of stamp is not a hard failure; we still stamp for Agents.
    {
        std::lock_guard<std::mutex> lock(detail::g_meta_mu);
        detail::g_meta_ring[detail::g_meta_head] =
            ElidedCastDeoptMeta{site_key, mutation_id, narrow_evidence, original_type_tag};
        detail::g_meta_head = (detail::g_meta_head + 1) % kElidedCastDeoptMetaCap;
        if (detail::g_meta_count < kElidedCastDeoptMetaCap)
            ++detail::g_meta_count;
        dce_deopt_meta_map_size.store(static_cast<std::uint64_t>(detail::g_meta_count),
                                      std::memory_order_relaxed);
    }
    dce_deopt_meta_stamped_total.fetch_add(1, std::memory_order_relaxed);
    dce_deopt_meta_last_mid.store(mutation_id, std::memory_order_relaxed);
    dce_deopt_meta_last_evidence.store(narrow_evidence, std::memory_order_relaxed);
    dce_deopt_meta_last_type_tag.store(original_type_tag, std::memory_order_relaxed);
    dce_deopt_meta_last_site_key.store(site_key, std::memory_order_relaxed);
}

// Purpose: lookup meta by site_key (JIT / IR deopt / tests)
// Pre: site_key from make_site_key at elision time
// Post: optional meta; lookup_hits bumped on hit
[[nodiscard]] inline std::optional<ElidedCastDeoptMeta>
lookup_elided_cast_deopt_meta(std::uint64_t site_key) noexcept {
    std::lock_guard<std::mutex> lock(detail::g_meta_mu);
    if (detail::g_meta_count == 0)
        return std::nullopt;
    // Walk newest → oldest (ring is write-forward).
    for (std::size_t n = 0; n < detail::g_meta_count; ++n) {
        const std::size_t idx =
            (detail::g_meta_head + kElidedCastDeoptMetaCap - 1 - n) % kElidedCastDeoptMetaCap;
        if (detail::g_meta_ring[idx].site_key == site_key) {
            dce_deopt_meta_lookup_hits.fetch_add(1, std::memory_order_relaxed);
            return detail::g_meta_ring[idx];
        }
    }
    return std::nullopt;
}

// Purpose: forced-deopt / query expose of last stamped mid+evidence (AC1)
// Pre: at least one evidence-backed stamp in process lifetime (or last_*)
// Post: deopt_expose_total++; returns last stamped meta snapshot
[[nodiscard]] inline ElidedCastDeoptMeta expose_last_deopt_meta() noexcept {
    ElidedCastDeoptMeta m;
    m.site_key = dce_deopt_meta_last_site_key.load(std::memory_order_relaxed);
    m.mutation_id = dce_deopt_meta_last_mid.load(std::memory_order_relaxed);
    m.narrow_evidence =
        static_cast<std::uint32_t>(dce_deopt_meta_last_evidence.load(std::memory_order_relaxed));
    m.original_type_tag =
        static_cast<std::uint32_t>(dce_deopt_meta_last_type_tag.load(std::memory_order_relaxed));
    dce_deopt_meta_deopt_expose_total.fetch_add(1, std::memory_order_relaxed);
    return m;
}

// Purpose: test isolation — clear ring + size (not stamped_total lifetime)
inline void clear_elided_cast_deopt_meta_for_test() noexcept {
    std::lock_guard<std::mutex> lock(detail::g_meta_mu);
    detail::g_meta_ring.fill(ElidedCastDeoptMeta{});
    detail::g_meta_head = 0;
    detail::g_meta_count = 0;
    dce_deopt_meta_map_size.store(0, std::memory_order_relaxed);
    dce_deopt_meta_last_mid.store(0, std::memory_order_relaxed);
    dce_deopt_meta_last_evidence.store(0, std::memory_order_relaxed);
    dce_deopt_meta_last_type_tag.store(0, std::memory_order_relaxed);
    dce_deopt_meta_last_site_key.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler::dce_deopt
