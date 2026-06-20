// serve/ws_deque.h — Chase-Lev work-stealing concurrent deque
//
// Lock-free work-stealing deque for fiber scheduling (Phase 3+4).
// - Owner thread: push() from bottom, pop() from bottom (LIFO)
// - Other threads: steal() from top (FIFO)
//
// Based on the Chase-Lev algorithm:
//   "Dynamic Circular Work-Stealing Deque" — Chase & Lev, SPAA 2005
//
// Memory ordering design (Issue #30):
//   - bottom_ is atomic with relaxed ordering; fences provide sync
//   - push: store item (with release fence) before bottom_ store
//   - pop:  seq_cst fence syncs bottom_ decrement with stealers' top_
//   - steal: acquire top_ load, seq_cst fence to see bottom_,
//            CAS to claim item (seq_cst on success)
//
// Cache alignment:
//   - top_ and bottom_ on separate cache lines to prevent false sharing
//   - Class itself aligns to 64 bytes (ARM64 cache line size)
//   - The ring buffer uses std::vector for safe resize (owner-only)

#ifndef AURA_SERVE_WS_DEQUE_H
#define AURA_SERVE_WS_DEQUE_H

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace aura::serve {

// ── Cache line size (ARM64 L1 data cache) ────────────
static constexpr std::size_t kCacheLine = 64;

template <typename T> class alignas(kCacheLine) WorkStealingDeque {
public:
    WorkStealingDeque()
        : bottom_(0) {
        top_.store(0, std::memory_order_relaxed);
        set_capacity(INITIAL_CAPACITY);
    }

    ~WorkStealingDeque() = default;

    WorkStealingDeque(const WorkStealingDeque&) = delete;
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;
    WorkStealingDeque(WorkStealingDeque&&) = delete;
    WorkStealingDeque& operator=(WorkStealingDeque&&) = delete;

    // ── push (owner only) ────────────────────────────
    void push(T item) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);

        if (static_cast<uint64_t>(b - t) >= static_cast<uint64_t>(mask_)) {
            grow(b, t);
        }

        buffer_[static_cast<size_t>(b) & mask_] = item;
        // Release fence: item write visible before bottom_ increment.
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // ── pop (owner only) ─────────────────────────────
    T pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);

        // Seq_cst fence: synchronizes with stealers' seq_cst fence.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_acquire);
        T item = nullptr;

        if (t <= b) {
            item = buffer_[static_cast<size_t>(b) & mask_];

            if (t == b) {
                // Last item: race with stealers.
                if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                  std::memory_order_relaxed)) {
                    item = nullptr;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
        } else {
            bottom_.store(b + 1, std::memory_order_relaxed);
        }

        return item;
    }

    // ── steal (any thread) ───────────────────────────
    T steal() {
        int64_t t = top_.load(std::memory_order_acquire);
        // Seq_cst fence: pairs with owner's fence in pop().
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_acquire);

        T item = nullptr;
        if (t < b) {
            item = buffer_[static_cast<size_t>(t) & mask_];

            // CAS to claim the item at top_.
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
                item = nullptr;
            }
        }

        return item;
    }

    // ── diagnostics ──────────────────────────────────
    size_t size_approx() const {
        int64_t t = top_.load(std::memory_order_acquire);
        int64_t b = bottom_.load(std::memory_order_acquire);
        int64_t n = b - t;
        return n > 0 ? static_cast<size_t>(n) : 0;
    }

    bool empty_approx() const {
        return bottom_.load(std::memory_order_acquire) <= top_.load(std::memory_order_acquire);
    }

private:
    static constexpr int64_t INITIAL_CAPACITY = 64;

    void set_capacity(int64_t cap) {
        mask_ = cap - 1;
        buffer_.assign(static_cast<size_t>(cap), nullptr);
    }

    // Grow the ring buffer. Called when the deque is full.
    // The new capacity is double the old capacity.
    // Items are re-indexed in the new buffer.
    // NOTE: This is called BEFORE writing the item that triggered the grow,
    // so the snapshot of (b, t) is the current state BEFORE the pending write.
    // The item at old position 'b' (which triggered growth) hasn't been written
    // yet — it will be written to position 'b' in the new buffer after growth.
    void grow(int64_t b, int64_t t) {
        int64_t old_cap = static_cast<int64_t>(buffer_.size());
        int64_t new_cap = old_cap * 2;

        std::vector<T> new_buf(static_cast<size_t>(new_cap), nullptr);
        int64_t new_mask = new_cap - 1;

        // Copy valid items: those at indices [t, b-1] in the old buffer.
        // Position 'b' in the old buffer hasn't been written yet (the
        // current push will write it after growth) — so copy only up to b-1.
        for (int64_t i = t; i < b; ++i) {
            new_buf[static_cast<size_t>(i) & new_mask] = buffer_[static_cast<size_t>(i) & mask_];
        }

        buffer_.swap(new_buf);
        mask_ = new_mask;
    }

    // ── Layout (cache-line aware) ────────────────────

    // Cache line 0: modified by stealers (other threads)
    alignas(kCacheLine) std::atomic<int64_t> top_{0};
    char pad0_[kCacheLine - sizeof(std::atomic<int64_t>)];

    // Cache line 1: modified only by owner
    alignas(kCacheLine) std::atomic<int64_t> bottom_{0};
    char pad1_[kCacheLine - sizeof(std::atomic<int64_t>)];

    // Owned-only members (no false sharing risk for stealers)
    std::vector<T> buffer_;
    int64_t mask_;
};

} // namespace aura::serve

#endif // AURA_SERVE_WS_DEQUE_H
