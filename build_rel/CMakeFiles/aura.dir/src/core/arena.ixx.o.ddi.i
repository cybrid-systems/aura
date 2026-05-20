# 0 "/home/dev/code/aura/src/core/arena.ixx"
# 0 "<built-in>"
# 0 "<command-line>"
# 1 "/usr/include/stdc-predef.h" 1 3
# 0 "<command-line>" 2
# 1 "/home/dev/code/aura/src/core/arena.ixx"
export module aura.core.arena;
import std;

namespace aura::ast {


export struct ArenaStats {
    std::size_t capacity = 0;
    std::size_t used = 0;
    std::size_t peak_used = 0;
    std::size_t allocation_count = 0;
    std::size_t wasted = 0;

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
# 41 "/home/dev/code/aura/src/core/arena.ixx"
export class SmallObjectPool {
public:

    static constexpr std::size_t kTierSizes[] = {16, 32, 64};
    static constexpr std::size_t kNumTiers = 3;
    static constexpr std::size_t kSmallPoolSize = 3 * 1024 * 1024;
    static constexpr std::size_t kPerTierSize = kSmallPoolSize / kNumTiers;
    static constexpr std::size_t kMaxSmallSize = kTierSizes[kNumTiers - 1];

    SmallObjectPool() {
        buffer_.resize(kSmallPoolSize);
        for (std::size_t i = 0; i < kNumTiers; ++i) {
            classes_[i].start = buffer_.data() + i * kPerTierSize;
            classes_[i].bump = classes_[i].start;
            classes_[i].end = classes_[i].start + kPerTierSize;
            classes_[i].obj_sz = kTierSizes[i];
        }
    }



    void* try_allocate(std::size_t size) {
        for (auto& c : classes_) {
            if (size <= c.obj_sz) {
                void* ptr = c.bump;
                c.bump += c.obj_sz;
                if (c.bump <= c.end) {
                    allocated_from_small_ += c.obj_sz;
                    return ptr;
                }

                c.bump -= c.obj_sz;
                return nullptr;
            }
        }
        return nullptr;
    }


    void reset() {
        for (auto& c : classes_) {
            c.bump = c.start;
        }
        allocated_from_small_ = 0;
    }


    [[nodiscard]] std::size_t allocated() const { return allocated_from_small_; }


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
# 116 "/home/dev/code/aura/src/core/arena.ixx"
export class ASTArena {
public:
    explicit ASTArena(std::size_t initial_size = 8 * 1024 * 1024)
        : buffer_(initial_size),
          resource_(buffer_.data(), buffer_.size(),
                    std::pmr::null_memory_resource())
    {}


    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* raw = allocate_raw(sizeof(T), alignof(T));
        ++stats_.allocation_count;
        return std::construct_at(static_cast<T*>(raw),
                                 std::forward<Args>(args)...);
    }


    template <typename T>
    void destroy(T* ptr) {
        if (ptr) std::destroy_at(ptr);
    }


    void reset() {
        small_pool_.reset();
        resource_.release();
        stats_.used = 0;
        stats_.allocation_count = 0;
        stats_.wasted = 0;
    }


    [[nodiscard]] std::pmr::polymorphic_allocator<std::byte>
    allocator() noexcept {
        return {&resource_};
    }


    [[nodiscard]] ArenaStats stats() const noexcept {
        auto s = stats_;
        s.capacity = buffer_.size() + small_pool_.capacity();
        s.used = stats_.used + small_pool_.allocated();
        s.peak_used = std::max(s.peak_used, s.used);
        return s;
    }


    [[nodiscard]] std::size_t used() const noexcept {
        return stats_.used + small_pool_.allocated();
    }


    [[nodiscard]] std::size_t capacity() const noexcept {
        return buffer_.size() + small_pool_.capacity();
    }

private:
    void* allocate_raw(std::size_t size, std::size_t alignment) {

        if (size <= SmallObjectPool::kMaxSmallSize) {
            void* ptr = small_pool_.try_allocate(size);
            if (ptr) return ptr;

        }


        void* ptr = resource_.allocate(size, alignment);
        stats_.used += size;
        return ptr;
    }

    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
    SmallObjectPool small_pool_;
    ArenaStats stats_;
};






export class ArenaGroup {
public:

    ASTArena& module_arena(const std::string& name,
                           std::size_t initial_size = 8 * 1024 * 1024) {
        auto it = arenas_.find(name);
        if (it != arenas_.end()) return *it->second;
        auto [inserted, ok] = arenas_.emplace(
            name, std::make_unique<ASTArena>(initial_size));
        return *inserted->second;
    }


    void reset_module(const std::string& name) {
        auto it = arenas_.find(name);
        if (it != arenas_.end()) it->second->reset();
    }


    void reset_all() {
        for (auto& [_, arena] : arenas_)
            arena->reset();
    }


    [[nodiscard]] ArenaStats total_stats() const {
        ArenaStats total;
        for (auto& [_, arena] : arenas_) {
            total.merge(arena->stats());
        }
        return total;
    }


    [[nodiscard]] std::vector<std::pair<std::string, ArenaStats>>
    module_stats() const {
        std::vector<std::pair<std::string, ArenaStats>> result;
        for (auto& [name, arena] : arenas_) {
            result.emplace_back(name, arena->stats());
        }
        return result;
    }


    [[nodiscard]] std::size_t count() const { return arenas_.size(); }

private:
    std::unordered_map<std::string,
        std::unique_ptr<ASTArena>> arenas_;
};

}
