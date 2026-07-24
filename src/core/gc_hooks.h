// core/gc_hooks.h — Cross-module function pointers for the GC subsystem.
//
// Why this exists: the GC coordinator (aura::serve) needs to be
// observable from base-level allocators (aura::core) without a
// circular module dependency. We use plain C function pointers
// (not std::function) so the header has no STL dependency and can
// be included from any TU, including C++20 module global fragments.
//
// Conventions:
//   - All function pointers default to nullptr.
//   - Each pointer is `noexcept` (the GC never throws).
//   - Each pointer is set by the relevant subsystem at startup
//     (Fiber subsystem for safepoint, GC collector for the
//     others) and cleared at shutdown.
//
// Thread-safety: setting is single-threaded (during startup /
// shutdown); reading is the hot path and is lock-free. We use
// `std::atomic<void(*)(...)>` to make the data-race clearly
// defined; reads see either the old or new value.

#ifndef AURA_CORE_GC_HOOKS_H
#define AURA_CORE_GC_HOOKS_H

#include <cstddef>
#include <array>
#include <atomic>
#include <mutex>

namespace aura::gc_hooks {

// ── Safepoint check ─────────────────────────────────────────
// Called by the arena alloc path on every allocation. The
// implementation is `aura::serve::Fiber::check_gc_safepoint`.
// When null, the arena doesn't check safepoints (stdin mode,
// or the scheduler hasn't been initialized).
//
// Cost: when set, ~1 ns (atomic load + branch). The design
// recommends this for compute-heavy fibers that don't yield
// for long stretches but keep allocating.
using GcSafepointCheckFn = void (*)();
inline std::atomic<GcSafepointCheckFn> g_arena_safepoint_check{nullptr};

// ── Alloc accounting ────────────────────────────────────────
// Optional: called on every arena allocation to bump the
// GC's alloc counter. When the counter crosses the threshold,
// the GC triggers a collection cycle. Set by
// GCCollector::init at scheduler startup.
using GcRecordAllocFn = void (*)();
inline std::atomic<GcRecordAllocFn> g_arena_record_alloc{nullptr};

// ── Fiber-context probe (Issue #604) ────────────────────────
// Returns true when the calling thread is running inside a
// scheduled fiber (g_current_fiber != nullptr). compact() /
// defrag() consult this so that a compaction requested from a
// fiber context bumps compaction_yield_checks and hits the GC
// safepoint (coordinating the yield) rather than blindly
// trimming the buffer. Null in stdin mode / pre-scheduler, so
// the arena treats every compaction as non-fiber.
using GcFiberActiveFn = bool (*)();
inline std::atomic<GcFiberActiveFn> g_fiber_active{nullptr};

// ── Convenience: call if set ───────────────────────────────
inline void safepoint_check() noexcept {
    auto fn = g_arena_safepoint_check.load(std::memory_order_acquire);
    if (fn)
        fn();
}

inline void record_alloc() noexcept {
    auto fn = g_arena_record_alloc.load(std::memory_order_acquire);
    if (fn)
        fn();
}

inline bool fiber_active() noexcept {
    auto fn = g_fiber_active.load(std::memory_order_acquire);
    return fn ? fn() : false;
}

// Issue #1390: query whether a safepoint check function is
// registered. Used by ASTArena::request_defrag() to decide
// whether to emit a one-shot "no safepoint" warning, and
// exposed as (arena:safepoint-registered?) primitive.
inline bool safepoint_registered() noexcept {
    return g_arena_safepoint_check.load(std::memory_order_acquire) != nullptr;
}

// ── Safepoint active flag (Issue #1364) ───────────────────
// Set true for the duration of a STW GC pause (after all fibers
// arrived, until resume). Mutation primitives consult this via
// in_gc_safepoint() for telemetry (benign race — workspace_mtx_
// still serializes AST writes; see docs/development/safepoint-mutation.md).
inline std::atomic<bool> g_arena_safepoint_active{false};

[[nodiscard]] inline bool in_gc_safepoint() noexcept {
    return g_arena_safepoint_active.load(std::memory_order_acquire);
}

// RAII: nestable set of g_arena_safepoint_active for the pause window.
class ScopedSafepoint {
public:
    ScopedSafepoint() noexcept {
        prev_ = g_arena_safepoint_active.exchange(true, std::memory_order_acq_rel);
    }
    ~ScopedSafepoint() noexcept {
        g_arena_safepoint_active.store(prev_, std::memory_order_release);
    }
    ScopedSafepoint(const ScopedSafepoint&) = delete;
    ScopedSafepoint& operator=(const ScopedSafepoint&) = delete;

private:
    bool prev_ = false;
};

// Process-wide: fiber waited at safepoint while holding a mutation boundary.
// Bumped from Fiber::check_gc_safepoint (does not require CompilerMetrics).
inline std::atomic<std::uint64_t> g_safepoint_yield_on_mutation_total{0};
// Issue #1493: total wait time (µs) and event count while holding mutation.
inline std::atomic<std::uint64_t> g_safepoint_wait_while_mutation_held_us{0};
inline std::atomic<std::uint64_t> g_safepoint_wait_while_mutation_held_count{0};

inline void note_safepoint_yield_on_mutation() noexcept {
    g_safepoint_yield_on_mutation_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_safepoint_wait_while_mutation(std::uint64_t wait_us) noexcept {
    g_safepoint_wait_while_mutation_held_count.fetch_add(1, std::memory_order_relaxed);
    g_safepoint_wait_while_mutation_held_us.fetch_add(wait_us, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t safepoint_yield_on_mutation_total() noexcept {
    return g_safepoint_yield_on_mutation_total.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t safepoint_wait_while_mutation_held_us() noexcept {
    return g_safepoint_wait_while_mutation_held_us.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t safepoint_wait_while_mutation_held_count() noexcept {
    return g_safepoint_wait_while_mutation_held_count.load(std::memory_order_relaxed);
}

// ── Pending PanicCheckpoint GC defer (Issue #1489 / #651 / #1581) ───
// Process-wide depth: armed when an Evaluator holds a live
// PanicCheckpoint (save) and released on commit/restore.
// Scheduler GCCollector::request/collect and Evaluator::compact_sweep
// consult this so STW sweep does not reclaim pinned COW /
// StableNodeRef / EnvFrame state during the recovery window.
// Depth (not a bool) so nested evaluators / multi-checkpoint
// windows compose correctly.
//
// Issue #2002: per-evaluator discriminator table. The process-wide
// depth stays (aggregate metrics), but the per-evaluator table lets
// PanicCheckpointGuard cross-check expected_evaluator_id on arm/release
// and lets fiber-steal clear orphaned depth from the previous host.
// Bounded inline array + mutex (PanicCheckpoint save/restore is rare;
// per-call lock is fine).
inline std::atomic<std::uint32_t> g_gc_defer_pending_panic_depth{0};
namespace detail {
    constexpr std::size_t kMaxArmedEvaluators = 64;
    struct ArmedEvaluatorEntry {
        void* id = nullptr;
        std::uint32_t depth = 0;
    };
    inline std::array<ArmedEvaluatorEntry, kMaxArmedEvaluators> g_gc_defer_armed_table{};
    inline std::mutex g_gc_defer_armed_mtx{};
} // namespace detail
// Signals from Fiber::yield → block_gc_for_pending_checkpoint
// trampoline (may fire many times per armed window).
inline std::atomic<std::uint64_t> g_gc_defer_pending_panic_signals{0};
// compact_sweep / collect aborted because defer was armed.
inline std::atomic<std::uint64_t> g_gc_sweep_skipped_pending_panic{0};
// Issue #1581: GCCollector::request() refused because defer was armed
// (scheduler-facing early-out before arming gc_in_progress).
inline std::atomic<std::uint64_t> g_gc_request_deferred_pending_panic{0};
// Provenance of the last scheduler defer signal (fiber id + checkpoint
// epoch). Written by send_defer_gc_signal; read by tests/metrics.
inline std::atomic<std::uint64_t> g_gc_defer_last_fiber_id{0};
inline std::atomic<std::uint64_t> g_gc_defer_last_checkpoint_epoch{0};

inline void arm_gc_defer_pending_panic() noexcept {
    g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
}

inline void release_gc_defer_pending_panic() noexcept {
    auto prev = g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed);
    while (prev > 0) {
        if (g_gc_defer_pending_panic_depth.compare_exchange_weak(
                prev, prev - 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            return;
    }
}

// Issue #2002: per-evaluator arm. Updates the per-evaluator table
// AND the process-wide aggregate depth (so existing gc_deferred_for_pending_panic()
// checks continue to work without changes). If evaluator_id is null,
// falls back to the legacy process-wide increment (no discriminator).
inline void arm_gc_defer_pending_panic_for(void* evaluator_id) noexcept {
    if (!evaluator_id) {
        arm_gc_defer_pending_panic();
        return;
    }
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (auto& e : detail::g_gc_defer_armed_table) {
        if (e.id == evaluator_id) {
            ++e.depth;
            g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
    }
    for (auto& e : detail::g_gc_defer_armed_table) {
        if (e.id == nullptr) {
            e.id = evaluator_id;
            e.depth = 1;
            g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
    }
    // Overflow: still bump process-wide depth (legacy behavior). The
    // per-evaluator table is bounded; in practice # of evaluators with
    // active PanicCheckpoints is tiny.
    g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
}

// Issue #2002: per-evaluator release. Decrements the entry's depth;
// when the entry's depth hits 0, clears the id slot. Always decrements
// the process-wide aggregate too (so the aggregate matches what was
// armed, even if the per-evaluator slot is no longer found).
inline void release_gc_defer_pending_panic_for(void* evaluator_id) noexcept {
    if (!evaluator_id) {
        release_gc_defer_pending_panic();
        return;
    }
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (auto& e : detail::g_gc_defer_armed_table) {
        if (e.id == evaluator_id) {
            if (e.depth > 0)
                --e.depth;
            if (e.depth == 0)
                e.id = nullptr;
            break;
        }
    }
    auto prev = g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed);
    while (prev > 0) {
        if (g_gc_defer_pending_panic_depth.compare_exchange_weak(
                prev, prev - 1, std::memory_order_acq_rel, std::memory_order_relaxed))
            break;
    }
}

[[nodiscard]] inline bool gc_deferred_for_pending_panic() noexcept {
    return g_gc_defer_pending_panic_depth.load(std::memory_order_acquire) > 0;
}

// Issue #2002: per-evaluator discriminator check. Returns true if
// the specific evaluator_id has at least one live PanicCheckpoint
// armed in the per-evaluator table. Returns false if the id is null
// or not found. Cross-evaluator steal can use this to detect stale
// defer depth owned by the previous host.
[[nodiscard]] inline bool gc_deferred_for_evaluator(void* evaluator_id) noexcept {
    if (!evaluator_id)
        return false;
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (const auto& e : detail::g_gc_defer_armed_table) {
        if (e.id == evaluator_id)
            return e.depth > 0;
    }
    return false;
}

// Issue #2002: clear any deferred depth belonging to evaluator_id
// (used by fiber-steal path to orphan stale depth from the previous
// host). Returns the # of slots that were cleared (0 if none).
[[nodiscard]] inline std::uint32_t clear_gc_defer_for_evaluator(void* evaluator_id) noexcept {
    if (!evaluator_id)
        return 0;
    std::uint32_t cleared = 0;
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (auto& e : detail::g_gc_defer_armed_table) {
        if (e.id == evaluator_id) {
            cleared = e.depth;
            e.depth = 0;
            e.id = nullptr;
            break;
        }
    }
    if (cleared > 0) {
        auto prev = g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed);
        while (prev > 0) {
            const auto target = (prev >= cleared) ? prev - cleared : 0;
            if (g_gc_defer_pending_panic_depth.compare_exchange_weak(
                    prev, target, std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
    }
    return cleared;
}

[[nodiscard]] inline std::uint32_t gc_defer_pending_panic_depth() noexcept {
    return g_gc_defer_pending_panic_depth.load(std::memory_order_acquire);
}

inline void note_gc_defer_pending_panic_signal() noexcept {
    g_gc_defer_pending_panic_signals.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t gc_defer_pending_panic_signals() noexcept {
    return g_gc_defer_pending_panic_signals.load(std::memory_order_relaxed);
}

// Issue #1581: scheduler-facing defer signal. Records fiber/epoch
// provenance so GCCollector and observability can attribute skips.
// Always bumps the signal counter; callers still arm depth separately.
inline void send_defer_gc_signal(std::uint64_t fiber_id, std::uint64_t checkpoint_epoch) noexcept {
    g_gc_defer_last_fiber_id.store(fiber_id, std::memory_order_relaxed);
    g_gc_defer_last_checkpoint_epoch.store(checkpoint_epoch, std::memory_order_relaxed);
    note_gc_defer_pending_panic_signal();
}

[[nodiscard]] inline std::uint64_t gc_defer_last_fiber_id() noexcept {
    return g_gc_defer_last_fiber_id.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t gc_defer_last_checkpoint_epoch() noexcept {
    return g_gc_defer_last_checkpoint_epoch.load(std::memory_order_relaxed);
}

inline void note_gc_sweep_skipped_pending_panic() noexcept {
    g_gc_sweep_skipped_pending_panic.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t gc_sweep_skipped_pending_panic() noexcept {
    return g_gc_sweep_skipped_pending_panic.load(std::memory_order_relaxed);
}

inline void note_gc_request_deferred_pending_panic() noexcept {
    g_gc_request_deferred_pending_panic.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t gc_request_deferred_pending_panic() noexcept {
    return g_gc_request_deferred_pending_panic.load(std::memory_order_relaxed);
}

// True when compact_sweep / collect / request must skip destructive GC.
// Alias kept for scheduler call sites (#1581 AC naming).
[[nodiscard]] inline bool should_defer_compact_for_pending_checkpoint() noexcept {
    return gc_deferred_for_pending_panic();
}

// ── Arena auto-compact notification (Issue #743) ────────────
// Called from arena.ixx when allocate_raw auto-compact fires
// or fiber-safe compact/defrag coordinates a safepoint.
// Wired by CompilerService at startup to bump CompilerMetrics.
using ArenaAutoCompactTriggerFn = void (*)();
inline std::atomic<ArenaAutoCompactTriggerFn> g_arena_auto_compact_trigger{nullptr};

using ArenaFiberSafeCompactFn = void (*)();
inline std::atomic<ArenaFiberSafeCompactFn> g_arena_fiber_safe_compact{nullptr};

inline void notify_auto_compact_trigger() noexcept {
    auto fn = g_arena_auto_compact_trigger.load(std::memory_order_acquire);
    if (fn)
        fn();
}

inline void notify_fiber_safe_compact() noexcept {
    auto fn = g_arena_fiber_safe_compact.load(std::memory_order_acquire);
    if (fn)
        fn();
}

} // namespace aura::gc_hooks

#endif // AURA_CORE_GC_HOOKS_H
