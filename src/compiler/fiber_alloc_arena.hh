// Issue #4714: per-fiber allocation arena for strings, with promotion
// on publish (BEAM-style copy-on-send).
//
// ── Why ──────────────────────────────────────────────────────────────
// Every string mint today is one critical section on
// Evaluator::alloc_storage_lock_ (src/compiler/evaluator.ixx:6359) through
// LockedStringHeap::mint(). COW cannot help: a new string is a NEW object,
// not a shared one, so two fibers building strings serialize on a lock they
// do not logically share. #4709 (a global hash key corrupted under a
// rebind + eval-current fiber pair) is the observable symptom of that
// sharing: a reclaim released a slot another fiber was still using.
//
// ── Shape landed here ────────────────────────────────────────────────
// FiberStringArena is a per-fiber nursery:
//   * a std::pmr::monotonic_buffer_resource plus an UNBOUND
//     LockedStringHeap (mu_ == nullptr) — per the mint contract in
//     locked_string_heap.hh, an unbound heap mints without taking a lock;
//   * mint() is therefore the fast path: no mutex, no shared vector growth,
//     no index TOCTOU with a sibling fiber;
//   * drop() returns the whole arena in one monotonic release — an
//     unjoined / failed fiber frees O(#chunks), not per string.
//
// Promotion on publish is copy-on-send: publish_into() deep-copies every
// not-yet-published slot into the shared old-space heap (Evaluator::
// string_heap_) under ONE narrow lock acquisition for the batch (the heap
// mutex is a recursive_mutex, so holding it around the mint loop is legal
// and collapses N acquisitions into 1). Reads of the shared heap stay
// lock-free/immutable after promotion, exactly as before.
//
// ── Owner tagging: side table, not value bits ───────────────────────
// A string value is a 56-bit payload (value_tags.h AURA_MAKE_STRING) that
// indexes the ONE shared string_heap_; the low byte is the kind tag and is
// fully used (AURA_STRING_LOW8). There is no spare bit in the value
// encoding, and the shared heap was read at ~309 direct slot-indexing sites
// (issue #4714 follow-up migrated them to Evaluator::string_at()), so a
// second index space cannot be introduced without migrating
// every read site (issue #4714 follow-up: those 309 reads now go through
// Evaluator::string_at(), the one place the index space is decoded — see
// lib/aura_value_abi.h AURA_STR_SPACE_SHIFT — so a peer space is expressible;
// what still blocks a per-fiber space is LIFETIME, not the tag layout: a
// minted payload reaches shared pairs_/cells_ before the fiber publishes).
// This header therefore keeps ownership in a SIDE TABLE
// (FiberArenaTable: fiber id -> arena, and per-arena local->shared remap
// after promotion) instead of tagging values. Rationale: the tag would have
// to be consulted by every reader; the side table is only consulted by the
// fiber that owns the nursery and by the publish step.
//
// ── Row 2 (production routing): FiberSlabArena ──────────────────────
// The copy-on-send arena above is a NURSERY: its slots are private, so a
// payload minted into it cannot be handed to a shared reader until it is
// copied out. Row 2 needs the production `push_string_heap` door to stop
// taking alloc_storage_lock_, and a private index space is not expressible
// (see the lifetime note above), so FiberSlabArena takes the other road:
// it claims an EXCLUSIVE BATCH of REAL shared-heap slots (LockedStringHeap
// ::claim_slots — one lock per batch) and then mints with no lock at all.
//
//   * the payload is an ordinary space-0 shared index from the moment the
//     mint returns, so every reader, pair/cell/env store, hash key, AOT
//     immediate and GC walk keeps working with NO change;
//   * a batch claim is the only lock acquisition: 1 per batch (512 slots,
//     doubling to 8192), i.e. 0 acquisitions for the steady-state mints
//     inside a claimed batch — that is the Row 2 measurement;
//   * publishing is copy-free: the slots already ARE shared old space, so
//     a publish only returns the unused tail (one narrow lock) and fences
//     so the receiving thread sees this fiber's writes;
//   * a fiber exit returns the unused tail; slots a value already names
//     stay live and the file-heap reclaim frees them when unreachable
//     (a channel send needs no copy at all — the slot is shared already).
//
// The claim is what makes this safe where a private index space is not:
// the index name space, the tag layout and the ABI stay exactly as they
// are, and no value ever names a slot that is not in the shared heap.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/atomic_fence_port.h" // Issue #2931: fences compile out under TSan
#include "locked_string_heap.hh"

namespace aura::compiler {

// ── counters (module-local) ─────────────────────────────────────────
// Deliberately function-local statics: the evaluator module and a test TU
// get one entity each, and every observation reaches them through
// Evaluator accessors, so a test never compares its copy to the module's.
namespace fiber_arena_stats {

    inline std::atomic<std::uint64_t>& fast_path_mints() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    // Every lock actually acquired on a nursery mint path (observed inside
    // LockedStringHeap::Guard, so it cannot be faked by an unbound heap that
    // silently skips the lock).
    inline std::atomic<std::uint64_t>& fast_path_locks() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& table_locks() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& promote_locks() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& promoted_strings() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& dropped_arenas() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    // ── Issue #4714 Row 2: the production slab path ──
    // Batch claims (each is ONE shared-heap lock acquisition), mints that
    // took no lock, publishes (the Row 3 "promotion" batches) and drops.
    inline std::atomic<std::uint64_t>& slab_claims() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& slab_mints() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& slab_publish_locks() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& slab_dropped() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }
    inline std::atomic<std::uint64_t>& slab_table_locks() noexcept {
        static std::atomic<std::uint64_t> v{0};
        return v;
    }

} // namespace fiber_arena_stats

// One fiber's private string nursery.
class FiberStringArena {
public:
    // Local slot -> shared heap index for the slots already published.
    // slots before `published` are valid; later ones are not yet in the
    // shared heap.
    struct PromoteResult {
        std::vector<std::uint64_t> remap; // delta: local - first_local
        std::size_t first_local = 0;
        std::uint64_t promoted = 0;
        std::uint64_t locks_taken = 0;
    };

    explicit FiberStringArena(std::pmr::memory_resource* upstream = std::pmr::new_delete_resource())
        : resource_(upstream)
        , heap_(&resource_) {
        // mu_ is null: the mint contract's unlocked path.
        heap_.set_lock_observer(&fiber_arena_stats::fast_path_locks());
    }
    FiberStringArena(const FiberStringArena&) = delete;
    FiberStringArena& operator=(const FiberStringArena&) = delete;
    ~FiberStringArena() { drop(); }

    // Fast path: no lock, fiber-local growth. Returns the local slot.
    std::uint64_t mint(std::string s) {
        const auto idx = heap_.mint(std::move(s));
        fiber_arena_stats::fast_path_mints().fetch_add(1, std::memory_order_relaxed);
        return idx;
    }
    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
    [[nodiscard]] const std::string& at(std::size_t i) const { return heap_[i]; }
    [[nodiscard]] std::uint64_t published() const noexcept { return published_; }

    // Copy-on-send: deep-copy every unpublished slot into the shared
    // old-space heap under ONE lock acquisition. Safe to call repeatedly;
    // only the delta is copied.
    PromoteResult publish_into(LockedStringHeap& shared) {
        PromoteResult out;
        const auto n = heap_.size();
        out.first_local = static_cast<std::size_t>(published_);
        if (out.first_local >= n)
            return out;
        std::recursive_mutex* mu = shared.mutex();
        if (mu) {
            mu->lock();
            ++out.locks_taken;
            fiber_arena_stats::promote_locks().fetch_add(1, std::memory_order_relaxed);
        }
        try {
            // One growth for the whole batch, then mint without the
            // per-slot recursive re-lock (mint_locked: we hold the mutex).
            shared.reserve(shared.size() + (n - out.first_local));
            out.remap.reserve(n - out.first_local);
            for (std::size_t i = out.first_local; i < n; ++i) {
                // Deep copy: the nursery's buffer dies with the arena.
                out.remap.push_back(shared.mint_locked(std::string(heap_[i])));
            }
        } catch (...) {
            if (mu)
                mu->unlock();
            throw;
        }
        if (mu)
            mu->unlock();
        out.promoted = static_cast<std::uint64_t>(out.remap.size());
        published_ = static_cast<std::uint64_t>(n);
        fiber_arena_stats::promoted_strings().fetch_add(out.promoted, std::memory_order_relaxed);
        return out;
    }

    // Fiber exit without publish: O(#chunks) wholesale release.
    void drop() noexcept {
        if (dropped_)
            return;
        dropped_ = true;
        heap_.clear();
        resource_.release();
    }

    [[nodiscard]] std::atomic<std::uint64_t>* lock_observer() const noexcept {
        return heap_.lock_observer();
    }

private:
    std::pmr::monotonic_buffer_resource resource_;
    LockedStringHeap heap_;
    std::uint64_t published_ = 0;
    bool dropped_ = false;
};

// fiber id -> arena. The only shared state on the nursery path; the
// Evaluator caches the resolved arena per thread so steady-state mints
// never touch this mutex.
class FiberArenaTable {
public:
    FiberStringArena& get_or_create(std::uint64_t fiber_id) {
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = arenas_.find(fiber_id);
            if (it != arenas_.end())
                return *it->second;
            auto owned = std::make_unique<FiberStringArena>();
            auto* raw = owned.get();
            arenas_.emplace(fiber_id, std::move(owned));
            return *raw;
        }
    }

    // Publish the fiber's nursery into shared old space, then drop the
    // arena. The arena is removed from the table under the table mutex,
    // but the promotion itself runs outside it so concurrent fibers never
    // wait on the shared heap while holding the table mutex.
    FiberStringArena::PromoteResult publish_and_drop(LockedStringHeap& shared,
                                                     std::uint64_t fiber_id) {
        std::unique_ptr<FiberStringArena> owned = take(fiber_id);
        if (!owned)
            return {};
        auto out = owned->publish_into(shared);
        owned->drop();
        fiber_arena_stats::dropped_arenas().fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    // Fiber exit without publish.
    void drop(std::uint64_t fiber_id) {
        auto owned = take(fiber_id);
        if (!owned)
            return;
        owned->drop();
        fiber_arena_stats::dropped_arenas().fetch_add(1, std::memory_order_relaxed);
    }

    // Diagnostic read: the caller must own the fiber, or the fiber must
    // have finished (no concurrent mint into this arena).
    [[nodiscard]] FiberStringArena* find(std::uint64_t fiber_id) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = arenas_.find(fiber_id);
        return it == arenas_.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] bool contains(std::uint64_t fiber_id) {
        std::lock_guard<std::mutex> g(mu_);
        return arenas_.find(fiber_id) != arenas_.end();
    }

    [[nodiscard]] std::size_t size() {
        std::lock_guard<std::mutex> g(mu_);
        return arenas_.size();
    }

    [[nodiscard]] std::uint64_t epoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

private:
    std::unique_ptr<FiberStringArena> take(std::uint64_t fiber_id) {
        std::unique_ptr<FiberStringArena> owned;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = arenas_.find(fiber_id);
            if (it == arenas_.end())
                return owned;
            owned = std::move(it->second);
            arenas_.erase(it);
        }
        // A dropped arena may still be cached by its owning thread; bump
        // the epoch so the next mint re-resolves instead of dereferencing
        // the freed pointer.
        epoch_.fetch_add(1, std::memory_order_release);
        return owned;
    }

    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, std::unique_ptr<FiberStringArena>> arenas_;
    std::atomic<std::uint64_t> epoch_{0};
};

// Issue #4714 Row 2: the production per-fiber mint arena.
//
// One arena per fiber, created on the first mint that happens inside that
// fiber and dropped when the fiber publishes or exits. It owns a batch of
// EXCLUSIVELY claimed shared-heap slots and writes into them with no lock;
// see the header comment for why the slots are real shared slots and not a
// private heap.
class FiberSlabArena {
public:
    // One StringSlotStore block is 512 slots, so the first batch costs the
    // same storage a single pre-#4714 mint already paid (16 KiB) and the
    // batch doubles up to kMaxBatch, keeping the amortized claim cost at
    // 1 lock per kMaxBatch mints for a long-lived fiber.
    static constexpr std::size_t kFirstBatch = 512;
    static constexpr std::size_t kMaxBatch = 8192;
    // Returns the tenant stamp it applied to the batch (0 = never stamped).
    using SlotStamper = std::uint64_t (*)(void* ctx, std::span<const std::uint64_t> slots);
    using SlotStamperVoid = void (*)(void* ctx, std::span<const std::uint64_t> slots);

    FiberSlabArena(LockedStringHeap& shared, std::uint64_t fiber_id, SlotStamper stamp,
                   SlotStamperVoid unstamp, void* ctx) noexcept
        : heap_(&shared)
        , fiber_id_(fiber_id)
        , stamp_(stamp)
        , unstamp_(unstamp)
        , ctx_(ctx) {}
    FiberSlabArena(const FiberSlabArena&) = delete;
    FiberSlabArena& operator=(const FiberSlabArena&) = delete;
    ~FiberSlabArena() = default;

    // Fast path: fill the next claimed slot. nullopt when this arena cannot
    // take the mint (heap slot space exhausted, or a structural write moved
    // the claimed slots and the re-claim found nothing); `s` is untouched
    // in that case and the caller falls back to the locked shared mint.
    std::optional<std::uint64_t> mint(std::string& s) {
        if (gen_ != heap_->write_generation()) [[unlikely]]
            claim(/*release_tail=*/false); // structural write moved the batch
        else if (next_ >= slots_.size()) [[unlikely]]
            claim(/*release_tail=*/true); // exhausted: close it, claim the next
        if (next_ >= slots_.size()) [[unlikely]]
            return std::nullopt;
        const auto idx = slots_[next_];
        if (!heap_->write_claimed_slot(idx, s)) [[unlikely]]
            return std::nullopt;
        ++next_;
        ++minted_;
        fiber_arena_stats::slab_mints().fetch_add(1, std::memory_order_relaxed);
        return idx;
    }

    [[nodiscard]] std::uint64_t tenant() const noexcept { return tenant_; }
    [[nodiscard]] std::size_t minted() const noexcept { return minted_; }
    [[nodiscard]] std::uint64_t claims() const noexcept { return claims_; }
    [[nodiscard]] std::size_t pending() const noexcept { return slots_.size() - next_; }
    [[nodiscard]] std::uint64_t fiber_id() const noexcept { return fiber_id_; }
    // Local index -> shared slot index for the OPEN batch (a closed batch's
    // slots keep their content but are no longer addressable here; they are
    // ordinary shared slots by then). Diagnostics/tests only.
    [[nodiscard]] std::uint64_t slot_at(std::size_t local) const noexcept {
        if (local >= slots_.size())
            return static_cast<std::uint64_t>(-1);
        return slots_[local];
    }

    // Publish: nothing is copied — the payloads are already space-0 shared
    // slots. Return the unused tail to the heap under ONE narrow lock and
    // fence so a thread that receives a value sees this fiber's bytes.
    // Returns the published slot indices (local order).
    std::vector<std::uint64_t> publish() {
        std::vector<std::uint64_t> out(slots_.begin(),
                                       slots_.begin() + static_cast<std::ptrdiff_t>(next_));
        close_batch();
        // Issue #2931: the port compiles the fence out under TSan (the
        // messaging handoff's mutex carries the ordering there); a normal
        // build gets a real release fence so a receiving thread sees the
        // bytes this fiber wrote before the value was handed over.
        ::aura::util::thread_fence(std::memory_order_release);
        fiber_arena_stats::slab_publish_locks().fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    // Fiber exit: the same tail release, without the publish fence. Slots a
    // value already names are NOT returned (a channel send may have handed
    // one to another fiber); the file-heap reclaim frees them once nothing
    // references them.
    void drop() {
        close_batch();
        fiber_arena_stats::slab_dropped().fetch_add(1, std::memory_order_relaxed);
    }

private:
    // Close the current batch and claim the next one under ONE lock.
    //   release_tail = true  : the batch is exhausted/replaced normally, so
    //                          its unused tail goes back to the free list;
    //   release_tail = false : a structural write moved the indices, so the
    //                          old slots are only DISCLAIMED (after a
    //                          compaction an index may name a live string).
    void claim(bool release_tail) {
        std::uint64_t stamp_tenant = 0;
        const auto used = release_tail
                              ? std::span<const std::uint64_t>(slots_.data(), next_)
                              : std::span<const std::uint64_t>(slots_.data(), slots_.size());
        const auto unused = release_tail ? std::span<const std::uint64_t>(slots_.data() + next_,
                                                                          slots_.size() - next_)
                                         : std::span<const std::uint64_t>{};
        auto got = heap_->claim_slots(
            batch_, used, unused,
            [this, &stamp_tenant](std::span<const std::uint64_t> s) {
                stamp_tenant = stamp_ ? stamp_(ctx_, s) : std::uint64_t{0};
            },
            [this](std::span<const std::uint64_t> s) {
                if (unstamp_)
                    unstamp_(ctx_, s);
            });
        gen_ = heap_->write_generation();
        slots_ = std::move(got);
        next_ = 0;
        if (slots_.empty()) [[unlikely]]
            return;
        tenant_ = stamp_tenant;
        if (batch_ < kMaxBatch)
            batch_ = std::min(kMaxBatch, batch_ * 2);
        ++claims_;
        fiber_arena_stats::slab_claims().fetch_add(1, std::memory_order_relaxed);
    }

    // Close the batch: keep the filled slots (ordinary shared slots now),
    // return the unused tail to the free list, ONE lock for the batch.
    void close_batch() {
        const auto filled = std::span<const std::uint64_t>(slots_.data(), next_);
        const auto unused =
            std::span<const std::uint64_t>(slots_.data() + next_, slots_.size() - next_);
        (void)heap_->close_claimed_batch(filled, unused, [this](std::span<const std::uint64_t> s) {
            if (unstamp_)
                unstamp_(ctx_, s);
        });
        slots_.clear();
        next_ = 0;
    }

    LockedStringHeap* heap_ = nullptr;
    std::uint64_t fiber_id_ = 0;
    SlotStamper stamp_ = nullptr;
    SlotStamperVoid unstamp_ = nullptr;
    void* ctx_ = nullptr;
    std::vector<std::uint64_t> slots_;
    std::size_t next_ = 0;
    std::size_t batch_ = kFirstBatch;
    std::uint64_t gen_ = 0;
    std::uint64_t tenant_ = 0;
    std::uint64_t claims_ = 0;
    std::size_t minted_ = 0;
};

// fiber id -> FiberSlabArena. The only shared state on the mint path; the
// Evaluator caches the resolved arena per thread, so steady-state mints
// never touch this mutex (same shape as FiberArenaTable above).
class FiberSlabTable {
public:
    FiberSlabArena& get_or_create(std::uint64_t fiber_id, LockedStringHeap& shared,
                                  FiberSlabArena::SlotStamper stamp,
                                  FiberSlabArena::SlotStamperVoid unstamp, void* ctx) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = arenas_.find(fiber_id);
        if (it != arenas_.end())
            return *it->second;
        auto owned = std::make_unique<FiberSlabArena>(shared, fiber_id, stamp, unstamp, ctx);
        auto* raw = owned.get();
        arenas_.emplace(fiber_id, std::move(owned));
        return *raw;
    }

    // Remove the arena (publish / exit). The caller owns it afterwards and
    // runs the (locking) tail release outside this mutex.
    std::unique_ptr<FiberSlabArena> take(std::uint64_t fiber_id) {
        std::unique_ptr<FiberSlabArena> owned;
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = arenas_.find(fiber_id);
            if (it == arenas_.end())
                return owned;
            owned = std::move(it->second);
            arenas_.erase(it);
        }
        // A dropped arena may still be cached by its owning thread; bump
        // the epoch so the next mint re-resolves instead of dereferencing
        // the freed pointer.
        epoch_.fetch_add(1, std::memory_order_release);
        return owned;
    }

    [[nodiscard]] FiberSlabArena* find(std::uint64_t fiber_id) {
        std::lock_guard<std::mutex> g(mu_);
        auto it = arenas_.find(fiber_id);
        return it == arenas_.end() ? nullptr : it->second.get();
    }

    [[nodiscard]] std::size_t size() {
        std::lock_guard<std::mutex> g(mu_);
        return arenas_.size();
    }

    [[nodiscard]] std::uint64_t epoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<std::uint64_t, std::unique_ptr<FiberSlabArena>> arenas_;
    std::atomic<std::uint64_t> epoch_{0};
};

} // namespace aura::compiler
