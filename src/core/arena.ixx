export module aura.core.arena;
import std;

namespace aura::ast {

// ── ArenaStats — per-arena memory accounting ─────────────────────
export struct ArenaStats {
    std::size_t capacity = 0;        // total buffer size
    std::size_t used = 0;            // bytes consumed
    std::size_t peak_used = 0;       // historical high-water mark
    std::size_t allocation_count = 0; // number of allocation calls
    std::size_t wasted = 0;          // alignment padding

    std::string format() const {
        return std::format("arena: {:.1f}MB / {:.1f}MB (peak {:.1f}MB) | {} allocs | {}B wasted",
                           used / 1048576.0, capacity / 1048576.0,
                           peak_used / 1048576.0,
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
    static constexpr std::size_t kTierSizes[]  = {16, 32, 64};
    static constexpr std::size_t kNumTiers     = 3;
    static constexpr std::size_t kSmallPoolSize = 3 * 1024 * 1024; // 3MB total
    static constexpr std::size_t kPerTierSize  = kSmallPoolSize / kNumTiers; // 1MB each
    static constexpr std::size_t kMaxSmallSize = kTierSizes[kNumTiers - 1];  // 64

    SmallObjectPool() {
        buffer_.resize(kSmallPoolSize);
        for (std::size_t i = 0; i < kNumTiers; ++i) {
            classes_[i].start  = buffer_.data() + i * kPerTierSize;
            classes_[i].bump   = classes_[i].start;
            classes_[i].end    = classes_[i].start + kPerTierSize;
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
        std::byte* bump  = nullptr;
        std::byte* end   = nullptr;
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
//
// Allocation path:
//   create<T>() → sizeof(T) <= 64 → SmallObjectPool
//               → else             → pmr monotonic_buffer_resource
//
export class ASTArena {
public:
    explicit ASTArena(std::size_t initial_size = 8 * 1024 * 1024)
        : buffer_(initial_size),
          resource_(buffer_.data(), buffer_.size(),
                    std::pmr::null_memory_resource())
    {}

    // Allocate and construct an object of type T
    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        contract_assert(sizeof(T) > 0);
        void* raw = allocate_raw(sizeof(T), alignof(T));
        contract_assert(raw != nullptr);
        ++stats_.allocation_count;
        return std::construct_at(static_cast<T*>(raw),
                                 std::forward<Args>(args)...);
    }

    // Destroy a single object (rarely needed — reset() bulk-frees)
    template <typename T>
    void destroy(T* ptr) {
        if (ptr) std::destroy_at(ptr);
    }

    // Release all allocated memory in one shot
    void reset() {
        small_pool_.reset();
        resource_.release();
        stats_.used = 0;
        stats_.allocation_count = 0;
        stats_.wasted = 0;
    }

    // Get a pmr-compatible allocator for std::pmr containers
    [[nodiscard]] std::pmr::polymorphic_allocator<std::byte>
    allocator() noexcept {
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

private:
    void* allocate_raw(std::size_t size, std::size_t alignment) {
        // Try small-object pool first (for objects <= 64 bytes)
        if (size <= SmallObjectPool::kMaxSmallSize) {
            void* ptr = small_pool_.try_allocate(size);
            if (ptr) return ptr;
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
};

// ── ArenaGroup — multi-arena manager ─────────────────────────────
//
// Manages a collection of named arenas, each representing a module
// or compilation unit. Enables fine-grained reset and memory reporting.
//
export class ArenaGroup {
public:
    // Get or create an arena for a module
    ASTArena& module_arena(const std::string& name,
                           std::size_t initial_size = 8 * 1024 * 1024) {
        auto it = arenas_.find(name);
        if (it != arenas_.end()) return *it->second;
        auto [inserted, ok] = arenas_.emplace(
            name, std::make_unique<ASTArena>(initial_size));
        return *inserted->second;
    }

    // Reset a specific module's arena
    void reset_module(const std::string& name) {
        auto it = arenas_.find(name);
        if (it != arenas_.end()) it->second->reset();
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
    [[nodiscard]] std::vector<std::pair<std::string, ArenaStats>>
    module_stats() const {
        std::vector<std::pair<std::string, ArenaStats>> result;
        for (auto& [name, arena] : arenas_) {
            result.emplace_back(name, arena->stats());
        }
        return result;
    }

    // Number of managed arenas
    [[nodiscard]] std::size_t count() const { return arenas_.size(); }

private:
    std::unordered_map<std::string,
        std::unique_ptr<ASTArena>> arenas_;
};

} // namespace aura::ast
