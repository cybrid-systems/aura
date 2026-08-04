// transaction_guard.hh — Issue #1964 cycle 3 + Issue #2555
// Unified transaction guard API wrapping MutationBoundaryGuard +
// PanicCheckpoint into a single transaction layer.
//
// Issue #1964 AC #2 cycle 3: consolidate MutationBoundaryGuard +
// PanicCheckpointRAII into a single transaction layer.
// Issue #2555: replace cycle-3 scaffold simulation with a real
// type-erased RAII that delegates acquire/release (+ optional panic
// save/restore) to a TransactionGuardHost. Core stays free of
// aura.compiler.evaluator (same pattern as PanicCheckpointHost).
//
// Module consumers: `import aura.core.transaction_guard;` (or
// `import aura.core;`). This header is kept for source-cite ACs and
// non-module TUs; it mirrors the module surface.
//
// Semantics:
// - Ctor: host.try_acquire → Acquired (holds boundary) or Rejected.
//   On success, optionally arms panic via host.save when the host
//   does not already own panic inside try_acquire (MBG path sets
//   host_owns_panic_checkpoint=true and skips dual save).
// - commit(): mark success; dtor commits boundary + drops checkpoint.
// - mark_failed() / dtor without commit: success_flag=false so the
//   host release rolls back (MBG restore_panic_checkpoint path).
// - recover_panic(): explicit restore path → PanicRecovered.
//
// Legacy MutationBoundaryGuard + PanicCheckpointGuard remain for
// one more cycle; migration tracked by
// scripts/coverage/checks/check_transaction_guard_migration_2555.py.
//
// See docs/agent-safety-mechanisms-simplification.md §"Guard
// scope" invariant for the canonical transaction-layer scope.

#ifndef AURA_CORE_TRANSACTION_GUARD_HH
#define AURA_CORE_TRANSACTION_GUARD_HH

// Prefer the module when available. Header-only fallback for tools /
// source-cite that do not compile as modules.
#if !defined(AURA_TRANSACTION_GUARD_MODULE_ONLY)

#include <atomic>
#include <cstdint>
#include <exception>
#include <utility>

namespace aura::core {

enum class TransactionGuardResult : std::uint8_t {
    Acquired = 0,
    Rejected = 1,
    PanicRecovered = 2,
};

struct TransactionGuardMetrics {
    std::atomic<std::uint64_t> acquired_total{0};
    std::atomic<std::uint64_t> rejected_total{0};
    std::atomic<std::uint64_t> panic_recovered_total{0};
    std::atomic<std::uint64_t> panic_checkpoint_active{0};
    std::atomic<std::uint64_t> committed_total{0};
    std::atomic<std::uint64_t> released_total{0};
};

inline TransactionGuardMetrics& g_transaction_guard_metrics() noexcept {
    static TransactionGuardMetrics m;
    return m;
}

struct TransactionGuardHost {
    void* ctx = nullptr;
    void* expected_evaluator_id = nullptr;
    void* (*try_acquire)(void* ctx, std::uint64_t pending, bool* success_flag) noexcept = nullptr;
    void (*release)(void* ctx, void* handle) noexcept = nullptr;
    bool (*save)(void* ctx) noexcept = nullptr;
    bool (*restore)(void* ctx) noexcept = nullptr;
    bool (*clear)(void* ctx) noexcept = nullptr;
    bool host_owns_panic_checkpoint = false;
};

class TransactionGuard {
public:
    TransactionGuard() = delete;

    explicit TransactionGuard(TransactionGuardHost host, std::uint64_t pending_count = 1) noexcept;

    ~TransactionGuard() noexcept;

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;
    TransactionGuard(TransactionGuard&& o) noexcept;
    TransactionGuard& operator=(TransactionGuard&&) = delete;

    [[nodiscard]] TransactionGuardResult result() const noexcept;
    [[nodiscard]] bool acquired() const noexcept;
    [[nodiscard]] bool success() const noexcept;
    [[nodiscard]] bool committed() const noexcept;
    [[nodiscard]] bool* success_flag() noexcept;

    void commit() noexcept;
    void mark_failed() noexcept;
    [[nodiscard]] TransactionGuardResult recover_panic() noexcept;

private:
    void release_impl(bool explicit_recover) noexcept;

    TransactionGuardHost host_{};
    void* handle_ = nullptr;
    bool success_ = false;
    bool saved_ = false;
    bool host_panic_active_ = false;
    bool committed_ = false;
    bool released_ = false;
    TransactionGuardResult result_ = TransactionGuardResult::Rejected;
    int uncaught_at_enter_ = 0;
};

// Inline definitions for non-module TUs (module provides its own).
inline TransactionGuard::TransactionGuard(TransactionGuardHost host,
                                          std::uint64_t pending_count) noexcept
    : host_(host) {
    auto& m = g_transaction_guard_metrics();
    if (!host_.try_acquire || !host_.ctx || !host_.release) {
        result_ = TransactionGuardResult::Rejected;
        m.rejected_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    success_ = true;
    handle_ = host_.try_acquire(host_.ctx, pending_count, &success_);
    if (!handle_) {
        success_ = false;
        result_ = TransactionGuardResult::Rejected;
        m.rejected_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uncaught_at_enter_ = std::uncaught_exceptions();
    if (!host_.host_owns_panic_checkpoint && host_.save && host_.ctx) {
        saved_ = host_.save(host_.ctx);
        if (saved_)
            m.panic_checkpoint_active.fetch_add(1, std::memory_order_relaxed);
    } else if (host_.host_owns_panic_checkpoint) {
        m.panic_checkpoint_active.fetch_add(1, std::memory_order_relaxed);
        saved_ = true;
        host_panic_active_ = true;
    }
    result_ = TransactionGuardResult::Acquired;
    m.acquired_total.fetch_add(1, std::memory_order_relaxed);
}

inline TransactionGuard::~TransactionGuard() noexcept {
    release_impl(/*explicit_recover=*/false);
}

inline TransactionGuard::TransactionGuard(TransactionGuard&& o) noexcept
    : host_(o.host_)
    , handle_(o.handle_)
    , success_(o.success_)
    , saved_(o.saved_)
    , host_panic_active_(o.host_panic_active_)
    , committed_(o.committed_)
    , released_(o.released_)
    , result_(o.result_)
    , uncaught_at_enter_(o.uncaught_at_enter_) {
    o.handle_ = nullptr;
    o.saved_ = false;
    o.host_panic_active_ = false;
    o.released_ = true;
    o.result_ = TransactionGuardResult::Rejected;
}

inline TransactionGuardResult TransactionGuard::result() const noexcept {
    return result_;
}
inline bool TransactionGuard::acquired() const noexcept {
    return result_ == TransactionGuardResult::Acquired && handle_ != nullptr && !released_;
}
inline bool TransactionGuard::success() const noexcept {
    return success_;
}
inline bool TransactionGuard::committed() const noexcept {
    return committed_;
}
inline bool* TransactionGuard::success_flag() noexcept {
    return &success_;
}

inline void TransactionGuard::commit() noexcept {
    if (!acquired())
        return;
    committed_ = true;
    success_ = true;
    g_transaction_guard_metrics().committed_total.fetch_add(1, std::memory_order_relaxed);
}

inline void TransactionGuard::mark_failed() noexcept {
    if (!acquired())
        return;
    success_ = false;
    committed_ = false;
}

inline TransactionGuardResult TransactionGuard::recover_panic() noexcept {
    if (released_)
        return result_;
    if (result_ != TransactionGuardResult::Acquired) {
        release_impl(/*explicit_recover=*/true);
        return result_;
    }
    success_ = false;
    committed_ = false;
    if (!host_panic_active_ && saved_ && host_.restore && host_.ctx) {
        if (host_.expected_evaluator_id != nullptr && host_.expected_evaluator_id != host_.ctx) {
            if (host_.clear && host_.ctx)
                (void)host_.clear(host_.ctx);
        } else {
            (void)host_.restore(host_.ctx);
        }
        saved_ = false;
    }
    release_impl(/*explicit_recover=*/true);
    return result_;
}

inline void TransactionGuard::release_impl(bool explicit_recover) noexcept {
    if (released_)
        return;
    released_ = true;
    auto& m = g_transaction_guard_metrics();

    if (result_ != TransactionGuardResult::Acquired || handle_ == nullptr) {
        handle_ = nullptr;
        return;
    }

    if (success_ && !committed_ && std::uncaught_exceptions() > uncaught_at_enter_) {
        success_ = false;
    }

    const bool failed = !success_;

    if (!host_panic_active_ && saved_) {
        if (failed) {
            if (host_.expected_evaluator_id != nullptr &&
                host_.expected_evaluator_id != host_.ctx) {
                if (host_.clear && host_.ctx)
                    (void)host_.clear(host_.ctx);
            } else if (host_.restore && host_.ctx) {
                (void)host_.restore(host_.ctx);
            }
        } else if (host_.clear && host_.ctx) {
            (void)host_.clear(host_.ctx);
        }
        saved_ = false;
        m.panic_checkpoint_active.fetch_sub(1, std::memory_order_acq_rel);
    } else if (host_panic_active_) {
        m.panic_checkpoint_active.fetch_sub(1, std::memory_order_acq_rel);
        host_panic_active_ = false;
    }

    if (host_.release && host_.ctx && handle_) {
        host_.release(host_.ctx, handle_);
    }
    handle_ = nullptr;
    m.released_total.fetch_add(1, std::memory_order_relaxed);

    if (failed || explicit_recover) {
        result_ = TransactionGuardResult::PanicRecovered;
        m.panic_recovered_total.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace aura::core

#endif // !AURA_TRANSACTION_GUARD_MODULE_ONLY

#endif // AURA_CORE_TRANSACTION_GUARD_HH
