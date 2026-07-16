// lock_order_audit.h — Issue #1523 / #1388 canonical lock-order verifier.
//
// Canonical acquire order (never reverse):
//   Mutate → Workspace → EnvFrames → DepGraph
//
// Thread-local depth counters detect inversions (acquiring a lower
// level while a higher level is held). Zero-cost when depths are zero
// (single relaxed loads). Used by CompilerService invalidate paths +
// Evaluator workspace/env locks.
//
// Contended mutate: try_lock fails → bump mutate_mtx_contended_total,
// then blocking lock.
//
#ifndef AURA_COMPILER_LOCK_ORDER_AUDIT_H
#define AURA_COMPILER_LOCK_ORDER_AUDIT_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace aura::compiler::lock_order {

// Levels match #1388: lower index = must be acquired earlier.
enum class Level : std::uint8_t {
    Mutate = 0,
    Workspace = 1,
    EnvFrames = 2,
    DepGraph = 3,
    kCount = 4,
};

// Process-wide observability (mirrored into CompilerMetrics by Agents).
inline std::atomic<std::uint64_t> g_lock_inversion_detected_total{0};
inline std::atomic<std::uint64_t> g_mutate_mtx_contended_total{0};
inline std::atomic<std::uint64_t> g_lock_order_acquire_total{0};
inline std::atomic<std::uint64_t> g_lock_order_release_total{0};

// Per-thread re-entry depth for each level.
inline thread_local std::uint8_t g_depth[static_cast<std::uint8_t>(Level::kCount)] = {};

[[nodiscard]] inline bool is_held(Level L) noexcept {
    return g_depth[static_cast<std::uint8_t>(L)] > 0;
}

[[nodiscard]] inline bool any_higher_held(Level L) noexcept {
    const auto li = static_cast<std::uint8_t>(L);
    for (std::uint8_t h = static_cast<std::uint8_t>(li + 1);
         h < static_cast<std::uint8_t>(Level::kCount); ++h) {
        if (g_depth[h] > 0)
            return true;
    }
    return false;
}

// Returns true if acquire is legal; false if inversion (still records
// depth so release pairing works — production continues after metric).
inline bool on_acquire(Level L) noexcept {
    g_lock_order_acquire_total.fetch_add(1, std::memory_order_relaxed);
    const bool inv = any_higher_held(L);
    if (inv)
        g_lock_inversion_detected_total.fetch_add(1, std::memory_order_relaxed);
    ++g_depth[static_cast<std::uint8_t>(L)];
    return !inv;
}

inline void on_release(Level L) noexcept {
    g_lock_order_release_total.fetch_add(1, std::memory_order_relaxed);
    auto& d = g_depth[static_cast<std::uint8_t>(L)];
    if (d > 0)
        --d;
}

// ── RAII ordered unique lock ──────────────────────────────────
template <typename Mutex> class OrderedUniqueLock {
public:
    OrderedUniqueLock() = default;

    // Blocking unique acquire with order check.
    // If Level::Mutate and try_lock fails, bumps contended then locks.
    explicit OrderedUniqueLock(Mutex& m, Level L) noexcept
        : level_(L) {
        on_acquire(L);
        if (L == Level::Mutate) {
            if (!m.try_lock()) {
                g_mutate_mtx_contended_total.fetch_add(1, std::memory_order_relaxed);
                m.lock();
            }
            lock_ = std::unique_lock<Mutex>(m, std::adopt_lock);
        } else {
            lock_ = std::unique_lock<Mutex>(m);
        }
        active_ = true;
    }

    // Adopt already-locked mutex (caller must have locked correctly).
    OrderedUniqueLock(Mutex& m, Level L, std::adopt_lock_t) noexcept
        : level_(L) {
        on_acquire(L);
        lock_ = std::unique_lock<Mutex>(m, std::adopt_lock);
        active_ = true;
    }

    // Skip if already held at this level (nested outer owns lock).
    // Returns a default-constructed (inactive) lock when skipped.
    static OrderedUniqueLock acquire_if_needed(Mutex& m, Level L) noexcept {
        if (is_held(L))
            return OrderedUniqueLock{}; // inactive
        return OrderedUniqueLock{m, L};
    }

    OrderedUniqueLock(OrderedUniqueLock&& other) noexcept
        : lock_(std::move(other.lock_))
        , level_(other.level_)
        , active_(other.active_) {
        other.active_ = false;
    }
    OrderedUniqueLock& operator=(OrderedUniqueLock&& other) noexcept {
        if (this != &other) {
            release();
            lock_ = std::move(other.lock_);
            level_ = other.level_;
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }
    OrderedUniqueLock(const OrderedUniqueLock&) = delete;
    OrderedUniqueLock& operator=(const OrderedUniqueLock&) = delete;

    ~OrderedUniqueLock() { release(); }

    [[nodiscard]] bool owns_lock() const noexcept { return active_ && lock_.owns_lock(); }
    [[nodiscard]] explicit operator bool() const noexcept { return owns_lock(); }

    void release() noexcept {
        if (!active_)
            return;
        if (lock_.owns_lock())
            lock_.unlock();
        on_release(level_);
        active_ = false;
    }

private:
    std::unique_lock<Mutex> lock_;
    Level level_ = Level::Mutate;
    bool active_ = false;
};

// ── RAII ordered shared lock ──────────────────────────────────
template <typename Mutex> class OrderedSharedLock {
public:
    OrderedSharedLock() = default;

    explicit OrderedSharedLock(Mutex& m, Level L) noexcept
        : level_(L) {
        on_acquire(L);
        lock_ = std::shared_lock<Mutex>(m);
        active_ = true;
    }

    static OrderedSharedLock acquire_if_needed(Mutex& m, Level L) noexcept {
        if (is_held(L))
            return OrderedSharedLock{};
        return OrderedSharedLock{m, L};
    }

    OrderedSharedLock(OrderedSharedLock&& other) noexcept
        : lock_(std::move(other.lock_))
        , level_(other.level_)
        , active_(other.active_) {
        other.active_ = false;
    }
    OrderedSharedLock& operator=(OrderedSharedLock&& other) noexcept {
        if (this != &other) {
            release();
            lock_ = std::move(other.lock_);
            level_ = other.level_;
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }
    OrderedSharedLock(const OrderedSharedLock&) = delete;
    OrderedSharedLock& operator=(const OrderedSharedLock&) = delete;

    ~OrderedSharedLock() { release(); }

    [[nodiscard]] bool owns_lock() const noexcept { return active_ && lock_.owns_lock(); }

    void release() noexcept {
        if (!active_)
            return;
        if (lock_.owns_lock())
            lock_.unlock();
        on_release(level_);
        active_ = false;
    }

private:
    std::shared_lock<Mutex> lock_;
    Level level_ = Level::Mutate;
    bool active_ = false;
};

// Test helpers: reset TLS (do not call while locks held).
inline void reset_tls_for_test() noexcept {
    for (auto& d : g_depth)
        d = 0;
}

} // namespace aura::compiler::lock_order

#endif // AURA_COMPILER_LOCK_ORDER_AUDIT_H
