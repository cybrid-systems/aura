module;
#include <memory>
#include <vector>
#include <cstddef>
#include <new>

export module aura.core.arena;

namespace aura::ast {

export class ASTArena {
public:
    explicit ASTArena(size_t initial_size = 64 * 1024)
        : buffer_(initial_size)
    {}

    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        void* raw = alloc(sizeof(T), alignof(T));
        return std::construct_at(static_cast<T*>(raw), std::forward<Args>(args)...);
    }

    void reset() { pos_ = 0; }

    size_t used() const { return pos_; }

private:
    void* alloc(size_t size, size_t alignment) {
        size_t start = (pos_ + alignment - 1) & ~(alignment - 1);
        size_t end = start + size;
        if (end > buffer_.size())
            throw std::bad_alloc();
        pos_ = end;
        return &buffer_[start];
    }

    std::vector<std::byte> buffer_;
    size_t pos_ = 0;
};

} // namespace aura::ast
