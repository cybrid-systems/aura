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
#include <cstdlib>
#include <string_view>
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
    // Issue #2173: bumped to 512 (max possible) — the effective per-process
    // cap is configurable via AURA_GC_DEFER_MAX_ARMED env var (default 64,
    // clamp 8..512). All arm/release/clear loops bound on gc_defer_max_armed()
    // instead of the constexpr so the env var + test setters take effect.
    constexpr std::size_t kMaxArmedEvaluators = 512;
    struct ArmedEvaluatorEntry {
        void* id = nullptr;
        std::uint32_t depth = 0;
    };
    inline std::array<ArmedEvaluatorEntry, kMaxArmedEvaluators> g_gc_defer_armed_table{};
    inline std::mutex g_gc_defer_armed_mtx{};
    // Issue #2173: per-process override for the effective max-armed cap.
    // 0 = use env-derived default (AURA_GC_DEFER_MAX_ARMED, default 64).
    // Tests set this via set_gc_defer_max_armed_for_test(n) to control
    // the loop bound on arm/release/clear without depending on env.
    inline std::atomic<std::size_t> g_max_armed_override{0};
    // Issue #2173: per-process override for the overflow policy.
    // 0 = use env-derived default (AURA_GC_DEFER_OVERFLOW_POLICY, default
    // ProcessWide). Tests set via set_gc_defer_overflow_policy_for_test(p).
    inline std::atomic<int> g_overflow_policy_override{0};
    // Issue #2338: production lock for overflow policy. When 1, env-empty
    // branch in gc_defer_overflow_policy() returns HardFail (not the legacy
    // ProcessWide silent fallback). Set by apply_production_security_defaults
    // after sandbox resolution; tests can override via the existing
    // set_gc_defer_overflow_policy_for_test(p) setter (which wins over
    // the lock — highest priority).
    inline std::atomic<int> g_production_locked{0};
} // namespace detail

// Issue #2173: overflow policy enum. ProcessWide is the legacy default
// (bump process-wide depth + table_overflow_total on overflow). HardFail
// returns false from try_arm_gc_defer_pending_panic_for without bumping
// process-wide depth — instead bumps dedicated
// g_gc_defer_arm_rejected_overflow_total. Expand is reserved for a
// future Phase 3 heap-backed table; current implementation falls back
// to ProcessWide semantics when Expand is selected.
enum class GcDeferOverflowPolicy : std::uint8_t {
    ProcessWide = 0,
    HardFail = 1,
    Expand = 2,
};

// Issue #2173: configurable per-process max-armed cap. Reads
// AURA_GC_DEFER_MAX_ARMED env var at first call (cached in static),
// clamped to [8, 512]. Tests override via set_gc_defer_max_armed_for_test.
[[nodiscard]] inline std::size_t gc_defer_max_armed() noexcept {
    const auto override = detail::g_max_armed_override.load(std::memory_order_acquire);
    if (override > 0) {
        if (override < 8)
            return 8;
        if (override > 512)
            return 512;
        return override;
    }
    static const std::size_t cached = []() noexcept -> std::size_t {
        const char* env = std::getenv("AURA_GC_DEFER_MAX_ARMED");
        if (!env || !*env)
            return std::size_t{64};
        char* end = nullptr;
        const long v = std::strtol(env, &end, 10);
        if (end == env)
            return std::size_t{64};
        if (v < 8)
            return std::size_t{8};
        if (v > 512)
            return std::size_t{512};
        return static_cast<std::size_t>(v);
    }();
    return cached;
}

// Issue #2173: configurable overflow policy. Reads
// AURA_GC_DEFER_OVERFLOW_POLICY env var at first call. Valid values:
// "ProcessWide" (default, legacy), "HardFail", "Expand" (Phase 3, falls
// back to ProcessWide semantics). Tests override via
// set_gc_defer_overflow_policy_for_test.
[[nodiscard]] inline GcDeferOverflowPolicy gc_defer_overflow_policy() noexcept {
    const auto override = detail::g_overflow_policy_override.load(std::memory_order_acquire);
    if (override > 0) {
        if (override == 1)
            return GcDeferOverflowPolicy::HardFail;
        if (override == 2)
            return GcDeferOverflowPolicy::Expand;
        return GcDeferOverflowPolicy::ProcessWide;
    }
    static const GcDeferOverflowPolicy cached = []() noexcept -> GcDeferOverflowPolicy {
        const char* env = std::getenv("AURA_GC_DEFER_OVERFLOW_POLICY");
        if (!env || !*env) {
            // Issue #2338: production default is HardFail (not ProcessWide
            // silent fallback). Dev / AURA_SANDBOX=off keeps ProcessWide.
            // Per-process production lock is captured at first call (cache
            // initialized lazily after security_defaults settles).
            return detail::g_production_locked.load(std::memory_order_acquire) == 1
                       ? GcDeferOverflowPolicy::HardFail
                       : GcDeferOverflowPolicy::ProcessWide;
        }
        // std::string_view reads up to but not including the null terminator,
        // and operator== does element-wise comparison (no over-read UB that
        // memcmp(env, "HardFail", 10) would trigger when "HardFail" is only
        // 9 chars + null).
        const std::string_view v(env);
        if (v == "HardFail")
            return GcDeferOverflowPolicy::HardFail;
        if (v == "Expand")
            return GcDeferOverflowPolicy::Expand;
        if (v == "ProcessWide")
            return GcDeferOverflowPolicy::ProcessWide;
        return GcDeferOverflowPolicy::ProcessWide;
    }();
    return cached;
}

// Issue #2173: test setters (per-process override). Reset to use env
// default by calling the reset variant (sets override back to 0).
inline void set_gc_defer_max_armed_for_test(std::size_t n) noexcept {
    detail::g_max_armed_override.store(n, std::memory_order_release);
}
inline void reset_gc_defer_max_armed_for_test() noexcept {
    detail::g_max_armed_override.store(0, std::memory_order_release);
}
inline void set_gc_defer_overflow_policy_for_test(GcDeferOverflowPolicy p) noexcept {
    detail::g_overflow_policy_override.store(static_cast<int>(p), std::memory_order_release);
}
inline void reset_gc_defer_overflow_policy_for_test() noexcept {
    detail::g_overflow_policy_override.store(0, std::memory_order_release);
}

// Issue #2338: production lock setters/getters. Set by
// apply_production_security_defaults when sandbox != off. Tests use the
// existing set_gc_defer_overflow_policy_for_test(p) which wins over
// the lock (override > 0 check in gc_defer_overflow_policy).
inline void set_gc_defer_production_locked(bool v) noexcept {
    detail::g_production_locked.store(v ? 1 : 0, std::memory_order_release);
}
[[nodiscard]] inline bool gc_defer_production_locked() noexcept {
    return detail::g_production_locked.load(std::memory_order_acquire) == 1;
}

// Issue #2173: bumped when arm_gc_defer_pending_panic_for (or
// try_arm_gc_defer_pending_panic_for) overflows the bounded table AND
// the active overflow policy is HardFail. ProcessWide overflow bumps
// g_gc_defer_table_overflow_total instead (legacy semantics). Distinct
// counter so operators can distinguish "silent fallback" from
// "arm rejected" via (query:gc-defer-reason-stats).
inline std::atomic<std::uint64_t> g_gc_defer_arm_rejected_overflow_total{0};
[[nodiscard]] inline std::uint64_t gc_defer_arm_rejected_overflow_total() noexcept {
    return g_gc_defer_arm_rejected_overflow_total.load(std::memory_order_relaxed);
}

// Issue #2088: unified GcDeferReason bitmask. Combines all independent
// defer signals (panic, ffi-pin, future render-pin) into a single
// process-wide bitmask. Per-reason depth atomics stay (panic nesting,
// ffi-pin refcount) — the bitmask is just the "any reason armed" flag
// + observability surface for Agent dashboards. Combination bugs
// (FFI pin held + panic released, or reverse) are prevented because
// every consumer consults the single should_defer_destructive_gc().
// Declared here (before arm_gc_defer_pending_panic / arm_ffi_pin_defer
// at lines 186/241) so the bit-toggling calls compile in either order.
enum class GcDeferReason : std::uint32_t {
    None = 0,
    Panic = 1u << 0,
    FfiPin = 1u << 1,
    RenderPin = 1u << 2,    // Issue #2160: frame-level render-critical present defer
    MutationHold = 1u << 3, // Issue #2204: outermost MutationBoundaryGuard hold
};
inline constexpr std::uint32_t kGcDeferReasonNone = 0;
// Issue #2088: process-wide defer-reason bitmask. Atomic bitmask
// (not depth counter) — nested arm/release of the same reason is
// tracked by the per-reason depth atomics above. arm_defer sets the
// bit; release_defer clears the bit (no depth on the bitmask itself).
inline std::atomic<std::uint32_t> g_gc_defer_reasons{0};

// Issue #2088: set/clear a single reason bit. arm_defer / release_defer
// are idempotent — arming an already-set bit is a no-op; releasing
// an unset bit is a no-op. Returns the resulting bitmask.
inline std::uint32_t arm_defer(GcDeferReason r) noexcept {
    const auto bit = static_cast<std::uint32_t>(r);
    if (bit == kGcDeferReasonNone)
        return g_gc_defer_reasons.load(std::memory_order_acquire);
    auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    while (true) {
        const auto next = prev | bit;
        if (next == prev)
            return prev;
        if (g_gc_defer_reasons.compare_exchange_weak(prev, next, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed))
            return next;
    }
}
inline std::uint32_t release_defer(GcDeferReason r) noexcept {
    const auto bit = static_cast<std::uint32_t>(r);
    if (bit == kGcDeferReasonNone)
        return g_gc_defer_reasons.load(std::memory_order_acquire);
    auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    while (true) {
        const auto next = prev & ~bit;
        if (next == prev)
            return prev;
        if (g_gc_defer_reasons.compare_exchange_weak(prev, next, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed))
            return next;
    }
}
// Snapshot the current reason bitmask (Agent-visible).
inline std::uint32_t defer_reasons_snapshot() noexcept {
    return g_gc_defer_reasons.load(std::memory_order_acquire);
}
// Issue #2088: single predicate replacing should_defer_compact_for_pending_checkpoint
// + ffi_pin_defer_active across all consumers (GCCollector::request/collect,
// compact_sweep, render hotpath). Returns true if ANY reason is armed.
[[nodiscard]] inline bool should_defer_destructive_gc() noexcept {
    return g_gc_defer_reasons.load(std::memory_order_acquire) != kGcDeferReasonNone;
}
// Signals from Fiber::yield → block_gc_for_pending_checkpoint
// trampoline (may fire many times per armed window).
inline std::atomic<std::uint64_t> g_gc_defer_pending_panic_signals{0};
// compact_sweep / collect aborted because defer was armed.
inline std::atomic<std::uint64_t> g_gc_sweep_skipped_pending_panic{0};
// Issue #1581: GCCollector::request() refused because defer was armed
// (scheduler-facing early-out before arming gc_in_progress).
inline std::atomic<std::uint64_t> g_gc_request_deferred_pending_panic{0};
// Issue #2086: bumped when arm_gc_defer_pending_panic_for overflows
// the bounded kMaxArmedEvaluators=64 per-evaluator table and falls back
// to process-wide-only arm (under many concurrent evaluators).
inline std::atomic<std::uint64_t> g_gc_defer_table_overflow_total{0};
// Provenance of the last scheduler defer signal (fiber id + checkpoint
// epoch). Written by send_defer_gc_signal; read by tests/metrics.
inline std::atomic<std::uint64_t> g_gc_defer_last_fiber_id{0};
inline std::atomic<std::uint64_t> g_gc_defer_last_checkpoint_epoch{0};

// Issue #2203: steal-complete single entry observability (process-wide).
// Bumped by aura_evaluator_on_steal_complete on every successful
// cross-worker steal (worker.cpp try_steal_from success path).
// g_steal_complete_total: every successful steal that hit the ABI.
// g_gc_defer_orphan_cleared_on_steal_total: sum of depths cleared via
// clear_gc_defer_for_evaluator from the steal-complete entry (orphan
// panic-defer from previous host). Distinct from CompilerMetrics
// gc_defer_orphan_cleared_total (which also tracks resume/migration).
inline std::atomic<std::uint64_t> g_steal_complete_total{0};                   // #2203
inline std::atomic<std::uint64_t> g_gc_defer_orphan_cleared_on_steal_total{0}; // #2203
// Issue #2314: residual GcDeferReason clear invoked from the steal-
// complete entry when defer_reasons_snapshot() != 0 post #2203 panic
// clear. Distinct from g_gc_defer_orphan_cleared_on_steal_total which
// counts cleared Panic depths — this counts steal-complete entries
// that invoked the residual interlock (once per entry, regardless of
// bits cleared).
inline std::atomic<std::uint64_t> g_residual_defer_cleared_on_steal_total{0}; // #2314
[[nodiscard]] inline std::uint64_t steal_complete_total() noexcept {
    return g_steal_complete_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t gc_defer_orphan_cleared_on_steal_total() noexcept {
    return g_gc_defer_orphan_cleared_on_steal_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t residual_defer_cleared_on_steal_total() noexcept {
    return g_residual_defer_cleared_on_steal_total.load(std::memory_order_relaxed);
}

// Issue #2314: shared helper for residual GcDeferReason clear — used by
// BOTH outermost Guard success exit (#2269 Clear policy) AND steal-
// complete orphan interlock (#2314 AC1.2). Idempotent: force_clear_all_
// gc_defer_for_evaluator is itself atomic + CAS-based; calling twice
// does not double-clear or double-bump. Returns the count of cleared
// panic depths + reconciled bits + hold-released flag for counter
// bumping (no caller-side idempotency bookkeeping required).
struct ResidualClearResult {
    std::uint64_t panic_depth_cleared = 0;
    std::uint64_t bits_reconciled = 0;
    bool hold_released = false;
};

inline ResidualClearResult force_clear_residual_defer_for_evaluator(void* evaluator_id) noexcept {
    ResidualClearResult r{};
    const auto fr = force_clear_all_gc_defer_for_evaluator(evaluator_id);
    r.panic_depth_cleared = static_cast<std::uint64_t>(fr.panic_depth_cleared);
    r.bits_reconciled = static_cast<std::uint64_t>(fr.bits_reconciled);
    if (mutation_hold_defer_active()) {
        release_mutation_hold_defer();
        r.hold_released = true;
    }
    // Final reconcile after hold release (hold bit ≠ Panic).
    r.bits_reconciled += reconcile_gc_defer_bits_after_clear();
    return r;
}

// Issue #2088: process-wide arm-count mirrors for Agent dashboards
// (query:gc-defer-reason-stats). Bumped when a reason bit transitions
// 0→set (first nest level), not on every nested arm. Combined any_total
// bumps when the whole bitmask transitions empty→non-empty.
inline std::atomic<std::uint64_t> g_gc_defer_arm_panic_total{0};
inline std::atomic<std::uint64_t> g_gc_defer_arm_ffi_pin_total{0};
inline std::atomic<std::uint64_t> g_gc_defer_arm_render_pin_total{0};
inline std::atomic<std::uint64_t> g_gc_defer_arm_mutation_hold_total{0}; // #2204
inline std::atomic<std::uint64_t> g_gc_defer_any_total{0};

// Note first arm of a reason (bit was clear). Updates process-wide arm
// counters. Called under successful arm_defer 0→set transitions only.
inline void note_defer_reason_armed(GcDeferReason r, std::uint32_t prev_mask) noexcept {
    const auto bit = static_cast<std::uint32_t>(r);
    if (bit == kGcDeferReasonNone)
        return;
    if ((prev_mask & bit) != 0)
        return; // already armed — nested
    if (prev_mask == kGcDeferReasonNone)
        g_gc_defer_any_total.fetch_add(1, std::memory_order_relaxed);
    if (r == GcDeferReason::Panic)
        g_gc_defer_arm_panic_total.fetch_add(1, std::memory_order_relaxed);
    else if (r == GcDeferReason::FfiPin)
        g_gc_defer_arm_ffi_pin_total.fetch_add(1, std::memory_order_relaxed);
    else if (r == GcDeferReason::RenderPin)
        g_gc_defer_arm_render_pin_total.fetch_add(1, std::memory_order_relaxed);
    else if (r == GcDeferReason::MutationHold)
        g_gc_defer_arm_mutation_hold_total.fetch_add(1, std::memory_order_relaxed);
}

inline void arm_gc_defer_pending_panic() noexcept {
    g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
    // Issue #2088: also toggle the Panic bit on the unified reason
    // bitmask. Idempotent — arm_defer is a no-op when the bit is
    // already set. Per-reason depth stays for nesting observability.
    const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    (void)arm_defer(GcDeferReason::Panic);
    note_defer_reason_armed(GcDeferReason::Panic, prev);
}

inline void release_gc_defer_pending_panic() noexcept {
    auto prev = g_gc_defer_pending_panic_depth.load(std::memory_order_relaxed);
    while (prev > 0) {
        const auto next = prev - 1;
        if (g_gc_defer_pending_panic_depth.compare_exchange_weak(
                prev, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Issue #2088: clear Panic bit only when depth hits 0 (last
            // nested release). Intermediate releases keep the bit set so
            // should_defer_destructive_gc() still defers.
            if (next == 0)
                (void)release_defer(GcDeferReason::Panic);
            return;
        }
    }
    // Depth already 0 — ensure bit is clear (idempotent safety).
    (void)release_defer(GcDeferReason::Panic);
}

// Issue #2002: per-evaluator arm. Updates the per-evaluator table
// AND the process-wide aggregate depth (so existing gc_deferred_for_pending_panic()
// checks continue to work without changes). If evaluator_id is null,
// falls back to the legacy process-wide increment (no discriminator).
// Issue #2173: loop bound on gc_defer_max_armed() (env-configurable,
// default 64). Overflow path dispatches on gc_defer_overflow_policy():
//   - ProcessWide (legacy): bump process-wide depth + g_gc_defer_table_overflow_total
//   - HardFail: bump g_gc_defer_arm_rejected_overflow_total, no process depth bump
inline void arm_gc_defer_pending_panic_for(void* evaluator_id) noexcept {
    if (!evaluator_id) {
        arm_gc_defer_pending_panic();
        return;
    }
    const auto cap = gc_defer_max_armed();
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (std::size_t i = 0; i < cap; ++i) {
        auto& e = detail::g_gc_defer_armed_table[i];
        if (e.id == evaluator_id) {
            ++e.depth;
            g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
            // Issue #2088: keep Panic bit set for per-eval nested arm.
            const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
            (void)arm_defer(GcDeferReason::Panic);
            note_defer_reason_armed(GcDeferReason::Panic, prev);
            return;
        }
    }
    for (std::size_t i = 0; i < cap; ++i) {
        auto& e = detail::g_gc_defer_armed_table[i];
        if (e.id == nullptr) {
            e.id = evaluator_id;
            e.depth = 1;
            g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
            // Issue #2088: first per-eval slot → arm Panic bit.
            const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
            (void)arm_defer(GcDeferReason::Panic);
            note_defer_reason_armed(GcDeferReason::Panic, prev);
            return;
        }
    }
    // Overflow: dispatch on policy (Issue #2173).
    const auto policy = gc_defer_overflow_policy();
    if (policy == GcDeferOverflowPolicy::HardFail) {
        // HardFail: don't bump process-wide depth; bump dedicated counter.
        // Do not arm the Panic bit either — defer is NOT actually armed,
        // caller must observe try_arm_gc_defer_pending_panic_for return
        // value (or check this counter) before assuming GC deferral.
        g_gc_defer_arm_rejected_overflow_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // ProcessWide (default + Expand fallback): legacy behavior. Bump
    // process-wide depth + table_overflow_total + arm Panic bit.
    g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
    g_gc_defer_table_overflow_total.fetch_add(1, std::memory_order_relaxed);
    const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    (void)arm_defer(GcDeferReason::Panic);
    note_defer_reason_armed(GcDeferReason::Panic, prev);
}

// Issue #2173: try-arm variant. Same semantics as arm_gc_defer_pending_panic_for
// except returns bool: true = armed (per-eval slot or ProcessWide overflow
// fallback), false = rejected by HardFail overflow policy. Caller MUST
// check the return value when HardFail policy is active — GC deferral is
// NOT armed on false return.
[[nodiscard]] inline bool try_arm_gc_defer_pending_panic_for(void* evaluator_id) noexcept {
    if (!evaluator_id) {
        arm_gc_defer_pending_panic();
        return true;
    }
    const auto cap = gc_defer_max_armed();
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (std::size_t i = 0; i < cap; ++i) {
        auto& e = detail::g_gc_defer_armed_table[i];
        if (e.id == evaluator_id) {
            ++e.depth;
            g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
            const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
            (void)arm_defer(GcDeferReason::Panic);
            note_defer_reason_armed(GcDeferReason::Panic, prev);
            return true;
        }
    }
    for (std::size_t i = 0; i < cap; ++i) {
        auto& e = detail::g_gc_defer_armed_table[i];
        if (e.id == nullptr) {
            e.id = evaluator_id;
            e.depth = 1;
            g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
            const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
            (void)arm_defer(GcDeferReason::Panic);
            note_defer_reason_armed(GcDeferReason::Panic, prev);
            return true;
        }
    }
    // Overflow: dispatch on policy.
    const auto policy = gc_defer_overflow_policy();
    if (policy == GcDeferOverflowPolicy::HardFail) {
        g_gc_defer_arm_rejected_overflow_total.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_gc_defer_pending_panic_depth.fetch_add(1, std::memory_order_acq_rel);
    g_gc_defer_table_overflow_total.fetch_add(1, std::memory_order_relaxed);
    const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    (void)arm_defer(GcDeferReason::Panic);
    note_defer_reason_armed(GcDeferReason::Panic, prev);
    return true;
}
// Issue #2005: explicit ffi-pin defer — increments while any
// (ffi:pin-buffer) primitive holds a LifetimePin for an FFI buffer that
// the render hotpath / MutationBoundary lightweight path depends on.
// compact_sweep / GCCollector consult ffi_pin_defer_active() before
// destructive reclaim; if true, defer (return empty CompactSweepResult +
// bump ffi_defer_because_pin_total in CompilerMetrics).
inline std::atomic<std::uint32_t> g_ffi_pin_defer_depth{0};
inline void arm_ffi_pin_defer() noexcept {
    g_ffi_pin_defer_depth.fetch_add(1, std::memory_order_acq_rel);
    // Issue #2088: also toggle FfiPin bit on the unified bitmask.
    const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    (void)arm_defer(GcDeferReason::FfiPin);
    note_defer_reason_armed(GcDeferReason::FfiPin, prev);
}
inline void release_ffi_pin_defer() noexcept {
    auto prev = g_ffi_pin_defer_depth.load(std::memory_order_relaxed);
    while (prev > 0) {
        const auto next = prev - 1;
        if (g_ffi_pin_defer_depth.compare_exchange_weak(prev, next, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
            // Issue #2088: clear FfiPin bit only on last release (depth→0).
            if (next == 0)
                (void)release_defer(GcDeferReason::FfiPin);
            return;
        }
    }
    // Depth already 0 — ensure bit is clear.
    (void)release_defer(GcDeferReason::FfiPin);
}
[[nodiscard]] inline bool ffi_pin_defer_active() noexcept {
    return g_ffi_pin_defer_depth.load(std::memory_order_acquire) > 0;
}
// Issue #2088 / #2005: nesting depth accessor (Agent + tests).
[[nodiscard]] inline std::uint32_t ffi_pin_defer_depth() noexcept {
    return g_ffi_pin_defer_depth.load(std::memory_order_acquire);
}

// Issue #2160: frame-level RenderPin defer (distinct from buffer-level FfiPin).
// Nested via process-wide depth so multi-thread concurrent present stays armed
// until the last exit. Wired from arena_policy::enter/exit_render_hotpath so
// Soft compact soft-gate + unified should_defer_destructive_gc share one enter.
inline std::atomic<std::uint32_t> g_render_pin_defer_depth{0};
// request()/collect()/compact_sweep deferred because RenderPin was armed.
inline std::atomic<std::uint64_t> g_gc_request_deferred_render_total{0};
inline std::atomic<std::uint64_t> g_gc_sweep_skipped_render_total{0};
inline std::atomic<std::uint64_t> g_defer_because_render_total{0};

inline void arm_render_pin_defer() noexcept {
    g_render_pin_defer_depth.fetch_add(1, std::memory_order_acq_rel);
    const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    (void)arm_defer(GcDeferReason::RenderPin);
    note_defer_reason_armed(GcDeferReason::RenderPin, prev);
}
inline void release_render_pin_defer() noexcept {
    auto prev = g_render_pin_defer_depth.load(std::memory_order_relaxed);
    while (prev > 0) {
        const auto next = prev - 1;
        if (g_render_pin_defer_depth.compare_exchange_weak(prev, next, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed)) {
            if (next == 0)
                (void)release_defer(GcDeferReason::RenderPin);
            return;
        }
    }
    (void)release_defer(GcDeferReason::RenderPin);
}
[[nodiscard]] inline bool render_pin_defer_active() noexcept {
    return g_render_pin_defer_depth.load(std::memory_order_acquire) > 0;
}
[[nodiscard]] inline std::uint32_t render_pin_defer_depth() noexcept {
    return g_render_pin_defer_depth.load(std::memory_order_acquire);
}
inline void note_gc_request_deferred_render() noexcept {
    g_gc_request_deferred_render_total.fetch_add(1, std::memory_order_relaxed);
    g_defer_because_render_total.fetch_add(1, std::memory_order_relaxed);
}
inline void note_gc_sweep_skipped_render() noexcept {
    g_gc_sweep_skipped_render_total.fetch_add(1, std::memory_order_relaxed);
    g_defer_because_render_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2204: outermost MutationBoundaryGuard MutationHold defer.
// Process-wide depth so concurrent outermost Guards on distinct
// Evaluators keep the bit set until the last outer release. Nested
// guards must NOT call arm (outer already holds the bit / depth).
// Wired from MutationBoundaryGuard AcquireTag ctor / dtor only —
// not per-eval (steal clear leaves process bit alone; #2086).
inline std::atomic<std::uint32_t> g_mutation_hold_defer_depth{0};
inline std::atomic<std::uint64_t> g_gc_request_deferred_mutation_hold_total{0};
inline std::atomic<std::uint64_t> g_gc_sweep_skipped_mutation_hold_total{0};
inline std::atomic<std::uint64_t> g_defer_because_mutation_hold_total{0};

inline void arm_mutation_hold_defer() noexcept {
    g_mutation_hold_defer_depth.fetch_add(1, std::memory_order_acq_rel);
    const auto prev = g_gc_defer_reasons.load(std::memory_order_relaxed);
    (void)arm_defer(GcDeferReason::MutationHold);
    note_defer_reason_armed(GcDeferReason::MutationHold, prev);
}
inline void release_mutation_hold_defer() noexcept {
    auto prev = g_mutation_hold_defer_depth.load(std::memory_order_relaxed);
    while (prev > 0) {
        const auto next = prev - 1;
        if (g_mutation_hold_defer_depth.compare_exchange_weak(prev, next, std::memory_order_acq_rel,
                                                              std::memory_order_relaxed)) {
            if (next == 0)
                (void)release_defer(GcDeferReason::MutationHold);
            return;
        }
    }
    (void)release_defer(GcDeferReason::MutationHold);
}
[[nodiscard]] inline bool mutation_hold_defer_active() noexcept {
    return g_mutation_hold_defer_depth.load(std::memory_order_acquire) > 0;
}
[[nodiscard]] inline std::uint32_t mutation_hold_defer_depth() noexcept {
    return g_mutation_hold_defer_depth.load(std::memory_order_acquire);
}
inline void note_gc_request_deferred_mutation_hold() noexcept {
    g_gc_request_deferred_mutation_hold_total.fetch_add(1, std::memory_order_relaxed);
    g_defer_because_mutation_hold_total.fetch_add(1, std::memory_order_relaxed);
}
inline void note_gc_sweep_skipped_mutation_hold() noexcept {
    g_gc_sweep_skipped_mutation_hold_total.fetch_add(1, std::memory_order_relaxed);
    g_defer_because_mutation_hold_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2002: per-evaluator release. Decrements the entry's depth;
// when the entry's depth hits 0, clears the id slot. Always decrements
// the process-wide aggregate too (so the aggregate matches what was
// armed, even if the per-evaluator slot is no longer found).
// Issue #2173: loop bound on gc_defer_max_armed() (only iterates slots
// that the active cap allows, matching the arm-loop bound).
inline void release_gc_defer_pending_panic_for(void* evaluator_id) noexcept {
    if (!evaluator_id) {
        release_gc_defer_pending_panic();
        return;
    }
    const auto cap = gc_defer_max_armed();
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (std::size_t i = 0; i < cap; ++i) {
        auto& e = detail::g_gc_defer_armed_table[i];
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
        const auto next = prev - 1;
        if (g_gc_defer_pending_panic_depth.compare_exchange_weak(
                prev, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Issue #2088: clear Panic bit when process-wide depth hits 0.
            if (next == 0)
                (void)release_defer(GcDeferReason::Panic);
            break;
        }
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
// Issue #2173: loop bound on gc_defer_max_armed().
[[nodiscard]] inline bool gc_deferred_for_evaluator(void* evaluator_id) noexcept {
    if (!evaluator_id)
        return false;
    const auto cap = gc_defer_max_armed();
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (std::size_t i = 0; i < cap; ++i) {
        const auto& e = detail::g_gc_defer_armed_table[i];
        if (e.id == evaluator_id)
            return e.depth > 0;
    }
    return false;
}

// Issue #2002: clear any deferred depth belonging to evaluator_id
// (used by fiber-steal path to orphan stale depth from the previous
// host). Returns the # of slots that were cleared (0 if none).
// Issue #2173: loop bound on gc_defer_max_armed(). Process-wide depth
// decrement + Panic bit clear on 0 remain correct for all slots that
// were actually table-backed (HardFail overflow path doesn't arm the
// bit, so there's nothing to clear for those slots).
[[nodiscard]] inline std::uint32_t clear_gc_defer_for_evaluator(void* evaluator_id) noexcept {
    if (!evaluator_id)
        return 0;
    std::uint32_t cleared = 0;
    const auto cap = gc_defer_max_armed();
    std::lock_guard<std::mutex> lock(detail::g_gc_defer_armed_mtx);
    for (std::size_t i = 0; i < cap; ++i) {
        auto& e = detail::g_gc_defer_armed_table[i];
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
                    prev, target, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                // Issue #2088: clear Panic bit when process-wide depth hits 0
                // after steal orphan-clear (no remaining armed panics).
                if (target == 0)
                    (void)release_defer(GcDeferReason::Panic);
                break;
            }
        }
    }
    return cleared;
}

// Issue #2296: process-wide bit-vs-depth reconcile after per-eval clear.
// Multi-eval + high-frequency steal can leave the Panic bit set while
// process depth has already drained to 0 (arm/release race lag). Returns
// the number of bits force-cleared (0 = already consistent).
inline std::atomic<std::uint64_t> g_gc_defer_bit_reconcile_total{0};
[[nodiscard]] inline std::uint64_t gc_defer_bit_reconcile_total() noexcept {
    return g_gc_defer_bit_reconcile_total.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint32_t reconcile_gc_defer_bits_after_clear() noexcept {
    std::uint32_t fixed = 0;
    // Panic bit must track process-wide panic depth.
    if (g_gc_defer_pending_panic_depth.load(std::memory_order_acquire) == 0) {
        const auto mask = g_gc_defer_reasons.load(std::memory_order_acquire);
        if ((mask & static_cast<std::uint32_t>(GcDeferReason::Panic)) != 0) {
            (void)release_defer(GcDeferReason::Panic);
            g_gc_defer_bit_reconcile_total.fetch_add(1, std::memory_order_relaxed);
            ++fixed;
        }
    }
    return fixed;
}

// Issue #2296: Phase-5 Clear / multi-eval force-clear for one evaluator.
// Clears per-eval panic table + process depth, then reconciles the
// process-wide bitmask. MutationHold is process-wide (not per-eval) —
// caller re-releases hold separately when this eval owned the residual.
struct ForceClearGcDeferResult {
    std::uint32_t panic_depth_cleared = 0;
    std::uint32_t bits_reconciled = 0;
};

[[nodiscard]] inline ForceClearGcDeferResult
force_clear_all_gc_defer_for_evaluator(void* evaluator_id) noexcept {
    ForceClearGcDeferResult r{};
    r.panic_depth_cleared = clear_gc_defer_for_evaluator(evaluator_id);
    // Always reconcile even when cleared==0: multi-eval lag can leave
    // Panic bit set after another evaluator already drained depth.
    r.bits_reconciled = reconcile_gc_defer_bits_after_clear();
    return r;
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
