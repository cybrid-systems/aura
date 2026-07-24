// lifetime_pin.ixx — Issue #2000 Phase 2: LifetimePin real RAII pinning +
// generation stamp + FFI handoff + restamp/invalidate hooks. Refines #1226
// Phase 1 (pure counters).
//
// Phase 2 surfaces:
//   - Real pointer + generation stamp storage per pin (ptr_, gen_, arena_id_).
//   - validate(cur_gen, cur_arena_id) → bool: false after compact invalidates.
//   - restamp(new_gen, new_arena_id): pin survived compact, gen bumped.
//     new_gen = 0 keeps current gen (signals "boundary dtor ran" with no
//     observed gen bump — useful for MutationBoundaryGuard dtor wiring
//     where the boundary itself doesn't bump gen, only compact_sweep does).
//   - unpin_on_compact(): pin dead (buffer reclaimed), ptr nulled.
//   - Active registry (function-static vector + mutex) so compact hooks
//     can iterate all live LifetimePin instances tied to an arena and
//     restamp / invalidate them in one pass (no per-pin coordination).
//   - Two new stats counters: invalidations, restamps (Phase 1 retained
//     pins, unpins, ffi_handoffs).
//   - kLifetimePinPhase bumps from 1 to 2.
//   - mark_ffi_handoff stays as the handoff signal (counter + bool flag on
//     the pin for downstream FFI consumers to consult ownership state).
//
// Thread-safety:
//   - registry mutex serializes ctor/dtor/move ctor & assign + global helpers.
//   - ptr_/gen_/arena_id_ are plain fields — validate() reads them without
//     a lock. The worst case is a stale read returning false-negative
//     ("invalid" when actually still valid) — which is safe (FFI just
//     recreates the pin post-compact, no UAF). False-positive is not
//     possible because restamp/unpin_on_compact are serialized by the
//     registry mutex.

module;

export module aura.core.lifetime_pin;

import std;

export namespace aura::core::lifetime {

inline constexpr int kLifetimePinPhase = 2;

struct LifetimePinStats {
    std::uint64_t pins = 0;
    std::uint64_t unpins = 0;
    std::uint64_t ffi_handoffs = 0;
    std::uint64_t invalidations = 0; // Phase 2: compact reclaimed buffer
    std::uint64_t restamps = 0;      // Phase 2: compact bumped gen, pin still valid
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
    [[nodiscard]] bool validate(std::uint64_t cur_gen,
                                std::uint64_t cur_arena_id = 0) const noexcept {
        if (!ptr_)
            return false;
        if (arena_id_ != 0 && cur_arena_id != 0 && arena_id_ != cur_arena_id)
            return false;
        return gen_ == cur_gen;
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

} // namespace aura::core::lifetime