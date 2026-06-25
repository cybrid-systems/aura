module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <format>
#include <memory>
#include <memory_resource>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "core/gc_hooks.h"
export module aura.core.arena;
import std;

namespace aura::ast {

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

    std::string format() const {
        return std::format("arena: {:.1f}MB / {:.1f}MB (peak {:.1f}MB) | {} allocs | {}B wasted | "
                           "{} compactions (last saved {}B, total {}B) | "
                           "{} defrags (last saved {}B)",
                           used / 1048576.0, capacity / 1048576.0, peak_used / 1048576.0,
                           allocation_count, wasted, compaction_count, last_compaction_saved,
                           total_compaction_saved, defrag_attempted_count, last_defrag_saved);
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
        if (other.defrag_attempted_count > 0)
            last_defrag_saved = other.last_defrag_saved;
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
    void* try_allocate(std::size_t size) {
        for (auto& c : classes_) {
            if (size <= c.obj_sz) {
                void* ptr = c.bump;
                c.bump += c.obj_sz;
                if (c.bump <= c.end) {
                    allocated_from_small_ += c.obj_sz;
                    return ptr;
                }
                // This tier is exhausted — reset bump and signal overflow
                c.bump -= c.obj_sz; // undo
                return nullptr;
            }
        }
        return nullptr; // too large for any tier
    }

    // Reset all tier bump pointers (but keep buffer allocated)
    void reset() {
        for (auto& c : classes_) {
            c.bump = c.start;
        }
        allocated_from_small_ = 0;
    }

    // Total bytes consumed from the small pool
    [[nodiscard]] std::size_t allocated() const { return allocated_from_small_; }

    // Capacity of the small pool
    [[nodiscard]] std::size_t capacity() const { return kSmallPoolSize; }

private:
    struct Tier {
        std::byte* start = nullptr;
        std::byte* bump = nullptr;
        std::byte* end = nullptr;
        std::size_t obj_sz = 0;
    };

    std::vector<std::byte> buffer_;
    Tier classes_[kNumTiers];
    std::size_t allocated_from_small_ = 0;
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
    explicit ASTArena(std::size_t initial_size = 8 * 1024 * 1024)
        : initial_size_(initial_size)
        , buffer_(initial_size)
        , resource_(buffer_.data(), buffer_.size(), std::pmr::new_delete_resource()) {}

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
    void request_defrag() noexcept {
        defrag_requested_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool defrag_requested() const noexcept {
        return defrag_requested_.load(std::memory_order_acquire);
    }
    void clear_defrag_request() noexcept {
        defrag_requested_.store(false, std::memory_order_release);
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
    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) post(r : r != nullptr) {
        void* raw = allocate_raw(sizeof(T), alignof(T));
        ++stats_.allocation_count;
        auto* result = std::construct_at(static_cast<T*>(raw), std::forward<Args>(args)...);
        dtors_.push_back({result, +[](void* p) { static_cast<T*>(p)->~T(); }});
        return result;
    }

    // Destroy a single object: call its destructor AND unregister the
    // entry so the bulk-dtor pass at reset/~ASTArena doesn't double-
    // destroy. If `ptr` wasn't tracked (caller's responsibility) the
    // dtor still runs as a best-effort fallback.
    template <typename T> void destroy(T* ptr) {
        if (!ptr)
            return;
        for (auto it = dtors_.begin(); it != dtors_.end(); ++it) {
            if (it->ptr == ptr) {
                ptr->~T();
                dtors_.erase(it);
                return;
            }
        }
        // Not tracked (e.g. allocated by an upstream helper, or
        // ownership already moved). Best-effort dtor call.
        ptr->~T();
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
    [[nodiscard]] std::size_t compact() noexcept {
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
        if (saved > 0) {
            stats_.compaction_count++;
            stats_.last_compaction_saved = saved;
            stats_.total_compaction_saved += saved;
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
        // Issue #300 Phase 3: clear the request flag at the start
        // of the defrag. A subsequent request can set it again if
        // needed. The clear happens before the buffer mutation so
        // the fiber safepoint sees the clear as soon as possible.
        defrag_requested_.store(false, std::memory_order_release);
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
        if (saved > 0) {
            stats_.defrag_attempted_count++;
            stats_.last_defrag_saved = saved;
        }
        // Note: NOT touching stats_.compaction_count /
        // last_compaction_saved. This is intentionally a separate
        // counter from compact().
        return saved;
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
            std::size_t saved = before - initial_size_;
            stats_.compaction_count++;
            stats_.last_compaction_saved = saved;
            stats_.total_compaction_saved += saved;
        }
    }

    // Number of live tracked objects (for tests / diagnostics)
    [[nodiscard]] std::size_t live_count() const noexcept { return dtors_.size(); }

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

    // Issue #187 (P0): reset the pmr resource to point at the
    // (possibly resized) buffer_. Use release() instead of
    // destroy_at + construct_at — the latter corrupts the vtable
    // on some libc++/libstdc++ versions when the resource is
    // referenced by external pmr containers (FlatAST's 18+ pmr
    // vectors all hold memory_resource* = &resource_). release()
    // just resets the bump pointer to the buffer start, leaving
    // the vtable intact. All live in-buffer allocations (pointed
    // to by external pmr containers) are still valid because
    // buffer_ hasn't moved (resize may shrink but never reallocates
    // while shrinking, and any ptr below stats_.used is in the
    // preserved prefix).
    void rebuild_resource_() noexcept {
        resource_.release();
    }

    void* allocate_raw(std::size_t size, std::size_t alignment) pre(size > 0)
        pre(alignment > 0 && (alignment & (alignment - 1)) == 0) {
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
                return ptr;
            }
            // Small-object tier exhausted — fall through to main arena
        }

        // Allocate from main pmr buffer
        void* ptr = resource_.allocate(size, alignment);
        stats_.used += size;
        return ptr;
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
        compact_threshold_ = std::clamp(ratio, 0.0, 0.95);
    }
    [[nodiscard]] double compact_threshold() const noexcept { return compact_threshold_; }

    // Get or create an arena for a module
    ASTArena& module_arena(const std::string& name, std::size_t initial_size = 8 * 1024 * 1024) {
        auto it = arenas_.find(name);
        if (it != arenas_.end())
            return *it->second;
        auto [inserted, ok] = arenas_.emplace(name, std::make_unique<ASTArena>(initial_size));
        return *inserted->second;
    }

    // Reset a specific module's arena
    void reset_module(const std::string& name) {
        auto it = arenas_.find(name);
        if (it != arenas_.end())
            it->second->reset();
    }

    // Reset all arenas
    void reset_all() {
        for (auto& [_, arena] : arenas_)
            arena->reset();
    }

    // Issue #187 (P0): compact a specific module's arena. Returns
    // bytes reclaimed, or 0 if the module isn't found.
    [[nodiscard]] std::size_t compact_module(const std::string& name) {
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

    // Aggregate stats across all managed arenas
    [[nodiscard]] ArenaStats total_stats() const {
        ArenaStats total;
        for (auto& [_, arena] : arenas_) {
            total.merge(arena->stats());
        }
        return total;
    }

    // Per-module stats breakdown
    [[nodiscard]] std::vector<std::pair<std::string, ArenaStats>> module_stats() const {
        std::vector<std::pair<std::string, ArenaStats>> result;
        for (auto& [name, arena] : arenas_) {
            result.emplace_back(name, arena->stats());
        }
        return result;
    }

    // Number of managed arenas
    [[nodiscard]] std::size_t count() const { return arenas_.size(); }

    // Issue #187 (P0): JSON snapshot of all managed arenas for
    // observability (the `observability_json` primitive surfaces
    // this to Aura code via the (arena:stats) primitive).
    [[nodiscard]] std::string stats_json() const {
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
            out += std::format("{{\"name\":\"{}\",\"used\":{},\"capacity\":{},"
                               "\"peak_used\":{},\"allocs\":{},\"compaction_count\":{},"
                               "\"last_compaction_saved\":{},\"total_compaction_saved\":{},"
                               "\"fragmentation_ratio\":{:.3f},"
                               "\"defrag_attempted_count\":{},\"last_defrag_saved\":{}}}",
                               esc_name, s.used, s.capacity, s.peak_used, s.allocation_count,
                               s.compaction_count, s.last_compaction_saved,
                               s.total_compaction_saved, s.fragmentation_ratio(),
                               s.defrag_attempted_count, s.last_defrag_saved);
        }
        out += "],\"compact_threshold\":" + std::to_string(compact_threshold_) + "}";
        return out;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ASTArena>> arenas_;
    double compact_threshold_ = 0.50; // Issue #187: default 50% fragmentation triggers compact
};

} // namespace aura::ast
