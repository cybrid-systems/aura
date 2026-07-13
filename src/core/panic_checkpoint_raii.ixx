// panic_checkpoint_raii.ixx — Issue #1239 / #1363: PanicCheckpoint RAII
//
// Phase 2 (#1363): wired to Evaluator::save/restore_panic_checkpoint via
// type-erased PanicCheckpointHost (void* + fn ptrs) so this core module
// does not depend on aura.compiler.evaluator.

module;

export module aura.core.panic_checkpoint_raii;

import std;

export namespace aura::core::panic_cp {

// Phase 2: real save/restore wiring (was 1 = scaffold-only).
inline constexpr int kPanicCheckpointRaiiPhase = 2;

struct PanicCheckpointStats {
    std::uint64_t guards_constructed = 0;
    std::uint64_t auto_rollbacks = 0;
    std::uint64_t commits = 0;
    std::uint64_t saves_ok = 0;
    std::uint64_t saves_failed = 0;
    std::uint64_t restores_ok = 0;
    std::uint64_t restores_failed = 0;
    // Issue #1393: count of restore attempts where
    // PanicCheckpointGuard's bound Evaluator (host.expected_evaluator_id)
    // no longer matches the host's ctx pointer. This signals a
    // cross-evaluator restore attempt (e.g. via aot:reload /
    // persist:load / fiber with cross-evaluator body). The Guard
    // bumps this counter and skips restore (no UB) — operators
    // can monitor via the stats accessor or primitive.
    std::uint64_t restores_discriminator_failed = 0;
};

inline PanicCheckpointStats g_panic_checkpoint_raii_stats{};

inline void reset_panic_checkpoint_raii_stats() noexcept {
    g_panic_checkpoint_raii_stats = {};
}

// Type-erased host: Evaluator (or a test double) binds save/restore.
// save/restore may be null (stats-only / no-op host).
//
// Issue #1393: added `expected_evaluator_id` discriminator.
// PanicCheckpointGuard dtor verifies host.expected_evaluator_id ==
// host.ctx; on mismatch it bumps restores_discriminator_failed and
// skips restore. This catches cross-evaluator restore attempts
// (aot:reload / persist:load / fiber cross-evaluator body) where
// the void* ctx is no longer the active Evaluator. The
// discriminator lives in the host (not Guard) so a Guard that
// outlives its Evaluator can detect the mismatch on dtor without
// needing a thread_local "current evaluator" pointer.
struct PanicCheckpointHost {
    void* ctx = nullptr;
    void* expected_evaluator_id = nullptr; // Issue #1393: cross-evaluator discriminator
    bool (*save)(void* ctx) noexcept = nullptr;
    bool (*restore)(void* ctx) noexcept = nullptr;
};

// RAII guard: save on construct; restore on dtor unless commit().
// Exception-safe panic recovery when host points at Evaluator.
class PanicCheckpointGuard {
public:
    explicit PanicCheckpointGuard(PanicCheckpointHost host) noexcept
        : host_(host) {
        ++g_panic_checkpoint_raii_stats.guards_constructed;
        if (host_.save && host_.ctx) {
            saved_ = host_.save(host_.ctx);
            if (saved_)
                ++g_panic_checkpoint_raii_stats.saves_ok;
            else
                ++g_panic_checkpoint_raii_stats.saves_failed;
        }
    }

    ~PanicCheckpointGuard() noexcept {
        if (committed_)
            return;
        // Issue #1393: cross-evaluator discriminator check.
        // If expected_evaluator_id is set (non-null) AND differs
        // from ctx, this Guard was constructed on a different
        // Evaluator than the host is now bound to. Cross-evaluator
        // restore would operate on the wrong state → bump the
        // discriminator-failed counter and skip restore. The user
        // is expected to manually re-establish the checkpoint on
        // the new Evaluator if needed.
        if (host_.expected_evaluator_id != nullptr && host_.expected_evaluator_id != host_.ctx) {
            ++g_panic_checkpoint_raii_stats.restores_discriminator_failed;
            ++g_panic_checkpoint_raii_stats.auto_rollbacks;
            return;
        }
        if (saved_ && host_.restore && host_.ctx) {
            if (host_.restore(host_.ctx))
                ++g_panic_checkpoint_raii_stats.restores_ok;
            else
                ++g_panic_checkpoint_raii_stats.restores_failed;
        }
        ++g_panic_checkpoint_raii_stats.auto_rollbacks;
    }

    PanicCheckpointGuard(const PanicCheckpointGuard&) = delete;
    PanicCheckpointGuard& operator=(const PanicCheckpointGuard&) = delete;

    // Allow return-by-value / placement from factory helpers.
    PanicCheckpointGuard(PanicCheckpointGuard&& o) noexcept
        : host_(o.host_)
        , saved_(o.saved_)
        , committed_(o.committed_) {
        o.committed_ = true; // moved-from: no restore on dtor
        o.saved_ = false;
    }
    PanicCheckpointGuard& operator=(PanicCheckpointGuard&&) = delete;

    void commit() noexcept {
        committed_ = true;
        ++g_panic_checkpoint_raii_stats.commits;
    }

    [[nodiscard]] bool saved() const noexcept { return saved_; }
    [[nodiscard]] bool committed() const noexcept { return committed_; }

private:
    PanicCheckpointHost host_{};
    bool saved_ = false;
    bool committed_ = false;
};

} // namespace aura::core::panic_cp
