#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Issue #5476: stable-address, atomically readable cell heap.
//
// cells_ used to be a pmr::vector<EvalValue>; Env::lookup reads a cell on
// every variable reference while another body may alloc_cell (push_back)
// and reallocate the vector. Locking per lookup cost 4x on hot recursion
// (measured 1710ms -> 6.3s on the 2M recursion bench), so the storage is
// chunked instead: fixed blocks of std::atomic<int64_t> (EvalValue is a
// single int64), block pointers published in a fixed atomic array, and a
// published size with release/acquire ordering. Readers never lock and
// never chase a reallocating buffer.
namespace aura::compiler {

class CellHeap {
public:
    static constexpr std::size_t kBlockShift = 12;
    static constexpr std::size_t kBlockSize = std::size_t{1} << kBlockShift;
    static constexpr std::size_t kBlockMask = kBlockSize - 1;
    static constexpr std::size_t kMaxBlocks = 4096; // 16M cells

    struct Block {
        std::array<std::atomic<std::int64_t>, kBlockSize> v;
    };

    CellHeap() noexcept {
        for (auto& p : blocks_)
            p.store(nullptr, std::memory_order_relaxed);
    }
    ~CellHeap() {
        for (auto& p : blocks_) {
            Block* b = p.load(std::memory_order_relaxed);
            delete b;
        }
    }
    CellHeap(const CellHeap&) = delete;
    CellHeap& operator=(const CellHeap&) = delete;
    CellHeap(CellHeap&&) = delete;
    CellHeap& operator=(CellHeap&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept {
        return published_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] types::EvalValue get(std::size_t i) const noexcept {
        const Block* b = block_at(i);
        return types::EvalValue(b->v[i & kBlockMask].load(std::memory_order_relaxed));
    }
    [[nodiscard]] types::EvalValue operator[](std::size_t i) const noexcept { return get(i); }

    class Ref {
    public:
        Ref(CellHeap& h, std::size_t i) noexcept
            : heap_(&h)
            , i_(i) {}
        operator types::EvalValue() const noexcept { return heap_->get(i_); }
        Ref& operator=(types::EvalValue v) noexcept {
            heap_->write(i_, v);
            return *this;
        }
        Ref& operator=(const Ref& o) noexcept { return *this = static_cast<types::EvalValue>(o); }

    private:
        CellHeap* heap_;
        std::size_t i_;
    };
    [[nodiscard]] Ref operator[](std::size_t i) noexcept { return Ref(*this, i); }

    void push_back(types::EvalValue v) {
        std::scoped_lock lock(mu_);
        const std::size_t i = published_.load(std::memory_order_relaxed);
        Block* b = ensure_block(i);
        b->v[i & kBlockMask].store(v.val, std::memory_order_relaxed);
        published_.store(i + 1, std::memory_order_release);
    }
    void pop_back() {
        std::scoped_lock lock(mu_);
        const std::size_t n = published_.load(std::memory_order_relaxed);
        if (n != 0)
            published_.store(n - 1, std::memory_order_release);
    }
    void resize(std::size_t n) {
        std::scoped_lock lock(mu_);
        const std::size_t old = published_.load(std::memory_order_relaxed);
        if (n > old) {
            for (std::size_t i = old; i < n; ++i)
                ensure_block(i)->v[i & kBlockMask].store(0, std::memory_order_relaxed);
        }
        published_.store(n, std::memory_order_release);
    }
    void clear() noexcept {
        std::scoped_lock lock(mu_);
        published_.store(0, std::memory_order_release);
    }
    void shrink_to_fit() noexcept {}

private:
    [[nodiscard]] Block* block_at(std::size_t i) const noexcept {
        const std::size_t b = i >> kBlockShift;
        return blocks_[b].load(std::memory_order_acquire);
    }
    Block* ensure_block(std::size_t i) {
        const std::size_t bi = i >> kBlockShift;
        Block* b = blocks_[bi].load(std::memory_order_acquire);
        if (b != nullptr)
            return b;
        b = new Block{};
        for (auto& c : b->v)
            c.store(0, std::memory_order_relaxed);
        blocks_[bi].store(b, std::memory_order_release);
        return b;
    }
    void write(std::size_t i, types::EvalValue v) noexcept {
        // The slot was published before this reader got its id; the block
        // pointer is stable, so an atomic store is enough.
        Block* b = block_at(i);
        b->v[i & kBlockMask].store(v.val, std::memory_order_release);
    }

    std::array<std::atomic<Block*>, kMaxBlocks> blocks_{};
    std::atomic<std::size_t> published_{0};
    std::mutex mu_;
};

} // namespace aura::compiler
