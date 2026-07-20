// transaction_guard.hh — Issue #1964 cycle 3
// Unified transaction guard API wrapping MutationBoundaryGuard +
// PanicCheckpointRAII into a single transaction layer.
//
// Issue #1964 AC #2 cycle 3: consolidate MutationBoundaryGuard +
// PanicCheckpointRAII into a single transaction layer.
//
// Cycle 3 ship scope (design + minimal API surface):
// - Defines TransactionGuard struct that combines the boundary
//   check (MutationBoundaryGuard::try_acquire semantics) +
//   panic checkpoint (PanicCheckpointRAII semantics) into one
//   RAII type.
// - Provides TransactionGuardResult enum (Acquired / Rejected /
//   PanicRecovered) for the 3 outcomes a transaction can have.
// - The legacy MutationBoundaryGuard + PanicCheckpointRAII types
//   remain in their existing files for backward compatibility;
//   cycle 3-followup migrates callers one at a time.
//
// Cycle 3 follow-up plan:
// - Migrate agent body + orch spawn/join paths to use
//   TransactionGuard instead of separate MutationBoundaryGuard
//   + PanicCheckpointRAII instantiations.
// - Update MutationBoundaryGuard to internally wrap
//   TransactionGuard (or vice versa, depending on which has
//   wider consumer base).
// - Consolidate observability counters (currently spread across
//   MutationBoundaryGuard + PanicCheckpointRAII + per-call-site
//   atomics) into a single TransactionGuardMetrics struct.
// - Update linter to track migration progress.
//
// See docs/agent-safety-mechanisms-simplification.md §"Guard
// scope" invariant for the canonical transaction-layer scope.

#ifndef AURA_CORE_TRANSACTION_GUARD_HH
#define AURA_CORE_TRANSACTION_GUARD_HH

#include <atomic>
#include <cstdint>

namespace aura::core {

enum class TransactionGuardResult : std::uint8_t {
    Acquired = 0,       // Boundary acquired; mutation can proceed.
    Rejected = 1,       // Boundary rejected (quota / try_acquire failed).
    PanicRecovered = 2, // Panic checkpoint recovered (rolled back).
};

// Process-wide observability counters for the unified transaction
// layer. Replaces the per-mechanism counters in
// MutationBoundaryGuard + PanicCheckpointRAII as those are
// migrated. Cycle 3-followup owns the consolidation; cycle 3
// ship only adds the canonical counter struct.
struct TransactionGuardMetrics {
    std::atomic<std::uint64_t> acquired_total{0};
    std::atomic<std::uint64_t> rejected_total{0};
    std::atomic<std::uint64_t> panic_recovered_total{0};
    std::atomic<std::uint64_t> panic_checkpoint_active{0};
};

inline TransactionGuardMetrics& g_transaction_guard_metrics() noexcept {
    static TransactionGuardMetrics m;
    return m;
}

// TransactionGuard — the unified RAII type. Cycle 3 ship is the
// API surface only; actual call-site migration is cycle 3-followup.
//
// Construction: tries to acquire the mutation boundary (like
// MutationBoundaryGuard::try_acquire). On success, the guard
// holds the boundary for the lifetime of the object. On
// destruction, the boundary is released (and the panic
// checkpoint is restored if a panic happened during the
// transaction).
class TransactionGuard {
public:
    TransactionGuard() noexcept {
        auto& m = g_transaction_guard_metrics();
        // Cycle 3 ship: simulate boundary acquisition. The real
        // implementation delegates to MutationBoundaryGuard +
        // PanicCheckpointRAII in cycle 3-followup.
        m.acquired_total.fetch_add(1, std::memory_order_relaxed);
        m.panic_checkpoint_active.fetch_add(1, std::memory_order_relaxed);
        result_ = TransactionGuardResult::Acquired;
    }

    ~TransactionGuard() noexcept {
        auto& m = g_transaction_guard_metrics();
        m.panic_checkpoint_active.fetch_sub(1, std::memory_order_acq_rel);
        // Cycle 3-followup: restore panic checkpoint + release
        // mutation boundary here.
    }

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    [[nodiscard]] TransactionGuardResult result() const noexcept { return result_; }

private:
    TransactionGuardResult result_ = TransactionGuardResult::Rejected;
};

} // namespace aura::core

#endif // AURA_CORE_TRANSACTION_GUARD_HH