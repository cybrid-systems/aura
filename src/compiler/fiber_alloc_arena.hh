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
// ── What is NOT here yet (honest scope) ─────────────────────────────
// The `push_string_heap` hot path still mints into the shared heap under
// alloc_storage_lock_. Routing it through the nursery requires the
// index-space migration above (a tagged payload bit + a shared accessor
// replacing the direct slot-indexing reads), because a value minted
// into a nursery index is indistinguishable from a shared index once it
// reaches an arbitrary reader. This arena + the publish hook is the
// prerequisite slice: the fast path, the copy-on-send promotion, the
// counters that prove the lock is not taken, and O(1) fiber exit.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

} // namespace aura::compiler
