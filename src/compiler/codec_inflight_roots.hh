// Issue #5294: GC roots for a payload codec's in-flight values.
//
// The channel codec rebuilds a hash / list / vector from the sender's byte
// string in a C++ frame: (hash) mints a table, each 'S' mints a string,
// each 'L'/'V' cons mints a pair. Between that mint and the moment the value
// is attached to its parent, the only reference is a local EvalValue in the
// decode lambda. The reclaims walk the VM's roots, so a mark proved the new
// table unreachable, freed it, and the caller's hash-ref / hash-keys then
// saw an empty or reused table. The same window corrupts a list under
// construction (car: not a pair, endless nesting).
//
// The registry is process-global, not thread-local: the channel handoff runs
// the sender and the receiver on different scheduler bodies, and the mark
// that frees the receiver's table can run on the sender's thread (fiber
// body exit) while the receiver is still filling it. A thread-local stack
// therefore never saw the root that mattered. Each decode registers one
// frame; every value it creates is added to that frame's list until the
// top-level decode returns. Both reclaims consult collect().
//
// This is the same provider shape as fiber_result_roots.hh (#4710) and
// ir_inflight_roots.hh (#4617): a module import cannot express the
// dependency, so a header carried by both sides states it.
#ifndef AURA_COMPILER_CODEC_INFLIGHT_ROOTS_HH_
#define AURA_COMPILER_CODEC_INFLIGHT_ROOTS_HH_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace aura::compiler::codec_inflight_roots {

namespace detail {

    // Function-local statics keep the registry alive across translation-unit
    // initialization order (the reclaims can run before any TU's dynamic init).
    // Recursive: a CreationGuard holds it across a mint and the push that
    // publishes the minted value, and push()/publish_result() re-enter it.
    inline std::recursive_mutex& registry_mutex() {
        static std::recursive_mutex mutex;
        return mutex;
    }

    inline std::map<std::uint64_t, std::vector<std::int64_t>>& frames() {
        static std::map<std::uint64_t, std::vector<std::int64_t>> frame_map;
        return frame_map;
    }

    inline std::atomic<std::uint64_t>& next_frame_id() {
        static std::atomic<std::uint64_t> counter{1};
        return counter;
    }

    inline std::atomic<std::uint32_t>& active_count() {
        static std::atomic<std::uint32_t> counter{0};
        return counter;
    }

    // The value a decode just handed back to its primitive. The decode frame is
    // destroyed on return, but the primitive's caller stores the value one frame
    // up; a sweep that starts in that gap (a fiber body exiting on another
    // thread) saw neither root. Keeping the last returned value here covers the
    // gap; the next channel call replaces it, so retention stays bounded.
    inline std::vector<std::int64_t>& pending_results() {
        static std::vector<std::int64_t> pending;
        return pending;
    }

    // Issue #5294: hash slots the last published payload still names. A release
    // must not free a table the caller can still reach through a payload it just
    // received, even when the mark's pair walk did not reach it. Atomics only:
    // the release runs under the hash lock and must not take the registry mutex.
    inline constexpr std::size_t kProtectedSlots = std::size_t{1} << 20U;

    inline std::array<std::atomic<std::uint8_t>, kProtectedSlots>& protected_slots() {
        static std::array<std::atomic<std::uint8_t>, kProtectedSlots> flags{};
        return flags;
    }

    inline std::map<std::uint64_t, std::vector<std::uint64_t>>& frame_hash_slots() {
        static std::map<std::uint64_t, std::vector<std::uint64_t>> slots;
        return slots;
    }

    inline std::vector<std::uint64_t>& pending_hash_slots() {
        static std::vector<std::uint64_t> slots;
        return slots;
    }

} // namespace detail

// Issue #5294: held across the mint of one value and the push that
// publishes it. A concurrent mark's collect() blocks on the same mutex, so
// it cannot snapshot a slot whose value has not reached the registry yet -
// the window that freed a receiver's table between (hash) and the push.
struct CreationGuard {
    CreationGuard()
        : lock_(detail::registry_mutex()) {}
    ~CreationGuard() = default;
    CreationGuard(const CreationGuard&) = delete;
    CreationGuard& operator=(const CreationGuard&) = delete;

private:
    std::unique_lock<std::recursive_mutex> lock_;
};

// Held by a reclaim across collect() and the pair/string size sample.
// Codec mints take this mutex before they allocate (CreationGuard), so a
// slot below the sampled size is already in the collected roots, and a slot
// allocated after the sample is past that size. Dropping the mutex between
// the two lets a decode publish a cons the collect missed and the wipe then
// clears: the outer list arrives as (1 ()), or the key string is released
// and hash-ref misses while hash-length stays 1.
struct RegistryLock {
    RegistryLock()
        : lock_(detail::registry_mutex()) {}
    ~RegistryLock() = default;
    RegistryLock(const RegistryLock&) = delete;
    RegistryLock& operator=(const RegistryLock&) = delete;

private:
    std::unique_lock<std::recursive_mutex> lock_;
};

// One in-flight top-level decode. Nested decodes reuse the frame: a child
// value must stay rooted while its parent attaches it.
class ScopedFrame {
public:
    ScopedFrame() {
        detail::active_count().fetch_add(1, std::memory_order_acq_rel);
        const std::scoped_lock<std::recursive_mutex> registry_lock(detail::registry_mutex());
        detail::frames()[frame_id];
    }
    ~ScopedFrame() {
        const std::scoped_lock<std::recursive_mutex> registry_lock(detail::registry_mutex());
        detail::frames().erase(frame_id);
        detail::frame_hash_slots().erase(frame_id);
        detail::active_count().fetch_sub(1, std::memory_order_acq_rel);
    }
    ScopedFrame(const ScopedFrame&) = delete;
    ScopedFrame& operator=(const ScopedFrame&) = delete;
    ScopedFrame(ScopedFrame&&) = delete;
    ScopedFrame& operator=(ScopedFrame&&) = delete;

    [[nodiscard]] std::uint64_t id() const { return frame_id; }

private:
    const std::uint64_t frame_id = detail::next_frame_id().fetch_add(1, std::memory_order_relaxed);
};

// Raw EvalValue bit patterns (#907: one tagged int64); the evaluator wraps
// them back into types::EvalValue, so the messaging partition and the
// reclaim need no shared value type here.
inline void push(std::uint64_t frame_id, std::int64_t raw) {
    const std::scoped_lock<std::recursive_mutex> registry_lock(detail::registry_mutex());
    detail::frames()[frame_id].push_back(raw);
}

inline void push_hash_slot(std::uint64_t frame_id, std::uint64_t hidx) {
    const std::scoped_lock<std::recursive_mutex> registry_lock(detail::registry_mutex());
    detail::frame_hash_slots()[frame_id].push_back(hidx);
}

[[nodiscard]] inline bool is_hash_slot_protected(std::uint64_t hidx) {
    if (hidx >= detail::kProtectedSlots)
        return false;
    return detail::protected_slots().at(hidx).load(std::memory_order_acquire) != 0;
}

// True while any decode is in flight. The automatic reclaims defer to the
// next allocation when a decode is building a value: the codec's own mints
// charge the reclaim window, and a mark that starts between a mint and the
// moment the value reaches this registry can only free what the program is
// still filling.
inline bool active() {
    return detail::active_count().load(std::memory_order_acquire) > 0;
}

inline void publish_result(std::int64_t raw) {
    const std::scoped_lock<std::recursive_mutex> registry_lock(detail::registry_mutex());
    detail::pending_results().clear();
    detail::pending_results().push_back(raw);
}

// Issue #5294: keep the WHOLE payload the decode just built rooted until the
// next channel call. The root value alone leaves inner hashes reachable only
// through pair slots, and a walk that misses them there hands the caller an
// empty table inside an otherwise intact list. The datum recv path must not
// follow this with publish_result: that keeps only the root word and drops
// the inner conses, strings and hashes just published.
inline void publish_frame(std::uint64_t frame_id) {
    const std::scoped_lock<std::recursive_mutex> registry_lock(detail::registry_mutex());
    const auto frame_it = detail::frames().find(frame_id);
    detail::pending_results().clear();
    if (frame_it != detail::frames().end())
        detail::pending_results() = frame_it->second;
    // Hand the slot protection over to this payload: the previous payload's
    // tables stop being protected only now, so a value the caller received
    // one call ago stays alive through this call too.
    for (const auto old : detail::pending_hash_slots()) {
        if (old < detail::kProtectedSlots)
            detail::protected_slots().at(old).store(0, std::memory_order_release);
    }
    detail::pending_hash_slots().clear();
    const auto slot_it = detail::frame_hash_slots().find(frame_id);
    if (slot_it != detail::frame_hash_slots().end()) {
        for (const auto hidx : slot_it->second) {
            if (hidx < detail::kProtectedSlots)
                detail::protected_slots().at(hidx).store(1, std::memory_order_release);
            detail::pending_hash_slots().push_back(hidx);
        }
    }
}

// Appends every in-flight codec value to out. Returns how many were added
// (0 = no decode is running anywhere in the process). Re-locks the registry,
// so a caller already holding RegistryLock can sample heap sizes before the
// lock drops.
inline std::size_t collect(std::vector<std::int64_t>& out) {
    const std::scoped_lock<std::recursive_mutex> registry_lock(detail::registry_mutex());
    std::size_t added = 0;
    if (!detail::pending_results().empty()) {
        out.insert(out.end(), detail::pending_results().begin(), detail::pending_results().end());
        added += detail::pending_results().size();
    }
    for (const auto& [frame_key, values] : detail::frames()) {
        (void)frame_key;
        out.insert(out.end(), values.begin(), values.end());
        added += values.size();
    }
    return added;
}

} // namespace aura::compiler::codec_inflight_roots

#endif // AURA_COMPILER_CODEC_INFLIGHT_ROOTS_HH_
