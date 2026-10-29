// fiber_cow_overlay.hh — Issue #4715: per-fiber copy-on-write overlay for
// env bindings.
//
// Problem: fiber bodies mutate the shared env (env_frames_ SoA / Env
// top-binding table) directly, so two concurrent bodies that write the same
// global race instead of producing a deterministic write-write verdict, and a
// failed body leaves half-applied bindings behind.
//
// This header owns the data layer only: a per-fiber overlay (a private
// name -> value map keyed by the fiber that opened it), a process-level
// commit log used for write-write conflict detection, and the commit
// protocol:
//
//   1. begin() snapshots the base epoch (workspace COW mutation epoch) and
//      the commit-log sequence. Both are read under the registry lock so a
//      concurrent commit cannot slip between the two reads.
//   2. define() / set() append to the private overlay — the shared env is
//      untouched while the body runs.
//   3. commit() takes the registry lock and:
//        - scans every pending binding in SORTED name order (deterministic)
//          for a log record committed after this overlay began — a sibling
//          fiber wrote the same binding => write-write CONFLICT, no partial
//          commit, the overlay is dropped (rollback by policy);
//        - otherwise applies every binding in sorted order and records the
//          claim. base_epoch == current_epoch is the fast-forward path
//          (cheap, no per-binding epoch compare); a moved epoch is the
//          per-binding merged path.
//   4. rollback() drops the overlay without touching the shared env.
//
// Determinism: conflict reporting is by sorted binding name, and the
// commit-log sequence (not wall-clock / thread order) decides which writer
// wins: the first commit of a base claims the binding, every later sibling
// commit from the same base sees commit_seq > base_seq and conflicts.
//
// The evaluator-side identity of "which fiber" is supplied by the caller
// (Evaluator::fiber_cow_overlay_key()); this header never touches the
// evaluator, so it is reusable by the workspace-node overlay that follows
// the env slice (issues #4715 follow-up).
#ifndef AURA_COMPILER_FIBER_COW_OVERLAY_HH
#define AURA_COMPILER_FIBER_COW_OVERLAY_HH

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aura::compiler::cow {

// One committed binding claim. `commit_seq` is the process-global commit
// counter at the moment the claim was recorded; a later overlay that began
// at `base_seq < commit_seq` for the same name must report a conflict.
struct CommitRecord {
    std::uint64_t commit_seq = 0;
    std::uint64_t base_epoch = 0;
    std::uint64_t fiber = 0;
    // Issue #4715 follow-up: the workspace mutation epoch observed when the
    // winner committed. Paired with base_epoch it is the "committed epoch"
    // half of the structured conflict condition.
    std::uint64_t epoch = 0;
};

// Issue #4715 follow-up: structured conflict condition. The primitive face
// renders this both as a trailing key=value suffix on the historical
// "fiber:cow-conflict: <name>" string (so existing callers/tests keep
// working) and as a hash via (fiber:overlay-conflict).
//
// read_only distinguishes the two validation arms:
//   false — write-write: another fiber committed the SAME binding after we
//           began (the #4715 AC1 first-committer-wins rule);
//   true  — read-set: we read a binding another fiber committed after we
//           began (write skew: our result was computed from a stale value).
struct ConflictInfo {
    std::string binding;
    std::uint64_t winner_fiber = 0;
    std::uint64_t winner_seq = 0;
    std::uint64_t winner_epoch = 0; // committed epoch
    std::uint64_t base_epoch = 0;   // our epoch
    std::uint64_t base_seq = 0;     // our commit-log base
    bool read_only = false;
    [[nodiscard]] bool valid() const noexcept { return !binding.empty(); }
};

// Aggregate observability for the (fiber:overlay-report) primitive.
struct OverlayStats {
    std::uint64_t begins = 0;
    std::uint64_t commits = 0;
    std::uint64_t fast_forward = 0;
    std::uint64_t merged = 0;
    std::uint64_t conflicts = 0;
    // Issue #4715 follow-up: the stale-read (write-skew) subset of
    // `conflicts`; the additive report field keeps the pre-follow-up
    // begins=/commits=/conflicts=/rollbacks= shape intact.
    std::uint64_t read_conflicts = 0;
    std::uint64_t rollbacks = 0;
    std::uint64_t writes = 0;
    std::uint64_t active = 0;
};

// Outcome of one commit. `conflict` names the first (sorted) conflicting
// binding; empty on success. A conflict never applies a partial write set.
struct CommitResult {
    bool ok = false;
    bool fast_forward = false;
    std::string conflict;
    // Issue #4715 follow-up: the structured condition behind `conflict`
    // (empty binding on success) and the sorted names actually applied.
    ConflictInfo conflict_info;
    std::vector<std::string> applied_names;
    // Every name this overlay held when commit was attempted (sorted); on a
    // rejection applied_names is empty and this is what the audit record
    // names as the dropped attempt.
    std::vector<std::string> attempted_names;
    std::size_t applied = 0;
};

// Per-fiber overlay state. Values are opaque to this header (V), so the
// evaluator instantiates it with EvalValue while a future workspace-node
// overlay can instantiate it with NodeId refs.
template <typename V> struct FiberOverlay {
    std::uint64_t fiber = 0;
    std::uint64_t base_epoch = 0;
    std::uint64_t base_seq = 0;
    bool active = false;
    std::unordered_map<std::string, V> writes;
    // Issue #4715 follow-up: shared-env bindings this body READ while the
    // overlay was open (name -> commit-log seq at first read). Commit
    // validates them against the log so a value computed from a stale read
    // cannot be committed (write skew). Names this overlay also wrote are
    // skipped — the write-write arm already owns them.
    std::unordered_map<std::string, std::uint64_t> reads;
    // Issue #4715 AC2: bindings the evaluator already materialized as a
    // PRIVATE cell (name -> cell id). The define / set! interception points
    // allocate one so the commit publishes a single cell index: a reader
    // resolves either the old cell or the new one, never a half-written
    // payload (the "old-or-new, never torn" property). Names written through
    // the EDSL (fiber:overlay-define / set!) carry no cell here; their commit
    // materializes one.
    std::unordered_map<std::string, std::uint64_t> cells;
};

// Process-level registry of open overlays + committed claims. One instance
// per process; state is partitioned by `owner` (the Evaluator pointer) so
// independent evaluators in one test process never share conflicts, and
// `fiber` distinguishes concurrent bodies of the same owner.
template <typename V> class OverlayRegistry {
public:
    // Snapshot the overlay base. Opening a second overlay for the same
    // owner+fiber replaces the stale one (a body that was cancelled or
    // unwound without committing cannot leak its writes into the next).
    std::uint64_t begin(const void* owner, std::uint64_t fiber, std::uint64_t base_epoch) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& st = owners_[owner];
        auto& ov = st.overlays[fiber];
        ov.fiber = fiber;
        ov.base_epoch = base_epoch;
        ov.base_seq = st.seq;
        ov.writes.clear();
        ov.reads.clear();
        ov.cells.clear();
        ov.active = true;
        st.last_conflict.erase(fiber);
        ++st.stats.begins;
        return ov.base_epoch;
    }

    // Private write. Auto-begins when the body never called begin().
    bool write(const void* owner, std::uint64_t fiber, std::string_view name, const V& value,
               std::uint64_t base_epoch) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& st = owners_[owner];
        auto& ov = st.overlays[fiber];
        if (!ov.active) {
            ov.fiber = fiber;
            ov.base_epoch = base_epoch;
            ov.base_seq = st.seq;
            ov.reads.clear();
            ov.active = true;
            ++st.stats.begins;
        }
        std::string key(name);
        ov.writes[key] = value;
        // #4715 AC2: an EDSL write replaces a captured cell — the commit
        // materializes a fresh cell from the value instead of publishing a
        // private cell whose payload is now stale.
        ov.cells.erase(key);
        ++st.stats.writes;
        return true;
    }

    // #4715 AC2: private write that is already materialized as a cell (the
    // evaluator define / set! interception). Same conflict contract as
    // write(); the commit hands the cell to the apply callback so the shared
    // env switches from the old cell index to this one in one step.
    bool write_cell(const void* owner, std::uint64_t fiber, std::string_view name, const V& value,
                    std::uint64_t cell, std::uint64_t base_epoch) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& st = owners_[owner];
        auto& ov = st.overlays[fiber];
        if (!ov.active) {
            ov.fiber = fiber;
            ov.base_epoch = base_epoch;
            ov.base_seq = st.seq;
            ov.reads.clear();
            ov.active = true;
            ++st.stats.begins;
        }
        std::string key(name);
        ov.writes[key] = value;
        ov.cells[key] = cell;
        ++st.stats.writes;
        return true;
    }

    // #4715 AC2: the private cell for a name the overlay captured, if any.
    std::optional<std::uint64_t> read_cell(const void* owner, std::uint64_t fiber,
                                           std::string_view name) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return std::nullopt;
        auto fit = oit->second.overlays.find(fiber);
        if (fit == oit->second.overlays.end() || !fit->second.active)
            return std::nullopt;
        auto cit = fit->second.cells.find(std::string(name));
        if (cit == fit->second.cells.end())
            return std::nullopt;
        return cit->second;
    }

    // Overlay-local read. Returns false when the binding is not shadowed,
    // so the caller falls through to the committed global env.
    //
    // Issue #4715 follow-up: a miss IS a shared-env read of `name`, so it is
    // recorded in the read set for stale-read validation at commit. A hit
    // (our own private write) records nothing: the write-write arm owns it.
    bool read(const void* owner, std::uint64_t fiber, std::string_view name, V& out) {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return false;
        auto& st = oit->second;
        auto fit = st.overlays.find(fiber);
        if (fit == st.overlays.end() || !fit->second.active)
            return false;
        std::string key(name);
        auto wit = fit->second.writes.find(key);
        if (wit == fit->second.writes.end()) {
            fit->second.reads.emplace(std::move(key), st.seq);
            return false;
        }
        out = wit->second;
        return true;
    }

    // Side-effect-free twin of read(): overlay membership / value probe that
    // must NOT enter the read set (fiber:overlay-has, capture pre-checks).
    bool peek(const void* owner, std::uint64_t fiber, std::string_view name, V& out) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return false;
        auto fit = oit->second.overlays.find(fiber);
        if (fit == oit->second.overlays.end() || !fit->second.active)
            return false;
        auto wit = fit->second.writes.find(std::string(name));
        if (wit == fit->second.writes.end())
            return false;
        out = wit->second;
        return true;
    }

    // Issue #4715 follow-up: the last structured conflict this fiber saw
    // (survives the overlay drop, cleared by the next begin()).
    std::optional<ConflictInfo> last_conflict(const void* owner, std::uint64_t fiber) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return std::nullopt;
        auto it = oit->second.last_conflict.find(fiber);
        if (it == oit->second.last_conflict.end())
            return std::nullopt;
        return it->second;
    }

    std::size_t pending(const void* owner, std::uint64_t fiber) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return 0;
        auto fit = oit->second.overlays.find(fiber);
        if (fit == oit->second.overlays.end() || !fit->second.active)
            return 0;
        return fit->second.writes.size();
    }

    bool active(const void* owner, std::uint64_t fiber) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return false;
        auto fit = oit->second.overlays.find(fiber);
        return fit != oit->second.overlays.end() && fit->second.active;
    }

    // Commit protocol (see header comment). ApplyFn is invoked with the
    // registry lock held, in sorted binding order, and must copy the value
    // into the shared env atomically w.r.t. other commits.
    //
    // #4715 AC2: ApplyFn may take a third argument (the private cell the
    // overlay captured for that name, 0 when the write came from the EDSL).
    // The two-argument form stays valid — every pre-AC2 caller and every
    // data-layer test row uses it.
    // #4715 follow-up: the optional NotifyFn runs LAST, still under the
    // registry lock, so a provenance record (mutation log) lands in commit
    // order without a second lock in the evaluator. It must not re-enter the
    // registry.
    template <class ApplyFn, class NotifyFn = std::nullptr_t>
    CommitResult commit(const void* owner, std::uint64_t fiber, std::uint64_t current_epoch,
                        ApplyFn&& apply, NotifyFn&& notify = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        CommitResult res;
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return res;
        auto& st = oit->second;
        auto fit = st.overlays.find(fiber);
        if (fit == st.overlays.end() || !fit->second.active)
            return res;
        FiberOverlay<V>& ov = fit->second;
        std::vector<std::string> names;
        names.reserve(ov.writes.size());
        for (const auto& kv : ov.writes)
            names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        res.attempted_names = names;
        // #4715 AC1: conflict scan is a full pass BEFORE any apply, so a
        // conflicting commit cannot leave a partial write set behind.
        // Write-write arm first (the historical rule), then the #4715
        // follow-up read-set arm: a binding we READ that another fiber
        // committed after we began makes everything we computed from that
        // value stale (write skew), so the commit must not land either.
        for (const auto& n : names) {
            auto rit = st.log.find(n);
            if (rit != st.log.end() && rit->second.commit_seq > ov.base_seq) {
                res.conflict = n;
                res.conflict_info = ConflictInfo{n,
                                                 rit->second.fiber,
                                                 rit->second.commit_seq,
                                                 rit->second.epoch,
                                                 ov.base_epoch,
                                                 ov.base_seq,
                                                 /*read_only=*/false};
                break;
            }
        }
        if (res.conflict.empty() && !ov.reads.empty()) {
            // Compare against the sequence observed AT THE READ, not the
            // overlay base: a binding another fiber committed before this
            // body read it was seen correctly, so it is not a stale read.
            std::vector<std::pair<std::string, std::uint64_t>> rnames;
            rnames.reserve(ov.reads.size());
            for (const auto& kv : ov.reads)
                if (ov.writes.find(kv.first) == ov.writes.end())
                    rnames.emplace_back(kv.first, kv.second);
            std::sort(rnames.begin(), rnames.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [n, read_seq] : rnames) {
                auto rit = st.log.find(n);
                if (rit != st.log.end() && rit->second.commit_seq > read_seq) {
                    res.conflict = n;
                    res.conflict_info = ConflictInfo{n,
                                                     rit->second.fiber,
                                                     rit->second.commit_seq,
                                                     rit->second.epoch,
                                                     ov.base_epoch,
                                                     ov.base_seq,
                                                     /*read_only=*/true};
                    break;
                }
            }
        }
        res.fast_forward = (ov.base_epoch == current_epoch);
        if (!res.conflict.empty()) {
            // Rollback by policy: the loser applies nothing.
            ++st.stats.conflicts;
            if (res.conflict_info.read_only)
                ++st.stats.read_conflicts;
            st.last_conflict[fiber] = res.conflict_info;
            st.overlays.erase(fit);
            if constexpr (!std::is_same_v<std::decay_t<NotifyFn>, std::nullptr_t>)
                notify(res);
            return res;
        }
        for (const auto& n : names) {
            if constexpr (std::is_invocable_v<ApplyFn&, const std::string&, const V&,
                                              std::uint64_t>) {
                auto cit = ov.cells.find(n);
                apply(n, ov.writes.at(n), cit == ov.cells.end() ? 0 : cit->second);
            } else {
                apply(n, ov.writes.at(n));
            }
            ++st.seq;
            st.log[n] = CommitRecord{st.seq, ov.base_epoch, fiber, current_epoch};
            ++res.applied;
        }
        res.ok = true;
        res.applied_names = names;
        if (res.fast_forward)
            ++st.stats.fast_forward;
        else
            ++st.stats.merged;
        ++st.stats.commits;
        st.overlays.erase(fit);
        st.last_conflict.erase(fiber);
        if constexpr (!std::is_same_v<std::decay_t<NotifyFn>, std::nullptr_t>)
            notify(res);
        return res;
    }

    // Failure / cancel path: drop the overlay, leave the shared env alone.
    bool rollback(const void* owner, std::uint64_t fiber) {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return false;
        auto fit = oit->second.overlays.find(fiber);
        if (fit == oit->second.overlays.end() || !fit->second.active)
            return false;
        oit->second.overlays.erase(fit);
        ++oit->second.stats.rollbacks;
        return true;
    }

    OverlayStats stats(const void* owner) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return OverlayStats{};
        OverlayStats s = oit->second.stats;
        for (const auto& kv : oit->second.overlays)
            if (kv.second.active)
                ++s.active;
        return s;
    }

    // Test/teardown hook: forget every overlay and claim for one owner.
    void purge(const void* owner) {
        std::lock_guard<std::mutex> lk(mu_);
        owners_.erase(owner);
    }

private:
    struct OwnerState {
        std::unordered_map<std::uint64_t, FiberOverlay<V>> overlays;
        // Committed claims by binding name (write-write conflict SSOT).
        std::unordered_map<std::string, CommitRecord> log;
        // Issue #4715 follow-up: last structured conflict per fiber, so the
        // loser can read the condition object after its overlay was dropped.
        std::unordered_map<std::uint64_t, ConflictInfo> last_conflict;
        std::uint64_t seq = 0;
        OverlayStats stats;
    };
    mutable std::mutex mu_;
    std::unordered_map<const void*, OwnerState> owners_;
};

// Process-wide instance for the env-binding overlay (header-inline so every
// evaluator TU sees exactly one registry in the final link unit).
template <typename V> inline OverlayRegistry<V>& registry() {
    static OverlayRegistry<V> instance;
    return instance;
}

} // namespace aura::compiler::cow

#endif // AURA_COMPILER_FIBER_COW_OVERLAY_HH
