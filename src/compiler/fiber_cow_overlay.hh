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
// Issue #4771: worldline (experiment-branch) primitives on top of the same
// log:
//   - current_seq() + begin_at(): a worldline forks from a HISTORICAL
//     snapshot. Record current_seq() when the snapshot is taken and open the
//     overlay later with begin_at(..., base_seq); every commit that landed
//     after the snapshot (even before the overlay opened) then conflicts.
//   - prepare() / finalize() / cancel(): two-phase commit. prepare() runs the
//     full conflict scan and, on success, RESERVES the write+read names so no
//     sibling can claim them; nothing is applied and no claim is logged. The
//     caller persists its WAL record, then finalize() applies + logs (it can
//     no longer conflict) or cancel() releases the reservation (WAL write
//     failed) — no "ghost commit" is left in the log. A sibling commit /
//     prepare that touches a reserved name gets `busy` (retryable, overlay
//     kept) instead of a conflict.
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
    // Issue #4771: two-phase commit counters (additive; a finalize also
    // counts in `commits`, a cancel also counts in `rollbacks`).
    std::uint64_t prepares = 0;
    std::uint64_t cancels = 0;
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
    // Issue #4771: another fiber's prepared (two-phase) ticket reserves
    // `conflict`. Retryable: the overlay is NOT dropped and nothing is
    // counted as a conflict; retry after that ticket finalizes / cancels.
    bool busy = false;
};

// Issue #4771: result of prepare(). On success (`ok`) the overlay's write and
// read names are reserved until finalize() / cancel() with this ticket.
// On a conflict the overlay was dropped exactly like a failed commit(); on
// `result.busy` it is kept and prepare() may be retried.
struct PrepareTicket {
    const void* owner = nullptr;
    std::uint64_t fiber = 0;
    std::uint64_t id = 0; // 0 = no ticket issued
    std::uint64_t current_epoch = 0;
    bool ok = false;
    CommitResult result;
    [[nodiscard]] bool valid() const noexcept { return ok && id != 0; }
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
    // Issue #4771: non-zero while a prepare() ticket holds this overlay's
    // reservations; the overlay is frozen (writes refused) until finalize /
    // cancel / rollback.
    std::uint64_t ticket = 0;
    std::vector<std::string> reserved;
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
        return begin_locked(st, fiber, base_epoch, st.seq);
    }

    // Issue #4771: open the overlay against a HISTORICAL commit-log base.
    // `base_seq` is a value previously returned by current_seq() (taken
    // together with the worldline's snapshot); commits recorded after it —
    // including ones that landed before this call — conflict at commit /
    // prepare. A base_seq ahead of the log is clamped to the current seq (a
    // base cannot be in the future). Returns the base epoch like begin().
    std::uint64_t begin_at(const void* owner, std::uint64_t fiber, std::uint64_t base_epoch,
                           std::uint64_t base_seq) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& st = owners_[owner];
        return begin_locked(st, fiber, base_epoch, std::min(base_seq, st.seq));
    }

    // Issue #4771: the commit-log sequence right now. Record it with a
    // snapshot so a later begin_at() can use the snapshot as its base.
    std::uint64_t current_seq(const void* owner) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        return oit == owners_.end() ? 0 : oit->second.seq;
    }

    // Issue #4771: the overlay's commit-log base (0 when none is open).
    std::uint64_t base_seq(const void* owner, std::uint64_t fiber) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return 0;
        auto fit = oit->second.overlays.find(fiber);
        if (fit == oit->second.overlays.end() || !fit->second.active)
            return 0;
        return fit->second.base_seq;
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
        if (ov.ticket != 0)
            return false; // #4771: frozen by a prepare() ticket
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
        if (ov.ticket != 0)
            return false; // #4771: frozen by a prepare() ticket
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
    // Issue #4771: commit() == prepare() + finalize() in one critical
    // section. A name reserved by another fiber's prepare() ticket makes it
    // return `busy` (overlay kept, retryable) instead of a conflict.
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
        if (fit == st.overlays.end() || !fit->second.active || fit->second.ticket != 0)
            return res;
        FiberOverlay<V>& ov = fit->second;
        const auto names = sorted_writes(ov);
        res.attempted_names = names;
        if (busy_locked(st, fiber, ov, names, res))
            return res;
        if (scan_conflict_locked(st, fiber, fit, names, current_epoch, res, notify))
            return res;
        apply_locked(st, fiber, fit, names, current_epoch, res, apply, notify);
        return res;
    }

    // Issue #4771: phase 1 of a two-phase commit. Runs the same conflict
    // scan as commit(); on success reserves every written and read name for
    // this fiber and returns a ticket — nothing is applied, no claim is
    // logged, the shared env is untouched. The caller then persists (WAL +
    // fsync) and calls finalize() or cancel(). A conflict drops the overlay
    // (stats / last_conflict / NotifyFn exactly as commit()); a reservation
    // held by another ticket returns result.busy with the overlay kept.
    template <class NotifyFn = std::nullptr_t>
    PrepareTicket prepare(const void* owner, std::uint64_t fiber, std::uint64_t current_epoch,
                          NotifyFn&& notify = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        PrepareTicket t;
        t.owner = owner;
        t.fiber = fiber;
        t.current_epoch = current_epoch;
        auto oit = owners_.find(owner);
        if (oit == owners_.end())
            return t;
        auto& st = oit->second;
        auto fit = st.overlays.find(fiber);
        if (fit == st.overlays.end() || !fit->second.active || fit->second.ticket != 0)
            return t;
        FiberOverlay<V>& ov = fit->second;
        const auto names = sorted_writes(ov);
        t.result.attempted_names = names;
        if (busy_locked(st, fiber, ov, names, t.result))
            return t;
        if (scan_conflict_locked(st, fiber, fit, names, current_epoch, t.result, notify))
            return t;
        ov.ticket = ++st.next_ticket;
        ov.reserved.clear();
        auto reserve = [&](const std::string& n) {
            auto [it, fresh] = st.reserved.emplace(n, fiber);
            if (fresh)
                ov.reserved.push_back(n);
        };
        for (const auto& n : names)
            reserve(n);
        for (const auto& kv : ov.reads)
            reserve(kv.first);
        t.id = ov.ticket;
        t.ok = true;
        t.result.fast_forward = (ov.base_epoch == current_epoch);
        ++st.stats.prepares;
        return t;
    }

    // Issue #4771: phase 2 — publish a prepared ticket. Cannot conflict: the
    // reservations kept every sibling off these names since prepare().
    // Applies in sorted order, logs the claims, releases the reservations.
    // A stale / unknown ticket returns ok=false and applies nothing.
    template <class ApplyFn, class NotifyFn = std::nullptr_t>
    CommitResult finalize(const PrepareTicket& ticket, ApplyFn&& apply,
                          NotifyFn&& notify = nullptr) {
        std::lock_guard<std::mutex> lk(mu_);
        CommitResult res;
        auto fit = ticket_overlay_locked(ticket);
        if (!fit)
            return res;
        auto& st = owners_.find(ticket.owner)->second;
        auto it = *fit;
        const auto names = sorted_writes(it->second);
        res.attempted_names = names;
        release_reservations_locked(st, it->second);
        apply_locked(st, ticket.fiber, it, names, ticket.current_epoch, res, apply, notify);
        return res;
    }

    // Issue #4771: abandon a prepared ticket (e.g. the WAL write failed).
    // Releases the reservations and drops the overlay; nothing is applied
    // and no claim is logged, so later overlays never see a ghost commit.
    // Counted as a rollback. Returns false for a stale / unknown ticket.
    bool cancel(const PrepareTicket& ticket) {
        std::lock_guard<std::mutex> lk(mu_);
        auto fit = ticket_overlay_locked(ticket);
        if (!fit)
            return false;
        auto& st = owners_.find(ticket.owner)->second;
        release_reservations_locked(st, (*fit)->second);
        st.overlays.erase(*fit);
        ++st.stats.rollbacks;
        ++st.stats.cancels;
        return true;
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
        release_reservations_locked(oit->second, fit->second); // #4771
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
    struct OwnerState;
    using OverlayMap = std::unordered_map<std::uint64_t, FiberOverlay<V>>;
    using OverlayIt = typename OverlayMap::iterator;

    std::uint64_t begin_locked(OwnerState& st, std::uint64_t fiber, std::uint64_t base_epoch,
                               std::uint64_t base_seq) {
        auto& ov = st.overlays[fiber];
        // #4771: re-opening over a prepared overlay releases its ticket.
        release_reservations_locked(st, ov);
        ov.fiber = fiber;
        ov.base_epoch = base_epoch;
        ov.base_seq = base_seq;
        ov.writes.clear();
        ov.reads.clear();
        ov.cells.clear();
        ov.active = true;
        st.last_conflict.erase(fiber);
        ++st.stats.begins;
        return ov.base_epoch;
    }

    static std::vector<std::string> sorted_writes(const FiberOverlay<V>& ov) {
        std::vector<std::string> names;
        names.reserve(ov.writes.size());
        for (const auto& kv : ov.writes)
            names.push_back(kv.first);
        std::sort(names.begin(), names.end());
        return names;
    }

    static void release_reservations_locked(OwnerState& st, FiberOverlay<V>& ov) {
        for (const auto& n : ov.reserved) {
            auto it = st.reserved.find(n);
            if (it != st.reserved.end() && it->second == ov.fiber)
                st.reserved.erase(it);
        }
        ov.reserved.clear();
        ov.ticket = 0;
    }

    // #4771: a name (written or read) reserved by ANOTHER fiber's ticket.
    static bool busy_locked(const OwnerState& st, std::uint64_t fiber, const FiberOverlay<V>& ov,
                            const std::vector<std::string>& names, CommitResult& res) {
        if (st.reserved.empty())
            return false;
        auto held_by_other = [&](const std::string& n) {
            auto it = st.reserved.find(n);
            return it != st.reserved.end() && it->second != fiber;
        };
        for (const auto& n : names) {
            if (held_by_other(n)) {
                res.busy = true;
                res.conflict = n;
                return true;
            }
        }
        std::vector<std::string> rnames;
        for (const auto& kv : ov.reads)
            rnames.push_back(kv.first);
        std::sort(rnames.begin(), rnames.end());
        for (const auto& n : rnames) {
            if (held_by_other(n)) {
                res.busy = true;
                res.conflict = n;
                return true;
            }
        }
        return false;
    }

    std::optional<OverlayIt> ticket_overlay_locked(const PrepareTicket& ticket) {
        if (!ticket.valid())
            return std::nullopt;
        auto oit = owners_.find(ticket.owner);
        if (oit == owners_.end())
            return std::nullopt;
        auto fit = oit->second.overlays.find(ticket.fiber);
        if (fit == oit->second.overlays.end() || !fit->second.active ||
            fit->second.ticket != ticket.id)
            return std::nullopt;
        return fit;
    }

    // Conflict scan shared by commit() / prepare(). Returns true (and drops
    // the overlay, records the condition, runs NotifyFn) on a conflict.
    template <class NotifyFn>
    static bool scan_conflict_locked(OwnerState& st, std::uint64_t fiber, OverlayIt fit,
                                     const std::vector<std::string>& names,
                                     std::uint64_t current_epoch, CommitResult& res,
                                     NotifyFn& notify) {
        FiberOverlay<V>& ov = fit->second;
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
        if (res.conflict.empty())
            return false;
        // Rollback by policy: the loser applies nothing.
        ++st.stats.conflicts;
        if (res.conflict_info.read_only)
            ++st.stats.read_conflicts;
        st.last_conflict[fiber] = res.conflict_info;
        release_reservations_locked(st, ov);
        st.overlays.erase(fit);
        if constexpr (!std::is_same_v<std::decay_t<NotifyFn>, std::nullptr_t>)
            notify(res);
        return true;
    }

    // Apply + log shared by commit() / finalize(). ApplyFn is invoked with
    // the registry lock held, in sorted binding order.
    template <class ApplyFn, class NotifyFn>
    static void apply_locked(OwnerState& st, std::uint64_t fiber, OverlayIt fit,
                             const std::vector<std::string>& names, std::uint64_t current_epoch,
                             CommitResult& res, ApplyFn& apply, NotifyFn& notify) {
        FiberOverlay<V>& ov = fit->second;
        res.fast_forward = (ov.base_epoch == current_epoch);
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
    }

    struct OwnerState {
        std::unordered_map<std::uint64_t, FiberOverlay<V>> overlays;
        // Committed claims by binding name (write-write conflict SSOT).
        std::unordered_map<std::string, CommitRecord> log;
        // Issue #4715 follow-up: last structured conflict per fiber, so the
        // loser can read the condition object after its overlay was dropped.
        std::unordered_map<std::uint64_t, ConflictInfo> last_conflict;
        std::uint64_t seq = 0;
        OverlayStats stats;
        // Issue #4771: names held by a prepared ticket (name -> fiber).
        std::unordered_map<std::string, std::uint64_t> reserved;
        std::uint64_t next_ticket = 0;
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
