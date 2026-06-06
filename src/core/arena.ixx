module;
#include <contracts>
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

    std::string format() const {
        return std::format("arena: {:.1f}MB / {:.1f}MB (peak {:.1f}MB) | {} allocs | {}B wasted",
                           used / 1048576.0, capacity / 1048576.0, peak_used / 1048576.0,
                           allocation_count, wasted);
    }

    void merge(const ArenaStats& other) {
        used += other.used;
        capacity += other.capacity;
        peak_used = std::max(peak_used, other.peak_used);
        allocation_count += other.allocation_count;
        wasted += other.wasted;
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
        : buffer_(initial_size)
        , resource_(buffer_.data(), buffer_.size(), std::pmr::new_delete_resource()) {}

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
    template <typename T, typename... Args> [[nodiscard]] T* create(Args&&... args)
        post (r: r != nullptr)
    {
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
        if (!ptr) return;
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
    void reset()
        post (used() == 0)
        post (stats_.allocation_count == 0)
    {
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

    void* allocate_raw(std::size_t size, std::size_t alignment)
        pre (size > 0)
        pre (alignment > 0 && (alignment & (alignment - 1)) == 0)
    {
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

    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
    SmallObjectPool small_pool_;
    ArenaStats stats_;
    std::vector<DtorEntry> dtors_;
};

// ── ArenaGroup — multi-arena manager ─────────────────────────────
//
// Manages a collection of named arenas, each representing a module
// or compilation unit. Enables fine-grained reset and memory reporting.
//
export class ArenaGroup {
public:
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

private:
    std::unordered_map<std::string, std::unique_ptr<ASTArena>> arenas_;
};

} // namespace aura::ast
