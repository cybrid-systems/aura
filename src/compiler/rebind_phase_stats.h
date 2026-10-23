// Issue #4680: opt-in per-phase timing for the rebind -> eval-current loop.
// AURA_PHASE_STATS=1 enables; off costs one predictable branch per scope.
// Times are inclusive (nested phases are also counted in their parent).
// Read via (engine:metrics "query:incremental-relower-stats") keys
// "phase-<name>-ns" / "phase-<name>-count".
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace aura::compiler::phase_stats {

enum Phase : unsigned {
    RebindPrim,   // whole mutate:rebind primitive
    Parse,        // parse_to_flat of the new body
    Typecheck,    // run_post_mutate_typecheck_no_lock
    Cascade,      // push_post_mutate_incremental_cascade
    CascadeIndex, // SymId -> Define index used by the cascade
    Drain,        // drain_cascade_bfs_invalidate
    InvalidateFn, // CompilerService::invalidate_function (hard)
    Restamp,      // dependent restamp loop in drain
    MarkDirty,    // CompilerService::mark_define_dirty
    PreAudit,     // prepare_unified_invalidation_pre_cascade_
    Relower,      // relower_dirty_defines_from_workspace
    Passes,       // run_incremental_dirty_pass_suite_
    EvalCurrent,  // whole eval-current primitive
    kCount
};

inline constexpr const char* kNames[kCount] = {
    "rebind-prim", "parse",         "typecheck",   "cascade",    "cascade-index",
    "drain",       "invalidate-fn", "restamp",     "mark-dirty", "pre-audit",
    "relower",     "passes",        "eval-current"};

inline std::atomic<std::uint64_t> g_ns[kCount]{};
inline std::atomic<std::uint64_t> g_count[kCount]{};

inline bool enabled() noexcept {
    static const bool on = [] {
        const char* e = std::getenv("AURA_PHASE_STATS");
        return e && e[0] == '1';
    }();
    return on;
}

struct Scope {
    Phase p;
    std::chrono::steady_clock::time_point t0{};
    bool on;
    explicit Scope(Phase ph) noexcept
        : p(ph)
        , on(enabled()) {
        if (on) [[unlikely]]
            t0 = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (on) [[unlikely]] {
            const auto d = std::chrono::steady_clock::now() - t0;
            g_ns[p].fetch_add(static_cast<std::uint64_t>(
                                  std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()),
                              std::memory_order_relaxed);
            g_count[p].fetch_add(1, std::memory_order_relaxed);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace aura::compiler::phase_stats
