// cpp26_contract_stats.h — Issue #742 / #2142 / #2435 / #3043: runtime
// observability for C++26 Contracts + consteval hot-path invariants
// (zero release cost by default).
//
// Plain header (not a module) so contract_handler.cpp, value_tags.h,
// arena.ixx, and pass_manager can all bump counters without crossing
// module boundaries.
//
// ── Issue #2142 / #2435: unified hot-path contract policy ────────────────
//
// Tier classification (Issue #2435):
//
//   **Hot** — absolute hottest loops (per-instr as_*, IRInstructionView
//   column access, view_at, eval_flat apply, mark_block_dirty cascade,
//   walk_soa hotpath). Use only AURA_HOT_* macros. Production default:
//   **off** (no atomic record, no assert) so 1e6-instr eval stays ≤1%
//   overhead vs fully disabled contracts.
//
//   **Cold** — mutation boundaries, pass pipeline entry (`pre`/`post`),
//   compact/remap, fiber resume fences, public API edges. Keep full
//   C++26 `pre`/`post`/`contract_assert` (language contracts) or
//   AURA_COLD_CONTRACT. These are not on the absolute hot loop.
//
// Single observe-first API for value/arena/ir_soa tight loops:
//
//   AURA_HOT_RECORD()           — bump hotpath_invariant_hits_total
//   AURA_HOT_CHECK(expr)        — enforce / observe / no-op per hot mode
//   AURA_HOT_CONTRACT(expr)     — RECORD + CHECK (preferred one-liner)
//   AURA_COLD_CONTRACT(expr)    — cold-edge enforce (debug/enforce only)
//   AURA_HOT_CHECK_CONSTEXPR    — constexpr-friendly column bounds (hot)
//
// Three tiers (Issue #2435 AC1 + #3043 Soft-observe):
//   * Enforce — -DAURA_CONTRACTS_HOT_MODE_ENFORCE / -DAURA_CONTRACTS_ENFORCE
//       or debug (!NDEBUG) without OFF/OBSERVE override:
//         RECORD (every call) + CHECK → contract_assert  (fail-closed)
//   * Soft-observe — -DAURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE /
//       -DAURA_HOT_SOFT_OBSERVE / -DAURA_CONTRACTS_HOT_MODE_OBSERVE /
//       -DAURA_CONTRACTS_OBSERVE:
//         sampled RECORD + CHECK → observe_hot_contract_false() only
//         (metrics, no abort). Sample period avoids per-call atomic RMW.
//   * Off — -DAURA_CONTRACTS_HOT_MODE_OFF or production (NDEBUG) default:
//         RECORD + CHECK → no-op  (zero cost on happy path)
//
// Env AURA_CONTRACTS_HOT_MODE=soft|observe|off|enforce is Agent-visible
// intent on query:cpp26-contracts-stats (hot-contracts-mode-env). It does
// not change compile-time OFF macros, so production default stays zero-cost.
//
// Do NOT scatter bare contract_assert on new absolute-hot accessors —
// use AURA_HOT_CHECK / AURA_HOT_CHECK_CONSTEXPR so interpreter/JIT walks
// pay nothing under production OFF. Cold edges keep language pre/post.
//
#ifndef AURA_CORE_CPP26_CONTRACT_STATS_H
#define AURA_CORE_CPP26_CONTRACT_STATS_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// ── Hot mode preprocessor selection (Issue #2435) ───────────────────────
// Priority: explicit HOT_MODE_* > ENFORCE/OBSERVE legacy flags > NDEBUG.
#if defined(AURA_CONTRACTS_HOT_MODE_ENFORCE) || defined(AURA_CONTRACTS_ENFORCE)
#define AURA_HOT_MODE_ENFORCE 1
#elif defined(AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE) || defined(AURA_HOT_SOFT_OBSERVE) ||           \
    defined(AURA_CONTRACTS_HOT_MODE_OBSERVE) || defined(AURA_CONTRACTS_OBSERVE)
// Issue #3043: Soft-observe is the production-optional metrics-only tier.
// Legacy OBSERVE flags alias the same mode.
#define AURA_HOT_MODE_OBSERVE 1
#define AURA_HOT_MODE_SOFT_OBSERVE 1
#elif defined(AURA_CONTRACTS_HOT_MODE_OFF)
#define AURA_HOT_MODE_OFF 1
#elif !defined(NDEBUG)
// Debug default: enforce (fail-closed) — matches pre-#2435 developer UX.
#define AURA_HOT_MODE_ENFORCE 1
#else
// Issue #2435 AC1 / #3043 AC1: production (NDEBUG) default — hot contracts OFF.
#define AURA_HOT_MODE_OFF 1
#endif

// contract_assert for enforce/observe builds (off path never needs it).
#if defined(AURA_HOT_MODE_ENFORCE) || defined(AURA_HOT_MODE_OBSERVE)
#include <contracts>
#endif

namespace aura::core::cpp26 {

inline constexpr int kHotContractUnifyIssue = 2142;
// Issue #2435: hot vs cold tier policy + production OFF default.
inline constexpr int kHotContractPlacementIssue = 2435;
// Issue #3043: Soft-observe tier (metrics, no abort) + sampled RECORD.
inline constexpr int kHotContractSoftObserveIssue = 3043;
// Soft-observe RECORD sample period (power of two). Acceptable upper
// bound vs OFF: one relaxed atomic per this many RECORD sites.
inline constexpr std::uint32_t kHotSoftObserveRecordSample = 256;

// Hot-mode enum for query surface (0=off, 1=observe, 2=enforce).
inline constexpr int kHotModeOff = 0;
inline constexpr int kHotModeObserve = 1;
inline constexpr int kHotModeEnforce = 2;

#if defined(AURA_HOT_MODE_ENFORCE)
inline constexpr int kHotContractsMode = kHotModeEnforce;
#elif defined(AURA_HOT_MODE_OBSERVE)
inline constexpr int kHotContractsMode = kHotModeObserve;
#else
inline constexpr int kHotContractsMode = kHotModeOff;
#endif

// Runtime contract violations caught by handle_contract_violation
// (enforce/observe semantic). Stats-only; relaxed ordering.
inline std::atomic<std::uint64_t> contract_violations_caught_total{0};

// Hot-path invariant probes (Arena alloc, Value classify, SoA view,
// Shape inline, Pass dirty-skip). Zero cost when hot mode is OFF.
inline std::atomic<std::uint64_t> hotpath_invariant_hits_total{0};

// Compile-time consteval/static_assert count baked into the binary.
// Bump when cxx26_invariants.ixx / value_tags.h / shape.h grow.
// Issue #1321: expanded to 36 (+4 dirty/tag/arena packing asserts).
// Issue #1466: bumped to 53 (+17 hot-path consteval invariants:
// EvalValueTag enum x9 + ShapeID boundary x4 + IR SoA breakdown x3 +
// tagged bit layout x1).
// Issue #1519: bumped to 65 (+12 SIMD/cache/dirty/shape/freelist asserts).
// Issue #1620: bumped to 77 (+12 Arena max/FlatAST dirty/NodeTag/Value
// Special encodings/SoAView phase consteval invariants).
inline constexpr std::int64_t kConstevalChecksTotal = 77;
// Approximate Contract pre/post/assert density across Arena + Value +
// Shape + dirty hot paths (manual inventory; Agents detect drift).
// Issue #1519: raised from 26 → 48 after hot-path Contract deepening.
// Issue #1620: raised 48 → 56 (FlatAST get/type_id + mark_dirty +
// shape bit-test + arena tier overflow path).
// Issue #2142: unified AURA_HOT_CONTRACT surface (value/arena/ir_soa).
// Issue #2435: placement policy (hot off in production); count unchanged.
inline constexpr std::int64_t kContractHotPathsShipped = 62;

// Issue #1321 Phase 1: coverage flags — hot accessors that gained contracts.
inline std::atomic<std::uint64_t> hotpath_contracts_expanded_active{1};
inline std::atomic<std::uint64_t> soa_view_bounds_contracts_active{1};
inline std::atomic<std::uint64_t> flatast_column_contracts_active{1};

// Issue #1466 Phase 1: new coverage flags — hot-path contract placement.
inline std::atomic<std::uint64_t> shape_inline_post_contracts_active{1};
inline std::atomic<std::uint64_t> arena_compact_contracts_active{1};
inline std::atomic<std::uint64_t> dirty_cascade_contracts_active{1};
// Issue #1519: deeper hot-path Contracts coverage flag + violation surface.
inline std::atomic<std::uint64_t> hotpath_contracts_1519_active{1};
// Issue #1620: Arena/Value/Shape/FlatAST hot-path Contracts expand flag.
inline std::atomic<std::uint64_t> hotpath_contracts_1620_active{1};
// Issue #2142: unified AURA_HOT_CONTRACT helper wired on primary hot paths.
inline std::atomic<std::uint64_t> hotpath_contracts_2142_active{1};
inline std::atomic<std::uint64_t> aura_hot_contract_wired{1};
// Issue #2435: hot/cold placement policy + production OFF wired.
inline std::atomic<std::uint64_t> hotpath_contracts_2435_active{1};
inline std::atomic<std::uint64_t> hot_contract_placement_wired{1};
inline std::atomic<std::uint64_t> hot_contracts_production_off_default{1};
// Issue #3043: Soft-observe wired (compile-time optional; default OFF).
inline std::atomic<std::uint64_t> hot_contract_soft_observe_wired{1};
inline std::atomic<std::uint64_t> hotpath_contracts_3043_active{1};
inline std::atomic<std::uint64_t> arena_tier_contracts_active{1};
inline std::atomic<std::uint64_t> value_as_star_contracts_active{1};
inline std::atomic<std::uint64_t> shape_bit_test_contracts_active{1};
inline std::atomic<std::uint64_t> flatast_get_type_contracts_active{1};
inline std::atomic<std::uint64_t> contract_violation_hotpath_count{0};
// Issue #1466: hot-path consteval invariant hits — bumped each time a
// new consteval invariant is added. Mirrors kConstevalChecksTotal but
// observable at runtime via (query:cpp26-contracts-stats).
inline std::atomic<std::uint64_t> consteval_invariants_total{77};

inline void record_contract_violation_caught() noexcept {
    contract_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_hotpath_invariant_hit() noexcept {
    hotpath_invariant_hits_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1519: hot-path contract violation (debug observe path / Agent signal).
inline void record_contract_violation_hotpath() noexcept {
    contract_violation_hotpath_count.fetch_add(1, std::memory_order_relaxed);
    record_contract_violation_caught();
}

// Issue #1466: bump the consteval invariant count when new invariants
// are added in cxx26_invariants.ixx. Called from the consteval
// self-check initialization (not from hot path — one-shot at boot).
inline void record_consteval_invariant_added() noexcept {
    consteval_invariants_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2142 / #3043: Soft-observe CHECK failure (metrics only, no abort).
inline void observe_hot_contract_false() noexcept {
    record_contract_violation_hotpath();
}

// Soft-observe RECORD: thread-local sample, not a per-call atomic RMW.
inline void record_hotpath_invariant_hit_sampled() noexcept {
    static thread_local std::uint32_t n = 0;
    if ((++n & (kHotSoftObserveRecordSample - 1u)) == 0)
        record_hotpath_invariant_hit();
}

[[nodiscard]] inline int current_hot_contracts_mode() noexcept {
    return kHotContractsMode;
}

// Cold-path env peek (query / startup). Does not change OFF macros.
[[nodiscard]] inline int peek_hot_contracts_mode_env() noexcept {
    static std::atomic<int> cached{-1};
    int v = cached.load(std::memory_order_relaxed);
    if (v >= 0)
        return v;
    int parsed = kHotModeOff;
    if (const char* e = std::getenv("AURA_CONTRACTS_HOT_MODE")) {
        if (std::strcmp(e, "soft") == 0 || std::strcmp(e, "observe") == 0 ||
            std::strcmp(e, "SOFT") == 0 || std::strcmp(e, "OBSERVE") == 0)
            parsed = kHotModeObserve;
        else if (std::strcmp(e, "enforce") == 0 || std::strcmp(e, "ENFORCE") == 0)
            parsed = kHotModeEnforce;
        else
            parsed = kHotModeOff;
    }
    int expected = -1;
    cached.compare_exchange_strong(expected, parsed, std::memory_order_relaxed);
    return cached.load(std::memory_order_relaxed);
}

} // namespace aura::core::cpp26

// ── Issue #2142 / #2435: AURA_HOT_* macros (see file header policy) ─────

// Hot RECORD — elided under production OFF (#2435). Soft-observe (#3043)
// samples so the happy path is not a per-call atomic RMW.
#if defined(AURA_HOT_MODE_OFF)
#define AURA_HOT_RECORD() ((void)0)
#elif defined(AURA_HOT_MODE_SOFT_OBSERVE) || defined(AURA_HOT_MODE_OBSERVE)
#define AURA_HOT_RECORD() ::aura::core::cpp26::record_hotpath_invariant_hit_sampled()
#else
#define AURA_HOT_RECORD() ::aura::core::cpp26::record_hotpath_invariant_hit()
#endif

// Hot CHECK — enforce / observe / ignore per hot mode.
#if defined(AURA_HOT_MODE_ENFORCE)
#define AURA_HOT_CHECK(expr) contract_assert(expr)
#elif defined(AURA_HOT_MODE_OBSERVE)
#define AURA_HOT_CHECK(expr)                                                                       \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::aura::core::cpp26::observe_hot_contract_false();                                     \
    } while (0)
#else
// Production OFF: zero cost.
#define AURA_HOT_CHECK(expr) ((void)0)
#endif

// Preferred one-liner: record + check (both respect hot mode).
#define AURA_HOT_CONTRACT(expr)                                                                    \
    do {                                                                                           \
        AURA_HOT_RECORD();                                                                         \
        AURA_HOT_CHECK(expr);                                                                      \
    } while (0)

// Issue #2435: cold-edge contract — mutation / pass / compact style edges.
// Enforce in debug + explicit ENFORCE; no-op in production OFF (language
// pre/post on those edges remain the primary cold gate).
#if defined(AURA_HOT_MODE_ENFORCE)
#define AURA_COLD_CONTRACT(expr) contract_assert(expr)
#else
#define AURA_COLD_CONTRACT(expr) ((void)0)
#endif

// Constexpr-friendly hot bounds check for IRInstructionView column access.
// Production OFF / observe: no check (observe not constexpr-friendly).
// Debug/enforce: contract_assert (constexpr-OK with contracts).
#if defined(AURA_HOT_MODE_ENFORCE)
#define AURA_HOT_CHECK_CONSTEXPR(expr) contract_assert(expr)
#else
#define AURA_HOT_CHECK_CONSTEXPR(expr) ((void)0)
#endif

#endif // AURA_CORE_CPP26_CONTRACT_STATS_H
