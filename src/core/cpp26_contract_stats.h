// cpp26_contract_stats.h — Issue #742 / #2142 / #2435 / #3043: runtime
// observability for C++26 Contracts + consteval hot-path invariants
// (zero release cost by default).
//
// Plain header (not a module) so contract_handler.cpp, value_tags.h,
// arena.ixx, and pass_manager can all bump counters without crossing
// module boundaries.

// Issue #3139 / #3313: production_defaults via C ABI so this header does
// not pull typed_mutation_audit.h (and its incomplete Evaluator) into
// every hot TU (value/arena/ast/ir_soa). Strong def in
// typed_mutation_audit_hooks.cpp; weak stub in light-link.
extern "C" int aura_production_defaults_active_probe() noexcept;
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
//   AURA_HOT_CONTRACT(expr)     — RECORD + CHECK (preferred one-liner;
//                                 NDEBUG OFF: one armed() load, #3501)
//   AURA_COLD_CONTRACT(expr)    — cold-edge enforce (debug/enforce only)
//   AURA_HOT_CHECK_CONSTEXPR    — constexpr-friendly column bounds (hot)
//
// Three tiers (Issue #2435 AC1 + #3043 Soft-observe + #3106 Harden-armed):
//   * Enforce — -DAURA_CONTRACTS_HOT_MODE_ENFORCE / -DAURA_CONTRACTS_ENFORCE
//       or debug (!NDEBUG) without OFF/OBSERVE override:
//         RECORD (every call) + CHECK → contract_assert  (fail-closed)
//   * Soft-observe — -DAURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE /
//       -DAURA_HOT_SOFT_OBSERVE / -DAURA_CONTRACTS_HOT_MODE_OBSERVE /
//       -DAURA_CONTRACTS_OBSERVE:
//         sampled RECORD + CHECK → observe_hot_contract_false() only
//         (metrics, no abort). Sample period avoids per-call atomic RMW.
//   * Soft-observe + Harden — -DAURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE_HARDEN /
//       -DAURA_HOT_SOFT_OBSERVE_HARDEN (Issue #3106, AC1-AC3):
//         sampled RECORD + CHECK on false → observe_hot_contract_false()
//         AND record_hotpath_contract_harden_trap() AND std::abort()
//         (fail-closed trap). Sample period still applies (AC3); happy
//         path remains a branch + sampled atomic (no per-call RMW, AC2).
//   * Off — -DAURA_CONTRACTS_HOT_MODE_OFF or production (NDEBUG) default:
//         compile-time OFF (Soft / unit / AURA_SANDBOX=off: armed()==0,
//         expr not evaluated). Issue #3313: when
//         production_defaults_active() arms, the same binary runs
//         Soft-observe + Harden (sampled RECORD + fail-closed abort)
//         without -DAURA_HOT_SOFT_OBSERVE_HARDEN.
//
// Env AURA_CONTRACTS_HOT_MODE=soft|observe|off|enforce is Agent-visible
// intent on query:cpp26-contracts-stats (hot-contracts-mode-env). It does
// not change compile-time OFF macros, so production default stays zero-cost.
// Env AURA_HOT_HARDEN=1|on|true is a runtime armed-state probe (Issue
// #3106 AC4); compile-time HARDEN flag overrides env for the actual trap
// dispatch (macro expansion is compile-time).
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

// ── Hot mode preprocessor selection (Issue #2435 / #3106) ──────────────
// Priority: ENFORCE > HARDEN-armed SOFT_OBSERVE (#3106) > plain SOFT_OBSERVE
// (#3043) > legacy OBSERVE aliases > explicit OFF > NDEBUG-default Enforce
// (debug) > NDEBUG-default Off (production).
#if defined(AURA_CONTRACTS_HOT_MODE_ENFORCE) || defined(AURA_CONTRACTS_ENFORCE)
#define AURA_HOT_MODE_ENFORCE 1
#elif defined(AURA_CONTRACTS_HOT_MODE_SOFT_OBSERVE_HARDEN) || defined(AURA_HOT_SOFT_OBSERVE_HARDEN)
// Issue #3106: harden-armed soft-observe. Keeps Soft-observe sampled
// RECORD path for metrics, turns a false CHECK into a fail-closed trap
// (observe_hot_contract_false + record_hotpath_contract_harden_trap +
// std::abort). Implies SOFT_OBSERVE on so RECORD stays sampled.
#define AURA_HOT_MODE_SOFT_OBSERVE 1
#define AURA_HOT_MODE_HARDEN 1
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
// Issue #3106: Soft-observe + Harden-armed tier (sampled RECORD + fail-
// closed trap on false CHECK). Closes the Soft-only window of #3043 under
// AI multi-round mutate (Value as_*, SoA view_at, dirty mark/cascade).
inline constexpr int kHotContractHardenIssue = 3106;
// Issue #3313: production_defaults arms Soft-observe+Harden for the
// NDEBUG OFF expansion (I1 residual of #2435/#3043/#3106).
inline constexpr int kHotContractProductionHardenIssue = 3313;
// Issue #3490: cache the armed observation so as_int / view_at do not
// re-enter the C ABI probe on every HOT_CHECK. apply_production /
// apply_dev store here (defaults can flip in tests).
inline constexpr int kHotContractHardenCacheIssue = 3490;
// Issue #3501: NDEBUG OFF AURA_HOT_CONTRACT loads armed() once (RECORD
// then CHECK was two relaxed loads on as_int / view_at).
inline constexpr int kHotContractSingleLoadIssue = 3501;
// Soft-observe RECORD sample period (power of two). Acceptable upper
// bound vs OFF: one relaxed atomic per this many RECORD sites. Applies to
// both plain Soft-observe (#3043) and Harden-armed Soft-observe (#3106
// AC3 — sample period still works under harden).
inline constexpr std::uint32_t kHotSoftObserveRecordSample = 256;

// Hot-mode enum for query surface (0=off, 1=observe, 2=enforce,
// 3=harden-armed soft-observe, #3106).
inline constexpr int kHotModeOff = 0;
inline constexpr int kHotModeObserve = 1;
inline constexpr int kHotModeEnforce = 2;
inline constexpr int kHotModeSoftenObserveHarden = 3;

#if defined(AURA_HOT_MODE_ENFORCE)
inline constexpr int kHotContractsMode = kHotModeEnforce;
#elif defined(AURA_HOT_MODE_HARDEN)
inline constexpr int kHotContractsMode = kHotModeSoftenObserveHarden;
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
// Issue #3106: harden-armed soft-observe wired (compile-time optional;
// default OFF). Soft observe counter increments AND harden-trap counter
// increments AND std::abort fires on a false HOT_CHECK under harden.
inline std::atomic<std::uint64_t> hot_contract_harden_wired{1};
inline std::atomic<std::uint64_t> hotpath_contracts_3106_active{1};
inline std::atomic<std::uint64_t> hotpath_contract_harden_trap_total{0};
inline std::atomic<std::uint64_t> hotpath_contracts_3313_active{1};
// Issue #3490: -1 unknown, 0 disarmed, 1 armed. Relaxed load on the
// hot CHECK; store from apply_production / apply_dev / first miss.
inline std::atomic<int> hot_contract_harden_armed_cache{-1};

inline void note_hot_contract_harden_armed(bool armed) noexcept {
    hot_contract_harden_armed_cache.store(armed ? 1 : 0, std::memory_order_relaxed);
}
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

// Issue #3106: harden-armed CHECK false path — observe counter bump plus
// a dedicated harden-trap counter bump (so Agents can see how often the
// fail-closed trap would have fired without crashing the process when
// testing under a non-fatal harness). The actual std::abort() is emitted
// inline by the AURA_HOT_CHECK macro under AURA_HOT_MODE_HARDEN so the
// trap is inlined at the call site (no extra layer on the absolute-hot
// path).
inline void record_hotpath_contract_harden_trap() noexcept {
    hotpath_contract_harden_trap_total.fetch_add(1, std::memory_order_relaxed);
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

// Issue #3106 AC4: hot-contract-harden-armed probe. Compile-time true
// when AURA_HOT_MODE_HARDEN is set (the macro emits the trap inline).
// Otherwise reads the env AURA_HOT_HARDEN at first call and caches the
// result (relaxed atomic, no per-call syscall). Env values "1", "on",
// "true" (case-insensitive) arm; anything else leaves it disarmed. The
// probe is observable via query:cpp26-contracts-stats so production-soak
// / agent-self-modify gates can assert harden is armed under their
// preset without re-deriving the compile flag.
[[nodiscard]] inline bool hot_contract_harden_armed() noexcept {
#if defined(AURA_HOT_MODE_HARDEN)
    return true;
#else
    // Issue #3490: load cached armed state first. After the first
    // observation (or apply_production / apply_dev store) as_int /
    // view_at never re-enter the C ABI production-defaults probe.
    int v = hot_contract_harden_armed_cache.load(std::memory_order_relaxed);
    if (v >= 0)
        return v != 0;
    int parsed = 0;
    if (const char* e = std::getenv("AURA_HOT_HARDEN")) {
        if (std::strcmp(e, "1") == 0 || std::strcmp(e, "on") == 0 || std::strcmp(e, "true") == 0 ||
            std::strcmp(e, "ON") == 0 || std::strcmp(e, "TRUE") == 0 || std::strcmp(e, "On") == 0 ||
            std::strcmp(e, "True") == 0)
            parsed = 1;
    }
    // Issue #3139 AC4 / #3313: under production_defaults_active(), the
    // runtime probe is implicitly armed for self-modify preset binaries
    // (no env required). Gated by parsed==0 so env ON is not re-probed.
    // Soft / unit / sandbox=off keep production_defaults_active() == 0
    // and stay disarmed (AC2 — expr not evaluated).
    if (parsed == 0 && aura_production_defaults_active_probe() != 0) {
        parsed = 1;
    }
    int expected = -1;
    hot_contract_harden_armed_cache.compare_exchange_strong(expected, parsed,
                                                            std::memory_order_relaxed);
    return hot_contract_harden_armed_cache.load(std::memory_order_relaxed) != 0;
#endif
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
// Issue #3313: Soft/unit armed()==0 → no-op (expr/RECORD skipped). Production
// defaults arm Soft-observe+Harden sampled RECORD.
#define AURA_HOT_RECORD()                                                                          \
    do {                                                                                           \
        if (::aura::core::cpp26::hot_contract_harden_armed())                                      \
            ::aura::core::cpp26::record_hotpath_invariant_hit_sampled();                           \
    } while (0)
#elif defined(AURA_HOT_MODE_SOFT_OBSERVE) || defined(AURA_HOT_MODE_OBSERVE)
#define AURA_HOT_RECORD() ::aura::core::cpp26::record_hotpath_invariant_hit_sampled()
#else
#define AURA_HOT_RECORD() ::aura::core::cpp26::record_hotpath_invariant_hit()
#endif

// Hot CHECK — enforce / harden-armed / observe / ignore per hot mode.
// Issue #3106: when AURA_HOT_MODE_HARDEN is set, a false CHECK bumps
// the Soft observe counter (so the metrics story is preserved under the
// new fail-closed trap), bumps the harden-trap counter (so Agents can
// see how often the trap would fire), then std::abort()s. Happy path is
// the same branch + sampled RECORD as plain Soft-observe (no extra
// atomic RMW per call).
#if defined(AURA_HOT_MODE_ENFORCE)
#define AURA_HOT_CHECK(expr) contract_assert(expr)
#elif defined(AURA_HOT_MODE_HARDEN)
#define AURA_HOT_CHECK(expr)                                                                       \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            ::aura::core::cpp26::observe_hot_contract_false();                                     \
            ::aura::core::cpp26::record_hotpath_contract_harden_trap();                            \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
#elif defined(AURA_HOT_MODE_OBSERVE)
#define AURA_HOT_CHECK(expr)                                                                       \
    do {                                                                                           \
        if (!(expr))                                                                               \
            ::aura::core::cpp26::observe_hot_contract_false();                                     \
    } while (0)
#else
// Production OFF: zero cost when !hot_contract_harden_armed() (Soft/unit;
// expr not evaluated). Issue #3313: production_defaults arms
// Soft-observe+Harden (observe + trap + abort) without a compile flag.
#define AURA_HOT_CHECK(expr)                                                                       \
    do {                                                                                           \
        if (::aura::core::cpp26::hot_contract_harden_armed()) {                                    \
            if (!(expr)) {                                                                         \
                ::aura::core::cpp26::observe_hot_contract_false();                                 \
                ::aura::core::cpp26::record_hotpath_contract_harden_trap();                        \
                std::abort();                                                                      \
            }                                                                                      \
        }                                                                                          \
    } while (0)
#endif

// Preferred one-liner: record + check (both respect hot mode).
// Issue #3501: NDEBUG OFF loads hot_contract_harden_armed() once.
// Armed: sampled RECORD + CHECK + trap. Unarmed: one load, no expr.
#if defined(AURA_HOT_MODE_OFF)
#define AURA_HOT_CONTRACT(expr)                                                                    \
    do {                                                                                           \
        if (::aura::core::cpp26::hot_contract_harden_armed()) {                                    \
            ::aura::core::cpp26::record_hotpath_invariant_hit_sampled();                           \
            if (!(expr)) {                                                                         \
                ::aura::core::cpp26::observe_hot_contract_false();                                 \
                ::aura::core::cpp26::record_hotpath_contract_harden_trap();                        \
                std::abort();                                                                      \
            }                                                                                      \
        }                                                                                          \
    } while (0)
#else
#define AURA_HOT_CONTRACT(expr)                                                                    \
    do {                                                                                           \
        AURA_HOT_RECORD();                                                                         \
        AURA_HOT_CHECK(expr);                                                                      \
    } while (0)
#endif

// Issue #2435: cold-edge contract — mutation / pass / compact style edges.
// Enforce in debug + explicit ENFORCE; no-op in production OFF (language
// pre/post on those edges remain the primary cold gate).
#if defined(AURA_HOT_MODE_ENFORCE)
#define AURA_COLD_CONTRACT(expr) contract_assert(expr)
#else
#define AURA_COLD_CONTRACT(expr) ((void)0)
#endif

// Constexpr-friendly hot bounds check for IRInstructionView column access.
// Debug/enforce: contract_assert (constexpr-OK with contracts).
// Issue #3490: runtime evaluation honors the same Harden arm as view_at
// (AURA_HOT_CHECK). Constant evaluation keeps the no-op (atomics/abort
// are not constexpr). Unarmed NDEBUG: armed() load then skip expr (AC5).
#if defined(AURA_HOT_MODE_ENFORCE)
#define AURA_HOT_CHECK_CONSTEXPR(expr) contract_assert(expr)
#else
#define AURA_HOT_CHECK_CONSTEXPR(expr)                                                             \
    do {                                                                                           \
        if !consteval {                                                                            \
            AURA_HOT_CHECK(expr);                                                                  \
        }                                                                                          \
    } while (0)
#endif

#endif // AURA_CORE_CPP26_CONTRACT_STATS_H
