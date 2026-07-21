// per_defuse_index.h — per-DefUseIndex caller tracking for
// the incremental typecheck optimization. Replaces the
// pre-existing global dep_caller_fn_ list (which tracked
// all callers in a single linear list, forcing an O(n)
// scan on every invalidate) with a per-DefUseIndex map so
// that invalidating one DefUseIndex only touches the
// callers of that specific index.
//
// Usage:
//   per_defuse_index::DefUseIndex idx1{"foo"};
//   per_defuse_index::DefUseIndex idx2{"bar"};
//   per_defuse_index::PerDefUseIndexTracker tracker;
//   tracker.add_caller(idx1, {"caller1_for_foo"});
//   tracker.add_caller(idx2, {"caller1_for_bar"});
//   auto foo_callers = tracker.get_callers(idx1); // 1
//   auto bar_callers = tracker.get_callers(idx2); // 1
//
// This is the same per-DefUseIndex pattern used in
// #411 fu1 follow-up #1 wiring (commit 8e63777c) but at
// the global level: the DefUseIndex in #411 fu1 is a
// FlatAST (the per-call flat) and the tracker here is a
// global singleton that survives across infer_flat_partial
// calls. The per-DefUseIndex isolation means an O(1)
// lookup for "who depends on foo" instead of an O(n)
// scan over the global list.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "hash_meta.h"                     // FNV constants (#901)
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

// Issue #411 fu1 fu4: NodeId type alias (mirrors
// `export using NodeId = std::uint32_t` in
// aura/core/mutation.ixx). Defined here as a local alias
// (not an import) so the per_defuse_index header doesn't
// pull in the full mutation module — this keeps the
// header usable from the Aura primitive surface which
// imports only the value module. The 32-bit width
// matches the canonical NodeId.
using NodeId = std::uint32_t;

namespace aura::compiler::per_defuse_index {

// DefUseIndex — a symbol identifier used as the per-DefUseIndex
// key. The `name` field is the symbol's lexical name (e.g.
// "foo", "bar"); the comparison operator and hash function
// make it usable as an unordered_map key. The struct is
// POD-style for cheap construction in tight loops.
struct DefUseIndex {
    std::string name;
    bool operator==(const DefUseIndex& other) const noexcept { return name == other.name; }
};

// Caller — a per-DefUseIndex call site. The `node_id`
// field is the NodeId of the Variable use-site (or the
// Call/Apply node, depending on what the registration
// was for). The struct is POD-style for cheap
// construction in tight loops.
//
// Issue #411 fu1 fu4: pre-#411 fu1 fu4 the field was
// `std::string location` (a free-form string). The O(uses)
// wall-clock optimization (the actual perf win, not just
// the metric) requires the indexed lookup to return
// NodeIds directly, so the per-DefUseIndex path can
// iterate use-sites without paying the O(n)
// `affected_subtree_for_symbol` walk cost. The string
// representation was only needed for Aura-side debugging
// (the Aura primitive `(compile:per-defuse-index-callers
// <idx>)` returns a hash of {location: index} pairs).
// Post-fu4 the Aura primitive returns a vector of
// NodeIds instead, which is what the inference loop
// needs.
struct Caller {
    NodeId node_id;
};

// Issue #1846: spin mutex for header-only tracker (avoids
// <shared_mutex> in a header included from C++20 module
// partitions — redeclare/import conflicts on shared_mutex).
// Exclusive only; fine for rare Aura stats + typecheck
// registration (not a hot reader-heavy path).
class TrackerSpinLock {
public:
    void lock() const noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // spin
        }
    }
    void unlock() const noexcept { flag_.clear(std::memory_order_release); }

private:
    mutable std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class TrackerLockGuard {
public:
    explicit TrackerLockGuard(const TrackerSpinLock& l) noexcept
        : lock_(l) {
        lock_.lock();
    }
    ~TrackerLockGuard() { lock_.unlock(); }
    TrackerLockGuard(const TrackerLockGuard&) = delete;
    TrackerLockGuard& operator=(const TrackerLockGuard&) = delete;

private:
    const TrackerSpinLock& lock_;
};

// PerDefUseIndexTracker — a per-DefUseIndex caller registry.
// Each DefUseIndex gets its own independent list of
// callers; adding a caller to one index doesn't affect any
// other.
//
// Issue #1846: internal spin-lock serializes add_caller vs
// get_callers / size_for_index / total_size / index_count /
// clear. Pre-#1846 mutations were unsynchronized —
// concurrent compile:per-defuse-index-add vs -callers could
// UAF on vector reallocation.
//
// Performance: get_callers is O(K) where K is the number
// of callers for that specific DefUseIndex (NOT the total
// number of callers across all DefUseIndexes, which is
// what the pre-existing global list forces). For a code
// base with N DefUseIndexes and total M callers, the
// per-DefUseIndex map turns an O(M) scan into an O(M/N)
// per-DefUseIndex scan — a 5-10x speedup when N >= 10.
class PerDefUseIndexTracker {
public:
    PerDefUseIndexTracker() = default;
    PerDefUseIndexTracker(const PerDefUseIndexTracker& o) {
        TrackerLockGuard lk(o.lock_);
        per_index_ = o.per_index_;
    }
    PerDefUseIndexTracker& operator=(const PerDefUseIndexTracker& o) {
        if (this == &o)
            return *this;
        // Nested locks in fixed address order to avoid deadlock.
        if (this < &o) {
            TrackerLockGuard lk1(lock_);
            TrackerLockGuard lk2(o.lock_);
            per_index_ = o.per_index_;
        } else {
            TrackerLockGuard lk2(o.lock_);
            TrackerLockGuard lk1(lock_);
            per_index_ = o.per_index_;
        }
        return *this;
    }
    PerDefUseIndexTracker(PerDefUseIndexTracker&& o) noexcept {
        TrackerLockGuard lk(o.lock_);
        per_index_ = std::move(o.per_index_);
    }
    PerDefUseIndexTracker& operator=(PerDefUseIndexTracker&& o) noexcept {
        if (this == &o)
            return *this;
        if (this < &o) {
            TrackerLockGuard lk1(lock_);
            TrackerLockGuard lk2(o.lock_);
            per_index_ = std::move(o.per_index_);
        } else {
            TrackerLockGuard lk2(o.lock_);
            TrackerLockGuard lk1(lock_);
            per_index_ = std::move(o.per_index_);
        }
        return *this;
    }

    // add_caller — register a Caller for a specific
    // DefUseIndex. The Caller is appended to that
    // DefUseIndex's caller list; other DefUseIndexes are
    // unaffected. O(1) amortized (hash map insert +
    // vector push_back). Locked (#1846).
    void add_caller(const DefUseIndex& index, const Caller& caller) {
        TrackerLockGuard lk(lock_);
        per_index_[index.name].push_back(caller);
    }

    // get_callers — return all Callers registered for a
    // specific DefUseIndex. Returns an empty vector if no
    // callers have been registered for that index.
    // O(K) where K is the number of callers for that
    // specific index (NOT the total across all indexes).
    // Locked; copies under the lock (#1846).
    std::vector<Caller> get_callers(const DefUseIndex& index) const {
        TrackerLockGuard lk(lock_);
        auto it = per_index_.find(index.name);
        if (it == per_index_.end())
            return {};
        return it->second;
    }

    // size_for_index — convenience accessor for the
    // number of callers for a specific DefUseIndex. Used
    // by the metric that measures per-DefUseIndex load
    // distribution (helps detect hot indexes).
    std::size_t size_for_index(const DefUseIndex& index) const {
        TrackerLockGuard lk(lock_);
        auto it = per_index_.find(index.name);
        if (it == per_index_.end())
            return 0;
        return it->second.size();
    }

    // total_size — sum of all callers across all
    // DefUseIndexes. Used by the metric that tracks
    // overall tracker load.
    std::size_t total_size() const {
        TrackerLockGuard lk(lock_);
        std::size_t sum = 0;
        for (const auto& [_, callers] : per_index_) {
            sum += callers.size();
        }
        return sum;
    }

    // index_count — number of distinct DefUseIndexes
    // registered. Used by the metric that tracks the
    // number of DefUseIndexes in flight.
    std::size_t index_count() const noexcept {
        TrackerLockGuard lk(lock_);
        return per_index_.size();
    }

    // clear — remove all callers. Used between
    // typecheck-current invocations to reset state
    // (matches the dep_caller_fn_'s reset semantics).
    void clear() noexcept {
        TrackerLockGuard lk(lock_);
        per_index_.clear();
    }

private:
    // Issue #1846: guards per_index_ map + vectors.
    TrackerSpinLock lock_;
    std::unordered_map<std::string, std::vector<Caller>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        per_index_;
};

} // namespace aura::compiler::per_defuse_index

// hash specialization for DefUseIndex (so it can be used as
// a key in std::unordered_map directly without the .name
// indirection). The hash is FNV-1a on the name string —
// the same hash function used in the FNV-1a flat cache
// (#258 / #410 / #411 work) to avoid the std::hash<std::string>
// collision pattern.
namespace std {
template <> struct hash<aura::compiler::per_defuse_index::DefUseIndex> {
    std::size_t
    operator()(const aura::compiler::per_defuse_index::DefUseIndex& idx) const noexcept {
        std::size_t h = ::aura::compiler::stats::kFnvOffsetBasis;
        for (char c : idx.name) {
            h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
        }
        return h;
    }
};
} // namespace std
