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

// ── ASTArena — pmr bump allocator ────────────────────────────────
//
// v1: 单 arena, pmr monotonic_buffer_resource
// v2: stats tracking, overflow fallback
// v3: (planned) small-object pool
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
        auto align = alignof(T);
        auto size = sizeof(T);
        auto pre_pos = stats_.used;
        void* raw = allocate(size, align);
        auto post_pos = stats_.used;
        stats_.wasted += (post_pos - pre_pos) - size;
        ++stats_.allocation_count;
        stats_.peak_used = std::max(stats_.peak_used, stats_.used);
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
        resource_.release();
        stats_.used = 0;
        stats_.allocation_count = 0;
        stats_.wasted = 0;
        // capacity & peak_used preserved across resets
    }

    // Get a pmr-compatible allocator for std::pmr containers
    [[nodiscard]] std::pmr::polymorphic_allocator<std::byte>
    allocator() noexcept {
        return {&resource_};
    }

    // Snapshot of current memory statistics
    [[nodiscard]] ArenaStats stats() const noexcept {
        auto s = stats_;
        s.capacity = buffer_.size();
        return s;
    }

    // Bytes consumed so far
    [[nodiscard]] std::size_t used() const noexcept {
        return stats_.used;
    }

    // Total buffer capacity
    [[nodiscard]] std::size_t capacity() const noexcept {
        return buffer_.size();
    }

private:
    void* allocate(std::size_t size, std::size_t alignment) {
        void* ptr = resource_.allocate(size, alignment);
        stats_.used += size;
        return ptr;
    }

    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
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
