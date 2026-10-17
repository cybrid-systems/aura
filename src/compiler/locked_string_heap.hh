// Issue #4543: the Evaluator string heap with a single, locked mint API.
//
// #2651 requires every string_heap_ growth to be serialized behind
// Evaluator::alloc_storage_lock_. The heap used to be a bare
// std::pmr::vector<std::string>, so any primitive could write the
// `idx = heap.size(); heap.push_back(s);` idiom without the lock; ~650 sites
// did, and TSan caught the race under fiber:spawn sharing (#4519 AC2a:
// free(): invalid pointer / 0xbe-fill teardown SEGV). The idiom is also a
// TOCTOU on the index even when each half is locked.
//
// LockedStringHeap makes that class of bug unrepresentable:
//   * there is no push_back / emplace_back / insert; the only way to grow
//     the heap is mint(), which takes the bound lock, appends, runs the
//     mint hook (Issue #4428 tenant stamp) and returns the new index, all
//     in one critical section;
//   * reads keep the vector surface (size / [] / iteration / implicit
//     const-vector and span views) so read-only helpers are unchanged;
//   * structural rewrites (truncate, clear, GC compaction swap) are
//     explicit, locked member functions; the raw vector is reachable only
//     through unsafe_storage(), whose callers must hold mutex() or run with
//     the world stopped.
//
// An unbound heap (no mutex: standalone test heaps) mints unlocked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace aura::compiler {

class LockedStringHeap {
public:
    using Vec = std::pmr::vector<std::string>;
    using value_type = std::string;
    using size_type = std::size_t;
    using reference = std::string&;
    using const_reference = const std::string&;
    using iterator = Vec::iterator;
    using const_iterator = Vec::const_iterator;
    using MintHook = void (*)(void* ctx, std::size_t idx) noexcept;

    LockedStringHeap() = default;
    explicit LockedStringHeap(std::pmr::memory_resource* r)
        : v_(r) {}
    LockedStringHeap(std::pmr::memory_resource* r, std::recursive_mutex* m)
        : v_(r)
        , mu_(m) {}
    LockedStringHeap(const LockedStringHeap&) = delete;
    LockedStringHeap& operator=(const LockedStringHeap&) = delete;

    // Issue #4428: called under the lock with the minted index.
    void set_mint_hook(MintHook fn, void* ctx) noexcept {
        hook_ = fn;
        hook_ctx_ = ctx;
    }
    [[nodiscard]] std::recursive_mutex* mutex() const noexcept { return mu_; }

    // The only growth path. Returns the index of the new string.
    template <class... Args> std::uint64_t mint(Args&&... args) {
        Guard g(mu_);
        const auto idx = v_.size();
        v_.emplace_back(std::forward<Args>(args)...);
        if (hook_)
            hook_(hook_ctx_, idx);
        return static_cast<std::uint64_t>(idx);
    }

    // ── reads (vector surface) ─────────────────────────────────────
    [[nodiscard]] size_type size() const noexcept { return v_.size(); }
    [[nodiscard]] bool empty() const noexcept { return v_.empty(); }
    [[nodiscard]] size_type capacity() const noexcept { return v_.capacity(); }
    reference operator[](size_type i) noexcept { return v_[i]; }
    const_reference operator[](size_type i) const noexcept { return v_[i]; }
    reference at(size_type i) { return v_.at(i); }
    const_reference at(size_type i) const { return v_.at(i); }
    reference back() noexcept { return v_.back(); }
    const_reference back() const noexcept { return v_.back(); }
    reference front() noexcept { return v_.front(); }
    const_reference front() const noexcept { return v_.front(); }
    std::string* data() noexcept { return v_.data(); }
    const std::string* data() const noexcept { return v_.data(); }
    iterator begin() noexcept { return v_.begin(); }
    iterator end() noexcept { return v_.end(); }
    const_iterator begin() const noexcept { return v_.begin(); }
    const_iterator end() const noexcept { return v_.end(); }
    const_iterator cbegin() const noexcept { return v_.cbegin(); }
    const_iterator cend() const noexcept { return v_.cend(); }
    [[nodiscard]] const Vec& view() const noexcept { return v_; }
    operator const Vec&() const noexcept { return v_; }
    operator std::span<const std::string>() const noexcept { return {v_.data(), v_.size()}; }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept {
        return v_.get_allocator().resource();
    }

    // ── structural writes (locked) ─────────────────────────────────
    // Shrink to n (panic checkpoint restore) or pad with empty strings.
    void resize(size_type n) {
        Guard g(mu_);
        v_.resize(n);
    }
    void clear() noexcept {
        Guard g(mu_);
        v_.clear();
    }
    void shrink_to_fit() {
        Guard g(mu_);
        v_.shrink_to_fit();
    }
    void reserve(size_type n) {
        Guard g(mu_);
        v_.reserve(n);
    }
    // GC compaction: install the rebuilt heap.
    void replace(Vec&& v) {
        Guard g(mu_);
        v_ = std::move(v);
    }
    // Escape hatch for code that must see the raw vector (bulk remap).
    // Caller holds mutex() or runs with every mutator stopped.
    Vec& unsafe_storage() noexcept { return v_; }

private:
    struct Guard {
        std::recursive_mutex* m;
        explicit Guard(std::recursive_mutex* mm)
            : m(mm) {
            if (m)
                m->lock();
        }
        ~Guard() {
            if (m)
                m->unlock();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
    Vec v_;
    std::recursive_mutex* mu_ = nullptr;
    MintHook hook_ = nullptr;
    void* hook_ctx_ = nullptr;
};

// Generic mint for helpers templated on the heap type (LockedStringHeap or a
// plain std::vector<std::string> scratch heap).
template <class Heap> inline std::uint64_t heap_mint(Heap& h, std::string s) {
    if constexpr (requires { h.mint(std::move(s)); }) {
        return h.mint(std::move(s));
    } else {
        const auto idx = h.size();
        h.push_back(std::move(s));
        return static_cast<std::uint64_t>(idx);
    }
}

} // namespace aura::compiler
