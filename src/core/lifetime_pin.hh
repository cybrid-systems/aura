// lifetime_pin.hh — Issue #2000 Phase 2 / #2048 header form of LifetimePin.
// Keep in sync with lifetime_pin.ixx for module consumers.
// Used by non-module TUs (render_primitives.cpp, etc.).

#ifndef AURA_CORE_LIFETIME_PIN_HH
#define AURA_CORE_LIFETIME_PIN_HH

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace aura::core::lifetime {

inline constexpr int kLifetimePinPhase = 2;
inline constexpr int kLifetimePinIssue = 2048; // joint batch-FFI present contract

struct LifetimePinStats {
    std::uint64_t pins = 0;
    std::uint64_t unpins = 0;
    std::uint64_t ffi_handoffs = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t restamps = 0;
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

} // namespace aura::core::lifetime

#endif // AURA_CORE_LIFETIME_PIN_HH
