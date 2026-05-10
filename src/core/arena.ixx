export module aura.core.arena;

import <cstddef>;
import <new>;
import <memory>;
import <vector>;

namespace aura::ast {

export class ASTArena {
public:
    explicit ASTArena(size_t initial_size = 64 * 1024)
        : buffer_(initial_size)
    {}

    template <typename T, typename... Args>
    [[nodiscard]] T* create(Args&&... args) {
        size_t start = (pos_ + alignof(T) - 1) & ~(alignof(T) - 1);
        size_t end = start + sizeof(T);
        if (end > buffer_.size())
            throw std::bad_alloc();
        auto* raw = &buffer_[start];
        pos_ = end;
        return std::construct_at(reinterpret_cast<T*>(raw),
                                 std::forward<Args>(args)...);
    }

    void reset() { pos_ = 0; }
    size_t used() const { return pos_; }

private:
    std::vector<std::byte> buffer_;
    size_t pos_ = 0;
};

} // namespace aura::ast
