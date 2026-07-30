// lock_order_audit.h — Issue #1523 / #1388 / #2043 / #2316 / #2354
// canonical lock-order verifier.
//
// Canonical acquire order (never reverse):
//   Mailbox → Mutate → HotUpdate → Workspace → EnvFrames → CompactEnv → DepGraph
//   → Orphan → WaitMap → Joiner → OwnedFibers → FiberRegistry → Closures → Module
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
//   Workspace  — workspace_mtx_ (MutationBoundaryGuard outermost)
//   EnvFrames  — env_frames_mtx_
//   CompactEnv — compact_env_frames_lock_
//   DepGraph   — dep_graph_mtx_
//
// Issue #2354 — scheduler / worker / closures / module ranks (production
// review 建议 6). Append-only so #2316 numeric ranks stay stable:
//   Orphan        — Scheduler::orphan_mutex_
//   WaitMap       — Scheduler::wait_map_mutex_
//   Joiner        — Scheduler::joiner_map_mutex_
//   OwnedFibers   — Scheduler::owned_fibers_mutex_
//   FiberRegistry — WorkerThread::fiber_registry_mutex_
//   Closures      — Evaluator::closures_mtx_
//   Module        — Evaluator::module_mtx_
// Documented Scheduler reap_orphans_now order:
//   orphan_mutex_ → wait_map_mutex_ → joiner_map_mutex_ → owned_fibers_mutex_
//
// Forbidden inversions (rank check fail → record metric or abort under
// canary / audit hard mode):
//   - Acquiring lower rank while any higher rank is held
//   - Scheduler: WaitMap while OwnedFibers held, etc.
//
// Runtime modes (lazy-init from env; Production default OFF = zero atomics):
//   AURA_LOCK_ORDER_AUDIT=1  — soft: counters + inversion metrics (no abort)
//   AURA_LOCK_ORDER_CANARY=1 — hard: soft + abort with rank diagnostics
//   both unset               — TLS depth still tracked (nest safety for
//                              OrderedUniqueLock::acquire_if_needed); no atomics
//
// Thread-local depth counters detect inversions (acquiring a lower
// level while a higher level is held). Used by CompilerService
// invalidate paths + Evaluator workspace/env locks + Scheduler/Worker.
//
// Contended mutate: try_lock fails → bump mutate_mtx_contended_total,
// then blocking lock.
//
#ifndef AURA_COMPILER_LOCK_ORDER_AUDIT_H
#define AURA_COMPILER_LOCK_ORDER_AUDIT_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace aura::compiler::lock_order {

// Levels: lower index = must be acquired earlier. Append-only after
// DepGraph so #2316 numeric ranks remain stable.
enum class Level : std::uint8_t {
    Mailbox = 0,    // multi_fiber_mailbox mu_ (delivery gate, #2312)
    Mutate = 1,     // mutate_mtx_ exclusive (CompilerService)
    HotUpdate = 2,  // HotUpdateRegistry mutex (reemit / drain)
    Workspace = 3,  // workspace_mtx_ (MutationBoundaryGuard)
    EnvFrames = 4,  // env_frames_mtx_
    CompactEnv = 5, // compact_env_frames_lock_
    DepGraph = 6,   // dep_graph_mtx_
    // Issue #2354: scheduler / worker / closures / module (append-only).
    Orphan = 7,         // Scheduler::orphan_mutex_
    WaitMap = 8,        // Scheduler::wait_map_mutex_
    Joiner = 9,         // Scheduler::joiner_map_mutex_
    OwnedFibers = 10,   // Scheduler::owned_fibers_mutex_
    FiberRegistry = 11, // WorkerThread::fiber_registry_mutex_
    Closures = 12,      // Evaluator::closures_mtx_
    Module = 13,        // Evaluator::module_mtx_
    kCount = 14,
};

// Process-wide observability (mirrored into CompilerMetrics by Agents).
inline std::atomic<std::uint64_t> g_lock_inversion_detected_total{0};
inline std::atomic<std::uint64_t> g_mutate_mtx_contended_total{0};
inline std::atomic<std::uint64_t> g_lock_order_acquire_total{0};
inline std::atomic<std::uint64_t> g_lock_order_release_total{0};
// Issue #2316: canary-only enforcement counter (also bumped on soft audit).
inline std::atomic<std::uint64_t> g_lock_order_violation_total{0}; // #2316 / #2354
// Legacy canary flag (kept for tests that poke it). Prefer mode atomics.
inline std::atomic<int> g_lock_order_canary_enabled{0}; // #2316
// Issue #2354: 0=uninit, 1=off, 2=soft (AUDIT), 3=hard (CANARY).
inline std::atomic<int> g_lock_order_mode{0};

[[nodiscard]] inline const char* level_name(Level L) noexcept {
    switch (L) {
        case Level::Mailbox:
            return "Mailbox";
        case Level::Mutate:
            return "Mutate";
        case Level::HotUpdate:
            return "HotUpdate";
        case Level::Workspace:
            return "Workspace";
        case Level::EnvFrames:
            return "EnvFrames";
        case Level::CompactEnv:
            return "CompactEnv";
        case Level::DepGraph:
            return "DepGraph";
        case Level::Orphan:
            return "Orphan";
        case Level::WaitMap:
            return "WaitMap";
        case Level::Joiner:
            return "Joiner";
        case Level::OwnedFibers:
            return "OwnedFibers";
        case Level::FiberRegistry:
            return "FiberRegistry";
        case Level::Closures:
            return "Closures";
        case Level::Module:
            return "Module";
        default:
            return "Unknown";
    }
}

// Resolve mode once (process-lifetime). AC1: when off, on_acquire is a
// single branch. Tests may force via force_audit_mode_for_test.
[[nodiscard]] inline int lock_order_mode() noexcept {
    int m = g_lock_order_mode.load(std::memory_order_acquire);
    if (m != 0)
        return m;
    // Hard canary wins over soft audit.
    const char* c = std::getenv("AURA_LOCK_ORDER_CANARY");
    if (c != nullptr && c[0] == '1') {
        g_lock_order_mode.store(3, std::memory_order_release);
        g_lock_order_canary_enabled.store(1, std::memory_order_release);
        return 3;
    }
    // Issue #2354: AURA_LOCK_ORDER_AUDIT=1 soft mode.
    const char* a = std::getenv("AURA_LOCK_ORDER_AUDIT");
    if (a != nullptr && a[0] == '1') {
        g_lock_order_mode.store(2, std::memory_order_release);
        return 2;
    }
    // Legacy: if tests pre-set canary flag to 1, treat as hard.
    if (g_lock_order_canary_enabled.load(std::memory_order_acquire) == 1) {
        g_lock_order_mode.store(3, std::memory_order_release);
        return 3;
    }
    g_lock_order_mode.store(1, std::memory_order_release);
    return 1;
}

[[nodiscard]] inline bool lock_order_audit_enabled() noexcept {
    return lock_order_mode() >= 2;
}

[[nodiscard]] inline bool lock_order_canary_enabled() noexcept {
    return lock_order_mode() == 3;
}

// Test helpers: force soft (2) or hard (3) or off (1). Do not call while
// locks held. Resets lazy-init so next mode() call is stable.
inline void force_audit_mode_for_test(int mode) noexcept {
    if (mode < 1)
        mode = 1;
    if (mode > 3)
        mode = 3;
    g_lock_order_mode.store(mode, std::memory_order_release);
    g_lock_order_canary_enabled.store(mode == 3 ? 1 : 0, std::memory_order_release);
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

inline void dump_held_ranks(FILE* out) noexcept {
    std::fprintf(out, "  held ranks:");
    bool any = false;
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Level::kCount); ++i) {
        if (g_depth[i] > 0) {
            std::fprintf(out, " %s(depth=%u)", level_name(static_cast<Level>(i)),
                         static_cast<unsigned>(g_depth[i]));
            any = true;
        }
    }
    if (!any)
        std::fprintf(out, " (none)");
    std::fprintf(out, "\n");
}

// Returns true if acquire is legal; false if inversion (still records
// depth so release pairing works — soft mode continues after metric).
// Hard canary (mode==3): abort with file:line + rank dump.
//
// AC1 (#2354) refined: production default OFF skips atomics / inversion
// diagnostics, BUT always updates TLS g_depth. Depth is correctness, not
// mere observability — OrderedUniqueLock::acquire_if_needed / nested
// MutationBoundary paths rely on is_held() to skip re-locking the same
// non-recursive mutex. Skipping depth when audit is off caused
// "Resource deadlock avoided" (EDEADLK) under default CI (#2354 regression).
inline bool on_acquire(Level L, const char* file = __builtin_FILE(),
                       int line = __builtin_LINE()) noexcept {
    // Depth always (nest / acquire_if_needed correctness).
    const bool inv = any_higher_held(L);
    ++g_depth[static_cast<std::uint8_t>(L)];
    if (!lock_order_audit_enabled())
        return true; // AC1: zero atomics when OFF
    g_lock_order_acquire_total.fetch_add(1, std::memory_order_relaxed);
    if (inv) {
        g_lock_inversion_detected_total.fetch_add(1, std::memory_order_relaxed);
        g_lock_order_violation_total.fetch_add(1, std::memory_order_relaxed);
        if (lock_order_canary_enabled()) {
            std::fprintf(stderr,
                         "LOCK_ORDER_CANARY: inversion at %s:%d "
                         "(acquiring level=%u %s while higher held)\n",
                         file, line, static_cast<unsigned>(L), level_name(L));
            dump_held_ranks(stderr);
            std::abort();
        }
    }
    return !inv;
}

inline void on_release(Level L) noexcept {
    // Depth always (pair with on_acquire).
    auto& d = g_depth[static_cast<std::uint8_t>(L)];
    if (d > 0)
        --d;
    if (!lock_order_audit_enabled())
        return; // AC1: zero atomics when OFF
    g_lock_order_release_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2354: RAII scope that pairs on_acquire/on_release around an
// existing lock_guard. Place *before* the lock_guard so rank is checked
// prior to blocking on the mutex.
class AuditScope {
public:
    explicit AuditScope(Level L, const char* file = __builtin_FILE(),
                        int line = __builtin_LINE()) noexcept
        : level_(L)
        , active_(true) {
        on_acquire(L, file, line);
    }
    ~AuditScope() {
        if (active_)
            on_release(level_);
    }
    AuditScope(const AuditScope&) = delete;
    AuditScope& operator=(const AuditScope&) = delete;
    AuditScope(AuditScope&& o) noexcept
        : level_(o.level_)
        , active_(o.active_) {
        o.active_ = false;
    }
    AuditScope& operator=(AuditScope&& o) noexcept {
        if (this != &o) {
            if (active_)
                on_release(level_);
            level_ = o.level_;
            active_ = o.active_;
            o.active_ = false;
        }
        return *this;
    }

private:
    Level level_;
    bool active_ = false;
};

// Issue #2354: combined audit + std::mutex lock (scheduler / worker sites).
class AuditedMutexLock {
public:
    AuditedMutexLock(std::mutex& m, Level L, const char* file = __builtin_FILE(),
                     int line = __builtin_LINE()) noexcept
        : level_(L)
        , active_(true) {
        on_acquire(L, file, line);
        lock_ = std::unique_lock<std::mutex>(m);
    }
    ~AuditedMutexLock() { release(); }
    AuditedMutexLock(const AuditedMutexLock&) = delete;
    AuditedMutexLock& operator=(const AuditedMutexLock&) = delete;
    AuditedMutexLock(AuditedMutexLock&& o) noexcept
        : lock_(std::move(o.lock_))
        , level_(o.level_)
        , active_(o.active_) {
        o.active_ = false;
    }
    void release() noexcept {
        if (!active_)
            return;
        if (lock_.owns_lock())
            lock_.unlock();
        on_release(level_);
        active_ = false;
    }

private:
    std::unique_lock<std::mutex> lock_;
    Level level_ = Level::Orphan;
    bool active_ = false;
};

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
