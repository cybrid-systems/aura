// lifetime_pin.hh — Issue #2000 Phase 2 / #2048 header form of LifetimePin.
// Keep in sync with lifetime_pin.ixx for module consumers.
// Used by non-module TUs (render_primitives.cpp, etc.).

#ifndef AURA_CORE_LIFETIME_PIN_HH
#define AURA_CORE_LIFETIME_PIN_HH

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "layout_stamp.hh" // Issue #2170: LayoutStamp validate overload

namespace aura::core::lifetime {

inline constexpr int kLifetimePinPhase = 2;
inline constexpr int kLifetimePinIssue = 2048; // joint batch-FFI present contract

struct LifetimePinStats {
    std::uint64_t pins = 0;
    std::uint64_t unpins = 0;
    std::uint64_t ffi_handoffs = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t restamps = 0;
    // Issue #2270: PinOwner transition counters (mark_ffi_handoff /
    // mark_ffi_owned / release_ffi bump these on each transition).
    std::uint64_t pin_owner_arena_transitions = 0;
    std::uint64_t pin_owner_ffi_borrowed_transitions = 0;
    std::uint64_t pin_owner_ffi_owned_transitions = 0;
};

inline LifetimePinStats& g_lifetime_pin_stats() noexcept {
    static LifetimePinStats s;
    return s;
}

class LifetimePin;

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
        ++g_lifetime_pin_stats().pins;
    }
    ~LifetimePin() noexcept {
        std::lock_guard<std::mutex> lock(pin_registry_mtx());
        auto& reg = pin_registry();
        auto it = std::find(reg.begin(), reg.end(), this);
        if (it != reg.end())
            reg.erase(it);
        ++g_lifetime_pin_stats().unpins;
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

    [[nodiscard]] bool validate(std::uint64_t cur_gen,
                                std::uint64_t cur_arena_id = 0) const noexcept {
        if (!ptr_)
            return false;
        if (arena_id_ != 0 && cur_arena_id != 0 && arena_id_ != cur_arena_id)
            return false;
        return gen_ == cur_gen;
    }

    // Issue #2170: LayoutStamp overload — unified freshness check.
    // Per #2170 ("document which fields pin uses — arena_gen + arena_id
    // only"), the pin only consults arena_gen + arena_id out of the 6
    // stamp fields. Other fields (flat_gen / mutation_epoch / env_gen
    // / defuse_version) are not part of pin validity — they describe
    // the surrounding mutation boundary / AOT emit context, but the
    // pin itself only survives an arena compact. Matches the existing
    // 2-arg validate() semantics exactly.
    [[nodiscard]] bool validate(const aura::core::LayoutStamp& stamp) const noexcept {
        if (!ptr_)
            return false;
        if (arena_id_ != 0 && stamp.arena_id != 0 && arena_id_ != stamp.arena_id)
            return false;
        return gen_ == stamp.arena_gen;
    }

    void restamp(std::uint64_t new_gen = 0, std::uint64_t new_arena_id = 0) noexcept {
        if (!ptr_)
            return;
        if (new_gen != 0)
            gen_ = new_gen;
        if (new_arena_id != 0)
            arena_id_ = new_arena_id;
        ++g_lifetime_pin_stats().restamps;
    }

    void unpin_on_compact() noexcept {
        if (!ptr_)
            return;
        ptr_ = nullptr;
        gen_ = 0;
        arena_id_ = 0;
        ffi_handoff_ = false;
        ++g_lifetime_pin_stats().invalidations;
    }

    void mark_ffi_handoff() noexcept {
        ffi_handoff_ = true;
        ++g_lifetime_pin_stats().ffi_handoffs;
    }
    [[nodiscard]] bool ffi_handoff() const noexcept { return ffi_handoff_; }

    // Issue #2270: ownership transfer / release methods (also defined
    // in lifetime_pin.ixx — mirrored here so non-module TU callers like
    // render_primitives.cpp can use them via this header).
    void mark_ffi_owned() noexcept;
    void release_ffi() noexcept;

private:
    void* ptr_ = nullptr;
    std::uint64_t gen_ = 0;
    std::uint64_t arena_id_ = 0;
    bool ffi_handoff_ = false;
};

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

inline std::size_t live_pin_count() noexcept {
    std::lock_guard<std::mutex> lock(pin_registry_mtx());
    auto& reg = pin_registry();
    std::size_t n = 0;
    for (auto* p : reg)
        if (p && p->pinned())
            ++n;
    return n;
}

// Issue #2280: epoch-scoped linear pin contract (header form for
// non-module TUs). Live linear objects (linear_rt::Owned|Borrowed|
// MutBorrowed) must be registered as pin roots at LinearWrap
// materialization time and unregistered at Move consume / Drop time.
// verify_linear_pins_under_moving_compact checks every live linear
// root against old_addresses and bumps linear_pin_miss_total on miss.
//
// Mirrors the inline definitions in lifetime_pin.ixx (module form).
// The `inline` keyword on the variables + functions allows multiple
// definitions across translation units (header consumers + module
// consumers) without ODR violations.
inline std::atomic<std::uint64_t> g_linear_pin_total{0};
inline std::atomic<std::uint64_t> g_linear_unpin_total{0};
inline std::atomic<std::uint64_t> g_linear_pin_miss_total{0};

inline std::unordered_set<void*>& linear_roots() {
    static std::unordered_set<void*> s;
    return s;
}
inline std::mutex& linear_roots_mtx() {
    static std::mutex m;
    return m;
}

struct LinearRootSnapshot {
    std::uint64_t pin_total = 0;
    std::uint64_t unpin_total = 0;
    std::uint64_t pin_miss_total = 0;
    std::size_t live_count = 0;
};

inline void pin_linear_root(void* obj) noexcept {
    if (!obj)
        return;
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    linear_roots().insert(obj);
    g_linear_pin_total.fetch_add(1, std::memory_order_relaxed);
}

inline void unpin_linear_root(void* obj) noexcept {
    if (!obj)
        return;
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    linear_roots().erase(obj);
    g_linear_unpin_total.fetch_add(1, std::memory_order_relaxed);
}

inline LinearRootSnapshot linear_root_snapshot() noexcept {
    LinearRootSnapshot s;
    s.pin_total = g_linear_pin_total.load(std::memory_order_relaxed);
    s.unpin_total = g_linear_unpin_total.load(std::memory_order_relaxed);
    s.pin_miss_total = g_linear_pin_miss_total.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    s.live_count = linear_roots().size();
    return s;
}

inline void reset_linear_roots_for_test() noexcept {
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    linear_roots().clear();
    g_linear_pin_total.store(0, std::memory_order_relaxed);
    g_linear_unpin_total.store(0, std::memory_order_relaxed);
    g_linear_pin_miss_total.store(0, std::memory_order_relaxed);
}

inline bool
verify_linear_pins_under_moving_compact(const std::unordered_set<void*>& old_addresses) noexcept {
    std::lock_guard<std::mutex> lock(linear_roots_mtx());
    auto& roots = linear_roots();
    if (roots.empty())
        return true; // AC3: no linear pins → no extra atomics
    for (auto* root : roots) {
        if (old_addresses.count(root) > 0) {
            g_linear_pin_miss_total.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}

// Issue #2280: wrapper — checks both arena pins AND linear roots under
// Moving compact. Returns false if either check fails. Mirrors the
// inline definition in lifetime_pin.ixx (module form).
inline bool
verify_pins_under_moving_compact(std::uint64_t arena_id,
                                 const std::unordered_set<void*>& old_addresses) noexcept {
    // Arena check first.
    {
        std::lock_guard<std::mutex> lock(pin_registry_mtx());
        auto& reg = pin_registry();
        for (auto* p : reg) {
            if (!p || !p->pinned())
                continue;
            if (arena_id != 0 && p->arena_id() != arena_id)
                continue;
            if (old_addresses.count(p->ptr()) > 0) {
                return false; // arena pin miss
            }
        }
    }
    // Linear check second (separate mutex; no deadlock risk).
    return verify_linear_pins_under_moving_compact(old_addresses);
}

} // namespace aura::core::lifetime

#endif // AURA_CORE_LIFETIME_PIN_HH
