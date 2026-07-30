// lock_order_audit.h — Issue #1523 / #1388 / #2043 / #2316 canonical
// lock-order verifier.
//
// Canonical acquire order (never reverse):
//   Mailbox → Mutate → HotUpdate → Workspace → EnvFrames → CompactEnv → DepGraph
//
// Issue #2043 — linear ownership + GC window (soft/hard invalidate):
//   While Level::Mutate is held:
//     1. prepare_unified_invalidation_pre_cascade_ (linear scan + GC coord)
//     2. dual-epoch bump + live-closure expire
//     3. dirty cascade / re-lower (may take Workspace / DepGraph)
//     4. finalize_linear_gc_invalidation_window_ (scan + enforce +
//        sync_linear_roots + linear_ownership_epoch bump + root audit)
//   Apply / fiber steal / GC must observe a complete window: either
//   pre-bump state or post-finalize state — never half-updated linear
//   ownership_state with live apply.
//
// Issue #2131 — GcCoordScope phase machine (same order every path):
//   PrePin → Cascade → PostAudit → Released
//   (gc_coord_scope.h). invalidate / soft-dirty / boundary / hot-swap /
//   compact open a Scope; reverse order or missing after_cascade bumps
//   phase_violations_total (+ abort when gc_coord::strict_mode).
//
// Issue #2316 — extended rank table (mailbox + hot-update + compact_env):
//   Mailbox    — multi_fiber_mailbox mu_ (delivery gate, #2312)
//   Mutate     — workspace_mtx_ exclusive (MutationBoundary, #2184 / #2253)
//   HotUpdate  — HotUpdateRegistry mutex (reemit / drain, #2205 / #2208 / #2273)
//   Workspace  — workspace / closures_mtx
//   EnvFrames  — env_frames_mtx_
//   CompactEnv — compact_env_frames_lock_
//   DepGraph   — dep_graph_mtx_
//   Forbidden inversions (rank check fail → record metric or abort under
//   canary):
//     - Acquiring Mailbox while Mutate+ held → fail (delivery during mutate)
//     - Acquiring Mutate while HotUpdate+ held → fail (mutate during reemit)
//     - Acquiring HotUpdate while Workspace+ held → fail
//     - Acquiring Workspace while EnvFrames+ held → fail (existing #1388)
//     - Acquiring EnvFrames while CompactEnv+ held → fail (existing #4393)
//     - Acquiring CompactEnv while DepGraph+ held → fail
//
// Runtime canary (optional, AURA_LOCK_ORDER_CANARY=1): on inversion,
// abort with file:line. Production default OFF (zero cost — single
// relaxed load of thread-local depth + compare).
//
// Thread-local depth counters detect inversions (acquiring a lower
// level while a higher level is held). Zero-cost when depths are zero
// (single relaxed loads). Used by CompilerService invalidate paths +
// Evaluator workspace/env locks.
//
// Contended mutate: try_lock fails → bump mutate_mtx_contended_total,
// then blocking lock.
//
// Issue #2043 — linear ownership + GC window (soft/hard invalidate):
//   While Level::Mutate is held:
//     1. prepare_unified_invalidation_pre_cascade_ (linear scan + GC coord)
//     2. dual-epoch bump + live-closure expire
//     3. dirty cascade / re-lower (may take Workspace / DepGraph)
//     4. finalize_linear_gc_invalidation_window_ (scan + enforce +
//        sync_linear_roots + linear_ownership_epoch bump + root audit)
//   Apply / fiber steal / GC must observe a complete window: either
//   pre-bump state or post-finalize state — never half-updated linear
//   ownership_state with live apply.
//
// Issue #2131 — GcCoordScope phase machine (same order every path):
//   PrePin → Cascade → PostAudit → Released
//   (gc_coord_scope.h). invalidate / soft-dirty / boundary / hot-swap /
//   compact open a Scope; reverse order or missing after_cascade bumps
//   phase_violations_total (+ abort when gc_coord::strict_mode).
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

// Levels match #1388 + #2316 extension: lower index = must be acquired
// earlier. Forbidden inversions documented in header comment above.
enum class Level : std::uint8_t {
    Mailbox = 0,    // multi_fiber_mailbox mu_ (delivery gate, #2312)
    Mutate = 1,     // workspace_mtx_ exclusive (MutationBoundary)
    HotUpdate = 2,  // HotUpdateRegistry mutex (reemit / drain)
    Workspace = 3,  // workspace / closures_mtx
    EnvFrames = 4,  // env_frames_mtx_
    CompactEnv = 5, // compact_env_frames_lock_
    DepGraph = 6,   // dep_graph_mtx_
    kCount = 7,
};

// Process-wide observability (mirrored into CompilerMetrics by Agents).
inline std::atomic<std::uint64_t> g_lock_inversion_detected_total{0};
inline std::atomic<std::uint64_t> g_mutate_mtx_contended_total{0};
inline std::atomic<std::uint64_t> g_lock_order_acquire_total{0};
inline std::atomic<std::uint64_t> g_lock_order_release_total{0};
// Issue #2316: dedicated canary counter (distinct from
// g_lock_inversion_detected_total which is observability; this is the
// canary-only enforcement counter). Always 0 when canary is disabled
// (production). Lazy-init canary flag from AURA_LOCK_ORDER_CANARY env.
inline std::atomic<std::uint64_t> g_lock_order_violation_total{0}; // #2316
inline std::atomic<int> g_lock_order_canary_enabled{0};            // #2316

[[nodiscard]] inline bool lock_order_canary_enabled() noexcept {
    int expected = g_lock_order_canary_enabled.load(std::memory_order_acquire);
    if (expected != 0)
        return expected == 1;
    // Lazy-init from env (zero-cost single getenv check on first call).
    const char* v = std::getenv("AURA_LOCK_ORDER_CANARY");
    const int v1 = (v != nullptr && v[0] == '1') ? 1 : 0;
    g_lock_order_canary_enabled.store(v1, std::memory_order_release);
    return v1 == 1;
}

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
// Issue #2316: under AURA_LOCK_ORDER_CANARY=1, inversion aborts with
// file:line. Production default OFF (zero cost — single relaxed load +
// compare skipped when env unset).
inline bool on_acquire(Level L, const char* file = __builtin_FILE(),
                       int line = __builtin_LINE()) noexcept {
    g_lock_order_acquire_total.fetch_add(1, std::memory_order_relaxed);
    const bool inv = any_higher_held(L);
    if (inv) {
        g_lock_inversion_detected_total.fetch_add(1, std::memory_order_relaxed);
        g_lock_order_violation_total.fetch_add(1, std::memory_order_relaxed);
        if (lock_order_canary_enabled()) {
            std::fprintf(stderr,
                         "LOCK_ORDER_CANARY: inversion at %s:%d "
                         "(level=%u, higher held)\n",
                         file, line, static_cast<unsigned>(L));
            std::abort();
        }
    }
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
