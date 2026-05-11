export module aura.core.arena;
import std;

namespace aura::ast {

// pmr-based AST arena — bump allocator with automatic alignment
// Backed by std::pmr::monotonic_buffer_resource for zero-fragmentation
// block allocation. Ideal for AST trees: allocate, use, reset in bulk.
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
        void* raw = allocate(sizeof(T), alignof(T));
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
        bytes_allocated_ = 0;
    }

    // Get a pmr-compatible allocator for std::pmr containers
    [[nodiscard]] std::pmr::polymorphic_allocator<std::byte>
    allocator() noexcept {
        return {&resource_};
    }

    // Bytes consumed so far (for profiling / diagnostics)
    // monotonic_buffer_resource doesn't expose current_size natively;
    // we track it via our own counter.
    [[nodiscard]] std::size_t used() const noexcept {
        return bytes_allocated_;
    }

    // Set a new total for arena tracking
    void record_allocation(std::size_t n) { bytes_allocated_ += n; }

private:
    void* allocate(std::size_t size, std::size_t alignment) {
        void* ptr = resource_.allocate(size, alignment);
        bytes_allocated_ += size;
        return ptr;
    }

    std::vector<std::byte> buffer_;
    std::pmr::monotonic_buffer_resource resource_;
    std::size_t bytes_allocated_ = 0;
};

} // namespace aura::ast
