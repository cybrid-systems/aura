// panic_checkpoint_raii.ixx — Issue #1239 Phase 1: PanicCheckpoint RAII scaffold.

module;

export module aura.core.panic_checkpoint_raii;

import std;

export namespace aura::core::panic_cp {

inline constexpr int kPanicCheckpointRaiiPhase = 1;

struct PanicCheckpointStats {
    std::uint64_t guards_constructed = 0;
    std::uint64_t auto_rollbacks = 0;
    std::uint64_t commits = 0;
};

inline PanicCheckpointStats g_panic_checkpoint_raii_stats{};

// RAII guard: records construction; rollback on dtor unless commit().
// Full wiring to Evaluator::save/restore_panic_checkpoint is follow-up.
class PanicCheckpointGuard {
public:
    PanicCheckpointGuard() noexcept { ++g_panic_checkpoint_raii_stats.guards_constructed; }
    ~PanicCheckpointGuard() noexcept {
        if (!committed_)
            ++g_panic_checkpoint_raii_stats.auto_rollbacks;
    }
    PanicCheckpointGuard(const PanicCheckpointGuard&) = delete;
    PanicCheckpointGuard& operator=(const PanicCheckpointGuard&) = delete;

    void commit() noexcept {
        committed_ = true;
        ++g_panic_checkpoint_raii_stats.commits;
    }

private:
    bool committed_ = false;
};

} // namespace aura::core::panic_cp
