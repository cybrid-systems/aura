export module aura.core.arena;
import std;

namespace aura::ast {

export class ASTArena {
public:
    explicit ASTArena(std::size_t initial_size = 64 * 1024) : buffer_(initial_size) {}
    template <typename T, typename... Args> [[nodiscard]] T* create(Args&&... args) {
        std::size_t s = (pos_ + alignof(T) - 1) & ~(alignof(T) - 1);
        std::size_t e = s + sizeof(T);
        if (e > buffer_.size()) throw std::bad_alloc();
        auto* raw = &buffer_[s]; pos_ = e;
        return std::construct_at(reinterpret_cast<T*>(raw), std::forward<Args>(args)...);
    }
    void reset() { pos_ = 0; }
    std::size_t used() const { return pos_; }
private:
    std::vector<std::byte> buffer_;
    std::size_t pos_ = 0;
};

} // namespace aura::ast
