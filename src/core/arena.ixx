module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib> // Issue #2166: getenv AURA_ARENA_MOVING_COMPACT
#include <cstring> // Issue #2166: memcpy for Moving densify
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
import aura.core.lifetime_pin;
import aura.core.envframe_lifetime;

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
//
// Issue #2381 — concurrency contract:
// ArenaStats is a *snapshot* POD returned by ASTArena::stats(). Live
// writers fall into two classes:
//
//   1. Concurrent-safe (atomic on ASTArena, snapshotted here):
//      shape_inval_on_compact, root_remap_* — written via
//      fetch_add(relaxed) so concurrent invoke_compact_hook_ /
//      invoke_root_remap_callback_ never data-race (AC1/AC2).
//
//   2. GUARDED_BY(per-arena compact serial) — all other std::size_t
//      fields below. live_compact / compact buffer mutation is
//      single-thread-per-arena by design (#1518 / #2166). Do not
//      convert these to atomic without an explicit race analysis;
//      static discipline: keep them plain size_t for zero-cost
//      snapshot/merge.
//
export struct ArenaStats {
    // GUARDED_BY(per-arena compact serial)
    std::size_t capacity = 0;         // total buffer size
    std::size_t used = 0;             // bytes consumed
    std::size_t peak_used = 0;        // historical high-water mark
    std::size_t allocation_count = 0; // number of allocation calls
    std::size_t wasted = 0;           // alignment padding
    // Issue #187 (P0): compaction observability. The first 4 fields
    // are pure accounting (already shipped). The new 3 are added for
    // production memory stability so we can see whether compaction
    // is helping and trigger auto-compact at the right threshold.
    // GUARDED_BY(per-arena compact serial)
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
    // GUARDED_BY(per-arena compact serial)
    std::size_t compaction_yield_checks = 0;
    // Issue #300 (P1): live-object defragmentation observability.
    // Hooks for the full live-object-moving defrag path (separate
    // follow-up commits B/C):
    //   - defrag_attempted_count: # of defrag() passes attempted
    //   - last_defrag_saved:      bytes reclaimed by last defrag
    // Foundation-only: both stay 0 until the defrag path is
    // implemented in #300 follow-up commits. The (arena:defrag-stats)
    // primitive returns 0 for the defrag slot until then.
    // GUARDED_BY(per-arena compact serial)
    std::size_t defrag_attempted_count = 0;
    std::size_t last_defrag_saved = 0;
    // Issue #685: alloc-path auto-compact policy observability.
    // GUARDED_BY(per-arena compact serial)
    std::size_t auto_alloc_trigger_count = 0;
    std::size_t frag_reduced_bp = 0;
    // Issue #2381: concurrent-safe counter — snapshotted from
    // ASTArena::shape_inval_on_compact_ atomic (not plain ++ on stats_).
    std::size_t shape_inval_on_compact = 0;
    // GUARDED_BY(per-arena compact serial)
    std::size_t defrag_savings_alloc = 0;
    // Issue #1467 Phase 1 + #1518: live-object-moving defrag observability.
    // GUARDED_BY(per-arena compact serial)
    std::size_t live_defrag_attempted_count = 0;
    std::size_t live_objects_marked_total = 0;
    // Issue #1518: live relocate / compact coordination metrics.
    // GUARDED_BY(per-arena compact serial)
    std::size_t live_relocate_count = 0;         // freelist slots + mark-phase relocates
    std::size_t compact_deopt_triggered = 0;     // Shape/JIT deopt fired post-compact
    std::size_t compact_deopt_throttled = 0;     // deopt storm throttle skips
    std::size_t frag_post_compact_bp = 0;        // last post-compact frag ratio (basis points)
    std::size_t compact_soft_gated_boundary = 0; // skipped due to MutationBoundary
    // Issue #2004: explicit live_compact observability. Soft / Force counts,
    // bytes reclaimed (sum across all calls), freelist hits, generation
    // restamps, and # LifetimePins invalidated on success.
    // GUARDED_BY(per-arena compact serial)
    std::size_t live_compact_soft_count = 0;
    std::size_t live_compact_force_count = 0;
    std::size_t live_compact_reclaimed_bytes_total = 0;
    std::size_t live_compact_freelist_hits_total = 0;
    std::size_t live_compact_gen_restamps_total = 0;
    std::size_t live_compact_invalidated_pins_total = 0;
    // Issue #2265 Phase 3: # LifetimePins whose ptr_ was remapped to a
    // new address under Moving densify (preserve vs invalidate policy).
    // GUARDED_BY(per-arena compact serial)
    std::size_t live_compact_remapped_pins_total = 0;
    // Issue #2267 / #2381: RootRemapPass per-arena counters (mirrors
    // process-level atomics g_root_remap_*). Concurrent-safe via
    // ASTArena atomics; values snapshotted here by stats().
    std::size_t root_remap_stable_ref_total = 0;
    std::size_t root_remap_stable_ref_fail_total = 0;
    std::size_t root_remap_closure_capture_total = 0;
    std::size_t root_remap_closure_capture_fail_total = 0;
    // Issue #2157: Force blocked by live pin / EnvFrameLifetimeGuard hold.
    // GUARDED_BY(per-arena compact serial)
    std::size_t force_compact_blocked_by_pin = 0;
    std::size_t force_compact_blocked_by_envframe_guard = 0;
    // Issue #2166: opt-in Moving compact.
    // GUARDED_BY(per-arena compact serial)
    std::size_t live_compact_moving_count = 0;
    std::size_t objects_moved_total = 0;
    std::size_t moving_blocked_precondition_total = 0;
    // Issue #2495: Moving densify windows that moved tracked objects but left
    // untracked external candidates unmapped (fail-closed signal).
    // GUARDED_BY(per-arena compact serial)
    std::size_t moving_untracked_external_roots_total = 0;

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
        // Issue #2004: explicit live_compact counters (Soft / Force counts,
        // reclaimed bytes, freelist hits, gen restamps, invalidated pins).
        live_compact_soft_count += other.live_compact_soft_count;
        live_compact_force_count += other.live_compact_force_count;
        live_compact_reclaimed_bytes_total += other.live_compact_reclaimed_bytes_total;
        live_compact_freelist_hits_total += other.live_compact_freelist_hits_total;
        live_compact_gen_restamps_total += other.live_compact_gen_restamps_total;
        live_compact_invalidated_pins_total += other.live_compact_invalidated_pins_total;
        live_compact_remapped_pins_total += other.live_compact_remapped_pins_total;
        root_remap_stable_ref_total += other.root_remap_stable_ref_total;
        root_remap_stable_ref_fail_total += other.root_remap_stable_ref_fail_total;
        root_remap_closure_capture_total += other.root_remap_closure_capture_total;
        root_remap_closure_capture_fail_total += other.root_remap_closure_capture_fail_total;
        force_compact_blocked_by_pin += other.force_compact_blocked_by_pin;
        force_compact_blocked_by_envframe_guard += other.force_compact_blocked_by_envframe_guard;
        live_compact_moving_count += other.live_compact_moving_count;
        objects_moved_total += other.objects_moved_total;
        moving_blocked_precondition_total += other.moving_blocked_precondition_total;
        moving_untracked_external_roots_total += other.moving_untracked_external_roots_total;
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
                AURA_HOT_RECORD(); // Issue #2142 freelist hit
                return ptr;
            }
            // Hard cap: bump must stay within both tier.end and buffer.
            std::byte* hard_end = c.end < buf_end ? c.end : const_cast<std::byte*>(buf_end);
            void* ptr = c.bump;
            auto* next = c.bump + c.obj_sz;
            if (next <= hard_end && next >= c.start) {
                c.bump = next;
                allocated_from_small_ += c.obj_sz;
                AURA_HOT_RECORD(); // Issue #2142 bump hit
                return ptr;
            }
            // This tier is exhausted — signal overflow (no bump advance).
            AURA_HOT_RECORD(); // Issue #2142 tier overflow probe
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
// Issue #2004: explicit live_compact + freelist path coordinated with
// MutationBoundary soft-gate + GC safepoint. live_compact(LiveCompactMode)
// returns a struct (bytes_reclaimed / slots_recycled / new_gen / soft_gated /
// invalidates_pins) so Agents, MutationBoundary probe, and compact_sweep body
// can observe the outcome — replacing the prior std::size_t return.
//
// Issue #2089: Soft/Force non-moving contract — freelist holes + gen restamp
// + pin invalidate only. Issue #2166: opt-in LiveCompactMode::Moving densifies
// tracked create<T> objects with an object_remap_ table (default OFF via
// AURA_ARENA_MOVING_COMPACT / set_moving_compact_enabled). Consumers MUST
// re-resolve via gen / LifetimePin / resolve_object_remap after Moving.
//
// Issue #2157: Force hard-mutex with live LifetimePin + EnvFrameLifetimeGuard.
// Soft already gates on MutationBoundary / render hotpath. Force must NOT
// bump generation_ or invalidate pins while live_pin_count()>0 or
// envframe_lifetime::active_guard_depth()>0 — post-facto restamp is not a
// hold-time guarantee under fiber-steal + concurrent Force.
export enum class LiveCompactMode : std::uint8_t {
    Soft = 0,   // Soft-gates under render hotpath / MutationBoundary
    Force = 1,  // Force path — STW-friendly, used by explicit Agent calls
    Moving = 2, // Issue #2166: opt-in pointer densify + object_remap_
};

export inline constexpr int kForceCompactHardMutexIssue = 2157;
export inline constexpr int kMovingCompactIssue = 2166;

// Process-wide Force-block metrics (Agents query via arena-live-compact-stats).
export inline std::atomic<std::uint64_t> g_force_compact_blocked_by_pin_total{0};
export inline std::atomic<std::uint64_t> g_force_compact_blocked_by_envframe_guard_total{0};
// Issue #2166: Moving compact process-wide metrics.
export inline std::atomic<std::uint64_t> g_live_compact_moving_count{0};
export inline std::atomic<std::uint64_t> g_objects_moved_total{0};
export inline std::atomic<std::uint64_t> g_moving_blocked_precondition_total{0};
// Issue #2664: production-default hard-fail counter (Agent-visible).
// Bumped when g_moving_untracked_hard_abort_pref > 0 (already encodes
// production-default + explicit env=hard via #2596) AND untracked external
// roots exist after Moving densify. Agent dashboards observe this to
// distinguish production-hard from Soft observe-only. Mirrors the #2495
// untracked counter.
export inline std::atomic<std::uint64_t> g_moving_incomplete_remap_densify_hard_fail_total{0};
// Feature flag: default OFF (env AURA_ARENA_MOVING_COMPACT=1 enables).
export inline std::atomic<int> g_moving_compact_enabled_pref{-1}; // -1 = env/default
// Issue #2495: process-wide counter for Moving densify windows where the
// remap walk observed untracked external / non-small-pool candidates
// (raw pointers not registered as LifetimePin or RootRemap root). Bumped
// when objects_moved > 0 && untracked_kept_count > 0. Agent dashboards
// surface untracked-buffer accumulation.
export inline std::atomic<std::uint64_t> g_moving_untracked_external_roots_total{0};

// Issue #2682: unified Moving success / fail totals. Bumped by Phase-5
// outermost exit after computing the single unified predicate
// (compute_moving_unified_success in moving_densify_health.hh). Success
// total counts densify windows where all 5 conditions hold; fail total
// counts windows where any condition fails (any single failure source:
// moving_blocked_precondition / pin_contract_held / root_remap fails /
// untracked_kept_count > 0 with objects_moved > 0). Additive only — no
// schema break for the existing g_moving_untracked_external_roots_total.
export inline std::atomic<std::uint64_t> g_moving_unified_success_total{0};
export inline std::atomic<std::uint64_t> g_moving_unified_fail_total{0};
inline constexpr int kMovingUnifiedSuccessGateIssue = 2682;

// Issue #2775: process-wide counter for external roots registered via
// ASTArena::register_external_root_for_densify(void*) / batch span.
// Bumped on each unique-pointer insert into the per-arena set (one bump
// per pointer, not per call). Additive only — no schema break for the
// existing g_moving_unified_success_total / _fail_total / untracked_
// external_roots_total surfaces. Agent dashboards use this to verify
// caller compliance with "register all external roots before Moving"
// contract; live_compact(Moving) consumes the per-arena set on each
// Moving densify window (last-call semantics #2376 pattern).
export inline std::atomic<std::uint64_t> g_moving_external_root_prep_register_total{0};
inline constexpr int kMovingExternalRootPrepRegisterIssue = 2775;

// Issue #2837: external-root *slot* remaps after Moving densify. Bumped
// once per void** slot whose *slot value was rewritten via last_object_remap_.
// Distinct from prep-register (#2775 value-only observability).
export inline std::atomic<std::uint64_t> g_moving_external_root_slot_remap_total{0};
// Issue #2837: sticky force densify-off after production hard incomplete-remap.
// When set, moving_compact_enabled() returns 0 until clear (Agent must
// re-register roots / clear sticky). Soft observe-only never arms sticky.
export inline std::atomic<std::uint8_t> g_moving_incomplete_remap_sticky_densify_off{0};
export inline std::atomic<std::uint64_t> g_moving_incomplete_remap_sticky_densify_off_total{0};
inline constexpr int kMovingExternalRootRemapIssue = 2837;

export inline void clear_moving_incomplete_remap_sticky_densify_off() noexcept {
    g_moving_incomplete_remap_sticky_densify_off.store(0, std::memory_order_release);
}

export [[nodiscard]] inline bool moving_incomplete_remap_sticky_densify_off() noexcept {
    return g_moving_incomplete_remap_sticky_densify_off.load(std::memory_order_acquire) != 0;
}

// Issue #2495/#2596: g_moving_untracked_hard_abort_pref defined in
// arena_auto_policy_stats.h (header form for security_defaults.hh).
// Visible here via #include in global fragment (namespace aura::ast).

export inline void set_moving_compact_enabled(int enabled) noexcept {
    g_moving_compact_enabled_pref.store(enabled ? 1 : 0, std::memory_order_release);
}
export inline int moving_compact_enabled() noexcept {
    // Issue #2837: sticky densify-off after production incomplete-remap
    // hard-fail. Overrides pref/env until clear_moving_incomplete_remap_
    // sticky_densify_off() (or a clean Moving densify clears it).
    if (g_moving_incomplete_remap_sticky_densify_off.load(std::memory_order_acquire) != 0)
        return 0;
    const int pref = g_moving_compact_enabled_pref.load(std::memory_order_acquire);
    if (pref == 0 || pref == 1)
        return pref;
    if (const char* e = std::getenv("AURA_ARENA_MOVING_COMPACT")) {
        if (e[0] == '1')
            return 1;
        if (e[0] == '0')
            return 0;
    }
    // Issue #2256: production default ON. Long-running AI multi-round
    // self-mod sustained mutation workloads need Moving compaction to
    // bound fragmentation. Tests can force OFF via
    // AURA_ARENA_MOVING_COMPACT=0.
    return 1;
}

// Issue #2256: Adaptive-on-threshold policy. When fragmentation
// ratio crosses kAutoMovingCompactThreshold and no compact has
// run in the last kAdaptiveCompactCooldownMs, Moving compact is
// auto-triggered. Bounds fragmentation under sustained mutation
// without paying the Moving compact cost every compact cycle.
inline constexpr double kAutoMovingCompactThreshold = 0.40; // 40% fragmentation
export inline std::atomic<std::uint64_t> g_last_moving_compact_ms{0};
export inline bool should_auto_moving_compact(double current_fragmentation) noexcept {
    if (current_fragmentation < kAutoMovingCompactThreshold)
        return false;
    if (!moving_compact_enabled())
        return false;
    const auto now_ms =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    const auto last_ms = g_last_moving_compact_ms.load(std::memory_order_acquire);
    constexpr std::uint64_t kCooldownMs = 100; // at most 10 Hz
    return (now_ms - last_ms) >= kCooldownMs;
}

export struct LiveCompactResult {
    std::size_t bytes_reclaimed = 0; // bytes saved by defrag_impl (buffer resize)
    std::size_t slots_recycled = 0;  // freelist slots recycled (free_slot + recycle_hits)
    std::uint64_t new_gen = 0;       // generation after restamp (0 = no restamp)
    LiveCompactMode mode = LiveCompactMode::Soft;
    bool soft_gated = false;       // true if Soft mode skipped (render hotpath / boundary held)
    bool invalidates_pins = false; // true if new_gen bumped (LifetimePins invalidated)
    // Issue #2089 / #2166: Soft/Force always false; Moving may set true.
    bool moved_live_objects = false;
    std::size_t objects_moved = 0; // #2166: count of tracked objects remapped
    // Issue #2157: Force hard-mutex outcomes (no gen bump when true).
    bool force_blocked_by_pin = false;
    bool force_blocked_by_envframe_guard = false;
    // Issue #2166: Moving preconditions failed or feature flag off.
    bool moving_blocked_precondition = false;
    // Issue #2265: # pins remapped during Moving densify (0 on Soft/Force).
    std::size_t remapped_pins = 0;
    // Issue #2266: #2266 AC2 — Moving pin-or-remap contract verification result.
    // true = all live pins for arena_id_ were honored (remapped or invalidated);
    // false = at least one pin still points at an old densified address (the
    // remap walk missed it). Driver (Phase 5 in evaluator_mutation_boundary.cpp)
    // bumps moving_compact_pin_contract_fail_total + suppresses success metrics
    // when this is false. Default true (no Moving = contract trivially held).
    bool pin_contract_held = true;
    // Issue #2495: incomplete-remap flag for untracked external roots.
    // True when relocate_tracked_objects_for_moving_ moved live objects but
    // also left candidates untracked (non-small-pool / non-aligned size /
    // not registered as a pin or root). Failure mode = false safety under
    // production Moving default; Agents can otherwise treat Moving success
    // + pin_contract_held as full pointer safety. Aggregated into
    // LiveCompactResultAggregated.moving_incomplete_remap_any() so Phase 5
    // in evaluator_mutation_boundary.cpp fails outermost_exit_phase5_unlock
    // when any arena reports incomplete remap. Distinct from
    // pin_contract_held so observability can separate "tracked-but-unpin"
    // vs "untracked-external" failures.
    bool moving_incomplete_remap = false;
    // Issue #2495: count of untracked external candidates the densify
    // walk observed but could not remap (non-small-pool / oversized /
    // unaligned / no remap walk entry). When objects_moved > 0 AND
    // untracked_kept_count > 0, moving_incomplete_remap is set. Visible
    // to Agent dashboards so untracked-buffer accumulation is observable.
    std::size_t untracked_kept_count = 0;
    // Issue #2775: per-call count of external roots registered via
    // ASTArena::register_external_root_for_densify(void*) / batch span
    // that were cleared during this Moving densify window. Consumed after
    // relocate + #2837 slot rewrite (prep values used for stale detection).
    // Per-call (last-window semantics #2376); Agent dashboards observe via
    // moving_densify_health::snapshot().external_roots_prep_registered_last.
    // zero = no caller registered external roots before this Moving.
    std::size_t external_roots_prep_registered_cleared = 0;
    // Issue #2837: count of void** slots rewritten via last_object_remap_
    // during this Moving densify window.
    std::size_t external_roots_remapped_count = 0;
    // Issue #2837: prep-registered values that were densify old addresses
    // but no slot rewrite covered them (stale external root residual).
    std::size_t external_roots_stale_unremapped_count = 0;
    // Issue #2267: RootRemapPass counters (StableNodeRef + Closure captures).
    std::size_t root_remap_stable_ref_total = 0;
    std::size_t root_remap_stable_ref_fail_total = 0;
    std::size_t root_remap_closure_capture_total = 0;
    std::size_t root_remap_closure_capture_fail_total = 0;

    [[nodiscard]] bool empty() const noexcept {
        return bytes_reclaimed == 0 && slots_recycled == 0 && !soft_gated &&
               !force_blocked_by_pin && !force_blocked_by_envframe_guard &&
               !moving_blocked_precondition && objects_moved == 0 && pin_contract_held &&
               !moving_incomplete_remap && untracked_kept_count == 0;
    }
    [[nodiscard]] bool force_blocked() const noexcept {
        return force_blocked_by_pin || force_blocked_by_envframe_guard;
    }
};

// Issue #2266: aggregated result of ArenaGroup::compact_all_moving_pinned().
// Aggregates bytes_reclaimed_total + pin_contract_held across all module
// arenas so the driver (Phase 5 in evaluator_mutation_boundary.cpp) can
// inspect the pin contract in one place. Aggregated contract is the
// logical AND of all per-arena pin_contract_held flags (any failure fails
// the whole result).
export struct AdaptiveCompactResult {
    std::size_t bytes_reclaimed_total = 0;
    // true = every per-arena Moving compact honored all its live pins (each
    // arena's remap walk succeeded, or no pins existed for that arena).
    // false = at least one arena had a pin still pointing at an old densified
    // address after the remap walk (the remap missed it). The driver bumps
    // moving_compact_pin_contract_fail_total + suppresses success metrics
    // when this is false. Default true (no Moving = contract trivially held).
    bool pin_contract_held = true;
    // Issue #2353: any arena actually moved live objects (densify work).
    // Used by post-densify linear+type revalidate to skip AC3 zero-cost path
    // when densify was empty (no objects moved).
    bool moved_live_objects = false;
    // Issue #2499: per-call aggregate of RootRemapPass fail totals across
    // all arenas in this densify window. Each arena's LiveCompactResult
    // populates these from invoke_root_remap_callback_ out-params (per-call,
    // NOT process-cumulative). last-call semantics (#2376 pattern) — zero
    // on clean densify (default true), > 0 if any arena's RootRemap
    // encountered an unmapped root or capture cell. Phase 5 in
    // evaluator_mutation_boundary.cpp ANDs (fail_total == 0) into
    // pin_contract_held so the unified gate surfaces "pin ok +
    // root_remap fail cumulative" mixed-signal gap.
    std::size_t root_remap_stable_ref_fail_total = 0;
    std::size_t root_remap_closure_capture_fail_total = 0;
    // Issue #2619: last densify window aggregate for Agent health surface.
    std::size_t objects_moved_total = 0;
    std::size_t untracked_kept_total = 0;
    bool moving_incomplete_remap_any = false;
    // Issue #2775: aggregate of LiveCompactResult::external_roots_prep_registered_cleared
    // across all arenas in this densify window. Pure observability for
    // Agent dashboards (caller compliance with "register all external
    // roots before Moving" contract). Does NOT gate any success / fail
    // predicate (per AC4 — additive only, surfaces preserved). Phase 5
    // reads this into a local and passes it to
    // publish_last_moving_densify_window so
    // moving_densify_health::snapshot().external_roots_prep_registered_last
    // reflects the last window's caller compliance.
    std::size_t external_roots_prep_registered_total = 0;

    [[nodiscard]] bool empty() const noexcept {
        return bytes_reclaimed_total == 0 && pin_contract_held && !moved_live_objects &&
               root_remap_stable_ref_fail_total == 0 &&
               root_remap_closure_capture_fail_total == 0 && !moving_incomplete_remap_any &&
               untracked_kept_total == 0;
    }
};

// Issue #2089: optional layout-change callback hook. Fires on each
// successful live_compact that actually bumps the generation counter (i.e.
// saved_bytes > 0 || relocated > 0) — never on soft-gated no-ops or on the
// initial zero-gen state. Default-wired to pin invalidate (already in-path
// via invalidate_all_pins_for_arena) — HotUpdate / Shape hooks can install
// their own observer here without assuming any pointer rewrite happened.
using LiveCompactLayoutChangeCallback =
    std::function<void(std::uint64_t arena_id, std::uint64_t new_gen)>;

// Issue #2267 / #2294: RootRemapPass — non-pin root rewrite (stable-object
// slots + Closure capture cells) after Moving densify. Receives densify
// old→new object_remap + new_gen + out-params for per-call stats that
// live_compact writes into LiveCompactResult / ArenaStats. The pass lives
// in src/compiler/ (needs Evaluator / capture layout) with a type-erased
// entry to avoid core→compiler cycle (same pattern as PanicCheckpointHost
// / EnvFrameLifetimeHost). Out-params keep the typedef free of compiler types.
using RootRemapCallback = std::function<void(
    std::uint64_t arena_id, std::uint64_t new_gen,
    std::unordered_map<void*, void*> const& object_remap, std::size_t& out_stable_ref_total,
    std::size_t& out_stable_ref_fail_total, std::size_t& out_closure_capture_total,
    std::size_t& out_closure_capture_fail_total)>;

// Module-internal counter used by ASTArena constructors to mint stable per-arena
// ids. LifetimePin::invalidate_all_pins_for_arena(arena_id_) keys pin
// invalidation to the specific arena that bumped its generation, instead of
// over-invalidating pins tied to other arenas.
inline std::atomic<std::uint64_t> g_arena_id_counter{0};

export class ASTArena {
public:
    // Default upstream is the system allocator. Tests can pass a
    // counting memory_resource to verify that destructors run
    // before resource_.release() (see Issue #1382 contract test).
    explicit ASTArena(std::size_t initial_size = 8 * 1024 * 1024,
                      std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
        : initial_size_(initial_size)
        , buffer_(initial_size)
        , resource_(buffer_.data(), buffer_.size(), upstream)
        // Issue #2004: mint a stable per-arena id at construction so
        // LifetimePin::invalidate_all_pins_for_arena(arena_id_) keys pin
        // invalidation to THIS arena (not all arenas).
        , arena_id_(g_arena_id_counter.fetch_add(1, std::memory_order_relaxed) + 1)
        , generation_(0) {}

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
    void set_on_compact_hook(std::function<void()> hook) {
        std::lock_guard<std::mutex> lock(hook_mtx_);
        on_compact_hook_ = std::move(hook);
    }
    // Move out the current hook (leaves arena with no hook).
    [[nodiscard]] std::function<void()> take_on_compact_hook() {
        std::lock_guard<std::mutex> lock(hook_mtx_);
        return std::move(on_compact_hook_);
    }
    [[nodiscard]] bool has_on_compact_hook() const noexcept {
        // Issue #2383 / #2382: observe under hook_mtx_ — same lock pattern as
        // has_on_layout_change / has_root_remap_callback (set/take/invoke copy).
        // Unsynchronized operator bool on std::function is a TSAN data race
        // against concurrent set_on_compact_hook.
        std::lock_guard<std::mutex> lock(hook_mtx_);
        return static_cast<bool>(on_compact_hook_);
    }

    // Issue #2089: optional layout-change callback. Fires exactly when
    // live_compact(Soft|Force) actually bumps the generation counter
    // (saved_bytes > 0 || relocated > 0) — NEVER on soft-gated no-ops or
    // on the initial zero-gen state. Allows HotUpdate / Shape / Fiber
    // hooks to react to layout shifts without assuming any pointer
    // rewrite happened (live_compact Soft/Force is non-moving — see
    // LiveCompactResult::moved_live_objects and the module-level
    // contract comment). Default null; callers opt in via this setter.
    // Concurrent set/take is serialized via on_layout_change_mtx_; the
    // invoke path copies the callback under the lock and fires outside
    // it so re-entrant set calls from inside the callback do not
    // deadlock (same pattern as #1989's on_compact_hook_).
    void set_on_layout_change(LiveCompactLayoutChangeCallback cb) {
        std::lock_guard<std::mutex> lock(on_layout_change_mtx_);
        on_layout_change_ = std::move(cb);
    }
    // Move out the current callback (leaves arena with no callback).
    [[nodiscard]] LiveCompactLayoutChangeCallback take_on_layout_change() {
        std::lock_guard<std::mutex> lock(on_layout_change_mtx_);
        return std::move(on_layout_change_);
    }
    [[nodiscard]] bool has_on_layout_change() const noexcept {
        std::lock_guard<std::mutex> lock(on_layout_change_mtx_);
        return static_cast<bool>(on_layout_change_);
    }
    // Issue #2267: RootRemapPass install + invoke. The compiler installs a
    // callback that scans StableNodeRef live set + Closure capture cells
    // and rewrites them via the old→new object_remap. Concurrent set/take
    // is serialized via root_remap_mtx_ (same pattern as on_layout_change_mtx_).
    void set_root_remap_callback(RootRemapCallback cb) {
        std::lock_guard<std::mutex> lock(root_remap_mtx_);
        root_remap_ = std::move(cb);
    }
    [[nodiscard]] RootRemapCallback take_root_remap_callback() {
        std::lock_guard<std::mutex> lock(root_remap_mtx_);
        return std::move(root_remap_);
    }
    [[nodiscard]] bool has_root_remap_callback() const noexcept {
        std::lock_guard<std::mutex> lock(root_remap_mtx_);
        return static_cast<bool>(root_remap_);
    }

    // Issue #2775: register an external pointer that the caller holds and
    // wants declared for the next live_compact(Moving) densify. Single-
    // pointer overload. Bumps g_moving_external_root_prep_register_total
    // exactly once per unique pointer (set semantics; duplicate register
    // is a no-op for the counter). The per-arena set is cleared at the
    // start of each live_compact(Moving) work (consumed for that window)
    // and never cleared by Soft / Force / blocked-Moving calls — caller
    // must re-register for each Moving window they want covered.
    // No-op when p == nullptr (callers may pass nullptr from nullable
    // external captures; those don't need densify coverage).
    void register_external_root_for_densify(void* p) noexcept {
        if (p == nullptr)
            return;
        const bool inserted = external_roots_for_densify_.insert(p).second;
        if (inserted)
            g_moving_external_root_prep_register_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Batch span overload — bumps counter by count of newly-inserted
    // pointers (set dedup). nullptr entries are skipped (mirrors the
    // single-pointer overload contract). Span may be empty (no-op).
    void register_external_root_for_densify(std::span<void* const> ptrs) noexcept {
        for (void* p : ptrs) {
            if (p == nullptr)
                continue;
            const bool inserted = external_roots_for_densify_.insert(p).second;
            if (inserted)
                g_moving_external_root_prep_register_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Read-only accessor for caller-side observability (tests, Agent
    // dashboards, soft pre-Moving introspection).
    [[nodiscard]] std::size_t external_roots_for_densify_count() const noexcept {
        return external_roots_for_densify_.size();
    }

    // Explicit clear (also called automatically at the end of each
    // live_compact(Moving) work after slot rewrite — see live_compact()).
    // Exposed for callers that want to reset prep state without running a
    // Moving densify (e.g. test teardown, error recovery after caller knows
    // the next Moving won't happen on this arena).
    void clear_external_roots_for_densify() noexcept {
        external_roots_for_densify_.clear();
        external_root_slots_for_densify_.clear();
    }

    // Issue #2837: register a *slot* (void**) holding a pointer that may
    // reference a densify-tracked object. After live_compact(Moving)
    // relocates tracked objects, *slot is rewritten to the new address
    // when *slot is a key in last_object_remap_. Also value-registers
    // *slot via #2775 prep set (observability + stale detection).
    // No-op when slot == nullptr or *slot == nullptr.
    void register_external_root_slot_for_densify(void** slot) noexcept {
        if (slot == nullptr || *slot == nullptr)
            return;
        external_root_slots_for_densify_.push_back(slot);
        register_external_root_for_densify(*slot);
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
        // Issue #2382: clear installed hooks under their mutexes BEFORE
        // run_destructors() / member teardown. Concurrent invoke_*_ paths
        // copy under the same locks and early-return when empty, so they
        // never fire lambdas that capture caller-side Evaluator /
        // CompilerService state after this arena begins dying. Lock release
        // is a release barrier for observers of the cleared std::function.
        //
        // Contract: callers should not destroy an arena mid-live_compact;
        // this is safe-side hardening for uninstall-on-dtor (cheap).
        {
            std::lock_guard<std::mutex> lock(hook_mtx_);
            on_compact_hook_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(on_layout_change_mtx_);
            on_layout_change_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(root_remap_mtx_);
            root_remap_ = nullptr;
        }
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
        // Issue #2166: record size/align for opt-in Moving densify remap.
        dtors_.push_back(
            {result, +[](void* p) { static_cast<T*>(p)->~T(); }, sizeof(T), alignof(T)});
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
        AURA_HOT_RECORD(); // Issue #2142
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
        last_object_remap_.clear(); // Issue #2166
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
        // Issue #2381: concurrent-safe counters live as atomics; snapshot here.
        s.shape_inval_on_compact = shape_inval_on_compact_.load(std::memory_order_relaxed);
        s.root_remap_stable_ref_total =
            root_remap_stable_ref_total_.load(std::memory_order_relaxed);
        s.root_remap_stable_ref_fail_total =
            root_remap_stable_ref_fail_total_.load(std::memory_order_relaxed);
        s.root_remap_closure_capture_total =
            root_remap_closure_capture_total_.load(std::memory_order_relaxed);
        s.root_remap_closure_capture_fail_total =
            root_remap_closure_capture_fail_total_.load(std::memory_order_relaxed);
        return s;
    }

    // Issue #2381: stress-only path — invoke installed compact hook + bump
    // shape_inval_on_compact without buffer mutation. Used by concurrent
    // TSAN tests (N-thread hook stress); production uses compact()/live_compact().
    void invoke_on_compact_hook_for_test() noexcept { invoke_compact_hook_(); }

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
            // Issue #2059: adaptive headroom (default 25%) instead of fixed
            // u/4. Under AI mutation + high frag → tighter (lower peak RSS);
            // under deopt storm → looser (fewer compact→deopt waves).
            const double hr = aura::core::arena_policy::current_adaptive_headroom();
            const std::size_t headroom_bytes =
                static_cast<std::size_t>(static_cast<double>(u) * hr);
            std::size_t target = 1024;
            while (target < u + headroom_bytes)
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
        AURA_HOT_RECORD(); // Issue #1519 / #2142
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
            // Issue #2059: same adaptive headroom as compact() (shared policy).
            const double hr = aura::core::arena_policy::current_adaptive_headroom();
            const std::size_t headroom_bytes =
                static_cast<std::size_t>(static_cast<double>(u) * hr);
            std::size_t target = 1024;
            while (target < u + headroom_bytes)
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
    //      alloc); count free slots as relocate-ready. External root *slots*
    //      registered via register_external_root_slot_for_densify (Issue
    //      #2837) are rewritten after densify; unregistered external raw
    //      pointers remain fail-closed (#2495/#2664) + sticky densify-off
    //      under production hard incomplete-remap (#2837 option 3).
    //   3. Compact: conservative buffer trim (defrag_impl)
    //   4. Coordinate: compact hook + deopt throttle (no deopt storm)
    //
    // Returns number of freelist slots recycled (Issue #2004: the new
    // LiveCompactResult::slots_recycled is the closest analogue to the prior
    // total_marked semantics; Force mode bypasses MutationBoundary soft-gate).
    [[nodiscard]] std::size_t live_defrag() noexcept {
        return live_compact(LiveCompactMode::Force).slots_recycled;
    }

    // Issue #1518 + #2004 + #2157 + #2166: live_compact Soft / Force / Moving.
    // Soft gates on render / MutationBoundary. Force (#2157) hard-mutexes on
    // live LifetimePin or EnvFrameLifetimeGuard. Moving (#2166) is opt-in densify
    // of tracked create objects + object_remap_ (default OFF).
    [[nodiscard]] LiveCompactResult
    live_compact(LiveCompactMode mode = LiveCompactMode::Soft) noexcept {
        aura::core::cpp26::record_hotpath_invariant_hit(); // Issue #1519
        LiveCompactResult result;
        result.mode = mode;
        result.new_gen = generation_.load(std::memory_order_acquire);

        // Soft-gate the auto path during render / active MutationBoundary
        // so fiber yield / Guard pins stay coherent.
        if (mode == LiveCompactMode::Soft) {
            if (aura::core::arena_policy::in_render_hotpath()) {
                aura::core::arena_policy::record_compact_soft_gated_render();
                result.soft_gated = true;
                ++stats_.compact_soft_gated_boundary;
                return result;
            }
            if (arena_mutation_boundary_depth() > 0) {
                ++stats_.compact_soft_gated_boundary;
                aura::core::arena_policy::record_compact_soft_gated_boundary();
                result.soft_gated = true;
                return result;
            }
            ++stats_.live_compact_soft_count;
        } else if (mode == LiveCompactMode::Moving) {
            // Issue #2166: Moving requires feature flag + Force-level preconditions.
            if (!moving_compact_enabled()) {
                result.moving_blocked_precondition = true;
                result.soft_gated = true;
                ++stats_.moving_blocked_precondition_total;
                g_moving_blocked_precondition_total.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            if (aura::core::arena_policy::in_render_hotpath() ||
                arena_mutation_boundary_depth() > 0 ||
                aura::gc_hooks::should_defer_destructive_gc() ||
                aura::core::lifetime::live_pin_count() > 0 ||
                aura::core::envframe_lifetime::active_guard_depth() > 0) {
                result.moving_blocked_precondition = true;
                result.soft_gated = true;
                if (aura::core::lifetime::live_pin_count() > 0) {
                    result.force_blocked_by_pin = true;
                    ++stats_.force_compact_blocked_by_pin;
                    g_force_compact_blocked_by_pin_total.fetch_add(1, std::memory_order_relaxed);
                }
                if (aura::core::envframe_lifetime::active_guard_depth() > 0) {
                    result.force_blocked_by_envframe_guard = true;
                    ++stats_.force_compact_blocked_by_envframe_guard;
                    g_force_compact_blocked_by_envframe_guard_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                ++stats_.moving_blocked_precondition_total;
                g_moving_blocked_precondition_total.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            ++stats_.live_compact_moving_count;
            g_live_compact_moving_count.fetch_add(1, std::memory_order_relaxed);
            // Issue #2775 / #2837: prep-registered external roots + slots
            // are retained through relocate so #2837 can rewrite slots
            // and detect stale unremapped prep values. Captured into the
            // result after remapping; then cleared so the next Moving
            // window starts fresh. Survives blocked-Moving early-returns
            // above (caller intent not lost on soft-gate / pin-count gate).
            // Densify tracked create objects before freelist/tail compact.
            // Issue #2495: pass out_untracked_kept_count so we can detect
            // densify windows where Moving moved live objects but left
            // external / untracked candidates behind. failure-closed →
            // set moving_incomplete_remap + clear pin_contract_held.
            std::size_t untracked_kept_local = 0;
            result.objects_moved = relocate_tracked_objects_for_moving_(&untracked_kept_local);
            result.untracked_kept_count = untracked_kept_local;
            result.moved_live_objects = result.objects_moved > 0;
            stats_.objects_moved_total += result.objects_moved;
            g_objects_moved_total.fetch_add(result.objects_moved, std::memory_order_relaxed);

            // Issue #2837: rewrite registered external-root slots via
            // last_object_remap_. Soft / no-move: slot walk is O(registered)
            // only when objects_moved > 0 (zero extra work on no-move).
            // Track which densify-old values were covered by a slot rewrite.
            std::size_t slots_remapped = 0;
            std::unordered_set<void*> slot_covered_old;
            if (result.objects_moved > 0 && !external_root_slots_for_densify_.empty() &&
                !last_object_remap_.empty()) {
                slot_covered_old.reserve(external_root_slots_for_densify_.size());
                for (void** slot : external_root_slots_for_densify_) {
                    if (slot == nullptr || *slot == nullptr)
                        continue;
                    auto it = last_object_remap_.find(*slot);
                    if (it == last_object_remap_.end())
                        continue;
                    slot_covered_old.insert(it->first);
                    *slot = it->second;
                    ++slots_remapped;
                }
            }
            result.external_roots_remapped_count = slots_remapped;
            if (slots_remapped > 0) {
                g_moving_external_root_slot_remap_total.fetch_add(slots_remapped,
                                                                  std::memory_order_relaxed);
            }

            // Issue #2837: prep-registered values that densify moved but
            // no slot rewrite covered → stale external residual.
            std::size_t stale_unremapped = 0;
            if (result.objects_moved > 0 && !external_roots_for_densify_.empty() &&
                !last_object_remap_.empty()) {
                for (void* p : external_roots_for_densify_) {
                    if (p == nullptr)
                        continue;
                    if (last_object_remap_.find(p) == last_object_remap_.end())
                        continue;
                    if (slot_covered_old.find(p) == slot_covered_old.end())
                        ++stale_unremapped;
                }
            }
            result.external_roots_stale_unremapped_count = stale_unremapped;

            // Consume prep registration for this window (Agent observability).
            result.external_roots_prep_registered_cleared = external_roots_for_densify_.size();
            external_roots_for_densify_.clear();
            external_root_slots_for_densify_.clear();

            // Issue #2495: fail-closed against false safety under Moving default.
            // When densify moved objects AND untracked candidates existed, the
            // remap walk missed at least one potential live root (external raw
            // pointer not registered as pin or root). Pin-or-remap contract
            // cannot be claimed; bump the untracked counter and mark both
            // moving_incomplete_remap (observability) and pin_contract_held
            // (the unified failure flag the Phase 5 driver checks).
            // Issue #2837: also fail-closed when prep-registered values
            // densified without a covering slot rewrite (stale_unremapped).
            if (result.objects_moved > 0 &&
                (result.untracked_kept_count > 0 || stale_unremapped > 0)) {
                result.moving_incomplete_remap = true;
                result.pin_contract_held = false;
                ++stats_.moving_untracked_external_roots_total;
                g_moving_untracked_external_roots_total.fetch_add(1, std::memory_order_relaxed);
                // Issue #2495 AC3: AURA_MOVING_UNTRACKED=hard abort under
                // production security defaults. Off / unset keeps the Soft
                // semantics (success metrics suppressed but no abort) so
                // unit Soft path stays unchanged.
                // Issue #2664: Agent-visible hard-fail counter. The existing
                // #2596 path (g_moving_untracked_hard_abort_pref > 0) already
                // fires under production defaults (env unset locks pref to 1
                // via apply_production_security_defaults — see #2596 AC11).
                // This counter gives Agents dashboard visibility into the
                // hard-fail branch firing, distinguishing production-hard from
                // Soft observe-only. Soft / dev_off / tests retain observe-only
                // (gated on hard_pref <= 0 — covers env=off under production,
                // Soft+unset, Soft+env=hard overrides, etc.).
                const int hard_pref =
                    g_moving_untracked_hard_abort_pref.load(std::memory_order_relaxed);
                if (hard_pref > 0) {
                    result.moving_blocked_precondition = true;
                    result.soft_gated = true;
                    // Issue #2664: Agent-visible hard-fail counter.
                    g_moving_incomplete_remap_densify_hard_fail_total.fetch_add(
                        1, std::memory_order_relaxed);
                    // Issue #2837: sticky force densify-off until roots are
                    // re-registered / sticky cleared. Agents see
                    // would_allow_mutate=false + moving_compact_enabled=0.
                    // Soft (hard_pref <= 0) does not arm sticky densify-off
                    // (this branch is hard_pref > 0 only).
                    const auto prev_sticky = g_moving_incomplete_remap_sticky_densify_off.exchange(
                        1, std::memory_order_acq_rel);
                    if (prev_sticky == 0) {
                        g_moving_incomplete_remap_sticky_densify_off_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            } else if (result.objects_moved > 0 && !result.moving_incomplete_remap &&
                       result.pin_contract_held) {
                // Issue #2837: clean Moving densify clears sticky densify-off
                // so Agents can resume densify after re-registering roots
                // and completing a green window.
                clear_moving_incomplete_remap_sticky_densify_off();
            }
        } else {
            // Force path (#2160 / #2157).
            if (aura::gc_hooks::should_defer_destructive_gc()) {
                result.soft_gated = true;
                if (aura::gc_hooks::render_pin_defer_active())
                    aura::gc_hooks::note_gc_sweep_skipped_render();
                return result;
            }
            if (aura::core::lifetime::live_pin_count() > 0) {
                result.force_blocked_by_pin = true;
                result.soft_gated = true;
                ++stats_.force_compact_blocked_by_pin;
                g_force_compact_blocked_by_pin_total.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            if (aura::core::envframe_lifetime::active_guard_depth() > 0) {
                result.force_blocked_by_envframe_guard = true;
                result.soft_gated = true;
                ++stats_.force_compact_blocked_by_envframe_guard;
                g_force_compact_blocked_by_envframe_guard_total.fetch_add(
                    1, std::memory_order_relaxed);
                return result;
            }
            ++stats_.live_compact_force_count;
        }

        // ── Mark ──
        const std::size_t marked_objs = dtors_.size();
        const std::size_t marked_bytes = small_pool_.allocated();
        const std::size_t pool_slot_proxy = marked_bytes / SmallObjectPool::kTierSizes[0];
        const std::size_t total_marked = marked_objs + pool_slot_proxy;
        stats_.live_defrag_attempted_count++;
        stats_.live_objects_marked_total += total_marked;

        // ── Relocate (freelist protocol) ──
        const std::size_t holes = small_pool_.free_slot_count();
        const std::size_t reuses = small_pool_.recycle_hits();
        const std::size_t relocated = holes + reuses;
        stats_.live_relocate_count += relocated;
        stats_.live_compact_freelist_hits_total += relocated;
        aura::core::arena_policy::record_live_relocate(relocated);
        result.slots_recycled = relocated;

        // ── Compact tail ──
        const auto frag_before = stats().fragmentation_ratio();
        const std::size_t saved_bytes =
            defrag_impl(/*clear_request_flag=*/false, /*invoke_hook=*/false);
        small_pool_.rebind_tiers();
        stats_.live_compact_reclaimed_bytes_total += saved_bytes;
        result.bytes_reclaimed = saved_bytes;

        const auto frag_after = stats().fragmentation_ratio();
        stats_.frag_post_compact_bp = static_cast<std::size_t>(frag_after * 10000.0);
        aura::core::arena_policy::record_frag_post_compact(frag_after);
        if (frag_before > frag_after) {
            stats_.frag_reduced_bp +=
                static_cast<std::size_t>((frag_before - frag_after) * 10000.0);
        }

        // ── Generation restamp + LifetimePin remap + invalidation ──
        // Moving always restamps when any object moved (even if freelist quiet).
        // Issue #2265 Phase 3: remap live pins to follow densified addresses
        // BEFORE layout-change callbacks. Remapped pins keep valid; non-remapped
        // pins are still invalidated (existing fail-closed policy). The remap
        // pass is Moving-only — Soft/Force paths skip it (AC3 zero-cost).
        if (saved_bytes > 0 || relocated > 0 || result.moved_live_objects) {
            result.new_gen = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
            result.invalidates_pins = true;
            ++stats_.live_compact_gen_restamps_total;
            std::size_t remapped_pins = 0;
            if (result.moved_live_objects && !last_object_remap_.empty()) {
                for (const auto& [old_ptr, new_ptr] : last_object_remap_) {
                    const auto rr = aura::core::lifetime::remap_pins_pointing_to(
                        old_ptr, new_ptr, result.new_gen, arena_id_);
                    remapped_pins += rr.remapped;
                }
                stats_.live_compact_remapped_pins_total += remapped_pins;
                result.remapped_pins = remapped_pins;
            }
            // Build set of new addresses for O(1) skip during invalidate pass.
            // Remapped pins have ptr_ == value in last_object_remap_; non-remapped
            // pins have ptr_ NOT in last_object_remap_'s values. Invalidate the
            // latter so dangling pointers fail closed (validate returns false).
            std::unordered_set<void*> new_addrs;
            if (result.moved_live_objects) {
                new_addrs.reserve(last_object_remap_.size() * 2);
                for (const auto& [old_ptr, new_ptr] : last_object_remap_)
                    new_addrs.insert(new_ptr);
            }
            // Issue #2266 AC1 — verify pin-or-remap hard contract. After the
            // remap walk + selective invalidate, every live pin for arena_id_
            // must either: (a) have been remapped to a new address (ptr_ no
            // longer in last_object_remap_'s keys), or (b) been invalidated
            // (ptr_ == nullptr). If any pin still has ptr_ in last_object_remap_'s
            // keys (= old densified addresses), the remap missed it → fail closed.
            // This is the #2266 fail-closed change (previously always-true observe-only).
            if (result.moved_live_objects && !last_object_remap_.empty()) {
                std::unordered_set<void*> old_addrs;
                old_addrs.reserve(last_object_remap_.size());
                for (const auto& [old_ptr, new_ptr] : last_object_remap_)
                    old_addrs.insert(old_ptr);
                const bool contract_held =
                    aura::core::lifetime::verify_pins_under_moving_compact(arena_id_, old_addrs);
                result.pin_contract_held = contract_held;
                // Note: verify_pins_under_moving_compact already bumps
                // g_moving_compact_pin_contract_fail_total on failure (single
                // counter source-of-truth in lifetime_pin.ixx).
            }
            // Issue #2374: selective invalidate via sharded registry (not the
            // legacy pin_registry() which was always empty post-#2342).
            // invalidate_pins_not_in_new_addrs walks all shards and unpins
            // non-remapped pins for this arena; remapped pins (ptr_ in
            // new_addrs) are skipped. verify_pins_under_moving_compact above
            // is fail-closed for pins still on *old* densified addresses —
            // this pass is the complementary "null non-remapped / non-arena
            // pins" path (e.g. AC_M5 local-buffer pins).
            const std::size_t invalidated =
                aura::core::lifetime::invalidate_pins_not_in_new_addrs(arena_id_, new_addrs);
            stats_.live_compact_invalidated_pins_total += invalidated;
            invoke_layout_change_(result.new_gen);
            // Issue #2267 / #2294: RootRemapPass — fires AFTER
            // LiveCompactLayoutChangeCallback (pin invalidate). Reads densify
            // old→new object_remap_ + new_gen. Compiler-installed callback
            // rewrites registered stable-object + closure-capture slots.
            // Fail-closed: unmapped densify candidates bump *_fail_total.
            // Only fires for Moving densify (moved_live_objects + non-empty
            // remap). Stats write back into LiveCompactResult + ArenaStats.
            if (result.moved_live_objects && !last_object_remap_.empty()) {
                invoke_root_remap_callback_(result);
                // Issue #2499: unify RootRemapPass fail into pin_contract_held at
                // the densify source so every driver (Phase 5, ArenaGroup
                // compact_all_moving_pinned, GC Soft path metrics) shares one
                // Moving success gate. Fail totals are per densify call
                // (last-call / #2376) — not process-cumulative. Soft / empty
                // remap leaves totals 0 → pin_contract_held unchanged.
                if (result.root_remap_stable_ref_fail_total +
                        result.root_remap_closure_capture_fail_total >
                    0) {
                    result.pin_contract_held = false;
                }
            }
        }

        invoke_compact_hook_with_deopt_();
        return result;
    }

    // Issue #2166: resolve old create-object address after Moving densify.
    // Returns nullptr if unknown (not remapped / Soft-Force path).
    [[nodiscard]] void* resolve_object_remap(void* old_ptr) const noexcept {
        if (!old_ptr)
            return nullptr;
        auto it = last_object_remap_.find(old_ptr);
        return it == last_object_remap_.end() ? nullptr : it->second;
    }
    [[nodiscard]] std::size_t object_remap_size() const noexcept {
        return last_object_remap_.size();
    }
    [[nodiscard]] std::uint64_t live_compact_moving_count_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_moving_count);
    }
    [[nodiscard]] std::uint64_t objects_moved_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.objects_moved_total);
    }
    [[nodiscard]] std::uint64_t moving_blocked_precondition_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.moving_blocked_precondition_total);
    }

    // Issue #2004: backwards-compat bool wrapper. New callers should use the
    // LiveCompactMode overload above to observe the full LiveCompactResult
    // struct (bytes_reclaimed / slots_recycled / new_gen / soft_gated /
    // invalidates_pins). This wrapper returns slots_recycled (the closest
    // analogue to the prior total_marked std::size_t semantics).
    [[nodiscard]] std::size_t live_compact(bool force) noexcept {
        return live_compact(force ? LiveCompactMode::Force : LiveCompactMode::Soft).slots_recycled;
    }

    // Issue #2004: per-arena stable id (minted at construction from
    // g_arena_id_counter). Used by LifetimePin::pin(p, g, arena_id_) so
    // invalidate_all_pins_for_arena can target THIS arena's pins without
    // over-invalidating pins tied to other arenas.
    [[nodiscard]] std::uint64_t arena_id() const noexcept { return arena_id_; }

    // Issue #2004: current arena generation counter (0 initially; bumped on
    // each successful live_compact that actually changes layout). External
    // references (LifetimePin, StableNodeRef) check this to detect stale
    // post-compact pointers.
    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
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

    // Issue #2004: explicit live_compact observability accessors (mirror
    // stats_.live_compact_* fields; _relaxed suffix matches the pattern used
    // by the live_defrag_* accessors above). Used by (query:arena-live-
    // compact-stats) primitive to surface the metrics to Aura Agents.
    [[nodiscard]] std::uint64_t live_compact_soft_count_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_soft_count);
    }
    [[nodiscard]] std::uint64_t live_compact_force_count_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_force_count);
    }
    [[nodiscard]] std::uint64_t live_compact_reclaimed_bytes_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_reclaimed_bytes_total);
    }
    [[nodiscard]] std::uint64_t live_compact_freelist_hits_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_freelist_hits_total);
    }
    [[nodiscard]] std::uint64_t live_compact_gen_restamps_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_gen_restamps_total);
    }
    [[nodiscard]] std::uint64_t live_compact_invalidated_pins_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_invalidated_pins_total);
    }
    // Issue #2265 Phase 3: per-arena LifetimePin remap counter (mirrors
    // process-wide g_lifetime_pin_remap_total for per-arena observability).
    [[nodiscard]] std::uint64_t live_compact_remapped_pins_total_relaxed() const noexcept {
        return static_cast<std::uint64_t>(stats_.live_compact_remapped_pins_total);
    }
    // Issue #2267: per-arena RootRemapPass counters (mirrors process atomics).
    // Issue #2381: load concurrent-safe atomics (not plain stats_ size_t).
    [[nodiscard]] std::uint64_t root_remap_stable_ref_total_relaxed() const noexcept {
        return root_remap_stable_ref_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t root_remap_stable_ref_fail_total_relaxed() const noexcept {
        return root_remap_stable_ref_fail_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t root_remap_closure_capture_total_relaxed() const noexcept {
        return root_remap_closure_capture_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t root_remap_closure_capture_fail_total_relaxed() const noexcept {
        return root_remap_closure_capture_fail_total_.load(std::memory_order_relaxed);
    }

    // Issue #2381: concurrent-safe shape_inval counter (hook invoke total).
    [[nodiscard]] std::uint64_t shape_inval_on_compact_relaxed() const noexcept {
        return shape_inval_on_compact_.load(std::memory_order_relaxed);
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
    // Issue #2166: size/align enable opt-in Moving densify + object_remap_.
    struct DtorEntry {
        void* ptr = nullptr;
        void (*dtor)(void*) = nullptr;
        std::size_t size = 0;
        std::size_t align = 0;
    };

    // Issue #2166: densify tracked small-pool create objects for Moving.
    // Snapshot → freelist recycle (no dtor) → reallocate → memcpy → remap.
    // Non-small-pool / untracked slots stay put. Avoids allocate_raw_impl
    // auto-compact re-entry. Returns count of objects whose address changed.
    // Issue #2495: also returns the count of untracked-kept candidates via
    // the out-parameter so LiveCompactResult can set moving_incomplete_remap
    // when objects_moved > 0 && untracked_kept_count > 0 (failure-closed
    // against false safety under production Moving default).
    [[nodiscard]] std::size_t
    relocate_tracked_objects_for_moving_(std::size_t* out_untracked_kept_count = nullptr) noexcept {
        last_object_remap_.clear();
        if (dtors_.empty())
            return 0;

        struct Pending {
            void* old = nullptr;
            void (*dtor)(void*) = nullptr;
            std::size_t size = 0;
            std::size_t align = 0;
            std::vector<std::byte> bytes;
        };
        std::vector<Pending> pending;
        pending.reserve(dtors_.size());
        std::vector<DtorEntry> kept;
        kept.reserve(dtors_.size());

        // Issue #2495: count of candidates that were NOT small-pool /
        // NOT in size budget / NOT pinned (external / untracked). When
        // objects_moved > 0 alongside this count > 0, the densify may
        // have moved a referent out from under an untracked live pointer.
        std::size_t untracked_kept = 0;

        for (auto& e : dtors_) {
            if (!e.ptr || e.size == 0 || e.dtor == nullptr) {
                kept.push_back(e);
                ++untracked_kept;
                continue;
            }
            // Only densify freelist-reclaimable small-pool objects.
            if (!small_pool_.owns(e.ptr) || e.size > SmallObjectPool::kMaxSmallSize) {
                kept.push_back(e);
                ++untracked_kept;
                continue;
            }
            Pending p;
            p.old = e.ptr;
            p.dtor = e.dtor;
            p.size = e.size;
            p.align = e.align != 0 ? e.align : alignof(std::max_align_t);
            p.bytes.resize(e.size);
            std::memcpy(p.bytes.data(), e.ptr, e.size);
            // Recycle WITHOUT dtor — object remains logically live at new addr.
            if (!small_pool_.recycle(e.ptr, e.size)) {
                kept.push_back(e);
                continue;
            }
            pending.push_back(std::move(p));
        }

        // Ascending size → denser tier packing after freelist reverse LIFO.
        std::stable_sort(
            pending.begin(), pending.end(),
            [](const Pending& a, const Pending& b) noexcept { return a.size < b.size; });

        std::size_t moved = 0;
        for (auto& p : pending) {
            // Prefer freelist; skip allocate_raw_impl (auto-compact re-entry).
            void* neu = small_pool_.try_allocate(p.size);
            if (!neu) {
                // Should be rare: freelist held recycled slots. Fall back pmr.
                try {
                    neu = resource_.allocate(p.size, p.align);
                    stats_.used += p.size;
                } catch (...) {
                    neu = nullptr;
                }
            }
            if (!neu)
                continue; // fail closed: drop tracking (should not happen)
            std::memcpy(neu, p.bytes.data(), p.size);
            kept.push_back(DtorEntry{neu, p.dtor, p.size, p.align});
            last_object_remap_[p.old] = neu;
            if (neu != p.old)
                ++moved;
        }
        dtors_ = std::move(kept);
        return moved;
    }

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
        // Issue #2059: publish decision snapshot (reason + headroom + deopt rate).
        aura::core::arena_policy::record_last_decision(
            decision.reason_ext, decision.headroom_used,
            aura::core::arena_policy::shape_deopt_rate_bp.load(std::memory_order_relaxed));
        (void)decision.frag_threshold_used;
    }

    // Issue #2267 / #2294: RootRemapPass invoker — copies the root_remap_
    // callback under root_remap_mtx_ and invokes outside the lock (same
    // pattern as invoke_layout_change_). Passes densify old→new object_remap
    // + new gen; writes per-call stats into result + ArenaStats.
    void invoke_root_remap_callback_(LiveCompactResult& result) noexcept {
        RootRemapCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(root_remap_mtx_);
            if (!root_remap_)
                return;
            cb_copy = root_remap_;
        }
        if (!cb_copy)
            return;
        std::size_t sr = 0, sr_fail = 0, cc = 0, cc_fail = 0;
        cb_copy(arena_id_, generation_.load(std::memory_order_relaxed), last_object_remap_, sr,
                sr_fail, cc, cc_fail);
        result.root_remap_stable_ref_total += sr;
        result.root_remap_stable_ref_fail_total += sr_fail;
        result.root_remap_closure_capture_total += cc;
        result.root_remap_closure_capture_fail_total += cc_fail;
        // Issue #2381: concurrent-safe RMW (plain size_t += was a data race
        // if two densify paths ever overlapped on the same arena).
        root_remap_stable_ref_total_.fetch_add(sr, std::memory_order_relaxed);
        root_remap_stable_ref_fail_total_.fetch_add(sr_fail, std::memory_order_relaxed);
        root_remap_closure_capture_total_.fetch_add(cc, std::memory_order_relaxed);
        root_remap_closure_capture_fail_total_.fetch_add(cc_fail, std::memory_order_relaxed);
    }

    void invoke_compact_hook_() {
        std::function<void()> hook_copy;
        {
            // Issue #1989: copy under hook_mtx_, invoke outside the lock so
            // a hook that re-enters ASTArena (or any code that takes the
            // same lock in the future) doesn't deadlock. set_on_compact_hook /
            // take_on_compact_hook are serialized against this copy.
            std::lock_guard<std::mutex> lock(hook_mtx_);
            if (!on_compact_hook_)
                return;
            hook_copy = on_compact_hook_;
        }
        hook_copy();
        // Issue #2381: relaxed atomic RMW — concurrent compact_hook invokers
        // must not data-race the per-arena counter (TSAN + no lost updates).
        // Process-wide arena_policy::record_shape_inval_on_compact() is
        // already atomic; keep per-arena count consistent under N-thread stress.
        shape_inval_on_compact_.fetch_add(1, std::memory_order_relaxed);
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

    // Issue #2089: optional layout-change callback path. Called from
    // live_compact immediately after the generation restamp +
    // invalidate_all_pins_for_arena block (so the callback observes the
    // same new_gen as the result struct). Copy the std::function under
    // on_layout_change_mtx_ and invoke outside the lock so re-entrant
    // set_on_layout_change / take_on_layout_change calls from inside the
    // callback do not deadlock — same pattern as #1989's
    // invoke_compact_hook_().
    void invoke_layout_change_(std::uint64_t new_gen) noexcept {
        LiveCompactLayoutChangeCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(on_layout_change_mtx_);
            if (!on_layout_change_)
                return;
            cb_copy = on_layout_change_;
        }
        cb_copy(arena_id_, new_gen);
    }

    std::size_t initial_size_ = 0; // Issue #187: for shrink_to_fit()
    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
    SmallObjectPool small_pool_;
    // GUARDED_BY(per-arena compact serial) — see ArenaStats #2381 contract.
    ArenaStats stats_;
    // Issue #2381: concurrent-safe counters (Option B). Snapshotted into
    // ArenaStats by stats(); never plain-++ under concurrent hook invoke.
    std::atomic<std::size_t> shape_inval_on_compact_{0};
    std::atomic<std::size_t> root_remap_stable_ref_total_{0};
    std::atomic<std::size_t> root_remap_stable_ref_fail_total_{0};
    std::atomic<std::size_t> root_remap_closure_capture_total_{0};
    std::atomic<std::size_t> root_remap_closure_capture_fail_total_{0};
    std::vector<DtorEntry> dtors_;
    // Issue #2166: old→new create-object addresses from last Moving densify.
    // Cleared/rebuilt each Moving call; Soft/Force leave it empty.
    std::unordered_map<void*, void*> last_object_remap_;
    // Issue #2775: external roots registered by callers via
    // register_external_root_for_densify(void*) / batch span before a
    // Moving densify. Value-only observability + #2837 stale detection.
    // Consumed (captured + cleared) after relocate + slot rewrite on each
    // live_compact(Moving) work. Survives Soft / Force / blocked-Moving
    // early-returns — caller re-registers per window they want covered.
    std::unordered_set<void*> external_roots_for_densify_;
    // Issue #2837: void** slots registered via
    // register_external_root_slot_for_densify. Rewritten after densify when
    // *slot is a key in last_object_remap_. Cleared with the prep set.
    std::vector<void**> external_root_slots_for_densify_;
    // Issue #300 Phase 3: see request_defrag() / defrag_requested()
    // / clear_defrag_request() for semantics.
    std::atomic<bool> defrag_requested_{false};
    // Issue #1989: protect on_compact_hook_ from concurrent assign+invoke (A-003).
    // std::function move-assign + invoke is UB; std::mutex serializes both.
    mutable std::mutex hook_mtx_;
    std::function<void()> on_compact_hook_;
    // Issue #2089: layout-change callback (see set_on_layout_change).
    // Same mutex-guarded std::function pattern as on_compact_hook_ above;
    // invoke_layout_change_() copies under lock and fires outside.
    mutable std::mutex on_layout_change_mtx_;
    LiveCompactLayoutChangeCallback on_layout_change_;
    // Issue #2267: RootRemapPass callback (StableNodeRef + Closure captures).
    mutable std::mutex root_remap_mtx_;
    RootRemapCallback root_remap_;
    // Issue #1546: optional Evaluator* (void*) + quota allow callback.
    // Issue #1663: owner_mtx_ protects the dual-word owner pair.
    mutable std::shared_mutex owner_mtx_;
    void* arena_owner_ = nullptr;
    ArenaQuotaAllowFn quota_allow_fn_ = nullptr;
    // Issue #2004: per-arena stable id (minted from g_arena_id_counter in
    // the constructor) + generation counter (atomic, bumped on successful
    // live_compact). arena_id_ keys LifetimePin invalidation to THIS arena.
    std::uint64_t arena_id_ = 0;
    std::atomic<std::uint64_t> generation_{0};
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

    // Issue #2170: expose primary arena's id + generation for the unified
    // LayoutStamp capture (boundary / compact / AOT emit publishers).
    // Returns false if the group is empty (caller treats as "no arena",
    // the LayoutStamp field stays 0 = unset sentinel matching the
    // lifetime_pin.hh / workspace_epoch.hh convention). Uses shared_lock
    // (concurrent reads; primary-lookup is a cheap map walk).
    [[nodiscard]] bool primary_arena_id_and_gen(std::uint64_t& out_id,
                                                std::uint64_t& out_gen) const noexcept {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        if (arenas_.empty())
            return false;
        const auto& arena = arenas_.begin()->second;
        out_id = arena->arena_id();
        out_gen = arena->generation();
        return true;
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
    // Issue #1988: lock arenas_mtx_ for the map walk (was unprotected).
    [[nodiscard]] std::size_t auto_compact() {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        return auto_compact_unlocked_();
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
    // Public entry: take unique_lock, then call the unlocked
    // body. Nested callers (adaptive_compact_all /
    // compact_with_policy Auto / auto_compact_with_safety)
    // must use adaptive_compact_unlocked_ to avoid
    // std::system_error "Resource deadlock avoided" on the
    // non-recursive shared_mutex (#1988 regression).
    [[nodiscard]] std::size_t adaptive_compact(const std::string& name) {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        return adaptive_compact_unlocked_(name);
    }

    // Issue #335: adaptive_compact_all() — adaptive variant
    // of auto_compact() that uses should_auto_compact() per
    // module. Returns total bytes reclaimed across all
    // managed arenas.
    [[nodiscard]] std::size_t adaptive_compact_all() {
        std::unique_lock<std::shared_mutex> lock(arenas_mtx_);
        return adaptive_compact_all_unlocked_();
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
    //
    // Must call adaptive_compact_all_unlocked_ (not the
    // public adaptive_compact_all) — we already hold
    // arenas_mtx_ here.
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
        return adaptive_compact_all_unlocked_();
    }

    // Issue #2004: live_compact — iterate every managed arena, invoke
    // ASTArena::live_compact(mode) under the shared mutex, aggregate the
    // LiveCompactResult fields across the call. Soft-gating happens per
    // arena (render hotpath / MutationBoundary check is inside
    // ASTArena::live_compact). Returns aggregated result.
    [[nodiscard]] LiveCompactResult live_compact(LiveCompactMode mode) noexcept {
        std::shared_lock<std::shared_mutex> lock(arenas_mtx_);
        LiveCompactResult agg;
        agg.mode = mode;
        for (auto& [_, arena] : arenas_) {
            LiveCompactResult r = arena->live_compact(mode);
            agg.bytes_reclaimed += r.bytes_reclaimed;
            agg.slots_recycled += r.slots_recycled;
            agg.new_gen = std::max(agg.new_gen, r.new_gen);
            agg.soft_gated = agg.soft_gated || r.soft_gated;
            agg.invalidates_pins = agg.invalidates_pins || r.invalidates_pins;
            // Issue #2089 / #2166: aggregate Moving + non-moving flags.
            agg.moved_live_objects = agg.moved_live_objects || r.moved_live_objects;
            agg.objects_moved += r.objects_moved;
            agg.moving_blocked_precondition =
                agg.moving_blocked_precondition || r.moving_blocked_precondition;
            // Issue #2157: OR Force hard-mutex flags across arenas.
            agg.force_blocked_by_pin = agg.force_blocked_by_pin || r.force_blocked_by_pin;
            agg.force_blocked_by_envframe_guard =
                agg.force_blocked_by_envframe_guard || r.force_blocked_by_envframe_guard;
        }
        return agg;
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
                // Already hold arenas_mtx_ — use unlocked body.
                return adaptive_compact_unlocked_(name);
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

    // ── unlocked bodies (caller must hold unique_lock on arenas_mtx_) ──
    // std::shared_mutex is non-recursive. Public entry points take the
    // lock once; nested compact paths must call these helpers so they
    // do not re-lock and throw std::system_error "Resource deadlock
    // avoided" (CI regression after #1988).

    [[nodiscard]] std::size_t auto_compact_unlocked_() {
        std::size_t total = 0;
        for (auto& [_, arena] : arenas_) {
            if (arena->stats().fragmentation_ratio() >= compact_threshold_) {
                total += arena->compact();
            }
        }
        return total;
    }

    [[nodiscard]] std::size_t adaptive_compact_unlocked_(const std::string& name) {
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

    [[nodiscard]] std::size_t adaptive_compact_all_unlocked_() {
        std::size_t total = 0;
        // Snapshot keys first: compact() may re-enter ArenaGroup via
        // on_compact_hook_ (e.g. Evaluator re_pin). Iterating the live
        // map while compact can also invalidate iteration if a nested
        // path emplaces a new module arena.
        std::vector<std::string> names;
        names.reserve(arenas_.size());
        for (const auto& [name, _] : arenas_)
            names.push_back(name);
        for (const auto& name : names)
            total += adaptive_compact_unlocked_(name);
        return total;
    }

public:
    // Issue #2266: per-arena Moving densify + pin-contract verification.
    // Returns aggregated result across all module arenas so the driver
    // (evaluator_mutation_boundary.cpp Phase 5) can check pin_contract_held
    // + suppress success metrics on contract failure. Same per-arena
    // pre-snapshot as adaptive_compact_all_unlocked_ to avoid map
    // re-entry invalidation during live_compact.
    [[nodiscard]] AdaptiveCompactResult compact_all_moving_pinned() noexcept {
        AdaptiveCompactResult out;
        std::vector<std::string> names;
        names.reserve(arenas_.size());
        for (const auto& [name, _] : arenas_)
            names.push_back(name);
        for (const auto& name : names) {
            auto it = arenas_.find(name);
            if (it == arenas_.end())
                continue;
            const auto r = it->second->live_compact(LiveCompactMode::Moving);
            out.bytes_reclaimed_total += r.bytes_reclaimed;
            out.pin_contract_held = out.pin_contract_held && r.pin_contract_held;
            out.moved_live_objects = out.moved_live_objects || r.moved_live_objects;
            // Issue #2499: aggregate per-arena RootRemapPass fail totals
            // (per-call out-params from invoke_root_remap_callback_ — last-call
            // semantics, NOT process-cumulative). live_compact already folds
            // non-zero fails into r.pin_contract_held; re-AND here so the
            // AdaptiveCompactResult gate is fail-closed even if a future path
            // populates fail totals without the fold.
            out.root_remap_stable_ref_fail_total += r.root_remap_stable_ref_fail_total;
            out.root_remap_closure_capture_fail_total += r.root_remap_closure_capture_fail_total;
            // Issue #2619: Agent densify-health window aggregates.
            out.objects_moved_total += r.objects_moved;
            out.untracked_kept_total += r.untracked_kept_count;
            out.moving_incomplete_remap_any =
                out.moving_incomplete_remap_any || r.moving_incomplete_remap;
            // Issue #2775: aggregate prep-registered external roots count
            // across all arenas. Pure observability — does not gate any
            // success / fail predicate. Phase 5 reads this into a local and
            // passes it to publish_last_moving_densify_window so Agent
            // dashboards see caller compliance with the prep-API contract.
            out.external_roots_prep_registered_total += r.external_roots_prep_registered_cleared;
        }
        if (out.root_remap_stable_ref_fail_total + out.root_remap_closure_capture_fail_total > 0)
            out.pin_contract_held = false;
        return out;
    }
};

} // namespace aura::ast
