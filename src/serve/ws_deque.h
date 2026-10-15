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
//   - The ring is published through an atomic pointer; grow() copies into a
//     new ring and retires the old one until destruction, so a stealer that
//     loaded the old ring never reads freed or concurrently-swapped memory
//     (tsan race in grow(), nightly e0290ca).

#ifndef AURA_SERVE_WS_DEQUE_H
#define AURA_SERVE_WS_DEQUE_H

#include <atomic>
#include "core/atomic_fence_port.h"
#include <cstdint>
#include <cstdlib>
#include <memory>
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
        Ring* r = ring_.load(std::memory_order_relaxed);

        if (static_cast<uint64_t>(b - t) >= static_cast<uint64_t>(r->mask)) {
            r = grow(r, b, t);
        }

        r->put(b, item);
        // Release fence: item write visible before bottom_ increment.
        aura::util::thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // ── pop (owner only) ─────────────────────────────
    T pop() {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);

        // Seq_cst fence: synchronizes with stealers' seq_cst fence.
        aura::util::thread_fence(std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_acquire);
        T item = nullptr;

        if (t <= b) {
            item = ring_.load(std::memory_order_relaxed)->get(b);

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
        aura::util::thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_acquire);

        T item = nullptr;
        if (t < b) {
            // Acquire pairs with grow()'s release publish of a new ring.
            item = ring_.load(std::memory_order_acquire)->get(t);

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

    struct Ring {
        explicit Ring(int64_t cap)
            : mask(cap - 1)
            , slots(new std::atomic<T>[static_cast<size_t>(cap)]) {
            for (int64_t i = 0; i < cap; ++i)
                slots[static_cast<size_t>(i)].store(nullptr, std::memory_order_relaxed);
        }
        T get(int64_t i) const {
            return slots[static_cast<size_t>(i & mask)].load(std::memory_order_relaxed);
        }
        void put(int64_t i, T v) {
            slots[static_cast<size_t>(i & mask)].store(v, std::memory_order_relaxed);
        }
        int64_t mask;
        std::unique_ptr<std::atomic<T>[]> slots;
    };

    void set_capacity(int64_t cap) {
        rings_.push_back(std::make_unique<Ring>(cap));
        ring_.store(rings_.back().get(), std::memory_order_relaxed);
    }

    // Grow the ring (owner only), called BEFORE the pending push writes
    // position b. Valid items [t, b) are copied; the old ring stays alive in
    // rings_ because a stealer may still be reading it.
    Ring* grow(Ring* old, int64_t b, int64_t t) {
        auto fresh = std::make_unique<Ring>((old->mask + 1) * 2);
        for (int64_t i = t; i < b; ++i)
            fresh->put(i, old->get(i));
        Ring* r = fresh.get();
        rings_.push_back(std::move(fresh));
        ring_.store(r, std::memory_order_release);
        return r;
    }

    // ── Layout (cache-line aware) ────────────────────

    // Cache line 0: modified by stealers (other threads)
    alignas(kCacheLine) std::atomic<int64_t> top_{0};
    char pad0_[kCacheLine - sizeof(std::atomic<int64_t>)];

    // Cache line 1: modified only by owner
    alignas(kCacheLine) std::atomic<int64_t> bottom_{0};
    char pad1_[kCacheLine - sizeof(std::atomic<int64_t>)];

    // Owned-only members (no false sharing risk for stealers)
    std::atomic<Ring*> ring_{nullptr};
    std::vector<std::unique_ptr<Ring>> rings_; // current + retired (owner-only)
};

} // namespace aura::serve

#endif // AURA_SERVE_WS_DEQUE_H
