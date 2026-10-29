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
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utf8_scalar.hh" // Issue #4548: code-point index cache

namespace aura::compiler {

// ── Reference-validity contract (Issue #4717) ───────────────────────────
// A reference from LockedStringHeap::operator[] (or Evaluator::string_at,
// which is the payload door) stays valid, with the same bytes, while:
//   1. OTHER slots are minted — the store never moves a constructed slot,
//      and a mint publishes only the new slot, its block pointer and size_;
//   2. the same slot is read from any thread — the read path walks atomic
//      root/page/block pointers and touches only the slot's own bytes.
// It does NOT survive:
//   3. an in-place content write to that slot (mutable_string_at /
//      note_write): the writer's obligation, unchanged from pre-#4717;
//   4. a structural write — resize-shrink, clear, replace, or a reclaim
//      release/reuse. Those are quiescent-only (world stopped, or the
//      reclaim's mark proved the slot unreachable).
// A released slot that is re-minted is a NEW object; the reclaim only
// releases slots no live value references, so no correct program observes
// the previous content through it.
//
// ── Issue #4717: stable-address string slot storage ─────────────────────
//
// The pre-#4717 heap was a std::pmr::vector<std::string>. Growing it MOVED
// every element, so a reader that held a reference (string_at returns a
// reference into the heap) dereferenced freed storage the moment a sibling
// body minted. With the #4717 body mutex gone that is a live data race:
// TSan reports LockedStringHeap::mint_locked_impl (vector.tcc realloc)
// against LockedStringHeap::data() (the span view).
//
// StringSlotStore fixes the mechanism: a slot, once constructed, never
// moves and is never destroyed until a structural write that requires
// quiescence (clear / replace / resize-shrink). Storage is
//
//     root[page]  -> page[block] -> block[slot]
//
//   * `root_` is a fixed array of atomic page pointers, sized once at
//     construction and never moved;
//   * a page is a fixed array of atomic block pointers, allocated on the
//     first slot that needs it and never moved;
//   * a block is kBlockSlots std::string slots, allocated on demand.
//
// Publication: a block pointer is stored with release AFTER its elements
// exist, and size_ is stored with release after the element write. A
// reader walks root -> page -> block with acquire loads and then touches
// ONLY that slot's std::string. It never reads the container metadata a
// mint mutates (the old vector's data/size pointers), which is what makes
// a concurrent mint plus read data-race free instead of merely unlikely.
//
// Writer discipline: emplace_back / resize-grow / assign are single-writer.
// The shared heap serializes writers on its bound mutex; a per-fiber arena
// is unbound and owned by one fiber. destroy paths (clear / replace /
// resize-shrink) require quiescence (world stopped), same as before.
class StringSlotStore {
public:
    using value_type = std::string;
    using size_type = std::size_t;

    static constexpr std::size_t kBlockSlotBits = 9; // 512 strings / block
    static constexpr std::size_t kBlockSlots = std::size_t{1} << kBlockSlotBits;
    static constexpr std::size_t kBlockMask = kBlockSlots - 1;
    static constexpr std::size_t kPageBlockBits = 10; // 1024 blocks / page
    static constexpr std::size_t kPageBlocks = std::size_t{1} << kPageBlockBits;
    static constexpr std::size_t kPageBlockMask = kPageBlocks - 1;
    static constexpr std::size_t kRootPages = 1024;
    // 1024 pages * 1024 blocks * 512 slots.
    static constexpr std::size_t kMaxSlots = kRootPages * kPageBlocks * kBlockSlots;

    using Block = std::array<std::string, kBlockSlots>;

    struct Page {
        std::atomic<Block*> blocks[kPageBlocks];
        Page() noexcept {
            for (auto& b : blocks)
                b.store(nullptr, std::memory_order_relaxed);
        }
    };

    StringSlotStore() noexcept { init_root(); }
    explicit StringSlotStore(std::pmr::memory_resource* r) noexcept
        : res_(r) {
        init_root();
    }
    StringSlotStore(const StringSlotStore&) = delete;
    StringSlotStore& operator=(const StringSlotStore&) = delete;
    ~StringSlotStore() { destroy_all(); }

    [[nodiscard]] size_type size() const noexcept { return size_.load(std::memory_order_acquire); }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    // Reserved capacity is not a heap-level concept any more: growth
    // allocates a block at a time. Kept for the vector-surface callers.
    [[nodiscard]] size_type capacity() const noexcept { return size(); }

    [[nodiscard]] std::string& operator[](size_type i) noexcept {
        return *const_cast<std::string*>(slot_ptr(i));
    }
    [[nodiscard]] const std::string& operator[](size_type i) const noexcept { return *slot_ptr(i); }
    std::string& at(size_type i) { return (*this)[i]; }
    const std::string& at(size_type i) const { return (*this)[i]; }
    // Issue #4714 Row 2: the slot's own storage, or nullptr when its block
    // was never allocated. A WRITE must use this and never operator[]:
    // operator[] substitutes the process-wide empty sentinel for a missing
    // block, and writing there would corrupt every reader of that sentinel.
    [[nodiscard]] std::string* writable_slot(size_type i) noexcept {
        Block* b = find_block(i);
        if (b == nullptr) [[unlikely]]
            return nullptr;
        return &(*b)[i & kBlockMask];
    }
    std::string& back() noexcept { return (*this)[size() - 1]; }
    const std::string& back() const noexcept { return (*this)[size() - 1]; }
    std::string& front() noexcept { return (*this)[0]; }
    const std::string& front() const noexcept { return (*this)[0]; }

    // Append one slot. Returns its index. Single writer (the heap lock,
    // or the owning fiber on an unbound arena heap).
    template <class... Args> size_type emplace_back(Args&&... args) {
        const size_type i = size_.load(std::memory_order_relaxed);
        Block* b = ensure_block(i);
        if (b == nullptr) [[unlikely]]
            return kMaxSlots;
        (*b)[i & kBlockMask] = std::string(std::forward<Args>(args)...);
        size_.store(i + 1, std::memory_order_release);
        return i;
    }

    // Grow with empty slots / shrink (destroys the tail: quiescent only).
    void resize(size_type n) {
        const size_type cur = size_.load(std::memory_order_relaxed);
        if (n > cur) {
            for (size_type i = cur; i < n; ++i) {
                Block* b = ensure_block(i);
                if (b == nullptr) [[unlikely]] {
                    n = i; // slot space exhausted: publish what exists
                    break;
                }
                std::string().swap((*b)[i & kBlockMask]);
            }
        } else {
            for (size_type i = n; i < cur; ++i) {
                Block* b = find_block(i);
                if (b != nullptr)
                    std::string().swap((*b)[i & kBlockMask]);
            }
        }
        size_.store(n, std::memory_order_release);
    }

    void clear() noexcept { destroy_all(); }

    // Install a bulk-built heap (GC compaction). Quiescent only.
    void assign(std::pmr::vector<std::string>&& src) {
        destroy_all();
        for (auto& s : src)
            (void)emplace_back(std::move(s));
    }

    void shrink_to_fit() noexcept {}
    void reserve(size_type) noexcept {}
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return res_; }

private:
    static const std::string& empty_slot() noexcept {
        static const std::string kEmpty;
        return kEmpty;
    }
    void init_root() noexcept {
        for (auto& p : root_)
            p.store(nullptr, std::memory_order_relaxed);
    }
    [[nodiscard]] Block* find_block(size_type i) const noexcept {
        const size_type blk = i >> kBlockSlotBits;
        const size_type pg = blk >> kPageBlockBits;
        if (pg >= kRootPages) [[unlikely]]
            return nullptr;
        Page* page = root_[pg].load(std::memory_order_acquire);
        if (page == nullptr) [[unlikely]]
            return nullptr;
        return page->blocks[blk & kPageBlockMask].load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::string* slot_ptr(size_type i) const noexcept {
        Block* b = find_block(i);
        if (b == nullptr) [[unlikely]]
            return &empty_slot();
        return &(*b)[i & kBlockMask];
    }
    // Single-writer growth: allocate the page / block this slot needs and
    // publish it with a release store.
    Block* ensure_block(size_type i) {
        const size_type blk = i >> kBlockSlotBits;
        const size_type pg = blk >> kPageBlockBits;
        if (pg >= kRootPages) [[unlikely]]
            return nullptr;
        Page* page = root_[pg].load(std::memory_order_relaxed);
        if (page == nullptr) {
            page = new Page();
            root_[pg].store(page, std::memory_order_release);
        }
        Block* b = page->blocks[blk & kPageBlockMask].load(std::memory_order_relaxed);
        if (b == nullptr) {
            if (res_ != nullptr) {
                void* mem = res_->allocate(sizeof(Block), alignof(Block));
                b = new (mem) Block();
            } else {
                b = new Block();
            }
            page->blocks[blk & kPageBlockMask].store(b, std::memory_order_release);
        }
        return b;
    }
    void destroy_all() noexcept {
        for (size_type p = 0; p < kRootPages; ++p) {
            Page* page = root_[p].load(std::memory_order_relaxed);
            if (page == nullptr)
                continue;
            for (size_type k = 0; k < kPageBlocks; ++k) {
                Block* b = page->blocks[k].load(std::memory_order_relaxed);
                if (b == nullptr)
                    continue;
                // Issue #4717: destroy the Block exactly ONCE. The
                // resource path must destroy the placement-new'd Block by
                // hand (deallocate does not run a destructor); the
                // non-resource path must NOT, because delete b runs
                // ~Block itself — doing both freed every string in the
                // block twice (glibc "double free or corruption (!prev)"
                // for any block holding a string past SSO, which is every
                // heap used without a memory resource).
                if (res_ != nullptr) {
                    b->~Block();
                    res_->deallocate(b, sizeof(Block), alignof(Block));
                } else {
                    delete b;
                }
                page->blocks[k].store(nullptr, std::memory_order_relaxed);
            }
            delete page;
            root_[p].store(nullptr, std::memory_order_relaxed);
        }
        size_.store(0, std::memory_order_release);
    }

    std::pmr::memory_resource* res_ = nullptr;
    std::atomic<Page*> root_[kRootPages];
    std::atomic<size_type> size_{0};
};
class LockedStringHeap {
public:
    using Vec = std::pmr::vector<std::string>;
    using value_type = std::string;
    using size_type = std::size_t;
    using reference = std::string&;
    using const_reference = const std::string&;
    // Issue #4717: slots are stable but not contiguous, so iteration is
    // index-based (the range-for surface only ever read the slots).
    struct iterator {
        LockedStringHeap* h = nullptr;
        std::size_t i = 0;
        std::string& operator*() const { return (*h)[i]; }
        iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const iterator& o) const { return i != o.i; }
    };
    struct const_iterator {
        const LockedStringHeap* h = nullptr;
        std::size_t i = 0;
        const std::string& operator*() const { return (*h)[i]; }
        const_iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const const_iterator& o) const { return i != o.i; }
    };
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

    // ── Issue #4714 Row 2: exclusive slot batches (the production mint) ──
    // claim_slots takes the ONE lock acquisition of a batch; every slot it
    // returns is then exclusively owned by the caller, which fills it with
    // write_claimed_slot() (NO lock) and either publishes it or returns it
    // with release_claimed_slots(). That is what lets two fibers mint
    // strings concurrently without serializing on alloc_storage_lock_: the
    // payload stays a plain space-0 index (no new tag, no second index
    // space), the slot's storage lives in this heap, and no other thread
    // can observe or touch a claimed slot until its owner publishes a value
    // that names it.
    //
    // Owner contract (Evaluator::fiber_slab_mint):
    //   * write_generation() must still match the value seen at claim time
    //     (a structural write — resize-shrink / clear / compaction — moved
    //     the slots otherwise, and the claim is re-taken);
    //   * a slot may be returned only when no value references it, which the
    //     publish tail and the fiber-exit path both know.
    // `prev_filled`/`prev_unused` close the batch the caller is done with
    // under the SAME lock acquisition: a fiber owns at most one open batch,
    // so a refill disclaims the exhausted one instead of leaking its claim
    // flags (a claimed slot is skipped by the reclaim forever).
    template <class StampFn, class OnRelease>
    std::vector<std::uint64_t> claim_slots(std::size_t n,
                                           std::span<const std::uint64_t> prev_filled,
                                           std::span<const std::uint64_t> prev_unused,
                                           StampFn&& stamp, OnRelease&& on_unused) {
        std::vector<std::uint64_t> out;
        Guard g(mu_, lock_observer_); // ONE shared-heap lock for the batch
        close_locked(prev_filled, prev_unused, on_unused);
        if (n == 0)
            return out;
        out.reserve(n);
        // (1) Released slots first: the reclaim proved no live value
        // references them, so they are free for exclusive reuse (#4608).
        while (out.size() < n && !free_slots_.empty()) {
            const auto idx = free_slots_.back();
            free_slots_.pop_back();
            if (idx >= v_.size()) [[unlikely]]
                continue;
            if (idx < slot_free_.size())
                slot_free_[idx] = 0;
            set_claimed_locked(idx, true);
            invalidate_cp_index_locked(idx);
            std::string().swap(v_[idx]); // already empty; defensive
            out.push_back(static_cast<std::uint64_t>(idx));
        }
        // (2) Then the append tail. ensure_block() runs under this lock, so
        // the owner's later write never allocates and never takes a lock.
        while (out.size() < n) {
            const auto idx = v_.size();
            if (idx >= StringSlotStore::kMaxSlots) [[unlikely]]
                break;
            v_.emplace_back(std::string());
            slot_free_.push_back(0);
            set_claimed_locked(idx, true);
            out.push_back(static_cast<std::uint64_t>(idx));
        }
        if (!out.empty())
            stamp(std::span<const std::uint64_t>(out.data(), out.size()));
        return out;
    }

    // Fill a claimed slot WITHOUT the lock: the caller is the slot's only
    // writer (see claim_slots). False when the index was invalidated by a
    // structural write or when its block is missing; `s` is then untouched
    // (swap, not move) so the caller can fall back to the locked mint.
    bool write_claimed_slot(std::uint64_t idx, std::string& s) {
        const auto i = static_cast<size_type>(idx);
        if (i >= v_.size()) [[unlikely]]
            return false;
        std::string* slot = v_.writable_slot(i);
        if (slot == nullptr) [[unlikely]]
            return false;
        slot->swap(s);
        return true;
    }

    // Close a claimed batch, ONE lock for the whole batch:
    //   * `filled` slots lose their claim but keep their content — they are
    //     ordinary shared slots from now on, so the file-heap reclaim owns
    //     their lifetime exactly as it owns every other slot's;
    //   * `unused` slots (never handed to a value) are emptied and go back
    //     on the free list;
    //   * on_unused() runs under the same lock for the caller's own
    //     index-parallel bookkeeping (the #4428 tenant record).
    // A batch abandoned because a structural write moved its indices is
    // closed with filled = all slots and an empty `unused`: the content
    // may name live strings after a compaction, so it is neither dropped
    // nor freed, only disclaimed.
    template <class OnRelease>
    std::size_t close_claimed_batch(std::span<const std::uint64_t> filled,
                                    std::span<const std::uint64_t> unused, OnRelease&& on_unused) {
        Guard g(mu_, lock_observer_);
        return close_locked(filled, unused, on_unused);
    }

private:
    template <class OnRelease>
    std::size_t close_locked(std::span<const std::uint64_t> filled,
                             std::span<const std::uint64_t> unused, OnRelease&& on_unused) {
        for (const auto raw : filled) {
            const auto i = static_cast<size_type>(raw);
            if (i < v_.size() && i < claimed_.size())
                claimed_[i] = 0;
        }
        std::size_t released = 0;
        for (const auto raw : unused) {
            const auto i = static_cast<size_type>(raw);
            // Slot 0 is the immortal empty string (the default error-cause
            // sentinel): never released, exactly as release_unmarked has it.
            if (i == 0 || i >= v_.size()) [[unlikely]]
                continue;
            std::string().swap(v_[i]);
            invalidate_cp_index_locked(i);
            if (slot_free_.size() < v_.size())
                slot_free_.resize(v_.size(), 0);
            if (!slot_free_[i]) {
                slot_free_[i] = 1;
                free_slots_.push_back(i);
            }
            if (i < claimed_.size())
                claimed_[i] = 0;
            ++released;
        }
        on_unused(unused);
        return released;
    }

public:
    [[nodiscard]] bool slot_is_claimed(size_type i) const noexcept {
        return i < claimed_.size() && claimed_[i] != 0;
    }
    [[nodiscard]] std::size_t claimed_slot_count() const noexcept { return claimed_.size(); }
    // Slots currently owned by a fiber batch (0 = no claim leaked). Test
    // door: takes the lock and counts, so it is O(heap) — never hot path.
    [[nodiscard]] std::size_t claimed_live_count() const {
        Guard g(mu_, lock_observer_);
        std::size_t n = 0;
        for (const auto c : claimed_)
            if (c)
                ++n;
        return n;
    }

private:
    void set_claimed_locked(size_type i, bool claimed) {
        if (claimed_.size() <= i)
            claimed_.resize(i + 1, 0);
        claimed_[i] = claimed ? 1 : 0;
    }

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
            // Issue #4714 Row 2: a slot claimed by a live fiber's slab is
            // owned by that fiber — the fiber's values are fiber-local until
            // it publishes, so the mark cannot see them. Skipping the
            // release is what keeps the owner's next write from racing a
            // free-list reuse.
            if (slot_is_claimed(i))
                continue;
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
    // Issue #4717: reads are index-only. The heap has no contiguous
    // storage any more (a minted slot must never move), so there is no
    // data() / span / vector view: every reader goes through operator[]
    // or StringPoolView (below), and every payload reader through
    // Evaluator::string_at().
    iterator begin() noexcept { return iterator{this, 0}; }
    iterator end() noexcept { return iterator{this, v_.size()}; }
    const_iterator begin() const noexcept { return const_iterator{this, 0}; }
    const_iterator end() const noexcept { return const_iterator{this, v_.size()}; }
    const_iterator cbegin() const noexcept { return const_iterator{this, 0}; }
    const_iterator cend() const noexcept { return const_iterator{this, v_.size()}; }
    [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return v_.resource(); }

    // ── code-point index (Issue #4548) ─────────────────────────────
    static constexpr std::size_t kCpIndexMinBytes = 256;
    static constexpr std::size_t kCpIndexStride = 64;
    static constexpr std::size_t kCpIndexMaxEntries = 4096;

    // In-place content write to slot idx (same size or not): drop every
    // cached index. Call with or without the lock held.
    void note_write(size_type /*idx*/) noexcept {
        Guard g(mu_, lock_observer_);
        write_gen_.fetch_add(1, std::memory_order_release);
    }
    // Issue #4714 Row 2: the generation is readable WITHOUT the lock. The
    // per-fiber slab path checks it per mint so a structural write
    // (resize-shrink / clear / GC compaction) that invalidated its claimed
    // indices is noticed before the next write lands in a moved slot.
    [[nodiscard]] std::uint64_t write_generation() const noexcept {
        return write_gen_.load(std::memory_order_acquire);
    }

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
        write_gen_.fetch_add(1, std::memory_order_release);
        v_.resize(n);
        slot_free_.resize(n, 0);
        // Issue #4714: a claimed index past the new end no longer exists.
        if (claimed_.size() > n)
            claimed_.resize(n);
        std::erase_if(free_slots_, [n](size_type i) { return i >= n; });
    }
    void clear() noexcept {
        Guard g(mu_, lock_observer_);
        write_gen_.fetch_add(1, std::memory_order_release);
        v_.clear();
        slot_free_.clear();
        claimed_.clear();
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
        write_gen_.fetch_add(1, std::memory_order_release);
        v_.assign(std::move(v));
        slot_free_.assign(v_.size(), 0);
        // Issue #4714: every claimed index now names a different slot; the
        // owner notices through write_generation() and re-claims.
        claimed_.clear();
        free_slots_.clear();
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
        const auto gen = write_generation();
        if (cp_meta_gen_ != gen) {
            cp_meta_.clear();
            cp_meta_gen_ = gen;
        }
        auto it = cp_meta_.find(i);
        if (it != cp_meta_.end() && it->second.data == s.data() && it->second.bytes == s.size() &&
            it->second.gen == gen)
            return it->second;
        if (it == cp_meta_.end()) {
            if (cp_meta_.size() >= kCpIndexMaxEntries)
                cp_meta_.clear();
            it = cp_meta_.try_emplace(i).first;
        }
        CpIndex& m = it->second;
        m.data = s.data();
        m.bytes = s.size();
        m.gen = gen;
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

    // Issue #4717: stable slots (a slot never moves once minted).
    StringSlotStore v_;
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
    // Issue #4714 Row 2: index-parallel to v_ (1 = exclusively owned by a
    // fiber's slab until it publishes or returns the slot). Guarded by mu_.
    std::vector<std::uint8_t> claimed_;
    // Issue #4548: guarded by mu_ like v_. Issue #4714 Row 2: atomic so the
    // lock-free slab path can read it (acquire) without taking mu_.
    std::atomic<std::uint64_t> write_gen_{0};
    mutable std::uint64_t cp_meta_gen_ = 0;
    mutable std::unordered_map<size_type, CpIndex> cp_meta_;
};

// ── Issue #4717: the read-only string-pool view ──────────────────────────
//
// Every reader of a string pool takes one of these instead of a
// std::span<const std::string>. A span required contiguous storage, which a
// stable-slot heap cannot provide (deque/vector growth moves elements, and
// the vector's data/size pointers are exactly what a concurrent mint
// writes). StringPoolView is index-addressable over any of the three
// backends that exist in the engine:
//
//   * LockedStringHeap — the shared heap (space 0) and permanent peer
//     heaps; reads are lock-free and reference-valid per the contract on
//     StringSlotStore;
//   * std::vector<std::string> — keyword / symbol tables and test pools
//     (builder-owned, immutable while a body runs);
//   * std::span<const std::string> — the compatibility door for the pure
//     kernels' existing call sites.
//
// A default-constructed view is empty; an out-of-range index reads as the
// empty string (the pre-#4717 span's operator[] was UB there, and every
// caller bounds-checks first — this only removes the UB, not the check).
class StringPoolView {
public:
    StringPoolView() noexcept = default;
    StringPoolView(const LockedStringHeap& h) noexcept
        : heap_(&h) {}
    StringPoolView(const LockedStringHeap* h) noexcept
        : heap_(h) {}
    StringPoolView(const std::vector<std::string>& v) noexcept
        : vec_(&v) {}
    StringPoolView(std::span<const std::string> s) noexcept
        : span_(s) {}

    [[nodiscard]] const std::string& operator[](std::size_t i) const noexcept {
        if (heap_ != nullptr)
            return (*heap_)[i];
        if (vec_ != nullptr)
            return i < vec_->size() ? (*vec_)[i] : empty_ref();
        return i < span_.size() ? span_[i] : empty_ref();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        if (heap_ != nullptr)
            return heap_->size();
        if (vec_ != nullptr)
            return vec_->size();
        return span_.size();
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] const LockedStringHeap* heap() const noexcept { return heap_; }
    [[nodiscard]] std::span<const std::string> span() const noexcept { return span_; }

    struct const_iterator {
        const StringPoolView* v = nullptr;
        std::size_t i = 0;
        const std::string& operator*() const { return (*v)[i]; }
        const_iterator& operator++() {
            ++i;
            return *this;
        }
        bool operator!=(const const_iterator& o) const { return i != o.i; }
    };
    const_iterator begin() const { return const_iterator{this, 0}; }
    const_iterator end() const { return const_iterator{this, size()}; }

private:
    static const std::string& empty_ref() noexcept {
        static const std::string kEmpty;
        return kEmpty;
    }

    const LockedStringHeap* heap_ = nullptr;
    const std::vector<std::string>* vec_ = nullptr;
    std::span<const std::string> span_{};
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
