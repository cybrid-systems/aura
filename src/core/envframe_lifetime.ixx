// envframe_lifetime.ixx — Issue #2003 / #2087 / #2164: EnvFrame explicit lifetime
//
// Phase 1 (#2003): RAII `EnvFrameLifetimeGuard` + type-erased host
// (mirrors PanicCheckpointHost from panic_checkpoint_raii.ixx) so this
// core module does NOT depend on aura.compiler.evaluator. The host
// carries a `scan_skip_freed(ctx, site)` callback + a ctx pointer; the
// guard invokes it on dtor (mandatory — no test-only flag).
//
// Phase 2 (#2087): env_id remap table + closures write-lock rewrite.
//
// Phase 3 (#2164): hold-pin — process-wide active guard depth so
// compact_env_frames / Force live_compact defer while a Guard is live
// (not only exit-time scan). Exit scan_skip_freed remains mandatory.
//
// Why explicit:
//   - MutationBoundaryGuard dtor / fiber steal / compact_sweep were each
//     doing their own ad-hoc dance of restamp + scan + enforce. Drift
//     between the 3 sites let a freed EnvFrame slip through as a live
//     GC root or a stale dual-path reference under long-running AI
//     self-evolution + concurrent fiber steal + GC pressure.
//   - Centralizing the protocol in one RAII type forces every site that
//     holds a guard to run the same scan + dual-path check on scope exit.
//   - Site tag lets diagnostics tell which path triggered the scan
//     (BoundaryExit / FiberSteal / CompactSweep).
//   - Hold-pin depth closes the “scan after the fact” window: concurrent
//     compact_env_frames cannot rewrite env_id under a live Guard.

module;

export module aura.core.envframe_lifetime;

import std;

export namespace aura::core::envframe_lifetime {

// Issue #2164: Phase 3 — hold-pin (active registry + compact gate).
// Phase 2 lineage (#2087) retained in schema-2087 query keys.
inline constexpr int kEnvFrameLifetimePhase = 3;

enum class EnvFrameLifetimeSite : std::uint8_t {
    BoundaryExit = 0,
    FiberSteal = 1,
    CompactSweep = 2,
};

// Type-erased host (same pattern as PanicCheckpointHost in
// panic_checkpoint_raii.ixx). The Evaluator-side impl populates this
// struct and passes it to the guard ctor. The guard's dtor invokes
// scan_skip_freed(ctx, site) when both are non-null.
struct EnvFrameLifetimeHost {
    void* ctx = nullptr;
    // Mandatory: scan live closures + skip freed/tombstoned slots +
    // enforce dual-path consistency. Called from guard dtor.
    void (*scan_skip_freed)(void* ctx, EnvFrameLifetimeSite site) = nullptr;
    // Optional: snapshot of live EnvFrame count at ctor (for diff in
    // observability). Returns 0 when unset.
    std::uint64_t (*live_env_frame_count)(void* ctx) = nullptr;
    // Issue #2164 Phase 2b: optional hold-generation snapshot (e.g. defuse
    // or compact generation). When set, dtor compares against ctor stamp.
    std::uint64_t (*hold_generation)(void* ctx) = nullptr;
};

// Process-wide stats struct + global accumulator. Defined BEFORE the
// guard class so its methods can ++g_envframe_lifetime_stats counters
// at construction / destruction time.
struct EnvFrameLifetimeStats {
    std::uint64_t guards_constructed = 0;
    std::uint64_t guards_destructed = 0;
    std::uint64_t scans_run = 0;
    // Issue #2164: compact_env_frames deferred because a Guard is live.
    std::uint64_t blocked_compact_while_guard_held = 0;
    // Per-site construct counts (BoundaryExit / FiberSteal / CompactSweep).
    std::uint64_t site_constructs[3] = {0, 0, 0};
    // Issue #2164 Phase 2b: hold generation advanced under a live Guard
    // (should stay 0 when compact gate is correct).
    std::uint64_t hold_gen_mismatch_total = 0;
    // Issue #2340: post-densify ownership-exit scan runs that
    // iterate live EnvFrameRef set + transfer_to / drop those
    // pointing into densified addresses (after Moving success in
    // live_compact / Phase 5). Distinct from `scans_run` (the
    // Guard's mandatory scan_skip_freed exit scan) — counts
    // explicit per-call-site densify scans, not Guard dtor scans.
    std::uint64_t densify_ownership_scan_total = 0;
};

inline EnvFrameLifetimeStats g_envframe_lifetime_stats{};

// Issue #2157 / #2164: process-wide active EnvFrameLifetimeGuard depth so
// Force live_compact + compact_env_frames can hard-gate without importing
// Evaluator. Ctor +1 / dtor -1 (saturating sub). Query via active_guard_depth().
inline std::atomic<std::uint64_t>& g_envframe_active_guard_depth() noexcept {
    static std::atomic<std::uint64_t> d{0};
    return d;
}

// Monotonic compact generation: advanced when compact_env_frames actually
// reclaims / remaps. Guard stamps this at enter for hold-gen validation.
inline std::atomic<std::uint64_t>& g_envframe_compact_generation() noexcept {
    static std::atomic<std::uint64_t> g{0};
    return g;
}

[[nodiscard]] inline std::uint64_t active_guard_depth() noexcept {
    return g_envframe_active_guard_depth().load(std::memory_order_acquire);
}

[[nodiscard]] inline std::uint64_t compact_generation() noexcept {
    return g_envframe_compact_generation().load(std::memory_order_acquire);
}

inline void note_compact_generation_bump() noexcept {
    g_envframe_compact_generation().fetch_add(1, std::memory_order_acq_rel);
}

// True when compact_env_frames / Force should defer (hold-pin).
[[nodiscard]] inline bool should_block_compact_for_guards() noexcept {
    return active_guard_depth() > 0;
}

// Call when compact_env_frames defers due to live Guard. Returns new total.
inline std::uint64_t note_blocked_compact_while_guard_held() noexcept {
    return ++g_envframe_lifetime_stats.blocked_compact_while_guard_held;
}

inline void reset_envframe_lifetime_stats() noexcept {
    g_envframe_lifetime_stats = {};
    g_envframe_active_guard_depth().store(0, std::memory_order_relaxed);
    // Leave compact_generation monotonic (do not reset) so hold stamps remain
    // comparable across test resets of construct counters only.
}

inline std::uint64_t envframe_lifetime_guards_constructed() noexcept {
    return g_envframe_lifetime_stats.guards_constructed;
}
inline std::uint64_t envframe_lifetime_guards_destructed() noexcept {
    return g_envframe_lifetime_stats.guards_destructed;
}
inline std::uint64_t envframe_lifetime_scans_run() noexcept {
    return g_envframe_lifetime_stats.scans_run;
}
inline std::uint64_t envframe_lifetime_blocked_compact_total() noexcept {
    return g_envframe_lifetime_stats.blocked_compact_while_guard_held;
}
inline std::uint64_t envframe_lifetime_hold_gen_mismatch_total() noexcept {
    return g_envframe_lifetime_stats.hold_gen_mismatch_total;
}
inline std::uint64_t envframe_lifetime_site_constructs(EnvFrameLifetimeSite site) noexcept {
    const auto i = static_cast<std::uint8_t>(site);
    return i < 3 ? g_envframe_lifetime_stats.site_constructs[i] : 0;
}

// Issue #2340: post-densify ownership-exit scan counter accessor.
// Bumped per scan run at the densify success site (after Moving
// densify succeeds in live_compact / Phase 5). The Guard's own
// scan_skip_freed exit scan bumps `scans_run`; this is a distinct
// counter that measures the explicit per-call-site densify scan.
inline std::uint64_t envframe_lifetime_densify_ownership_scan_total() noexcept {
    return g_envframe_lifetime_stats.densify_ownership_scan_total;
}

// Issue #2340: bump helper for the post-densify ownership-exit scan
// counter. Inline so wire-up sites (e.g. evaluator_gc.cpp CompactSweep
// helper + densify success path) can bump without a function call
// boundary; matches the existing site_constructs[] bump pattern.
inline void bump_envframe_lifetime_densify_ownership_scan_total() noexcept {
    ++g_envframe_lifetime_stats.densify_ownership_scan_total;
}

// Build a host from raw function pointers. Use when the wire-up site
// already has the trampoline + ctx handy (e.g. evaluator_gc.cpp).
// Returns an empty host when scan_skip_freed is null.
inline EnvFrameLifetimeHost make_envframe_lifetime_host_with(
    void* ctx, void (*scan_skip_freed)(void* ctx, EnvFrameLifetimeSite site)) noexcept {
    EnvFrameLifetimeHost h{};
    h.ctx = ctx;
    h.scan_skip_freed = scan_skip_freed;
    // Default hold-generation: process compact generation (no Evaluator dep).
    h.hold_generation = [](void*) noexcept -> std::uint64_t { return compact_generation(); };
    return h;
}

// EnvFrameLifetimeGuard — RAII hold-pin + exit scan.
// Ctor: +1 active depth, site counter, optional hold-gen stamp.
// Dtor: -1 depth, then mandatory scan_skip_freed, then hold-gen check.
class EnvFrameLifetimeGuard {
public:
    EnvFrameLifetimeGuard(EnvFrameLifetimeHost host, EnvFrameLifetimeSite site) noexcept
        : host_(host)
        , site_(site) {
        ++g_envframe_lifetime_stats.guards_constructed;
        const auto si = static_cast<std::uint8_t>(site_);
        if (si < 3)
            ++g_envframe_lifetime_stats.site_constructs[si];
        // Issue #2157 / #2164: publish hold so compact / Force can hard-mutex.
        g_envframe_active_guard_depth().fetch_add(1, std::memory_order_acq_rel);
        // Issue #2164 Phase 2b: stamp hold generation at enter.
        if (host_.hold_generation)
            hold_gen_at_enter_ = host_.hold_generation(host_.ctx);
        else
            hold_gen_at_enter_ = compact_generation();
    }
    ~EnvFrameLifetimeGuard() noexcept {
        // Drop depth before scan so concurrent Force/compact can proceed after hold.
        auto cur = g_envframe_active_guard_depth().load(std::memory_order_relaxed);
        for (;;) {
            const auto next = cur > 0 ? cur - 1 : 0;
            if (g_envframe_active_guard_depth().compare_exchange_weak(
                    cur, next, std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
        ++g_envframe_lifetime_stats.guards_destructed;
        // Mandatory exit scan (do not remove — #2003 / #2164 safety net).
        if (host_.scan_skip_freed && host_.ctx) {
            host_.scan_skip_freed(host_.ctx, site_);
            ++g_envframe_lifetime_stats.scans_run;
        }
        // Issue #2164 Phase 2b: fail-closed observability if compact gen advanced
        // under the hold (should not happen when compact gate is correct).
        const std::uint64_t now =
            host_.hold_generation ? host_.hold_generation(host_.ctx) : compact_generation();
        if (now != hold_gen_at_enter_)
            ++g_envframe_lifetime_stats.hold_gen_mismatch_total;
    }

    EnvFrameLifetimeGuard(const EnvFrameLifetimeGuard&) = delete;
    EnvFrameLifetimeGuard& operator=(const EnvFrameLifetimeGuard&) = delete;
    EnvFrameLifetimeGuard(EnvFrameLifetimeGuard&&) = delete;
    EnvFrameLifetimeGuard& operator=(EnvFrameLifetimeGuard&&) = delete;

    [[nodiscard]] EnvFrameLifetimeSite site() const noexcept { return site_; }
    [[nodiscard]] void* ctx() const noexcept { return host_.ctx; }
    [[nodiscard]] bool armed() const noexcept { return host_.scan_skip_freed && host_.ctx; }
    [[nodiscard]] std::uint64_t hold_gen_at_enter() const noexcept { return hold_gen_at_enter_; }

private:
    EnvFrameLifetimeHost host_;
    EnvFrameLifetimeSite site_;
    std::uint64_t hold_gen_at_enter_ = 0;
};

} // namespace aura::core::envframe_lifetime
