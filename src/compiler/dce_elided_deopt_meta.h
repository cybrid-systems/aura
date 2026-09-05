// dce_elided_deopt_meta.h — Issue #2611
// Bounded side map of elided CastOp deopt meta: {mutation_id, narrow_evidence,
// original_type_tag} keyed by site. Zero cost when no evidence-backed elision.
// Soft: stamp when evidence non-zero (optional path). Production: same stamp
// rule when evidence non-zero (bounded ring). No stamp without evidence (AC2).
#pragma once

#include "typed_mutation_audit.h"

// Issue #3547: live FlatAST type_id from TLS commit-readiness Evaluator.
// Strong def: evaluator_mutation_boundary.cpp. Weak stub returns 0.
extern "C" std::uint32_t aura_tls_workspace_type_id(std::uint32_t node) noexcept;

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
    // Issue #3547: type_id payload so dirty-cone reuse can re-verify
    // against live FlatAST (site-key-only cache was the G2 residual).
    std::uint32_t type_id = 0;
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
                                         std::uint32_t original_type_tag,
                                         std::uint32_t type_id = 0) noexcept {
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
            ElidedCastDeoptMeta{site_key, mutation_id, narrow_evidence, original_type_tag, type_id};
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

// Issue #3547: AST elision site_key is make_site_key(0, original_child, parent).
[[nodiscard]] inline std::uint32_t site_key_ast_node(std::uint64_t site_key) noexcept {
    if ((site_key >> 40) != 0)
        return 0;
    return static_cast<std::uint32_t>((site_key >> 16) & 0x00FFFFFFu);
}

// Purpose: drop one (or all, site_key==0) elided-cast deopt entries.
// Persist-reject / type-drift reuse this — no second cache.
inline void invalidate_elided_cast_deopt_meta(std::uint64_t site_key) noexcept {
    std::lock_guard<std::mutex> lock(detail::g_meta_mu);
    if (site_key == 0) {
        detail::g_meta_ring.fill(ElidedCastDeoptMeta{});
        detail::g_meta_head = 0;
        detail::g_meta_count = 0;
        dce_deopt_meta_map_size.store(0, std::memory_order_relaxed);
        return;
    }
    for (std::size_t n = 0; n < detail::g_meta_count; ++n) {
        const std::size_t idx =
            (detail::g_meta_head + kElidedCastDeoptMetaCap - 1 - n) % kElidedCastDeoptMetaCap;
        if (detail::g_meta_ring[idx].site_key == site_key)
            detail::g_meta_ring[idx] = ElidedCastDeoptMeta{};
    }
}

// Purpose: test/pass helper — invalidate when live type/evidence drifted.
// Returns true on mismatch (caller drops dirty-cone decision cache).
inline bool reverify_elided_cast_deopt_site(std::uint64_t site_key, std::uint32_t live_type_id,
                                            std::uint32_t live_evidence) noexcept {
    auto hit = lookup_elided_cast_deopt_meta(site_key);
    if (!hit)
        return false;
    const bool type_drift = live_type_id != 0 && hit->type_id != 0 && live_type_id != hit->type_id;
    const bool ev_drift =
        live_evidence != 0 && hit->narrow_evidence != 0 && live_evidence != hit->narrow_evidence;
    if (!type_drift && !ev_drift)
        return false;
    invalidate_elided_cast_deopt_meta(site_key);
    return true;
}

// Purpose: walk stamped sites vs live FlatAST type_id / evidence.
// Soft/no-TLS: live_tid==0 → no mismatch (zero extra).
[[nodiscard]] inline bool reverify_elided_cast_deopt_sites() noexcept {
    ElidedCastDeoptMeta snap[kElidedCastDeoptMetaCap];
    std::size_t n = 0;
    {
        std::lock_guard<std::mutex> lock(detail::g_meta_mu);
        n = detail::g_meta_count;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t idx =
                (detail::g_meta_head + kElidedCastDeoptMetaCap - 1 - i) % kElidedCastDeoptMetaCap;
            snap[i] = detail::g_meta_ring[idx];
        }
    }
    bool drift = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (snap[i].site_key == 0 || snap[i].type_id == 0)
            continue;
        const auto node = site_key_ast_node(snap[i].site_key);
        if (node == 0)
            continue;
        const auto live_tid = aura_tls_workspace_type_id(node);
        // Live evidence is unknown without an IR/AST evidence column (0 skip).
        // Type-id drift is the G2 residual; tests pass live evidence explicitly.
        if (reverify_elided_cast_deopt_site(snap[i].site_key, live_tid, /*live_evidence=*/0))
            drift = true;
    }
    return drift;
}

[[nodiscard]] inline std::uint32_t narrow_evidence_for_ast_node(std::uint32_t node) noexcept {
    if (node == 0)
        return 0;
    std::lock_guard<std::mutex> lock(detail::g_meta_mu);
    for (std::size_t n = 0; n < detail::g_meta_count; ++n) {
        const std::size_t idx =
            (detail::g_meta_head + kElidedCastDeoptMetaCap - 1 - n) % kElidedCastDeoptMetaCap;
        if (site_key_ast_node(detail::g_meta_ring[idx].site_key) == node)
            return detail::g_meta_ring[idx].narrow_evidence;
    }
    return 0;
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

namespace aura::compiler::typed_audit {
// Issue #3547: live-or-stamped narrow_evidence for an AST node.
// FlatAST has no evidence column; SSOT is the existing deopt-meta ring
// (plus tests passing an explicit live value into reverify).
[[nodiscard]] inline std::uint32_t current_narrow_evidence(std::uint32_t node) noexcept {
    return ::aura::compiler::dce_deopt::narrow_evidence_for_ast_node(node);
}
} // namespace aura::compiler::typed_audit
