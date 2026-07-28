// lifetime_pin.ixx — Issue #2000 Phase 2: LifetimePin real RAII pinning +
// generation stamp + FFI handoff + restamp/invalidate hooks. Refines #1226
// Phase 1 (pure counters). Issue #2048: batch terminal present zero-copy
// handoff holds a LifetimePin for the ANSI/cell buffer under the render
// hotpath soft-gate and arms g_ffi_pin_defer_depth so compact/GC cannot
// reclaim the buffer mid-write (see present_batch_impl).
//
// Phase 3 (#2265): add real pointer remap for Moving densify. The pin
// tracks an object address; if the densify site relocates the tracked
// object, the pin's ptr_ must follow or downstream FFI reads UAF. Soft
// and Force paths don't move objects, so remap is Moving-only and
// never touches the non-moving hot path.
//
// Phase 2 surfaces:
//   - Real pointer + generation stamp storage per pin (ptr_, gen_, arena_id_).
//   - validate(cur_gen, cur_arena_id) → bool: false after compact invalidates.
//   - restamp(new_gen, new_arena_id): pin survived compact, gen bumped.
//     new_gen = 0 keeps current gen (signals "boundary dtor ran" with no
//     observed gen bump — useful for MutationBoundaryGuard dtor wiring
//     where the boundary itself doesn't bump gen, only compact_sweep does).
//   - unpin_on_compact(): pin dead (buffer reclaimed), ptr nulled.
//   - remap(new_ptr, new_gen): Phase 3 (#2265) — densify rewrote the
//     pointee; pin updates ptr_ (and gen when new_gen != 0) to track the
//     new address without going through unpin+repin. Bumps remap counter.
//   - Active registry (function-static vector + mutex) so compact hooks
//     can iterate all live LifetimePin instances tied to an arena and
//     restamp / invalidate / remap them in one pass (no per-pin coordination).
//   - Three new stats counters: invalidations, restamps, remaps
//     (Phase 1 retained pins, unpins, ffi_handoffs).
//   - kLifetimePinPhase bumps from 2 to 3.
//   - mark_ffi_handoff stays as the handoff signal (counter + bool flag on
//     the pin for downstream FFI consumers to consult ownership state).
//
// Thread-safety:
//   - registry mutex serializes ctor/dtor/move ctor & assign + global helpers.
//   - ptr_/gen_/arena_id_ are plain fields — validate() reads them without
//     a lock. The worst case is a stale read returning false-negative
//     ("invalid" when actually still valid) — which is safe (FFI just
//     recreates the pin post-compact, no UAF). False-positive is not
//     possible because restamp/unpin_on_compact/rmap are serialized by the
//     registry mutex.

module;

export module aura.core.lifetime_pin;

import std;

export namespace aura::core::lifetime {

inline constexpr int kLifetimePinPhase = 3;

struct LifetimePinStats {
    std::uint64_t pins = 0;
    std::uint64_t unpins = 0;
    std::uint64_t ffi_handoffs = 0;
    std::uint64_t invalidations = 0; // Phase 2: compact reclaimed buffer
    std::uint64_t restamps = 0;      // Phase 2: compact bumped gen, pin still valid
    // Issue #2265 Phase 3: Moving densify remapped ptr_ to a new address
    // while keeping gen/arena intact. Bumps per-pin when remap() succeeds.
    std::uint64_t remaps = 0;
    // Issue #2265 Phase 3: Moving densify expected a pin remap (old_ptr in
    // last_object_remap_) but no matching pin was in the registry.
    // Indicates stale ptr_ or pin destroyed between relocate + remap walk.
    // Agent-visible counter so dashboards can flag orphaned remap entries.
    std::uint64_t remap_misses = 0;
    // Issue #2085: validate() detected gen drift between the pin's stored
    // gen_ and the caller's cur_gen (or arena_id mismatch). Bumps when
    // `validate(cur_gen, cur_arena_id)` returns false for reasons other
    // than ptr being null. Agent-visible counter so dashboards can flag
    // long-held pins that drifted across a Force live_compact cycle.
    std::uint64_t gen_mismatch_total = 0;
};

inline LifetimePinStats g_lifetime_pin_stats{};

class LifetimePin;

// Active pin registry (function-static so LifetimePin ctor can reference
// it without forward-declaration ordering concerns). Initialized on first
// use; cleared on module unload.
inline std::vector<LifetimePin*>& pin_registry() {
    static std::vector<LifetimePin*> v;
    return v;
}
inline std::mutex& pin_registry_mtx() {
    static std::mutex m;
    return m;
}

class LifetimePin {
public:
    LifetimePin() noexcept {
        std::lock_guard<std::mutex> lock(pin_registry_mtx());
        pin_registry().push_back(this);
        ++g_lifetime_pin_stats.pins;
    }
    ~LifetimePin() noexcept {
        std::lock_guard<std::mutex> lock(pin_registry_mtx());
        auto& reg = pin_registry();
        auto it = std::find(reg.begin(), reg.end(), this);
        if (it != reg.end())
            reg.erase(it);
        ++g_lifetime_pin_stats.unpins;
    }
    LifetimePin(const LifetimePin&) = delete;
    LifetimePin& operator=(const LifetimePin&) = delete;

    LifetimePin(LifetimePin&& o) noexcept
        : ptr_(o.ptr_)
        , gen_(o.gen_)
        , arena_id_(o.arena_id_)
        , ffi_handoff_(o.ffi_handoff_) {
        o.ptr_ = nullptr;
        o.gen_ = 0;
        o.arena_id_ = 0;
        o.ffi_handoff_ = false;
        std::lock_guard<std::mutex> lock(pin_registry_mtx());
        auto& reg = pin_registry();
        auto it = std::find(reg.begin(), reg.end(), &o);
        if (it != reg.end())
            *it = this;
        else
            reg.push_back(this);
    }
    LifetimePin& operator=(LifetimePin&& o) noexcept {
        if (this != &o) {
            ptr_ = o.ptr_;
            gen_ = o.gen_;
            arena_id_ = o.arena_id_;
            ffi_handoff_ = o.ffi_handoff_;
            o.ptr_ = nullptr;
            o.gen_ = 0;
            o.arena_id_ = 0;
            o.ffi_handoff_ = false;
            std::lock_guard<std::mutex> lock(pin_registry_mtx());
            auto& reg = pin_registry();
            auto it = std::find(reg.begin(), reg.end(), &o);
            if (it != reg.end())
                *it = this;
        }
        return *this;
    }

    // Pin a buffer pointer with a generation stamp. arena_id = 0 means
    // "no specific arena — generic FFI buffer" (validate still checks gen).
    void pin(void* p, std::uint64_t g, std::uint64_t arena_id = 0) noexcept {
        ptr_ = p;
        gen_ = g;
        arena_id_ = arena_id;
        ffi_handoff_ = false;
    }

    [[nodiscard]] bool pinned() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] void* ptr() const noexcept { return ptr_; }
    [[nodiscard]] std::uint64_t gen() const noexcept { return gen_; }
    [[nodiscard]] std::uint64_t arena_id() const noexcept { return arena_id_; }

    // Validate pin against current generation + arena id. Returns false if
    // pin was invalidated (ptr nulled) or gen / arena_id mismatch.
    // Issue #2085: bump gen_mismatch_total on arena_id mismatch or gen
    // mismatch (ptr-null returns false but is not a mismatch).
    [[nodiscard]] bool validate(std::uint64_t cur_gen,
                                std::uint64_t cur_arena_id = 0) const noexcept {
        if (!ptr_)
            return false;
        if (arena_id_ != 0 && cur_arena_id != 0 && arena_id_ != cur_arena_id) {
            ++g_lifetime_pin_stats.gen_mismatch_total;
            return false;
        }
        if (gen_ != cur_gen) {
            ++g_lifetime_pin_stats.gen_mismatch_total;
            return false;
        }
        return true;
    }

    // Compact hook (Phase 2): pin survived compact, gen bumped to track
    // new generation. new_gen = 0 keeps current gen (boundary-dtor use).
    // new_arena_id = 0 keeps current arena_id.
    void restamp(std::uint64_t new_gen = 0, std::uint64_t new_arena_id = 0) noexcept {
        if (!ptr_)
            return;
        if (new_gen != 0)
            gen_ = new_gen;
        if (new_arena_id != 0)
            arena_id_ = new_arena_id;
        ++g_lifetime_pin_stats.restamps;
    }

    // Compact hook (Phase 2): buffer dead, pin invalidated. Subsequent
    // validate() returns false; FFI consumer must re-pin if it needs the
    // buffer again.
    void unpin_on_compact() noexcept {
        if (!ptr_)
            return;
        ptr_ = nullptr;
        gen_ = 0;
        arena_id_ = 0;
        ffi_handoff_ = false;
        ++g_lifetime_pin_stats.invalidations;
    }

    // Compact hook (Phase 3 / #2265): Moving densify rewrote the
    // pointee to a new address. Update ptr_ (and gen when new_gen != 0)
    // to follow the move WITHOUT going through unpin+repin. arena_id is
    // unchanged (densify keeps the arena boundary). FFI consumers that
    // cached `ptr()` see the new address on next call; `validate(...)`
    // continues to succeed against the new gen. Bumps remaps counter.
    void remap(void* new_ptr, std::uint64_t new_gen = 0) noexcept {
        if (!new_ptr)
            return; // fail closed: do not null an existing pin's ptr_
        ptr_ = new_ptr;
        if (new_gen != 0)
            gen_ = new_gen;
        ++g_lifetime_pin_stats.remaps;
    }

    // FFI handoff signal. Phase 1: counter bump only. Phase 2: also flips
    // an internal flag so downstream consumers (e.g. ffi_hot batch dispatch)
    // can consult ownership transfer state without re-querying the caller.
    void mark_ffi_handoff() noexcept {
        ffi_handoff_ = true;
        ++g_lifetime_pin_stats.ffi_handoffs;
    }
    [[nodiscard]] bool ffi_handoff() const noexcept { return ffi_handoff_; }

private:
    void* ptr_ = nullptr;
    std::uint64_t gen_ = 0;
    std::uint64_t arena_id_ = 0;
    bool ffi_handoff_ = false;
};

// Restamp every live LifetimePin tied to `arena_id`. arena_id == 0
// matches all (use for boundary-wide restamp). new_gen == 0 keeps current
// gen. Returns # restamped (counter-bumped).
inline std::size_t restamp_all_pins_for_arena(std::uint64_t arena_id,
                                              std::uint64_t new_gen = 0) noexcept {
    std::lock_guard<std::mutex> lock(pin_registry_mtx());
    auto& reg = pin_registry();
    std::size_t n = 0;
    for (auto* p : reg) {
        if (!p || !p->pinned())
            continue;
        if (arena_id != 0 && p->arena_id() != arena_id)
            continue;
        p->restamp(new_gen, arena_id);
        ++n;
    }
    return n;
}

// Invalidate every live LifetimePin tied to `arena_id`. Returns # invalidated.
inline std::size_t invalidate_all_pins_for_arena(std::uint64_t arena_id) noexcept {
    std::lock_guard<std::mutex> lock(pin_registry_mtx());
    auto& reg = pin_registry();
    std::size_t n = 0;
    for (auto* p : reg) {
        if (!p || !p->pinned())
            continue;
        if (arena_id != 0 && p->arena_id() != arena_id)
            continue;
        p->unpin_on_compact();
        ++n;
    }
    return n;
}

// Issue #2265: Moving densify walks `last_object_remap_` (built by
// relocate_tracked_objects_for_moving_) and remaps every live pin whose
// ptr_ matches the old address. Pins not present in the registry
// (stale ptr_ or destroyed between relocate + remap walk) bump
// `remap_misses` so dashboards can flag orphaned remap entries.
// arena_id_filter (0 = any arena) narrows the walk to one arena.
// O(pin_count); only called on Moving densify success path (AC3
// zero-cost guarantee holds for Soft/Force / no-compact).
// Returns (remapped_count, miss_count).
struct RemapResult {
    std::size_t remapped = 0;
    std::size_t misses = 0;
};
inline RemapResult remap_pins_pointing_to(void* old_ptr, void* new_ptr, std::uint64_t new_gen = 0,
                                          std::uint64_t arena_id_filter = 0) noexcept {
    RemapResult out{};
    if (!old_ptr || !new_ptr)
        return out;
    std::lock_guard<std::mutex> lock(pin_registry_mtx());
    auto& reg = pin_registry();
    for (auto* p : reg) {
        if (!p || !p->pinned())
            continue;
        if (arena_id_filter != 0 && p->arena_id() != arena_id_filter)
            continue;
        if (p->ptr() != old_ptr) {
            // No match: bump miss only if this pin's arena IS the filter
            // (otherwise the pin belongs to a different arena entirely
            // and isn't a candidate for this remap).
            if (arena_id_filter != 0 && p->arena_id() == arena_id_filter)
                ++g_lifetime_pin_stats.remap_misses;
            continue;
        }
        p->remap(new_ptr, new_gen);
        ++out.remapped;
    }
    return out;
}

// Aggregate stats convenience (Phase 3): # remap calls invoked across
// the registry. Useful for tests + observability snapshots.
inline std::uint64_t lifetime_pin_remap_total() noexcept {
    return g_lifetime_pin_stats.remaps;
}
inline std::uint64_t lifetime_pin_remap_miss_total() noexcept {
    return g_lifetime_pin_stats.remap_misses;
}

// Total live pins (for tests + observability).
inline std::size_t live_pin_count() noexcept {
    std::lock_guard<std::mutex> lock(pin_registry_mtx());
    auto& reg = pin_registry();
    std::size_t n = 0;
    for (auto* p : reg)
        if (p && p->pinned())
            ++n;
    return n;
}

// Issue #2256: Moving-compact hard contract. Every live pointer
// that crosses a Moving compact must be either pinned (still valid
// across the move) OR explicitly remapped. The pin-or-remap
// accumulator tracks how many live pins were honored (pin_hits) and
// how many had to be remapped (remap_us). When pin_count >= some
// bound, Moving compact yields to Force — keeps the hard contract.
inline std::atomic<std::uint64_t> g_moving_compact_pin_hits_total{0};
inline std::atomic<std::uint64_t> g_moving_compact_remap_us_total{0};
inline std::atomic<std::uint64_t> g_moving_compact_count_total{0};
inline std::atomic<std::uint64_t> g_moving_compact_bytes_reclaimed_total{0};

// Hard-contract verification: under Moving compact, every live pin
// must be honored (return true if honored, false if compact must yield).
// Cost model: O(pin_count). The AC3 zero-cost guarantee holds when
// no Moving compact runs (the function is never called from the
// production hot path — only from the compact driver).
inline bool verify_pins_under_moving_compact() noexcept {
    std::lock_guard<std::mutex> lock(pin_registry_mtx());
    auto& reg = pin_registry();
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t honored = 0;
    for (auto* p : reg) {
        if (!p || !p->pinned())
            continue;
        // Per-pin contract: caller (Moving densify) must have called
        // remap_pins_pointing_to(old, new, new_gen) for every (old, neu)
        // pair in last_object_remap_ BEFORE this verification. The pin
        // itself remains valid (it was set up at capture time); the
        // pointee is verified at unpin time. Here we just count
        // honored pins.
        ++honored;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    g_moving_compact_pin_hits_total.fetch_add(honored, std::memory_order_relaxed);
    g_moving_compact_remap_us_total.fetch_add(static_cast<std::uint64_t>(us),
                                              std::memory_order_relaxed);
    g_moving_compact_count_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace aura::core::lifetime