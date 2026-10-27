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
//
// Issue #4548: string-length / string-ref / substring index code points
// (#4472), and used to walk the UTF-8 bytes from 0 on every call — O(n)
// per string-length, O(offset) per string-ref, and substring copied the
// whole source first. Strings of at least kCpIndexMinBytes get a cached
// code-point index, built on first use with one scan:
//   * all ASCII: code point i is byte i (O(1));
//   * otherwise: the byte offset of every kCpIndexStride-th code point, so
//     a lookup walks at most kCpIndexStride - 1 units.
// An entry is trusted only while the slot's data pointer, byte length and
// the heap's write generation all match. The generation moves on every
// structural write (resize / clear / replace / unsafe_storage) and on
// note_write(idx), which every in-place content writer calls
// (string-fill!, f64vector-set!, slot reuse in the agent loop).
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utf8_scalar.hh" // Issue #4548: code-point index cache

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

    // Issue #4714: optional measurement hook. Every Guard that ACTUALLY
    // takes the mutex increments the bound counter, so a per-fiber
    // (unbound, mu_ == nullptr) nursery can prove it never locks instead
    // of asserting it by construction. Null = no measurement.
    void set_lock_observer(std::atomic<std::uint64_t>* obs) noexcept { lock_observer_ = obs; }
    [[nodiscard]] std::atomic<std::uint64_t>* lock_observer() const noexcept {
        return lock_observer_;
    }

    // The only growth path. Returns the index of the new string.
    //
    // Issue #4608: a slot that the reclaim proved unreachable is
    // reused instead of growing the heap. Without the reuse path a
    // program that minted and dropped strings kept one slot per
    // string until exit (~70 bytes per string: the slot object, the
    // #4428 owner record and the vector's growth slack).
    template <class... Args> std::uint64_t mint(Args&&... args) {
        Guard g(mu_, lock_observer_);
        return mint_locked_impl(std::forward<Args>(args)...);
    }

    // Issue #4714: the append half of mint() WITHOUT taking the mutex.
    // The caller holds mutex() for the whole batch (copy-on-send
    // promotion deep-copies N nursery strings under one acquisition; N
    // recursive re-locks cost ~60 ns each and dominated the batch).
    // Same obligation as unsafe_storage(): hold the lock, or the heap is
    // unbound (single-owner).
    template <class... Args> std::uint64_t mint_locked(Args&&... args) {
        return mint_locked_impl(std::forward<Args>(args)...);
    }

private:
    template <class... Args> std::uint64_t mint_locked_impl(Args&&... args) {
        if (!free_slots_.empty()) {
            // Reuse a released slot. The swap drops the old content
            // (and its heap buffer) with the temporary, so a reused
            // slot never keeps the previous string's memory alive.
            const auto idx = free_slots_.back();
            free_slots_.pop_back();
            if (idx < slot_free_.size())
                slot_free_[idx] = 0;
            {
                std::string tmp(std::forward<Args>(args)...);
                v_[idx].swap(tmp);
            }
            // Issue #4608 + #4548: the reused slot is a fresh content
            // write, so the cached code-point index for that slot
            // must go (the same obligation note_write carries for an
            // in-place write). A stale index would be trusted when
            // the new content has the same byte length.
            invalidate_cp_index_locked(idx);
            // Issue #4428: the owner stamp is rewritten by the hook,
            // so a reused slot never carries the previous owner's
            // tenant (the hook runs on the reuse path too).
            if (hook_)
                hook_(hook_ctx_, idx);
            return static_cast<std::uint64_t>(idx);
        }
        const auto idx = v_.size();
        v_.emplace_back(std::forward<Args>(args)...);
        slot_free_.push_back(0);
        if (hook_)
            hook_(hook_ctx_, idx);
        return static_cast<std::uint64_t>(idx);
    }

public:
    // ── Issue #4608: slot release / reuse ───────────────────────────
    // A string value is an index into this heap, so a slot may only
    // be recycled when no live value references it any more. The
    // reclaim (file-mode / tree-walker) proves that with the same
    // mark it uses for pairs, then hands the dead slots back here.

    // Mark-time snapshot of the free slots. release_unmarked uses it
    // to tell "already free" apart from "free and re-minted since
    // the mark" (a concurrent mint may pop a free slot; such a slot
    // holds a live string again and must not be released).
    [[nodiscard]] std::vector<std::uint8_t> free_slot_snapshot() const {
        Guard g(mu_, lock_observer_);
        return slot_free_;
    }

    // Release the slots the reclaim found unreachable: seen[i] != 0
    // means slot i is still referenced. Slots that were already free
    // at snapshot time are left alone (they are on the free list
    // already, or a concurrent mint re-used them). Returns the
    // number of slots that are still live after the release.
    std::size_t release_unmarked(const std::vector<std::uint8_t>& seen,
                                 const std::vector<std::uint8_t>& free_at_mark) {
        Guard g(mu_, lock_observer_);
        const auto n = std::min(seen.size(), v_.size());
        std::size_t live = 0;
        for (std::size_t i = 1; i < n; ++i) {
            // Slot 0 is the immortal empty string (the default error
            // cause sentinel) — it is never released.
            if (seen[i]) {
                ++live;
                continue;
            }
            if (i < free_at_mark.size() && free_at_mark[i])
                continue; // already free (or re-minted since the mark)
            if (i < slot_free_.size() && slot_free_[i])
                continue; // already on the free list
            // Drop the content: swapping in an empty string releases
            // the old heap buffer with the temporary.
            std::string().swap(v_[i]);
            invalidate_cp_index_locked(i);
            if (i < slot_free_.size())
                slot_free_[i] = 1;
            free_slots_.push_back(i);
        }
        // Slots minted after the mark are newborn and stay live.
        if (v_.size() > n)
            live += (v_.size() - n);
        return live;
    }

    // Number of slots currently on the free list (reusable).
    [[nodiscard]] size_type free_slot_count() const noexcept { return free_slots_.size(); }
    // Slots that still hold a string (the heap high-water mark minus
    // the free ones).
    [[nodiscard]] size_type live_slot_count() const noexcept {
        return v_.size() - free_slots_.size();
    }
    [[nodiscard]] bool slot_is_free(size_type i) const noexcept {
        return i < slot_free_.size() && slot_free_[i] != 0;
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

    // ── code-point index (Issue #4548) ─────────────────────────────
    static constexpr std::size_t kCpIndexMinBytes = 256;
    static constexpr std::size_t kCpIndexStride = 64;
    static constexpr std::size_t kCpIndexMaxEntries = 4096;

    // In-place content write to slot idx (same size or not): drop every
    // cached index. Call with or without the lock held.
    void note_write(size_type /*idx*/) noexcept {
        Guard g(mu_, lock_observer_);
        ++write_gen_;
    }
    [[nodiscard]] std::uint64_t write_generation() const noexcept { return write_gen_; }

    // Code points in slot i; npos when i is out of range.
    [[nodiscard]] std::size_t code_point_count(size_type i) const {
        Guard g(mu_, lock_observer_);
        if (i >= v_.size())
            return npos;
        const auto& s = v_[i];
        if (s.size() < kCpIndexMinBytes)
            return utf8_unit_count(s);
        return cp_index(i).cp_len;
    }
    // Code point at character index cp of slot i; -1 when out of range.
    [[nodiscard]] int code_point_at(size_type i, std::size_t cp) const {
        Guard g(mu_, lock_observer_);
        if (i >= v_.size())
            return -1;
        const auto& s = v_[i];
        if (s.size() < kCpIndexMinBytes)
            return utf8_code_point_at(s, cp);
        const auto& m = cp_index(i);
        if (cp >= m.cp_len)
            return -1;
        if (m.ascii)
            return static_cast<int>(static_cast<unsigned char>(s[cp]));
        Utf8Unit u;
        std::size_t b = byte_offset_locked(s, m, cp);
        return utf8_unit_at(s, b, u) ? static_cast<int>(u.cp) : -1;
    }
    // Copy of code points [start, end) of slot i; past-the-end clamps,
    // start >= end is "". Only the slice is copied. False when i is out
    // of range.
    bool code_point_slice(size_type i, std::size_t start, std::size_t end, std::string& out) const {
        Guard g(mu_, lock_observer_);
        if (i >= v_.size())
            return false;
        const auto& s = v_[i];
        std::size_t sb = 0;
        std::size_t eb = 0;
        if (s.size() < kCpIndexMinBytes) {
            sb = utf8_byte_offset(s, start);
            eb = utf8_byte_offset(s, end);
        } else {
            const auto& m = cp_index(i);
            sb = start >= m.cp_len ? s.size() : byte_offset_locked(s, m, start);
            eb = end >= m.cp_len ? s.size() : byte_offset_locked(s, m, end);
        }
        if (sb >= eb)
            out.clear();
        else
            out.assign(s, sb, eb - sb);
        return true;
    }
    [[nodiscard]] std::size_t cp_index_entries() const noexcept { return cp_meta_.size(); }

    // ── structural writes (locked) ─────────────────────────────────
    // Shrink to n (panic checkpoint restore) or pad with empty strings.
    // Issue #4608: the free list is index-parallel to the heap, so a
    // shrink drops the free slots past the new end.
    void resize(size_type n) {
        Guard g(mu_, lock_observer_);
        ++write_gen_;
        v_.resize(n);
        slot_free_.resize(n, 0);
        std::erase_if(free_slots_, [n](size_type i) { return i >= n; });
    }
    void clear() noexcept {
        Guard g(mu_, lock_observer_);
        ++write_gen_;
        v_.clear();
        slot_free_.clear();
        free_slots_.clear();
    }
    void shrink_to_fit() {
        Guard g(mu_, lock_observer_);
        v_.shrink_to_fit();
    }
    void reserve(size_type n) {
        Guard g(mu_, lock_observer_);
        v_.reserve(n);
    }
    // GC compaction: install the rebuilt heap.
    // Issue #4608: the rebuilt heap has no free slots (the compaction
    // already dropped the dead ones), so the free list is reset.
    void replace(Vec&& v) {
        Guard g(mu_, lock_observer_);
        ++write_gen_;
        v_ = std::move(v);
        slot_free_.assign(v_.size(), 0);
        free_slots_.clear();
    }
    // Escape hatch for code that must see the raw vector (bulk remap).
    // Caller holds mutex() or runs with every mutator stopped.
    // Issue #4548: the caller may rewrite slots, so cached indexes go.
    // Issue #4608: the caller may rewrite the slots' liveness too, so
    // the free list is dropped (every slot is live until proven
    // otherwise by the next reclaim).
    Vec& unsafe_storage() noexcept {
        ++write_gen_;
        slot_free_.assign(v_.size(), 0);
        free_slots_.clear();
        return v_;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    struct CpIndex {
        const char* data = nullptr;
        std::size_t bytes = 0;
        std::uint64_t gen = 0;
        std::size_t cp_len = 0;
        bool ascii = false;
        std::vector<std::size_t> stride_offsets; // byte offset of cp k*stride
    };

    // Caller holds the lock and has checked i < size() and the threshold.
    const CpIndex& cp_index(size_type i) const {
        const auto& s = v_[i];
        // Any write since the last build invalidates every entry: drop
        // them all so dead strings' indexes do not linger.
        if (cp_meta_gen_ != write_gen_) {
            cp_meta_.clear();
            cp_meta_gen_ = write_gen_;
        }
        auto it = cp_meta_.find(i);
        if (it != cp_meta_.end() && it->second.data == s.data() && it->second.bytes == s.size() &&
            it->second.gen == write_gen_)
            return it->second;
        if (it == cp_meta_.end()) {
            if (cp_meta_.size() >= kCpIndexMaxEntries)
                cp_meta_.clear();
            it = cp_meta_.try_emplace(i).first;
        }
        CpIndex& m = it->second;
        m.data = s.data();
        m.bytes = s.size();
        m.gen = write_gen_;
        m.stride_offsets.clear();
        bool ascii = true;
        {
            const char* p = s.data();
            const std::size_t n = s.size();
            std::size_t k = 0;
            for (; k + 8 <= n; k += 8) {
                std::uint64_t w;
                std::memcpy(&w, p + k, 8);
                if (w & 0x8080808080808080ull) {
                    ascii = false;
                    break;
                }
            }
            for (; ascii && k < n; ++k)
                ascii = static_cast<unsigned char>(p[k]) < 0x80;
        }
        m.ascii = ascii;
        if (ascii) {
            m.cp_len = s.size();
            return m;
        }
        std::size_t b = 0;
        std::size_t n = 0;
        Utf8Unit u;
        while (utf8_unit_at(s, b, u)) {
            if (n % kCpIndexStride == 0)
                m.stride_offsets.push_back(b);
            b += u.nbytes;
            ++n;
        }
        m.cp_len = n;
        return m;
    }
    // Byte offset of code point cp < m.cp_len (non-ASCII: stride + walk).
    static std::size_t byte_offset_locked(const std::string& s, const CpIndex& m,
                                          std::size_t cp) noexcept {
        if (m.ascii)
            return cp;
        std::size_t b = m.stride_offsets[cp / kCpIndexStride];
        Utf8Unit u;
        for (std::size_t k = cp % kCpIndexStride; k > 0 && utf8_unit_at(s, b, u); --k)
            b += u.nbytes;
        return b;
    }

    struct Guard {
        std::recursive_mutex* m;
        std::atomic<std::uint64_t>* obs;
        Guard(std::recursive_mutex* mm, std::atomic<std::uint64_t>* o)
            : m(mm)
            , obs(o) {
            if (m) {
                m->lock();
                // Issue #4714: measured AFTER the acquire so the counter
                // can only over-count, never under-count, a lock.
                if (obs)
                    obs->fetch_add(1, std::memory_order_relaxed);
            }
        }
        ~Guard() {
            if (m)
                m->unlock();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };
    // Issue #4608: a slot that no live value references any more.
    // Drop the cached code-point index for one slot (the #4548
    // obligation note_write carries for an in-place write). The
    // caller holds the lock.
    void invalidate_cp_index_locked(size_type idx) noexcept { cp_meta_.erase(idx); }

    Vec v_;
    std::recursive_mutex* mu_ = nullptr;
    // Issue #4714: lock-observation hook (null = unmeasured).
    std::atomic<std::uint64_t>* lock_observer_ = nullptr;
    MintHook hook_ = nullptr;
    void* hook_ctx_ = nullptr;
    // Issue #4608: reusable slots (the reclaim hands the slots it
    // proved unreachable back). free_slots_ is the LIFO of free slot
    // indices; slot_free_ is index-parallel to v_ (1 = the slot is on
    // the free list). Both are guarded by mu_ like v_.
    std::vector<std::size_t> free_slots_;
    std::vector<std::uint8_t> slot_free_;
    // Issue #4548: guarded by mu_ like v_.
    std::uint64_t write_gen_ = 0;
    mutable std::uint64_t cp_meta_gen_ = 0;
    mutable std::unordered_map<size_type, CpIndex> cp_meta_;
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
