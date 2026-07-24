// envframe_lifetime.ixx — Issue #2003: EnvFrame explicit lifetime protocol
//
// Phase 1 (this module): RAII `EnvFrameLifetimeGuard` + type-erased host
// (mirrors PanicCheckpointHost from panic_checkpoint_raii.ixx) so this
// core module does NOT depend on aura.compiler.evaluator. The host
// carries a `scan_skip_freed(ctx, site)` callback + a ctx pointer; the
// guard invokes it on dtor (mandatory — no test-only flag).
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

module;

export module aura.core.envframe_lifetime;

import std;

export namespace aura::core::envframe_lifetime {

inline constexpr int kEnvFrameLifetimePhase = 1;

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
};

// Process-wide stats struct + global accumulator. Defined BEFORE the
// guard class so its methods can ++g_envframe_lifetime_stats counters
// at construction / destruction time.
struct EnvFrameLifetimeStats {
    std::uint64_t guards_constructed = 0;
    std::uint64_t guards_destructed = 0;
    std::uint64_t scans_run = 0;
};

inline EnvFrameLifetimeStats g_envframe_lifetime_stats{};

inline void reset_envframe_lifetime_stats() noexcept {
    g_envframe_lifetime_stats = {};
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

// Build a host from raw function pointers. Use when the wire-up site
// already has the trampoline + ctx handy (e.g. evaluator_gc.cpp).
// Returns an empty host when scan_skip_freed is null.
inline EnvFrameLifetimeHost make_envframe_lifetime_host_with(
    void* ctx, void (*scan_skip_freed)(void* ctx, EnvFrameLifetimeSite site)) noexcept {
    EnvFrameLifetimeHost h{};
    h.ctx = ctx;
    h.scan_skip_freed = scan_skip_freed;
    return h;
}

// EnvFrameLifetimeGuard — RAII wrapper. On dtor, calls
// host.scan_skip_freed(host.ctx, site_) when both are non-null.
// Construction is noexcept + O(1) (just copies the host struct).
// Destruction runs the scan + dual-path consistency check.
class EnvFrameLifetimeGuard {
public:
    EnvFrameLifetimeGuard(EnvFrameLifetimeHost host, EnvFrameLifetimeSite site) noexcept
        : host_(host)
        , site_(site) {
        ++g_envframe_lifetime_stats.guards_constructed;
    }
    ~EnvFrameLifetimeGuard() noexcept {
        ++g_envframe_lifetime_stats.guards_destructed;
        if (host_.scan_skip_freed && host_.ctx) {
            host_.scan_skip_freed(host_.ctx, site_);
            ++g_envframe_lifetime_stats.scans_run;
        }
    }

    EnvFrameLifetimeGuard(const EnvFrameLifetimeGuard&) = delete;
    EnvFrameLifetimeGuard& operator=(const EnvFrameLifetimeGuard&) = delete;
    EnvFrameLifetimeGuard(EnvFrameLifetimeGuard&&) = delete;
    EnvFrameLifetimeGuard& operator=(EnvFrameLifetimeGuard&&) = delete;

    [[nodiscard]] EnvFrameLifetimeSite site() const noexcept { return site_; }
    [[nodiscard]] void* ctx() const noexcept { return host_.ctx; }
    [[nodiscard]] bool armed() const noexcept { return host_.scan_skip_freed && host_.ctx; }

private:
    EnvFrameLifetimeHost host_;
    EnvFrameLifetimeSite site_;
};

} // namespace aura::core::envframe_lifetime
