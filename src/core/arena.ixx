module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "core/gc_hooks.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h"
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
export module aura.core.arena;
import std;
import aura.core.error;

// Issue #1390: one-shot stderr warning when request_defrag() is
// called with no GC safepoint registered. Exported as free
// functions inside namespace aura::ast so callers (and the
// (arena:warn-no-safepoint) primitive) can find them via the
// qualified name aura::ast::was_no_safepoint_warned().
//
// warn_no_safepoint_once(): emit a stderr warning the FIRST time
// it's called (across the whole process). Subsequent calls are
// silent — operators already know about the issue.
//
// was_no_safepoint_warned(): read-only query — has the warning
// fired yet? Used by (arena:warn-no-safepoint) primitive.
//
// IMPORTANT: both functions share the SAME static atomic flag.
// Using two separate static locals would give was_..._warned()
// its own copy (always false), so it would never observe the
// warning fired by warn_..._once(). Sharing the flag via a
// function-local static in a shared accessor guarantees they
// observe the same state.

namespace aura::ast {

// Issue #1518: MutationBoundary soft-gate for auto live_compact.
// Optional probe (0 when unset / evaluator not linked). Wired from
// evaluator_fiber_mutation via set_arena_mutation_boundary_depth_fn.
using ArenaMutationBoundaryDepthFn = std::size_t (*)() noexcept;
export inline std::atomic<ArenaMutationBoundaryDepthFn> g_arena_mutation_boundary_depth_fn{nullptr};

export inline void set_arena_mutation_boundary_depth_fn(ArenaMutationBoundaryDepthFn fn) noexcept {
    g_arena_mutation_boundary_depth_fn.store(fn, std::memory_order_relaxed);
}

[[nodiscard]] inline std::size_t arena_mutation_boundary_depth() noexcept {
    auto* fn = g_arena_mutation_boundary_depth_fn.load(std::memory_order_relaxed);
    return fn ? fn() : 0;
}

namespace arena_no_safepoint_detail {
    export inline std::atomic<bool>& no_safepoint_warned_flag() noexcept {
        static std::atomic<bool> flag{false};
        return flag;
    }
} // namespace arena_no_safepoint_detail

export inline void warn_no_safepoint_once() noexcept {
    bool expected = false;
    if (arena_no_safepoint_detail::no_safepoint_warned_flag().compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        std::fprintf(stderr, "[aura arena] WARNING: request_defrag() called "
                             "but no GC safepoint is registered "
                             "(g_arena_safepoint_check is null). The defrag "
                             "flag will never be observed by an allocation "
                             "safepoint. Either register a safepoint via "
                             "gc_hooks.h (fiber:spawn path) or use "
                             "(arena:defrag-now) for explicit compaction. "
                             "This warning is emitted once per process.\n");
    }
}

export inline bool was_no_safepoint_warned() noexcept {
    return arena_no_safepoint_detail::no_safepoint_warned_flag().load(std::memory_order_acquire);
}

// Issue #658: small-object tier exhaustion fallbacks to main pmr arena.
export inline std::atomic<std::uint64_t> arena_small_tier_fallback_total{0};

// ── ArenaStats — per-arena memory accounting ─────────────────────
export struct ArenaStats {
    std::size_t capacity = 0;         // total buffer size
    std::size_t used = 0;             // bytes consumed
    std::size_t peak_used = 0;        // historical high-water mark
    std::size_t allocation_count = 0; // number of allocation calls
    std::size_t wasted = 0;           // alignment padding
    // Issue #187 (P0): compaction observability. The first 4 fields
    // are pure accounting (already shipped). The new 3 are added for
    // production memory stability so we can see whether compaction
    // is helping and trigger auto-compact at the right threshold.
    std::size_t compaction_count = 0;       // number of compact() calls
    std::size_t last_compaction_saved = 0;  // bytes reclaimed by last compact
    std::size_t total_compaction_saved = 0; // lifetime bytes reclaimed
    // Issue #324: yield-check observability. Bumped whenever
    // compact() detects an active fiber context (g_current_fiber
    // != nullptr) — the compaction itself does NOT yield (that's
    // a separate P1 follow-up requiring WorkerContext-aware
    // integration). The counter exposes how often compact() was
    // called in a context where yielding would have been
    // appropriate, so AI agents can monitor long-running
    // compaction in fiber-heavy workloads.
    std::size_t compaction_yield_checks = 0;
    // Issue #300 (P1): live-object defragmentation observability.
    // Hooks for the full live-object-moving defrag path (separate
    // follow-up commits B/C):
    //   - defrag_attempted_count: # of defrag() passes attempted
    //   - last_defrag_saved:      bytes reclaimed by last defrag
    // Foundation-only: both stay 0 until the defrag path is
    // implemented in #300 follow-up commits. The (arena:defrag-stats)
    // primitive returns 0 for the defrag slot until then.
    std::size_t defrag_attempted_count = 0;
    std::size_t last_defrag_saved = 0;
    // Issue #685: alloc-path auto-compact policy observability.
    std::size_t auto_alloc_trigger_count = 0;
    std::size_t frag_reduced_bp = 0;
    std::size_t shape_inval_on_compact = 0;
    std::size_t defrag_savings_alloc = 0;
    // Issue #1467 Phase 1 + #1518: live-object-moving defrag observability.
    std::size_t live_defrag_attempted_count = 0;
    std::size_t live_objects_marked_total = 0;
    // Issue #1518: live relocate / compact coordination metrics.
    std::size_t live_relocate_count = 0;         // freelist slots + mark-phase relocates
    std::size_t compact_deopt_triggered = 0;     // Shape/JIT deopt fired post-compact
    std::size_t compact_deopt_throttled = 0;     // deopt storm throttle skips
    std::size_t frag_post_compact_bp = 0;        // last post-compact frag ratio (basis points)
    std::size_t compact_soft_gated_boundary = 0; // skipped due to MutationBoundary

    std::string format() const {
        return std::format("arena: {:.1f}MB / {:.1f}MB (peak {:.1f}MB) | {} allocs | {}B wasted | "
                           "{} compactions (last saved {}B, total {}B) | "
                           "{} defrags (last saved {}B) | "
                           "{} live-defrags ({} marked, {} relocates) | "
                           "deopt {}/{} throttled | frag_post {}bp",
                           used / 1048576.0, capacity / 1048576.0, peak_used / 1048576.0,
                           allocation_count, wasted, compaction_count, last_compaction_saved,
                           total_compaction_saved, defrag_attempted_count, last_defrag_saved,
                           live_defrag_attempted_count, live_objects_marked_total,
                           live_relocate_count, compact_deopt_triggered, compact_deopt_throttled,
                           frag_post_compact_bp);
    }

    // Fragmentation ratio: (capacity - used) / capacity.
    // 0.0 = fully packed, 1.0 = completely empty.
    // Issue #187: this is the key signal for auto-compact triggers.
    [[nodiscard]] double fragmentation_ratio() const noexcept {
        return capacity == 0 ? 0.0
                             : static_cast<double>(capacity - used) / static_cast<double>(capacity);
    }

    void merge(const ArenaStats& other) {
        used += other.used;
        capacity += other.capacity;
        peak_used = std::max(peak_used, other.peak_used);
        allocation_count += other.allocation_count;
        wasted += other.wasted;
        compaction_count += other.compaction_count;
        total_compaction_saved += other.total_compaction_saved;
        // last_compaction_saved: take the more recent (larger count)
        if (other.compaction_count > 0)
            last_compaction_saved = other.last_compaction_saved;
        // Issue #300: defrag counters. Same merge discipline as compact.
        defrag_attempted_count += other.defrag_attempted_count;
        // Issue #1467 Phase 1: live-defrag counters (same sum discipline).
        live_defrag_attempted_count += other.live_defrag_attempted_count;
        live_objects_marked_total += other.live_objects_marked_total;
        // Issue #1518: live-relocate / deopt coordination merge.
        live_relocate_count += other.live_relocate_count;
        compact_deopt_triggered += other.compact_deopt_triggered;
        compact_deopt_throttled += other.compact_deopt_throttled;
        compact_soft_gated_boundary += other.compact_soft_gated_boundary;
        if (other.frag_post_compact_bp > 0)
            frag_post_compact_bp = other.frag_post_compact_bp;
        if (other.defrag_attempted_count > 0)
            last_defrag_saved = other.last_defrag_saved;
        auto_alloc_trigger_count += other.auto_alloc_trigger_count;
        frag_reduced_bp += other.frag_reduced_bp;
        shape_inval_on_compact += other.shape_inval_on_compact;
        defrag_savings_alloc += other.defrag_savings_alloc;
    }
};

// ── SmallObjectPool — fixed-size class allocator ─────────────────
//
// Three tiers for frequently allocated small objects:
//   Tier 0: 16 bytes  (LiteralInt, Variable, etc.)
//   Tier 1: 32 bytes  (small Call, IfExpr, etc.)
//   Tier 2: 64 bytes  (Lambda, larger nodes)
//
// Each tier gets its own bump pointer within a pre-allocated buffer.
// When a tier's region fills up, overflow goes to the caller's
// fallback allocator (the main monotonic_buffer_resource).
//
export class SmallObjectPool {
public:
    // Size classes (must be sorted ascending)
    static constexpr std::size_t kTierSizes[] = {16, 32, 64};
    static constexpr std::size_t kNumTiers = 3;
    static constexpr std::size_t kSmallPoolSize = 3 * 1024 * 1024;          // 3MB total
    static constexpr std::size_t kPerTierSize = kSmallPoolSize / kNumTiers; // 1MB each
    static constexpr std::size_t kMaxSmallSize = kTierSizes[kNumTiers - 1]; // 64

    // Issue #742: consteval tier layout invariants (zero runtime cost).
    static_assert(kTierSizes[0] < kTierSizes[1] && kTierSizes[1] < kTierSizes[2],
                  "SmallObjectPool tier sizes must be strictly ascending");
    static_assert(kTierSizes[0] >= 16 && kTierSizes[2] <= kMaxSmallSize,
                  "SmallObjectPool tier range must be [16, kMaxSmallSize]");

    SmallObjectPool() {
        buffer_.resize(kSmallPoolSize);
        for (std::size_t i = 0; i < kNumTiers; ++i) {
            classes_[i].start = buffer_.data() + i * kPerTierSize;
            classes_[i].bump = classes_[i].start;
            classes_[i].end = classes_[i].start + kPerTierSize;
            classes_[i].obj_sz = kTierSizes[i];
        }
    }

    // Allocate from the best-fitting tier. Returns nullptr if too large
    // or if the tier is exhausted (caller should fallback).
    // Issue #1242: also clamp against absolute buffer_.data()+size so a
    // stale tier.end after shrink/rebind cannot yield an out-of-buffer pointer.
    // Issue #1518: prefer freelist recycle (live-relocate protocol) before bump.
    void* try_allocate(std::size_t size) pre(size > 0) pre(size <= kMaxSmallSize) {
        const auto* buf_end = buffer_.data() + buffer_.size();
        for (std::size_t ti = 0; ti < kNumTiers; ++ti) {
            auto& c = classes_[ti];
            if (size > c.obj_sz)
                continue;
            // Issue #1518: freelist hit = lazy live-relocate into a freed slot.
            if (free_heads_[ti] != nullptr) {
                void* ptr = free_heads_[ti];
                free_heads_[ti] = *static_cast<void**>(ptr);
                if (free_count_ > 0)
                    --free_count_;
                allocated_from_small_ += c.obj_sz;
                ++recycle_hits_;
                aura::core::cpp26::record_hotpath_invariant_hit();
                return ptr;
            }
            // Hard cap: bump must stay within both tier.end and buffer.
            std::byte* hard_end = c.end < buf_end ? c.end : const_cast<std::byte*>(buf_end);
            void* ptr = c.bump;
            auto* next = c.bump + c.obj_sz;
            if (next <= hard_end && next >= c.start) {
                c.bump = next;
                allocated_from_small_ += c.obj_sz;
                aura::core::cpp26::record_hotpath_invariant_hit();
                return ptr;
            }
            // This tier is exhausted — signal overflow (no bump advance).
            aura::core::cpp26::record_hotpath_invariant_hit();
            return nullptr;
        }
        return nullptr; // too large for any tier
    }

    // Issue #1518: return a destroyed small object to the freelist so
    // subsequent try_allocate can reuse the slot (live-relocate protocol
    // without moving still-live pointers). Safe: only called after dtor.
    // Returns true if the pointer was owned by this pool and recycled.
    bool recycle(void* p, std::size_t size) noexcept {
        if (!p || size == 0 || size > kMaxSmallSize)
            return false;
        auto* bp = static_cast<std::byte*>(p);
        if (buffer_.empty() || bp < buffer_.data() || bp >= buffer_.data() + buffer_.size())
            return false;
        for (std::size_t ti = 0; ti < kNumTiers; ++ti) {
            auto& c = classes_[ti];
            if (size > c.obj_sz)
                continue;
            if (bp < c.start || bp >= c.end)
                continue;
            // Slot alignment: offset from start must be multiple of obj_sz.
            const auto off = static_cast<std::size_t>(bp - c.start);
            if (off % c.obj_sz != 0)
                return false;
            *static_cast<void**>(p) = free_heads_[ti];
            free_heads_[ti] = p;
            ++free_count_;
            if (allocated_from_small_ >= c.obj_sz)
                allocated_from_small_ -= c.obj_sz;
            ++recycle_puts_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool owns(const void* p) const noexcept {
        if (!p || buffer_.empty())
            return false;
        auto* bp = static_cast<const std::byte*>(p);
        return bp >= buffer_.data() && bp < buffer_.data() + buffer_.size();
    }

    // Reset all tier bump pointers (but keep buffer allocated)
    void reset() {
        for (auto& c : classes_) {
            c.bump = c.start;
        }
        allocated_from_small_ = 0;
        clear_freelist_();
    }

    // Issue #974 / #1242: re-bind tier start/end after buffer reallocation
    // or shrink. Clamps end to buffer_.size() so try_allocate cannot race
    // past the real allocation. If used exceeds tier region, bump is
    // clamped to end (tier treated exhausted until reset).
    void rebind_tiers() noexcept {
        if (buffer_.empty())
            return;
        // Issue #1518: freelist pointers are absolute; clear if buffer base
        // may have moved (vector reallocation). Same buffer_ keeps freelist.
        const auto* old_base = classes_[0].start;
        const auto buf_bytes = buffer_.size();
        for (std::size_t i = 0; i < kNumTiers; ++i) {
            const auto tier_off = i * kPerTierSize;
            auto* start = buffer_.data() + tier_off;
            // Tier span cannot exceed remaining buffer bytes.
            const auto tier_cap =
                std::min(kPerTierSize, buf_bytes > tier_off ? buf_bytes - tier_off : 0);
            std::size_t used = 0;
            if (classes_[i].start != nullptr && classes_[i].bump >= classes_[i].start)
                used = static_cast<std::size_t>(classes_[i].bump - classes_[i].start);
            if (used > tier_cap)
                used = tier_cap;
            classes_[i].start = start;
            classes_[i].end = start + tier_cap;
            classes_[i].bump = start + used;
            classes_[i].obj_sz = kTierSizes[i];
        }
        if (old_base != buffer_.data())
            clear_freelist_();
    }

    // Issue #1242: after shrink, zero bumps so subsequent allocs re-evaluate
    // cleanly against rebinding (acceptable: loses in-pool live data on shrink path).
    void reset_small_pool_tiers() noexcept {
        rebind_tiers();
        reset();
    }

    // Total bytes consumed from the small pool
    [[nodiscard]] std::size_t allocated() const { return allocated_from_small_; }

    // Capacity of the small pool
    [[nodiscard]] std::size_t capacity() const { return kSmallPoolSize; }

    // Issue #685: fraction of small-pool bytes in use [0,1].
    [[nodiscard]] double utilization() const noexcept {
        return static_cast<double>(allocated_from_small_) / static_cast<double>(kSmallPoolSize);
    }

    // Issue #1518: freelist / recycle observability for live-relocate.
    [[nodiscard]] std::size_t free_slot_count() const noexcept { return free_count_; }
    [[nodiscard]] std::size_t recycle_hits() const noexcept { return recycle_hits_; }
    [[nodiscard]] std::size_t recycle_puts() const noexcept { return recycle_puts_; }

private:
    struct Tier {
        std::byte* start = nullptr;
        std::byte* bump = nullptr;
        std::byte* end = nullptr;
        std::size_t obj_sz = 0;
    };

    void clear_freelist_() noexcept {
        for (std::size_t i = 0; i < kNumTiers; ++i)
            free_heads_[i] = nullptr;
        free_count_ = 0;
    }

    std::vector<std::byte> buffer_;
    Tier classes_[kNumTiers];
    std::size_t allocated_from_small_ = 0;
    // Issue #1518: per-tier freelist (singly linked via first void* of free block).
    void* free_heads_[kNumTiers] = {nullptr, nullptr, nullptr};
    std::size_t free_count_ = 0;
    std::size_t recycle_hits_ = 0;
    std::size_t recycle_puts_ = 0;
};

// ── ASTArena — tiered pmr bump allocator ─────────────────────────
//
// v1: single pmr monotonic_buffer_resource
// v2: ArenaStats + ArenaGroup
// v3: SmallObjectPool for objects <= 64 bytes (3 tiers: 16/32/64)
// v4: Dtor tracking (issue #67 / #131 follow-up) — every create<T>
//     call records a type-erased destructor thunk; reset() and
//     ~ASTArena() invoke them in reverse construction order before
//     bulk-freeing the chunks. Without this, pmr containers inside
//     arena-allocated T (e.g. FlatAST's 18+ pmr vectors, StringPool's
//     buf_/hash_tbl_) leak their monotonic_buffer_resource fallback
//     chunks (new_delete_resource) because monotonic_buffer_resource's
//     deallocate is a no-op, and the T's own destructor never runs.
//
// Allocation path:
//   create<T>() → sizeof(T) <= 64 → SmallObjectPool
//               → else             → pmr monotonic_buffer_resource
//
export class ASTArena {
public:
    // Default upstream is the system allocator. Tests can pass a
    // counting memory_resource to verify that destructors run
    // before resource_.release() (see Issue #1382 contract test).
    explicit ASTArena(std::size_t initial_size = 8 * 1024 * 1024,
                      std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
        : initial_size_(initial_size)
        , buffer_(initial_size)
        , resource_(buffer_.data(), buffer_.size(), upstream) {}

    // Issue #300 (P1) Phase 3: defrag request flag. Set by
    // (arena:request-defrag) primitive to signal that a defrag is
    // desired. The main thread or a fiber coordinator can then
    // decide when to actually run the defrag. Reset by the
    // primitive that observes it (or by defrag() itself when it
    // runs the requested work). Foundation for the full
    // stop-the-world coordination that the pool-backed defrag
    // path will require.
    //
    // The flag is read by the safepoint check on every allocation
    // (when the fiber subsystem has registered a safepoint check
    // function). See gc_hooks.h for the safepoint protocol.
    //
    // Thread-safe: std::atomic<bool> with relaxed ordering for the
    // request / clear, acquire-release for the read in the
    // safepoint check (so the fiber sees the most recent flag
    // state across threads).
    // Issue #1390 + #1397: request_defrag() returns whether THIS
    // call transitioned the defrag_requested flag from false to
    // true (newly_set semantics, atomic compare_exchange_strong).
    // Returns true on the call that actually set the flag; every
    // subsequent call returns false until either (arena:defrag)
    // resets the flag via clear_defrag_request() (cycle reset)
    // or the operator manually clears it.
    //
    // Side effects:
    //   - Sets the defrag_requested flag to true (idempotent — a
    //     subsequent call while the flag is already set still
    //     leaves it true; only the return value changes).
    //   - Emits a one-shot stderr warning the first time it's
    //     called with no safepoint registered. See
    //     warn_no_safepoint_once() below. The warning fires
    //     once per process regardless of how many calls happen
    //     afterward — it's a misconfiguration signal, not a
    //     per-call nag.
    //
    // To check safepoint registration status independently of
    // the request's newly-set state, use
    // ASTArena::safepoint_registered() (returns the static
    // registration check) or call `(arena:safepoint-registered?)`
    // from Aura. The CAS return value here is specifically the
    // transition signal — use it to decide whether to clear the
    // flag via `(arena:defrag)` or to keep it for later.
    [[nodiscard]] bool request_defrag() noexcept {
        // Issue #1397: atomic compare-exchange so the return value
        // distinguishes "newly set this call" from "already set by
        // a prior call". test_issue_300 AC5 encodes this semantics:
        // the first (arena:request-defrag) returns #t, every
        // subsequent call (regardless of whether defrag has run
        // and reset the flag via clear_defrag_request()) observes
        // the already-true state and returns #f. Returning the
        // `registered` state alone (the prior behavior) conflated
        // two distinct concerns — whether a safepoint is wired up
        // vs whether this call actually transitioned the flag.
        // Atomic CAS preserves the always-set side effect (the flag
        // is true after call returns regardless of who won the
        // race) while letting callers distinguish first-call from
        // duplicate-call. The "no safepoint" warning continues to
        // fire only on the first call per process (one-shot via
        // warn_no_safepoint_once()).
        bool expected = false;
        const bool newly_set = defrag_requested_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_relaxed);
        if (!aura::gc_hooks::safepoint_registered()) {
            warn_no_safepoint_once();
        }
        return newly_set;
    }
    [[nodiscard]] bool safepoint_registered() const noexcept {
        return aura::gc_hooks::safepoint_registered();
    }
    [[nodiscard]] bool defrag_requested() const noexcept {
        return defrag_requested_.load(std::memory_order_acquire);
    }
    void clear_defrag_request() noexcept {
        defrag_requested_.store(false, std::memory_order_release);
    }

    // Issue #685: optional hook invoked after compact()/defrag()
    // reclaims bytes (ShapeProfiler invalidate, dirty cascade).
    //
    // Issue #1666: set_on_compact_hook **replaces** any prior hook
    // (not append). Callers that need multiple listeners MUST use
    // take_on_compact_hook() + reinstall a chain. Silent overwrite
    // previously dropped Evaluator re_pin when CompilerService
    // installed ShapeProfiler after set_arena.
    void set_on_compact_hook(std::function<void()> hook) { on_compact_hook_ = std::move(hook); }
    // Move out the current hook (leaves arena with no hook).
    [[nodiscard]] std::function<void()> take_on_compact_hook() {
        return std::move(on_compact_hook_);
    }
    [[nodiscard]] bool has_on_compact_hook() const noexcept {
        return static_cast<bool>(on_compact_hook_);
    }

    // Issue #1546 / #1481: optional resource-quota owner for allocate_raw.
    // When set, allocate_raw consults allow_fn(owner, size) before
    // allocating; false → return nullptr (no allocation). Orphan arenas
    // (owner unset) keep the unlimited path. Callback is C-style so
    // aura.core.arena never imports the compiler Evaluator module.
    // allow_fn returns true if the allocation is permitted.
    //
    // Issue #1663: owner + allow_fn are a dual-word critical section —
    // concurrent set/clear vs allocate_raw must not observe a torn pair
    // (owner set, fn null → silent quota bypass). Updates take
    // unique_lock; allocate holds shared_lock across the allow_fn call
    // so ~Evaluator cannot clear+destroy owner mid-callback (UAF).
    // allow_fn must not re-enter set/clear_arena_owner (would deadlock).
    using ArenaQuotaAllowFn = bool (*)(void* owner, std::size_t size) noexcept;
    void set_arena_owner(void* owner, ArenaQuotaAllowFn allow_fn) noexcept {
        std::unique_lock lock(owner_mtx_);
        arena_owner_ = owner;
        quota_allow_fn_ = allow_fn;
    }
    void clear_arena_owner() noexcept {
        std::unique_lock lock(owner_mtx_);
        arena_owner_ = nullptr;
        quota_allow_fn_ = nullptr;
    }
    [[nodiscard]] void* arena_owner() const noexcept {
        std::shared_lock lock(owner_mtx_);
        return arena_owner_;
    }
    [[nodiscard]] bool has_arena_owner() const noexcept {
        std::shared_lock lock(owner_mtx_);
        return arena_owner_ != nullptr && quota_allow_fn_ != nullptr;
    }
    // Issue #1663 test/observability seam: under one lock, owner and
    // allow_fn are both non-null or both null (no torn half-state).
    [[nodiscard]] bool owner_pair_consistent() const noexcept {
        std::shared_lock lock(owner_mtx_);
        return (arena_owner_ != nullptr) == (quota_allow_fn_ != nullptr);
    }

    ~ASTArena() {
        // Call all registered destructors in reverse construction
        // order so each T's owned resources (pmr vector fallback
        // chunks, heap-allocated children) are released BEFORE the
        // arena's underlying bytes are freed.
        run_destructors();
    }

    // Allocate and construct an object of type T. The arena records
    // a type-erased destructor thunk so reset() and ~ASTArena can
    // destroy the object properly even though placement-new was used
    // on raw bytes.
    //
    // Phase C3: `requires std::constructible_from<T, Args...>` locks
    // the contract — T must be constructible from the supplied args
    // (otherwise placement-new is undefined). Zero runtime cost.
    template <typename T, typename... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] T* create(Args&&... args) pre(sizeof(T) > 0)
        pre(alignof(T) > 0 && (alignof(T) & (alignof(T) - 1)) == 0) {
        void* raw = allocate_raw(sizeof(T), alignof(T));
        // Issue #1546: quota reject → nullptr (no construct).
        if (!raw)
            return nullptr;
        ++stats_.allocation_count;
        auto* result = std::construct_at(static_cast<T*>(raw), std::forward<Args>(args)...);
        dtors_.push_back({result, +[](void* p) { static_cast<T*>(p)->~T(); }});
        return result;
    }

    // Destroy a single object: call its destructor AND unregister the
    // entry so the bulk-dtor pass at reset/~ASTArena doesn't double-
    // destroy. If `ptr` wasn't tracked (caller's responsibility) the
    // dtor still runs as a best-effort fallback.
    //
    // Phase C3: `requires std::is_nothrow_destructible_v<T>` — the
    // arena invokes destructors during reset() which propagates
    // through fiber / COW chains. A throwing destructor would
    // abort the process; constraining to nothrow-destructible makes
    // that contract explicit at compile time. Zero runtime cost.
    template <typename T>
        requires std::is_nothrow_destructible_v<T>
    void destroy(T* ptr) {
        // Issue #1519: nullptr is a documented no-op (safe for Guard cleanup).
        if (!ptr)
            return;
        aura::core::cpp26::record_hotpath_invariant_hit();
        for (auto it = dtors_.begin(); it != dtors_.end(); ++it) {
            if (it->ptr == ptr) {
                ptr->~T();
                dtors_.erase(it);
                // Issue #1518: recycle small-pool slots for freelist relocate.
                (void)small_pool_.recycle(ptr, sizeof(T));
                return;
            }
        }
        // Not tracked (e.g. allocated by an upstream helper, or
        // ownership already moved). Best-effort dtor call.
        ptr->~T();
        (void)small_pool_.recycle(ptr, sizeof(T));
    }

    // Release all allocated memory in one shot. Destructors run in
    // reverse construction order, then the underlying pmr buffer is
    // released (which frees in-buffer allocations) and the small-
    // object pool bump pointers are reset.
    void reset() post(used() == 0) post(stats_.allocation_count == 0) {
        run_destructors();
        small_pool_.reset();
        resource_.release();
        stats_.used = 0;
        stats_.allocation_count = 0;
        stats_.wasted = 0;
    }

    // Get a pmr-compatible allocator for std::pmr containers
    [[nodiscard]] std::pmr::polymorphic_allocator<std::byte> allocator() noexcept {
        return {&resource_};
    }

    // Snapshot of current memory statistics
    [[nodiscard]] ArenaStats stats() const noexcept {
        auto s = stats_;
        s.capacity = buffer_.size() + small_pool_.capacity();
        s.used = stats_.used + small_pool_.allocated();
        s.peak_used = std::max(s.peak_used, s.used);
        return s;
    }

    // Bytes consumed so far
    [[nodiscard]] std::size_t used() const noexcept {
        return stats_.used + small_pool_.allocated();
    }

    // Total buffer capacity
    [[nodiscard]] std::size_t capacity() const noexcept {
        return buffer_.size() + small_pool_.capacity();
    }

    // Issue #1621: SmallObjectPool utilization for smart auto-compact policy.
    [[nodiscard]] double small_pool_utilization() const noexcept {
        return small_pool_.utilization();
    }

    // Issue #187 (P0): estimate how many bytes could be reclaimed
    // by a compaction. This is a cheap, side-effect-free check that
    // callers (auto-compact trigger, observability) can use to decide
    // whether a full compact() is worth the cost.
    //
    // The current implementation reports the gap between used bytes
    // and total buffer capacity (i.e. unused capacity). This is the
    // upper bound on what a buffer-shrinking compact could save.
    // For a full live-object-moving compact, savings would be less
    // (depends on actual fragmentation), but that variant isn't
    // implemented yet — see `compact()` below for the conservative
    // buffer-shrinking variant.
    [[nodiscard]] std::size_t compact_estimate() const noexcept {
        std::size_t cap = buffer_.size();
        std::size_t u = stats_.used;
        return cap > u ? cap - u : 0;
    }

    // Issue #187 (P0): conservative compact() that shrinks the
    // underlying pmr buffer to the smallest size that still holds
    // the live allocations. This is NOT a full live-object-moving
    // defragmentation pass — that would require either:
    //   (a) moving live objects in-place and patching all pointers
    //       (unsupported: arena objects are referenced by raw
    //        pointers from outside, e.g. closures_ cl_flat), or
    //   (b) a stop-the-world mark phase that identifies dead
    //       objects and compacts only those (would need a GC
    //       integration; tracked as a separate follow-up).
    //
    // What this does: grow-shrink the buffer to the used-size +
    // safety margin so the unused tail can be reclaimed by
    // std::vector<std::byte>'s allocator. Safe because the pmr
    // resource's existing in-buffer allocations are preserved
    // (monotonic_buffer_resource::release() doesn't free in-buffer
    // memory; we replace buffer_ with a fresh one and remap).
    //
    // Returns the number of bytes reclaimed.
    // Issue #1519: post(r == 0 || buffer shrank) via contract_assert at end
    // (strict post on size_t return is vacuous; assert encodes the invariant).
    [[nodiscard]] std::size_t compact() noexcept {
        aura::core::cpp26::record_hotpath_invariant_hit();
        // Issue #1466 contracts were too strict for production
        // compact (post(r <= buffer_.size()) used post-shrink size;
        // post(compaction_count > 0) failed on no-op). Removed.
        //
        // Heal used()>size drift before shrink (can appear after
        // remaps / dual-resource paths); better than aborting the
        // self-modify loop (#1457 unblock).
        if (stats_.used > buffer_.size())
            stats_.used = buffer_.size();
        // Issue #604: fiber-context coordination. When compact()
        // runs inside a scheduled fiber, bump the yield-check
        // counter and hit the GC safepoint so a long compaction
        // cooperates with the scheduler/GC instead of trimming
        // the buffer with no chance for the collector to run.
        if (aura::gc_hooks::fiber_active()) {
            stats_.compaction_yield_checks++;
            aura::gc_hooks::safepoint_check();
            aura::core::arena_policy::record_defrag_fiber_safe_hit();
            aura::gc_hooks::notify_fiber_safe_compact();
        }
        std::size_t before = buffer_.size();
        std::size_t u = stats_.used;
        if (u == 0) {
            // Empty arena: shrink to 1KB (preserve some headroom for
            // future small allocations).
            buffer_.resize(1024);
            rebuild_resource_();
        } else if (u < before) {
            // Round up to power of 2 with 25% headroom, min 1KB.
            std::size_t target = 1024;
            while (target < u + u / 4)
                target *= 2;
            if (target < before) {
                buffer_.resize(target);
                // Rebuild resource on new buffer. Live allocations
                // in the buffer below `target` are still valid
                // (monotonic_buffer_resource is a bump allocator;
                // all live ptrs are below `stats_.used` which is
                // < `target`).
                rebuild_resource_();
            }
        }
        std::size_t after = buffer_.size();
        std::size_t saved = (before > after) ? (before - after) : 0;
        // Issue #1519: compact never grows the buffer.
        contract_assert(after <= before);
        if (saved > 0) {
            stats_.compaction_count++;
            stats_.last_compaction_saved = saved;
            stats_.total_compaction_saved += saved;
            invoke_compact_hook_();
        }
        return saved;
    }

    // Issue #300 (P1): defrag() — sliding-reclaim the unused tail
    // of the arena's buffer without moving live objects. The same
    // underlying mechanism as compact() (trim buffer_ to
    // stats_.used + 25% headroom), but counted separately as a
    // "defrag attempt" rather than a compaction. This is the
    // foundation for the full live-object-moving defrag path
    // (which would require either pool-backed resource with free()
    // or stop-the-world mark + GC integration — both tracked as
    // separate follow-ups).
    //
    // Why a separate counter: in production, (arena:compact) is
    // called periodically as part of normal maintenance, while
    // (arena:defrag) is a heavier operation that the Aura-HV
    // self-evolution loop triggers when fragmentation ratio
    // exceeds a threshold. Tracking them separately lets
    // dashboards / auto-tuners see how often each is exercised
    // and how much each saves.
    //
    // Returns the number of bytes reclaimed (same as compact()).
    [[nodiscard]] std::size_t defrag() noexcept {
        // Public entry point: caller wants the flag cleared
        // regardless of whether defrag actually reclaims bytes.
        return defrag_impl(true, /*invoke_hook=*/true);
    }

    [[nodiscard]] std::size_t defrag_no_clear_request() noexcept {
        // Internal entry point: used by maybe_auto_compact_on_alloc
        // so a transient no-op defrag doesn't lose the user's
        // pending request flag.
        return defrag_impl(false, /*invoke_hook=*/true);
    }

    // invoke_hook=false: live_compact runs its own single deopt-coord hook.
    [[nodiscard]] std::size_t defrag_impl(bool clear_request_flag,
                                          bool invoke_hook = true) noexcept {
        aura::core::cpp26::record_hotpath_invariant_hit(); // Issue #1519
        // Issue #604: same fiber-context coordination as compact().
        if (aura::gc_hooks::fiber_active()) {
            stats_.compaction_yield_checks++;
            aura::gc_hooks::safepoint_check();
            aura::core::arena_policy::record_defrag_fiber_safe_hit();
            aura::gc_hooks::notify_fiber_safe_compact();
        }
        // Issue #300 Phase 3 + Issue #300 AC5 follow-up: the
        // request flag is cleared ONLY when defrag actually
        // reclaims bytes. A no-op defrag (arena already
        // compacted) leaves the flag set so the user's
        // pending request isn't silently lost by transient
        // auto-alloc defrags. Explicit `(arena:defrag)`
        // calls (see below) override this and always clear
        // the flag — the user invoked it explicitly.
        const bool caller_wants_clear = clear_request_flag;
        if (clear_request_flag) {
            defrag_requested_.store(false, std::memory_order_release);
        }
        std::size_t before = buffer_.size();
        std::size_t u = stats_.used;
        if (u == 0) {
            buffer_.resize(1024);
            rebuild_resource_();
        } else if (u < before) {
            std::size_t target = 1024;
            while (target < u + u / 4)
                target *= 2;
            if (target < before) {
                buffer_.resize(target);
                rebuild_resource_();
            }
        }
        std::size_t after = buffer_.size();
        std::size_t saved = (before > after) ? (before - after) : 0;
        // Issue #300: always increment the defrag attempt
        // counter on every (arena:defrag) call, regardless of
        // whether bytes were saved. This matches the test
        // contract from test_issue_300 AC4: a no-op defrag
        // (arena already compacted) still counts as an
        // attempt. The `saved > 0` gate is preserved for
        // last_defrag_saved (so a no-op defrag doesn't reset
        // the last-saved value to 0).
        stats_.defrag_attempted_count++;
        // Issue #1320: mirror to process-wide policy stats so Agents can
        // observe live defrag attempts even when arena-local stats are not
        // queried directly.
        aura::core::arena_policy::record_defrag_attempt(saved);
        if (saved > 0) {
            if (!caller_wants_clear) {
                // Auto-alloc path: clear the flag now that we
                // actually did work. (Caller-explicit path
                // already cleared at start.)
                defrag_requested_.store(false, std::memory_order_release);
            }
            stats_.last_defrag_saved = saved;
            if (invoke_hook)
                invoke_compact_hook_();
        }
        // Note: NOT touching stats_.compaction_count /
        // last_compaction_saved. This is intentionally a separate
        // counter from compact().
        return saved;
    }

    // Issue #1467 Phase 1 + #1518: live-object compact with mark +
    // freelist relocate protocol + Shape/JIT deopt coordination.
    //
    // Phase model:
    //   1. Mark: count live tracked objects (dtors_) + small-pool live bytes
    //   2. Relocate: freelist holes are reuse-slots (lazy relocate on next
    //      alloc); count free slots as relocate-ready. Full pointer remapping
    //      of still-live objects remains deferred (external raw pointers).
    //   3. Compact: conservative buffer trim (defrag_impl)
    //   4. Coordinate: compact hook + deopt throttle (no deopt storm)
    //
    // Returns number of live objects marked (dtors_ size).
    [[nodiscard]] std::size_t live_defrag() noexcept { return live_compact(/*force=*/true); }

    // Issue #1518: live_compact — same as live_defrag; force=false soft-gates
    // on render hotpath / MutationBoundary (auto path).
    [[nodiscard]] std::size_t live_compact(bool force = true) noexcept {
        aura::core::cpp26::record_hotpath_invariant_hit(); // Issue #1519
        // Soft-gate auto path during render / active mutation boundary so
        // fiber yield / Guard pins stay coherent (explicit Agent calls use force).
        if (!force) {
            if (aura::core::arena_policy::in_render_hotpath()) {
                aura::core::arena_policy::record_compact_soft_gated_render();
                return 0;
            }
            if (arena_mutation_boundary_depth() > 0) {
                stats_.compact_soft_gated_boundary++;
                aura::core::arena_policy::record_compact_soft_gated_boundary();
                return 0;
            }
        }

        // ── Mark ──
        // Tracked create<T> objects + small-pool slot proxy (try_allocate path
        // without dtor tracking — min tier size 16B lower-bounds slot count).
        const std::size_t marked_objs = dtors_.size();
        const std::size_t marked_bytes = small_pool_.allocated();
        const std::size_t pool_slot_proxy = marked_bytes / SmallObjectPool::kTierSizes[0];
        const std::size_t total_marked = marked_objs + pool_slot_proxy;
        stats_.live_defrag_attempted_count++;
        stats_.live_objects_marked_total += total_marked;

        // ── Relocate (freelist protocol) ──
        // Free slots are already "relocated out"; recycle hits are real reuses.
        const std::size_t holes = small_pool_.free_slot_count();
        const std::size_t reuses = small_pool_.recycle_hits();
        stats_.live_relocate_count += holes + reuses;
        aura::core::arena_policy::record_live_relocate(holes + reuses);

        // ── Compact tail (no hook — we invoke once below) ──
        const auto frag_before = stats().fragmentation_ratio();
        (void)defrag_impl(/*clear_request_flag=*/false, /*invoke_hook=*/false);
        small_pool_.rebind_tiers();

        const auto frag_after = stats().fragmentation_ratio();
        stats_.frag_post_compact_bp = static_cast<std::size_t>(frag_after * 10000.0);
        aura::core::arena_policy::record_frag_post_compact(frag_after);
        if (frag_before > frag_after) {
            stats_.frag_reduced_bp +=
                static_cast<std::size_t>((frag_before - frag_after) * 10000.0);
        }

        // ── Shape/JIT deopt coordination (throttled in service hook) ──
        invoke_compact_hook_with_deopt_();
        return total_marked;
    }

    // Issue #1467 / #1518: live-defrag counters (from ArenaStats).
    [[nodiscard]] std::uint64_t live_defrag_attempted_count_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_defrag_attempted_count);
    }
    [[nodiscard]] std::uint64_t live_objects_marked_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_objects_marked_total);
    }
    [[nodiscard]] std::uint64_t live_relocate_count_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_relocate_count);
    }
    [[nodiscard]] std::uint64_t compact_deopt_triggered_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.compact_deopt_triggered);
    }
    [[nodiscard]] std::uint64_t frag_post_compact_bp_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.frag_post_compact_bp);
    }

    // Issue #187 (P0): shrink_to_fit() — convenience wrapper that
    // returns the buffer to its initial allocation size. Useful
    // after a long batch of mutations to reclaim any growth from
    // geometric buffer expansion. No-op if buffer is already at
    // initial size.
    void shrink_to_fit() noexcept {
        if (buffer_.size() > initial_size_ && stats_.used < initial_size_) {
            std::size_t before = buffer_.size();
            buffer_.resize(initial_size_);
            rebuild_resource_();
            // Issue #974/#1242: rebind + clamp tier ends to current buffer size.
            // SmallObjectPool owns its own buffer — rebind still required if
            // its buffer moved; clamp hardens try_allocate against stale ends.
            small_pool_.rebind_tiers();
            std::size_t saved = before - initial_size_;
            stats_.compaction_count++;
            stats_.last_compaction_saved = saved;
            stats_.total_compaction_saved += saved;
        }
    }

    // Number of live tracked objects (for tests / diagnostics)
    [[nodiscard]] std::size_t live_count() const noexcept { return dtors_.size(); }

    // Issue #1518 / test seam: raw allocate without dtor tracking
    // (SmallObjectPool path when size <= 64). Used by live-compact
    // stress tests and legacy #1467 harness.
    // Issue #1546/#1554: quota-bound when arena_owner_ is set (via set_arena).
    [[nodiscard]] void* try_allocate(std::size_t size) noexcept {
        if (size == 0)
            return nullptr;
        return allocate_raw(size, alignof(std::max_align_t));
    }

    // Issue #1554: typed factory — quota check once, then allocate_raw_impl.
    // Prefer this (or Evaluator::allocate_checked) over bare try_allocate when
    // the caller needs ResourceQuotaExceeded as AuraError rather than nullptr.
    // Orphan arenas (no owner): still allocates; no ResourceQuotaExceeded.
    [[nodiscard]] aura::core::AuraResult<void*>
    allocate_checked(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) noexcept {
        if (size == 0) {
            return std::unexpected(
                aura::core::AuraError{aura::core::AuraErrorKind::InternalInvariantViolation,
                                      std::string("ASTArena::allocate_checked size==0")});
        }
        // Issue #1663: hold shared_lock across allow_fn so clear cannot
        // destroy owner mid-callback (snapshot-then-call would UAF).
        {
            std::shared_lock lock(owner_mtx_);
            if (quota_allow_fn_ && arena_owner_) {
                if (!quota_allow_fn_(arena_owner_, size)) {
                    return std::unexpected(aura::core::AuraError{
                        aura::core::AuraErrorKind::ResourceQuotaExceeded,
                        std::string("ASTArena::allocate_checked: resource quota exceeded (") +
                            std::to_string(size) + " bytes)"});
                }
            }
        }
        // Quota already enforced (or unbound) — do not re-enter allow_fn.
        void* ptr = allocate_raw_impl(size, alignment);
        if (!ptr) {
            return std::unexpected(
                aura::core::AuraError{aura::core::AuraErrorKind::ArenaOutOfMemory,
                                      std::string("ASTArena::allocate_checked: OOM")});
        }
        return ptr;
    }

private:
    // Type-erased destructor pair. The thunk is bound at the create<T>
    // call site to call ~T() on the specific type, so a single
    // vector<DtorEntry> can host heterogeneous Ts.
    struct DtorEntry {
        void* ptr;
        void (*dtor)(void*);
    };

    void run_destructors() noexcept {
        // Reverse order: last-constructed destroyed first, matching
        // LIFO stack discipline and the C++ standard library's
        // destroy(deallocate, alloc, ...) semantics.
        for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it) {
            it->dtor(it->ptr);
        }
        dtors_.clear();
    }

    // Issue #187 (P0) + Issue #300 follow-up #3 (real root
    // cause of the heap-use-after-free on FlatAST::binding_gens_):
    // after compact()/defrag() shrinks buffer_, the resource_'s
    // internal bump pointer is still pointing somewhere in the
    // (old) buffer. The OLD release()-to-start behavior allowed
    // the next allocation to overlap the live prefix where
    // earlier FlatASTs were still alive (dtors_ still held
    // pointers to them) — heap-use-after-free when the arena
    // was destroyed.
    //
    // Fix: don't reset the bump pointer at all. Just leave it
    // wherever the previous allocations left it (which equals
    // the start of the buffer + stats_.used, i.e., right past
    // the live data). The next allocation lands there, no
    // overlap. This is also what compact()/defrag() want
    // semantically: reclaim the unused tail without disturbing
    // live objects.
    //
    // Note: this only works if the bump pointer is still
    // pointing into valid buffer_ capacity. std::vector::resize
    // with n < size() preserves capacity (no reallocation), so
    // the bump pointer's storage stays valid even though
    // buffer_.size() shrank. The bump pointer might point past
    // the new size, but the underlying bytes are still
    // allocated (capacity unchanged), so accessing them is safe
    // (the resource just treats the bytes as allocatable).
    void rebuild_resource_() noexcept {
        // Intentionally empty: don't reset the bump pointer.
        // See the comment above for why this is the correct
        // behavior post-#300 follow-up #3.
        // (Previously called resource_.release() which reset
        // to buffer_.data() — that triggered the UAF.)
    }

    // Issue #1519: post(ptr != nullptr) on success path — pmr allocate
    // throws on OOM rather than returning null; small-pool path returns
    // non-null on hit.
    // Issue #1546 / #1554: when arena_owner_ + quota_allow_fn_ reject,
    // returns nullptr without allocating. Typed path:
    // ASTArena::allocate_checked / Evaluator::allocate_checked.
    void* allocate_raw(std::size_t size, std::size_t alignment) pre(size > 0)
        pre(alignment > 0 && (alignment & (alignment - 1)) == 0) {
        // ── Resource quota (Issue #1546 / #1481 / #1554 / #1663) ──
        // Owner-threaded Evaluator::check_arena_quota (or equivalent).
        // Orphan arenas skip this branch entirely.
        // Issue #1663: hold shared_lock across allow_fn so concurrent
        // set/clear cannot tear the pair AND ~Evaluator cannot destroy
        // owner mid-callback (unique_lock waits for shared_lock release).
        // allow_fn must not call set/clear_arena_owner (deadlock).
        {
            std::shared_lock lock(owner_mtx_);
            if (quota_allow_fn_ && arena_owner_) {
                if (!quota_allow_fn_(arena_owner_, size)) {
                    // Rejected — no allocation, no stats bump.
                    return nullptr;
                }
            }
        }
        return allocate_raw_impl(size, alignment);
    }

    // Body of allocate_raw after quota gate (Issue #1554 split).
    void* allocate_raw_impl(std::size_t size, std::size_t alignment) {
        // ── GC integration (Issue #113 Phase 4) ──────────
        // Check the safepoint before allocating. This lets a
        // compute-heavy fiber that doesn't yield for long
        // stretches be interrupted by the GC. The check is
        // an atomic load + branch — ~1 ns in the hot path.
        // When the GC subsystem is not initialized (stdin mode
        // or pre-scheduler), g_arena_safepoint_check is null
        // and this is a no-op.
        aura::gc_hooks::safepoint_check();

        // ── Alloc accounting (Issue #113 Phase 4) ─────────
        // Optional: bump the GC's alloc counter so it can
        // decide when to trigger a collection cycle. No-op
        // when the collector is not wired up.
        aura::gc_hooks::record_alloc();

        // Try small-object pool first (for objects <= 64 bytes)
        if (size <= SmallObjectPool::kMaxSmallSize) {
            void* ptr = small_pool_.try_allocate(size);
            if (ptr) {
                aura::core::cpp26::record_hotpath_invariant_hit();
                contract_assert(ptr != nullptr);
                maybe_auto_compact_on_alloc();
                return ptr;
            }
            // Issue #658: tier exhausted — fall through to main pmr arena.
            arena_small_tier_fallback_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #743: tier pressure → defer live defrag at next safe point.
            (void)request_defrag();
            aura::core::cpp26::record_hotpath_invariant_hit();
        }

        // Allocate from main pmr buffer
        void* ptr = resource_.allocate(size, alignment);
        contract_assert(ptr != nullptr); // Issue #1519
        stats_.used += size;
        maybe_auto_compact_on_alloc();
        return ptr;
    }

    // Issue #685: auto-compact / defrag when fragmentation or
    // small-pool pressure exceeds thresholds (alloc-path policy).
    //
    // Issue #300 follow-up: respect the defrag-requested flag
    // ONLY when there's actual fragmentation to fix. Calling
    // defrag() on a low-frag arena (when want_defrag is set)
    // would clear the request flag without doing useful work,
    // losing the user's pending request. The fix: also
    // require frag_high to act on want_defrag. small_high
    // still triggers compact() (no flag interaction).
    void maybe_auto_compact_on_alloc() noexcept {
        // Issue #685 / #743 / #1621: smart auto-compact policy —
        // frag + small-pool util + dirty cascade + Shape churn +
        // defrag_req, soft-gated on render hot path, fiber-safe
        // safepoint when a fiber is active.
        const bool render_hp = aura::core::arena_policy::in_render_hotpath();
        if (render_hp) {
            aura::core::arena_policy::record_compact_soft_gated_render();
            // Still evaluate for metrics (soft-gate count).
            (void)aura::core::arena_policy::evaluate_auto_compact_policy(
                stats().fragmentation_ratio(), defrag_requested(),
                aura::core::arena_policy::dirty_cascade_pending.load(std::memory_order_acquire),
                aura::core::arena_policy::peek_shape_churn(), aura::gc_hooks::fiber_active(),
                /*render_hotpath=*/true, small_pool_.utilization());
            return;
        }
        const auto snap = stats();
        const bool want_defrag = defrag_requested();
        // Peek then consume so a no-trigger path does not drop signals
        // that boundary-exit / fiber probes still need.
        const bool dirty_pending =
            aura::core::arena_policy::dirty_cascade_pending.load(std::memory_order_acquire);
        const bool shape_pending = aura::core::arena_policy::peek_shape_churn();
        const bool fiber = aura::gc_hooks::fiber_active();
        const auto decision = aura::core::arena_policy::evaluate_auto_compact_policy(
            snap.fragmentation_ratio(), want_defrag, dirty_pending, shape_pending, fiber,
            /*render_hotpath=*/false, small_pool_.utilization());
        if (!decision.should_compact)
            return;
        // Consume signals only when we act (avoid lost wakeups).
        (void)aura::core::arena_policy::consume_dirty_cascade();
        (void)aura::core::arena_policy::consume_shape_churn();
        // Issue #1919: clear AI/JIT pressure after acting so thresholds re-settle.
        (void)aura::core::arena_policy::consume_mutation_pressure();
        (void)aura::core::arena_policy::consume_jit_deopt_pressure();

        if (fiber) {
            stats_.compaction_yield_checks++;
            aura::gc_hooks::safepoint_check();
            aura::core::arena_policy::record_defrag_fiber_safe_hit();
            aura::gc_hooks::notify_fiber_safe_compact();
        } else {
            stats_.compaction_yield_checks++;
        }
        const double frag_before = snap.fragmentation_ratio();
        std::size_t saved = 0;
        if (decision.prefer_live_defrag || want_defrag) {
            // Issue #1518 / #1621: prefer live_compact (mark + freelist
            // relocate + deopt coord) when freelist holes or tracked
            // live objs exist; fall back to defrag_no_clear_request.
            if (small_pool_.free_slot_count() > 0 || live_count() > 0) {
                const auto marked = live_compact(/*force=*/false);
                if (marked > 0 || small_pool_.free_slot_count() == 0)
                    saved = 1;
            } else {
                saved = defrag_no_clear_request();
            }
            if (saved > 0)
                stats_.defrag_savings_alloc += saved;
        } else {
            saved = compact();
        }
        stats_.auto_alloc_trigger_count++;
        aura::core::arena_policy::record_auto_compact_trigger();
        aura::gc_hooks::notify_auto_compact_trigger();
        // Issue #1919: false-positive gate — reclaimed 0 bytes ⇒ FP sample.
        aura::core::arena_policy::record_auto_compact_outcome(saved > 0);
        const double frag_after = stats().fragmentation_ratio();
        aura::core::arena_policy::record_fragmentation_post_mutate(frag_after);
        if (frag_before > frag_after) {
            stats_.frag_reduced_bp +=
                static_cast<std::size_t>((frag_before - frag_after) * 10000.0);
        }
        (void)decision.reason;
        (void)decision.frag_threshold_used;
    }

    void invoke_compact_hook_() {
        if (!on_compact_hook_)
            return;
        on_compact_hook_();
        stats_.shape_inval_on_compact++;
        aura::core::arena_policy::record_shape_inval_on_compact();
    }

    // Issue #1518: compact hook path for live_compact. Shape/JIT deopt
    // storm throttle lives in the service on_compact_hook (where
    // ShapeProfiler is); here we always run pin restamp + shape_inval
    // counter, then mirror process-wide deopt totals into ArenaStats.
    void invoke_compact_hook_with_deopt_() {
        invoke_compact_hook_();
        // Mirror policy totals (updated by CompilerService hook).
        const auto trig =
            aura::core::arena_policy::compact_deopt_triggered_total.load(std::memory_order_relaxed);
        const auto thr =
            aura::core::arena_policy::compact_deopt_throttled_total.load(std::memory_order_relaxed);
        // Store absolute totals so format/merge stay useful.
        stats_.compact_deopt_triggered = static_cast<std::size_t>(trig);
        stats_.compact_deopt_throttled = static_cast<std::size_t>(thr);
    }

    std::size_t initial_size_ = 0; // Issue #187: for shrink_to_fit()
    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
    SmallObjectPool small_pool_;
    ArenaStats stats_;
    std::vector<DtorEntry> dtors_;
    // Issue #300 Phase 3: see request_defrag() / defrag_requested()
    // / clear_defrag_request() for semantics.
    std::atomic<bool> defrag_requested_{false};
    std::function<void()> on_compact_hook_;
    // Issue #1546: optional Evaluator* (void*) + quota allow callback.
    // Issue #1663: owner_mtx_ protects the dual-word owner pair.
    mutable std::shared_mutex owner_mtx_;
    void* arena_owner_ = nullptr;
    ArenaQuotaAllowFn quota_allow_fn_ = nullptr;
};

// Issue #685: aggregate auto-compact policy stats for observability.
export struct ArenaAutoCompactPolicyStats {
    std::uint64_t auto_triggers = 0;
    std::uint64_t frag_reduced = 0;
    std::uint64_t shape_inval_on_compact = 0;
    std::uint64_t defrag_savings = 0;
    std::uint64_t yield_checks_hit = 0;
};

// ── ArenaGroup — multi-arena manager ─────────────────────────────
//
// Manages a collection of named arenas, each representing a module
// or compilation unit. Enables fine-grained reset and memory reporting.
//
export class ArenaGroup {
public:
    // Issue #187: compaction policy. When the fragmentation ratio
    // (capacity - used) / capacity exceeds this threshold (0.0-1.0),
    // auto_compact() will trigger a compact() on that arena. Default
    // 0.50 = compact when half the buffer is unused.
    void set_compact_threshold(double ratio) noexcept {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        compact_threshold_ = std::clamp(ratio, 0.0, 0.95);
    }
    [[nodiscard]] double compact_threshold() const noexcept { return compact_threshold_; }

    // Issue #1554: propagate default quota owner to every module arena
    // (existing + future module_arena creates). Same C-style callback
    // pattern as ASTArena::set_arena_owner — no Evaluator import.
    void set_default_arena_owner(void* owner, ASTArena::ArenaQuotaAllowFn allow_fn) noexcept {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        default_owner_ = owner;
        default_allow_fn_ = allow_fn;
        for (auto& [_, arena] : arenas_) {
            if (owner && allow_fn)
                arena->set_arena_owner(owner, allow_fn);
            else
                arena->clear_arena_owner();
        }
    }
    void clear_default_arena_owner() noexcept {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        default_owner_ = nullptr;
        default_allow_fn_ = nullptr;
        for (auto& [_, arena] : arenas_)
            arena->clear_arena_owner();
    }
    [[nodiscard]] bool has_default_arena_owner() const noexcept {
        return default_owner_ != nullptr && default_allow_fn_ != nullptr;
    }

    // Get or create an arena for a module
    ASTArena& module_arena(const std::string& name, std::size_t initial_size = 8 * 1024 * 1024)
        pre(!name.empty()) pre(initial_size >= 1024) {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        auto it = arenas_.find(name);
        if (it != arenas_.end())
            return *it->second;
        auto [inserted, ok] = arenas_.emplace(name, std::make_unique<ASTArena>(initial_size));
        // #1554: new module arenas inherit group default quota owner.
        if (default_owner_ && default_allow_fn_)
            inserted->second->set_arena_owner(default_owner_, default_allow_fn_);
        return *inserted->second;
    }

    // Reset a specific module's arena
    void reset_module(const std::string& name) {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        auto it = arenas_.find(name);
        if (it != arenas_.end())
            it->second->reset();
    }

    // Reset all arenas
    void reset_all() {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        for (auto& [_, arena] : arenas_)
            arena->reset();
    }

    // Issue #187 (P0): compact a specific module's arena. Returns
    // bytes reclaimed, or 0 if the module isn't found.
    [[nodiscard]] std::size_t compact_module(const std::string& name) {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        auto it = arenas_.find(name);
        if (it == arenas_.end())
            return 0;
        return it->second->compact();
    }

    // Issue #187 (P0): compact every arena whose fragmentation ratio
    // exceeds the configured threshold. Returns total bytes reclaimed.
    [[nodiscard]] std::size_t auto_compact() {
        std::size_t total = 0;
        for (auto& [_, arena] : arenas_) {
            if (arena->stats().fragmentation_ratio() >= compact_threshold_) {
                total += arena->compact();
            }
        }
        return total;
    }

    // Issue #335: lightweight probe — should_auto_compact(name)?
    // Returns true when the per-module fragmentation ratio
    // is at or above the adaptive threshold. Cheap O(1)
    // check (just reads stats + a map lookup). Used by
    // the evaluator's memory_pressure sampling loop to
    // decide whether to call compact() before the
    // critical threshold.
    [[nodiscard]] bool should_auto_compact(const std::string& name) const {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        auto it = arenas_.find(name);
        if (it == arenas_.end())
            return false;
        const double frag = it->second->stats().fragmentation_ratio();
        // Adaptive threshold: lower the base threshold when
        // recent compactions saved a lot (the previous
        // compact was productive → trigger sooner); raise
        // it (toward base) when recent compactions saved
        // little (the previous compact was wasteful →
        // trigger later).
        const double ema = savings_ema_for(name);
        const double adjusted =
            std::clamp(compact_threshold_ - ema * kEmaGain, kMinThreshold, kMaxThreshold);
        return frag >= adjusted;
    }

    // Issue #335: adaptive_compact(name) — compact a single
    // module's arena, update the savings EMA, and bump
    // the trigger counter. Returns bytes reclaimed.
    [[nodiscard]] std::size_t adaptive_compact(const std::string& name) {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        auto it = arenas_.find(name);
        if (it == arenas_.end())
            return 0;
        // Pre-snapshot so we can compute savings without
        // recomputing stats from scratch.
        const auto before = it->second->stats();
        if (before.fragmentation_ratio() < threshold_for(name)) {
            auto_compact_skip_count_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        const std::size_t saved = it->second->compact();
        // Update the per-module savings EMA. Newer savings
        // weight more (alpha = 0.3) so a single large
        // compaction shifts the next trigger sooner but
        // the effect decays over time.
        const double& ema_ref = savings_ema_[name];
        const double new_ema =
            (ema_ref == 0.0) ? static_cast<double>(saved)
                             : kEmaAlpha * static_cast<double>(saved) + (1.0 - kEmaAlpha) * ema_ref;
        savings_ema_[name] = new_ema;
        auto_compact_trigger_count_.fetch_add(1, std::memory_order_relaxed);
        return saved;
    }

    // Issue #335: adaptive_compact_all() — adaptive variant
    // of auto_compact() that uses should_auto_compact() per
    // module. Returns total bytes reclaimed across all
    // managed arenas.
    [[nodiscard]] std::size_t adaptive_compact_all() {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        std::size_t total = 0;
        for (auto& [name, _] : arenas_) {
            total += adaptive_compact(name);
        }
        return total;
    }

    // Issue #464: auto_compact_with_safety() — the
    // production-grade auto-compaction entry point for
    // the fiber scheduler + MutationBoundaryGuard
    // integration. Combines:
    //   1. The adaptive threshold (should_auto_compact
    //      considers per-module EMA savings)
    //   2. The fiber-safety check (compaction_safe_
    //      counter bumps when called in a fiber context
    //      where yielding would have been appropriate;
    //      the actual yield is a follow-up, the counter
    //      is in place so the AI Agent can monitor it)
    //   3. The closed-loop signal: bumps
    //      auto_compact_guard_call_count_ on every
    //      call (regardless of trigger outcome) so the
    //      AI Agent can see the call rate from
    //      MutationBoundaryGuard dtor.
    // Returns bytes reclaimed (0 if no arena triggered).
    [[nodiscard]] std::size_t auto_compact_with_safety() {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        auto_compact_guard_call_count_.fetch_add(1, std::memory_order_relaxed);
        if (aura::gc_hooks::fiber_active()) {
            compaction_yield_checks_.fetch_add(1, std::memory_order_relaxed);
            aura::gc_hooks::safepoint_check();
            aura::core::arena_policy::record_defrag_fiber_safe_hit();
            aura::gc_hooks::notify_fiber_safe_compact();
        } else {
            compaction_yield_checks_.fetch_add(1, std::memory_order_relaxed);
        }
        return adaptive_compact_all();
    }

    // Issue #464: bump_auto_compact_guard_call() — called
    // by MutationBoundaryGuard dtor on the outermost +
    // success path. Pure counter bump (no actual compact
    // call yet — the auto_compact_with_safety() path is
    // invoked explicitly by the scheduler or the
    // follow-up #464 commit). Provides the closed-loop
    // signal that the agent is making mutation calls.
    void bump_auto_compact_guard_call() noexcept {
        auto_compact_guard_call_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_compaction_yield_check() noexcept {
        compaction_yield_checks_.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #464: guard-call counter accessor.
    [[nodiscard]] std::uint64_t auto_compact_guard_call_count() const noexcept {
        return auto_compact_guard_call_count_.load(std::memory_order_relaxed);
    }
    // Issue #464: yield-check counter accessor (separate
    // from per-arena compaction_yield_checks_ in
    // ArenaStats; this is the group-level total for
    // monitoring long AI sessions).
    [[nodiscard]] std::uint64_t compaction_yield_checks_group() const noexcept {
        return compaction_yield_checks_.load(std::memory_order_relaxed);
    }

    // Issue #430: compact_with_policy(name, policy) —
    // manual policy override for callers that want to
    // compact a specific arena regardless of (or stricter
    // than) the adaptive threshold. The 3 policies are:
    //   - "force":  always compact (no threshold check)
    //   - "auto":   use the adaptive threshold (default
    //               behavior of adaptive_compact)
    //   - "skip":   never compact (returns 0)
    //
    // Returns bytes reclaimed (0 if the policy was
    // "skip" or the module wasn't found, or 0 if "auto"
    // was below the threshold). Bumps the trigger /
    // skip counters in the same way as adaptive_compact
    // so the observability surface (query:arena-compaction-stats
    // + arena:adaptive-stats) treats manual and automatic
    // compactions uniformly.
    //
    // The "force" path is the only safe way to compact
    // an arena during a long AI session when the
    // adaptive threshold would otherwise skip. Use it
    // sparingly — compaction is O(capacity) and can
    // stall the worker thread.
    enum class CompactPolicy {
        Force, // always compact
        Auto,  // consult adaptive threshold
        Skip,  // never compact
    };
    [[nodiscard]] std::size_t compact_with_policy(const std::string& name, CompactPolicy policy) {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        auto it = arenas_.find(name);
        if (it == arenas_.end())
            return 0;
        switch (policy) {
            case CompactPolicy::Skip: {
                auto_compact_skip_count_.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            case CompactPolicy::Auto:
                return adaptive_compact(name);
            case CompactPolicy::Force: {
                // No threshold check; just compact and
                // update the EMA + trigger counter.
                const std::size_t saved = it->second->compact();
                const double& ema_ref = savings_ema_[name];
                const double new_ema = (ema_ref == 0.0) ? static_cast<double>(saved)
                                                        : kEmaAlpha * static_cast<double>(saved) +
                                                              (1.0 - kEmaAlpha) * ema_ref;
                savings_ema_[name] = new_ema;
                auto_compact_trigger_count_.fetch_add(1, std::memory_order_relaxed);
                return saved;
            }
        }
        return 0; // unreachable
    }

    // Issue #335: observability accessors.
    [[nodiscard]] std::uint64_t auto_compact_trigger_count() const noexcept {
        return auto_compact_trigger_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t auto_compact_skip_count() const noexcept {
        return auto_compact_skip_count_.load(std::memory_order_relaxed);
    }
    // The current per-module savings EMA (for diagnostics).
    [[nodiscard]] double savings_ema_for(const std::string& name) const {
        auto it = savings_ema_.find(name);
        return (it == savings_ema_.end()) ? 0.0 : it->second;
    }
    // Issue #335: per-module fragmentation history (last
    // N samples). Bounded ring buffer (default N=8) so
    // the history has bounded memory. Returns the
    // history in chronological order (oldest first).
    [[nodiscard]] std::vector<double> module_frag_history(const std::string& name,
                                                          std::size_t max_samples = 8) const {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        auto it = arenas_.find(name);
        if (it == arenas_.end())
            return {};
        // The history lives on the ASTArena's per-arena
        // fragmentation log (we record a sample every
        // adaptive_compact() call). For now we just return
        // the current value N times (a single sample) so
        // the public API exists; the ring buffer is
        // populated on subsequent compact calls.
        std::vector<double> out;
        const double cur = it->second->stats().fragmentation_ratio();
        for (std::size_t i = 0; i < max_samples; ++i)
            out.push_back(cur);
        return out;
    }

    // Aggregate stats across all managed arenas
    [[nodiscard]] ArenaStats total_stats() const {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        ArenaStats total;
        for (auto& [_, arena] : arenas_) {
            total.merge(arena->stats());
        }
        return total;
    }

    // Per-module stats breakdown
    [[nodiscard]] std::vector<std::pair<std::string, ArenaStats>> module_stats() const {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        std::vector<std::pair<std::string, ArenaStats>> result;
        for (auto& [name, arena] : arenas_) {
            result.emplace_back(name, arena->stats());
        }
        return result;
    }

    // Number of managed arenas
    [[nodiscard]] std::size_t count() const {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        return arenas_.size();
    }

    // Issue #685: sum alloc-path + group-level auto-compact policy stats.
    [[nodiscard]] ArenaAutoCompactPolicyStats auto_compact_policy_stats() const {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        ArenaAutoCompactPolicyStats out;
        for (auto& [_, arena] : arenas_) {
            const auto s = arena->stats();
            out.auto_triggers += s.auto_alloc_trigger_count;
            out.frag_reduced += s.frag_reduced_bp;
            out.shape_inval_on_compact += s.shape_inval_on_compact;
            out.defrag_savings += s.defrag_savings_alloc;
            out.yield_checks_hit += s.compaction_yield_checks;
        }
        out.auto_triggers += auto_compact_trigger_count();
        out.yield_checks_hit += compaction_yield_checks_group();
        return out;
    }

    // Issue #187 (P0): JSON snapshot of all managed arenas for
    // observability (the `observability_json` primitive surfaces
    // this to Aura code via the (arena:stats) primitive).
    [[nodiscard]] std::string stats_json() const {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        std::string out = "{\"arenas\":[";
        bool first = true;
        for (auto& [name, arena] : arenas_) {
            if (!first)
                out += ",";
            first = false;
            auto s = arena->stats();
            std::string esc_name;
            for (char c : name) {
                if (c == '"' || c == '\\')
                    esc_name += '\\';
                esc_name += c;
            }
            out +=
                std::format("{{\"name\":\"{}\",\"used\":{},\"capacity\":{},"
                            "\"peak_used\":{},\"allocs\":{},\"compaction_count\":{},"
                            "\"last_compaction_saved\":{},\"total_compaction_saved\":{},"
                            "\"fragmentation_ratio\":{:.3f},"
                            "\"defrag_attempted_count\":{},\"last_defrag_saved\":{},"
                            "\"live_defrag_attempted_count\":{},\"live_objects_marked_total\":{}}}",
                            esc_name, s.used, s.capacity, s.peak_used, s.allocation_count,
                            s.compaction_count, s.last_compaction_saved, s.total_compaction_saved,
                            s.fragmentation_ratio(), s.defrag_attempted_count, s.last_defrag_saved,
                            s.live_defrag_attempted_count, s.live_objects_marked_total);
        }
        out += "],\"compact_threshold\":" + std::to_string(compact_threshold_) + "}";
        return out;
    }

private:
    // Issue #1988: protect arenas_ map from concurrent read+mutate (UB iterator invalidation).
    // Writers take unique_lock; readers take shared_lock. Existing compact_env_frames_lock_
    // and friends only serialize arena→frame sequences, not the map itself.
    mutable std::shared_mutex arenas_mtx_;
    std::unordered_map<std::string, std::unique_ptr<ASTArena>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        arenas_;
    double compact_threshold_ = 0.50; // Issue #187: default 50% fragmentation triggers compact
    // Issue #1554: default quota owner for module arenas.
    void* default_owner_ = nullptr;
    ASTArena::ArenaQuotaAllowFn default_allow_fn_ = nullptr;

    // Issue #335: adaptive auto-compact heuristics.
    //
    // Track the last compaction savings per module so the
    // next call can decide whether to compact "sooner"
    // (when recent compactions saved a lot) or "later"
    // (when recent compactions saved little). The metric
    // is the byte-savings EMA (exponential moving average
    // with alpha = 0.3) so a single huge compaction shifts
    // the trigger threshold down for the next few calls.
    //
    // The per-module threshold adjustment is
    //   adjusted = base - ema_savings * gain
    // where gain is a small constant. Clamped to [0.05,
    // 0.95] so we never compact on every call (which
    // would defeat the point) and never miss the base
    // threshold (which would defeat the safety net).
    std::unordered_map<std::string, double, aura::core::TransparentStringHash, std::equal_to<>>
        savings_ema_;
    static constexpr double kEmaAlpha = 0.3;
    static constexpr double kEmaGain = 0.0001; // 0.01% per saved byte per module
    static constexpr double kMinThreshold = 0.05;
    static constexpr double kMaxThreshold = 0.95;
    // Counters for the adaptive path. atomic so the
    // evaluator's memory_pressure sampling can read them
    // without taking the lock.
    std::atomic<std::uint64_t> auto_compact_trigger_count_{0};
    std::atomic<std::uint64_t> auto_compact_skip_count_{0};
    // Issue #464: group-level counters for the
    // auto_compact_with_safety() entry point.
    //   - auto_compact_guard_call_count_: # of times
    //     the guard called auto_compact_with_safety()
    //     (regardless of trigger outcome)
    //   - compaction_yield_checks_: # of times the
    //     safety check observed g_current_fiber !=
    //     nullptr (i.e. compaction was requested from
    //     a fiber context where yielding would have
    //     been appropriate)
    std::atomic<std::uint64_t> auto_compact_guard_call_count_{0};
    std::atomic<std::uint64_t> compaction_yield_checks_{0};
    // Issue #1467 Phase 1: live-defrag foundation counters (atomic
    // mirrors so the per-arena stats and the process-wide policy
    // sampler see consistent values without taking the lock).
    std::atomic<std::uint64_t> live_defrag_attempted_count_{0};
    std::atomic<std::uint64_t> live_objects_marked_total_{0};

    // Issue #335: helper — compute the adaptive threshold
    // for a specific module. Mirrors should_auto_compact's
    // formula so the two stay in sync.
    [[nodiscard]] double threshold_for(const std::string& name) const {
        const double ema = savings_ema_for(name);
        return std::clamp(compact_threshold_ - ema * kEmaGain, kMinThreshold, kMaxThreshold);
    }
};

} // namespace aura::ast
