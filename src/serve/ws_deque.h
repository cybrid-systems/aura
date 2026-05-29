// serve/ws_deque.h — Chase-Lev work-stealing concurrent deque
//
// Lock-free work-stealing deque for fiber scheduling.
// - Owner thread: push() from bottom, pop() from bottom (LIFO)
// - Other threads: steal() from top (FIFO)
//
// Based on the Chase-Lev algorithm:
//   "Dynamic Circular Work-Stealing Deque" — Chase & Lev, SPAA 2005
//
// Key design: storage uses T* (not std::atomic<T>*) to avoid
// std::atomic copy-construction issues with std::vector. The
// memory ordering for concurrent access is ensured by fences
// and the atomic top_/bottom_ indices only.
//
#ifndef AURA_SERVE_WS_DEQUE_H
#define AURA_SERVE_WS_DEQUE_H

#include <atomic>
#include <cstdint>
#include <vector>
#include <cstdlib>

namespace aura::serve {

template<typename T>
class WorkStealingDeque {
public:
    WorkStealingDeque()
        : top_(0), bottom_(0) {
        set_capacity(INITIAL_CAPACITY);
    }

    ~WorkStealingDeque() = default;

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;
    WorkStealingDeque(WorkStealingDeque&&) = delete;
    WorkStealingDeque& operator=(WorkStealingDeque&&) = delete;

    // ── push (owner only) ────────────────────────────
    void push(T item) {
        int64_t b = bottom_;
        int64_t t = top_.load(std::memory_order_acquire);

        if (static_cast<uint64_t>(b - t) >= static_cast<uint64_t>(mask_)) {
            resize();
        }

        buffer_[static_cast<size_t>(b) & mask_] = item;
        // Ensure item write is visible before bottom_ update
        std::atomic_thread_fence(std::memory_order_release);
        bottom_ = b + 1;
    }

    // ── pop (owner only) ─────────────────────────────
    T pop() {
        int64_t b = bottom_ - 1;
        bottom_ = b;

        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_acquire);
        T item = nullptr;

        if (t <= b) {
            item = buffer_[static_cast<size_t>(b) & mask_];

            if (t == b) {
                // Last item — must CAS to synchronize with stealers
                if (!top_.compare_exchange_strong(t, t + 1,
                        std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    item = nullptr;
                }
                bottom_ = b + 1;  // restore
            }
        } else {
            bottom_ = b + 1;  // restore
        }

        return item;
    }

    // ── steal (any thread) ────────────────────────────
    T steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_;

        T item = nullptr;
        if (t < b) {
            item = buffer_[static_cast<size_t>(t) & mask_];

            if (!top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
                item = nullptr;
            }
        }

        return item;
    }

    // ── diagnostics ──────────────────────────────────
    size_t size_approx() const {
        int64_t t = top_.load(std::memory_order_acquire);
        int64_t b = bottom_;
        int64_t n = b - t;
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

    bool empty_approx() const {
        return bottom_ <= top_.load(std::memory_order_acquire);
    }

private:
    static constexpr int64_t INITIAL_CAPACITY = 64;

    void set_capacity(int64_t cap) {
        mask_ = cap - 1;
        buffer_.assign(static_cast<size_t>(cap), nullptr);
    }

    void resize() {
        int64_t t = top_.load(std::memory_order_acquire);
        int64_t b = bottom_;
        int64_t old_cap = static_cast<int64_t>(buffer_.size());
        int64_t new_cap = old_cap * 2;

        std::vector<T> new_buf(static_cast<size_t>(new_cap), nullptr);
        int64_t new_mask = new_cap - 1;

        for (int64_t i = t; i < b; ++i) {
            new_buf[static_cast<size_t>(i) & new_mask] =
                buffer_[static_cast<size_t>(i) & mask_];
        }

        buffer_.swap(new_buf);
        mask_ = new_mask;
    }

    // Top index (atomic) — shared between owner and stealers
    std::atomic<int64_t> top_{0};
    // Bottom index (non-atomic) — owned by this thread only
    int64_t bottom_ = 0;
    // Ring buffer of T* (plain pointers, atomicity via fences + indices)
    std::vector<T> buffer_;
    // Mask = capacity - 1
    int64_t mask_;
};

} // namespace aura::serve

#endif // AURA_SERVE_WS_DEQUE_H
