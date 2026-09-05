// agent_scope.h — Issue #2083 / #2161: scoped multi-agent coordination.
// Issue #2226: promoted from opt-in feature flag to default multi-agent
// supervision root. AgentScope is now always available under aura::orch
// (the class body no longer lives inside a feature-flag ifdef; always on).
// Issue #2537: hierarchical parent/children tree — explicit owner pointers
// (parent_ raw non-owning + children_ unique_ptr vector). No process-global
// registry. cancel_all / ~AgentScope propagate cancel top-down; drain is
// bottom-up (children unique_ptrs destroyed before this scope joins).
//
// STATUS: Default / Documented multi-agent supervision surface.
// MVP linter (scripts/coverage/checks/check_orch_mvp_scope.py --strict) still forbids
// the process-global registry identifiers removed in #1966
// (AgentRegistry / global_agent_registry / conduct_parallel), so the
// "no global registry" contract from #2083 is preserved by the linter,
// not by a build-time gate.
// Issue #2161: scope-level watch_all (batch liveness + optional stall cancel).
//
// Distinct from evaluator-local OrchAgentNameTable (#2078) and
// serve::parallel_orch::parallel_intend (#1587). Issue #3216 identity
// planes (no unified resolve, no process-global table):
//   - name-table (OrchAgentNameTable / agent_names_): per-Evaluator
//     bookkeeping for Aura orch:spawn-agent / orch:agent-join.
//     Issue #3442: message prims resolve name-table first, then
//     AgentScope::find on the same Evaluator — resolve fallback, not
//     a plane merge and not a second owning put.
//   - scope-handle (AgentScope::handles_): supervision authority;
//     orch:scope-resolve live find.
//   - directory (directory_snapshot / orch:agent-directory): read-only
//     projection of the same scope tree. Not a second name table.
//     Issue #3444: Aura orch:scope-child returns that scope_path so
//     spawn / watch / join / resolve can target a child via :path.
//   - HandoffToken / join_via_handoff: observation-only cross-Evaluator
//     lifecycle close (#3148 / #3216). Not a fourth plane; no ownership
//     move; no session-spanning workflow.
//   - parallel_intend: short-lived batch thunks (no long-lived names).
//   - AgentScope: long-lived named agents, parent-cancel + join_all
//     semantics, bound to an explicit owner (Scheduler reference).
//     Hierarchy (#2537) is a tree of scopes, still no static map.
//
// Rules (per Issue #2083 AC4 / #2226 / #2537):
//   - No process-global registry (the orch MVP scope linter still forbids
//     the multi-agent process-static identifiers removed in #1966).
//   - Scope destructor is the supervision root (cancel + best-effort drain
//     + reservation release, mirroring join_agents #2082 contract).
//   - Hierarchy (#2537): parent owns children via unique_ptr; cancel
//     propagates to descendants first, then local handles; dtor drains
//     children then self. Single-owner serial model (#2399) still applies
//     to each scope; child ops are same-thread (or explicitly serialized).
//   - Default-on (no #define required). Documented in src/orch/README.md
//     as the supported multi-agent supervision root.

#ifndef AURA_ORCH_AGENT_SCOPE_H
#define AURA_ORCH_AGENT_SCOPE_H

#include "orch/agent_spawn.h"

#include "compiler/typed_mutation_audit.h" // #2946 production_defaults_active

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aura::orch {

// Issue #2537: hierarchical AgentScope (parent/children cancel tree).
inline constexpr int kAgentScopeHierarchyIssue = 2537;

// Issue #2751: session-level Agent directory surface (per-Evaluator /
// per-AgentScope snapshot; NOT a process-global registry).
inline constexpr int kAgentDirectoryIssue = 2751;
// Issue #2777: read APIs take ScopeEnterGuard (#2399 incomplete sweep).
inline constexpr int kAgentScopeReadGuardIssue = 2777;
// Issue #2781: hierarchy cancel_all must not false-positive #2399 misuse
// when parent recursion walks children (no per-child ScopeEnterGuard).
inline constexpr int kAgentScopeHierarchyCancelIssue = 2781;
// Issue #2782: AgentScope borrowed Scheduler* lifetime — Scheduler
// notifies observers before fiber teardown; ops fail-closed if dangling.
inline constexpr int kAgentScopeSchedulerLifetimeIssue = 2782;
// Issue #2976: opt-in MutexGuarded concurrency (default SingleOwner).
inline constexpr int kAgentScopeConcurrencyIssue = 2976;
// Issue #3444: Aura addressing key for the C++ hierarchy (#2537 /
// #2631). One per-Evaluator map; :path / :child-index walks child_at.
// orch:scope-child returns scope-path matching directory_snapshot.
inline constexpr int kScopeChildAddressIssue = 3444;
// Issue #3496: Aura orch:scope-join-all root drop must see descendant
// handles (join_all/watch_all stay local; cancel_all already recurses).
inline constexpr int kJoinAllTreeSettledIssue = 3496;
// Issue #3497: production same-name spawn over a reclaimed-pending
// handles_ slot is a typed deny (no emplace). Name-table put already
// fail-closes; this is the scope-handle plane. Soft / Off: one
// production load, existing append (zero extra scan).
inline constexpr int kScopeSpawnPendingNameIssue = 3497;

// Issue #3444: directory_snapshot encodes root as "root" and children
// as "0" / "0/1". Same rule for orch:scope-child's returned path.
[[nodiscard]] inline std::string format_child_scope_path(std::string_view parent_path,
                                                         std::size_t index) {
    if (parent_path.empty() || parent_path == "root")
        return std::to_string(index);
    return std::string(parent_path) + "/" + std::to_string(index);
}

inline std::atomic<std::uint64_t> g_agent_scope_bp_seq{1};

[[nodiscard]] inline std::string make_agent_scope_bp_id() {
    const auto n = g_agent_scope_bp_seq.fetch_add(1, std::memory_order_relaxed);
    return "as:" + std::to_string(n);
}

// Soft / AURA_SANDBOX=off: do not inherit (process bucket, zero-cost).
// Production defaults: inherit so sibling scopes cannot cross-poison.
// Body moved to agent_spawn.h (canonical home — see comment block above
// the inline definition there). agent_scope.h includes agent_spawn.h
// (line 43), so this declaration is still visible at the call site at
// the bottom of this header.
[[nodiscard]] inline bool production_scope_bp_inherit() noexcept;

// Issue #2976: per-scope concurrency mode. Default SingleOwner is
// zero-lock (misuse metric / optional abort — unchanged). MutexGuarded
// serializes mutating + directory APIs with a per-scope recursive_mutex.
enum class ScopeConcurrency : std::uint8_t {
    SingleOwner = 0,  // default: misuse metric (+ optional abort)
    MutexGuarded = 1, // recursive_mutex around mutating + directory APIs
};

struct AgentScopeOptions {
    ScopeConcurrency concurrency = ScopeConcurrency::SingleOwner;
};

// Issue #3125: cross-scope directory merge surface. Caller passes an
// explicit span<AgentScope* const> (not a process-global walk). Each
// non-null source scope contributes its AgentDirectorySnapshot; the
// merge applies a single CrossScopeFilter (alive_only / name_prefix /
// source_scope_paths allow-list / dedup_by_name) and labels every entry
// with the source's bp_scope_id() and a stable source_seq. Counters
// mirrored into OrchModuleStats (cross_scope_directory_total /
// entries_total / sources_total) so dashboards can chart fan-out.
// No global registry — caller owns the source list.
inline constexpr int kCrossScopeDirectoryIssue = 3125;

// Issue #2751: one row in a session-scoped agent directory snapshot.
// Best-effort at call time (not transactional with concurrent spawn).
struct AgentDirectoryEntry {
    std::string name;
    std::uint64_t id = 0;
    std::string status;     // "alive" | "done" | "cancelled" | "spawn-failed" |
                            // "unknown" | "reclaimed"
    std::string scope_path; // "" / "root" for root; "0", "0/1" for child indices
    // Issue #3220: production Timeout after auto-wait — reservation /
    // name-table still held. Empty on Soft / reclaimable handles.
    std::string lifecycle; // "" | "reclaimed-pending"
    bool ok = false;
    // Issue #3527: three-plane Reclaimed sync. Appended at struct END
    // (#2906). Directory / scope-resolve / name-table project the same
    // pending flags so dashboards do not see status=alive while a
    // same-name put is blocked. No new query key.
    bool reclaimed_deferred = false;
    bool must_wait_reclaimed = false;
};

// Issue #2751: read-only directory filter options.
struct AgentDirectoryFilter {
    bool include_descendants = true; // walk child scopes (#2537)
    bool alive_only = false;         // drop done / cancelled / spawn-failed
    std::string name_prefix;         // empty = no name filter
};

// Issue #2751: full snapshot returned by AgentScope::directory_snapshot.
struct AgentDirectorySnapshot {
    std::vector<AgentDirectoryEntry> entries;
    std::size_t scopes_visited = 0; // root + descendants walked
    int schema = kAgentDirectoryIssue;
};

// Issue #3125: one row in a cross-scope merged directory snapshot.
// Augments AgentDirectoryEntry with source_path (the source scope's
// bp_scope_id() — "as:7" etc., session-local, stable) and source_seq
// (0..N-1 index into the input span, stable per call). scope_path
// stays as the per-source relative path ("" / "root" / "0/1") so the
// caller's #2751 path readers keep working after a merge.
struct CrossScopeEntry {
    std::string name;
    std::uint64_t id = 0;
    std::string status;           // "alive" | "done" | "cancelled" | "spawn-failed" | "unknown"
    std::string scope_path;       // per-source relative scope_path
    std::string source_path;      // bp_scope_id() of the source scope
    std::uint64_t source_seq = 0; // input span index (0..N-1)
    std::string lifecycle;        // Issue #3220: "" | "reclaimed-pending"
    bool ok = false;
    // Issue #3527: copied from AgentDirectoryEntry (struct END, #2906).
    bool reclaimed_deferred = false;
    bool must_wait_reclaimed = false;
};

// Issue #3125: read-only filter for cross-scope directory merge.
// source_scope_paths allow-list is matched against each source's
// bp_scope_id() (string compare). empty = include all sources.
// dedup_by_name drops later occurrences across sources (first wins).
struct CrossScopeFilter {
    bool alive_only = false;                     // drop done/cancelled/spawn-failed
    std::string name_prefix;                     // empty = no name filter
    std::vector<std::string> source_scope_paths; // empty = include all sources
    bool dedup_by_name = true;                   // first wins on duplicate names
};

// Issue #3125: full snapshot returned by cross_scope_directory().
// scopes_visited sums across all sources (root + descendants per source).
// sources_count == input span size (== 0 means caller passed no sources).
// entries_dropped counts entries filtered out by alive_only / name_prefix
// / dedup — observability for dashboards without exposing filter internals.
struct CrossScopeSnapshot {
    std::vector<CrossScopeEntry> entries;
    std::size_t scopes_visited = 0;
    std::size_t sources_count = 0;
    std::size_t entries_dropped = 0;
    int schema = kCrossScopeDirectoryIssue;
};

// Issue #2399 / #2946: concurrent AgentScope enter policy.
// Priority (mirrors #2838 production-default inject):
//   1. AURA_AGENT_SCOPE_CONCURRENT_ABORT=0 → SoftMetric (opt-out even prod)
//   2. AURA_AGENT_SCOPE_CONCURRENT_ABORT=1 → HardAbort (force even Soft)
//   3. Soft / AURA_SANDBOX=off → SoftMetric
//   4. production_defaults_active → HardDeny (structured fail, #2946)
//   5. else SoftMetric
// HardDeny: second enter does not take ownership and mutators must not
// mutate handles_ (spawn returns a failed handle without push).
// HardAbort: fprintf + std::abort (existing #2399 env path).
enum class AgentScopeConcurrentPolicy : std::uint8_t {
    SoftMetric = 0,
    HardDeny = 1,
    HardAbort = 2,
};

[[nodiscard]] inline AgentScopeConcurrentPolicy resolve_agent_scope_concurrent_policy() noexcept {
    const char* e = std::getenv("AURA_AGENT_SCOPE_CONCURRENT_ABORT");
    if (e != nullptr && e[0] == '0' && e[1] == '\0')
        return AgentScopeConcurrentPolicy::SoftMetric; // operator opt-out
    if (e != nullptr && e[0] == '1' && e[1] == '\0')
        return AgentScopeConcurrentPolicy::HardAbort; // force abort
    const char* sb = std::getenv("AURA_SANDBOX");
    const bool dev_off = (sb != nullptr && sb[0] != '\0' && std::string_view(sb) == "off");
    if (dev_off)
        return AgentScopeConcurrentPolicy::SoftMetric;
    if (aura::compiler::typed_audit::production_defaults_active())
        return AgentScopeConcurrentPolicy::HardDeny; // #2946 production default
    return AgentScopeConcurrentPolicy::SoftMetric;
}

// Issue #3208: resolve effective on_join_fail.
//   explicit policy  → honor on_join_fail (including ReportOnly)
//   AURA_JOIN_FAIL_ACTION=report|0 → ReportOnly
//   AURA_JOIN_FAIL_ACTION=cancel|1 → Cancel
//   production_defaults_active + unset → Cancel
//   Soft / Off + unset → ReportOnly (zero extra action)
[[nodiscard]] inline AgentFailureAction
resolve_on_join_fail(const AgentFailurePolicy* explicit_policy) noexcept {
    if (explicit_policy)
        return explicit_policy->on_join_fail;
    const char* e = std::getenv("AURA_JOIN_FAIL_ACTION");
    if (e != nullptr && e[0] != '\0') {
        const std::string_view sv(e);
        if (sv == "report" || sv == "report-only" || sv == "0" || sv == "off")
            return AgentFailureAction::ReportOnly;
        if (sv == "cancel" || sv == "1" || sv == "on")
            return AgentFailureAction::Cancel;
    }
    if (aura::compiler::typed_audit::production_defaults_active())
        return AgentFailureAction::Cancel;
    return AgentFailureAction::ReportOnly;
}

// Issue #2399: true when concurrent enter hard-aborts (env=1 only).
// Production hard-deny (#2946) is structured fail, not abort — use
// resolve_agent_scope_concurrent_policy() for the full matrix.
[[nodiscard]] inline bool agent_scope_concurrent_abort_enabled() noexcept {
    return resolve_agent_scope_concurrent_policy() == AgentScopeConcurrentPolicy::HardAbort;
}

// Issue #2946: true when concurrent enter is hard-denied (abort or structured).
[[nodiscard]] inline bool agent_scope_concurrent_hard_deny_enabled() noexcept {
    const auto p = resolve_agent_scope_concurrent_policy();
    return p == AgentScopeConcurrentPolicy::HardDeny || p == AgentScopeConcurrentPolicy::HardAbort;
}

// Issue #2161: stall response for scope-level watch_all.
// Restart is out of scope (needs re-spawn + name-table rules).
enum class StallPolicy : std::uint8_t {
    ReportOnly = 0, // aggregate counts only; no cancel
    Cancel = 1,     // cancel_on_stall for Stalled agents only
};

// Issue #2161: aggregated liveness snapshot for one watch_all pass.
// Counts map 1:1 to KeepaliveWatchStatus (+ cancelled when stall cancel fired).
// Issue #2887: bp_* counts are additive from the on_backpressure pass
// (0 when policy.on_backpressure == ReportOnly / BP below threshold).
struct ScopeWatchResult {
    std::size_t alive = 0;
    std::size_t stalled = 0;
    std::size_t done = 0;
    std::size_t closed = 0;
    std::size_t cancelled = 0;    // subset of stalled that got request_cancel
    std::size_t bp_degraded = 0;  // #2887: handles that got a BP action
    std::size_t bp_cancelled = 0; // #2887: subset with request_cancel
    std::size_t bp_throttled = 0; // #2887: subset with helper_stop only
    // Issue #3250: RestartN fuel vs skip (this watch pass). Soft skip
    // stays local (no process atomic).
    std::size_t restart_attempted = 0;
    std::size_t restart_skipped_no_spec = 0;
    std::size_t restart_ok = 0;
};

// Scoped multi-agent supervision root. Owns its handles via std::vector
// (no global registry). Destructor cancels + best-effort drains + releases
// reservations (#2082 cancel-before-release contract).
//
// Hierarchy (Issue #2537): a scope may own child scopes via unique_ptr.
// parent_ is a non-owning back-pointer. No static/global agent table —
// the tree is built only through spawn_child() on an explicit owner.
//
// Cancel / destroy order (AC2, documented):
//   1. cancel_all: recurse into children first, then request_cancel on
//      this scope's local handles (top-down cancel propagation).
//   2. ~AgentScope: cancel_all, then destroy children_ (each child dtor
//      drains its own handles — bottom-up drain), then join this scope.
// RestartN / watch_all remain scope-local (no cross-scope restart map).
//
// Thread-safety: spawn / spawn_child / join_all / cancel_all / watch_all /
// handles / directory_snapshot / child_at / size are NOT safe to call
// concurrently from multiple threads under SingleOwner (default). The
// owner must serialize access (matches the underlying Scheduler
// single-owner model). Child scopes inherit the same serial model
// (#2399 / #2777).
//
// Issue #2976: opt-in MutexGuarded takes a per-scope recursive_mutex on
// those APIs so multi-thread hosts need not wrap every call. Default
// remains SingleOwner (zero lock). Hierarchy v1: parent-before-child
// when a MutexGuarded parent walks a MutexGuarded child (cancel/find);
// hosts should still treat the tree as root-serialized. No process-global
// registry — mutex is per-scope only.
//
// Issue #2399 / #2777 / #2946: concurrent enter is *detected* (metric
// + production hard deny / optional abort) but not locked under
// SingleOwner — no mutex on that path, no global registry. Soft:
// metric-only (body may still run). Production (#2946): HardDeny —
// mutators skip handle mutation. Env AURA_AGENT_SCOPE_CONCURRENT_ABORT=1
// HardAbort; =0 Soft opt-out. Same-thread re-entry (e.g. ~AgentScope →
// cancel_all → join_all) is allowed via depth. Read APIs (#2777) also
// take ScopeEnterGuard so directory_snapshot / handles / child_at
// concurrent with ~AgentScope are not silent.
class AgentScope {
public:
    // Issue #2782: stores Scheduler* (not a raw reference) and registers
    // as a lifetime observer. If Scheduler is destroyed first, the
    // observer nulls sched_ + fiber pointers so later ops fail-closed
    // instead of UAF. API still takes Scheduler& at construction.
    // Issue #2976: optional AgentScopeOptions selects SingleOwner (default)
    // or MutexGuarded. Existing AgentScope(sched) callers are unchanged.
    explicit AgentScope(serve::Scheduler& sched, AgentScopeOptions opts = {}) noexcept
        : sched_(&sched)
        , mode_(opts.concurrency)
        , bp_scope_id_(make_agent_scope_bp_id()) {
        sched.register_agent_scope_observer(this, &AgentScope::on_scheduler_destroyed_trampoline_);
    }

    // Issue #3015: session-local BP gauge key (not a registry). Stable
    // for this scope's lifetime. Production spawn inherits it when the
    // spec left bp_scope_id empty.
    [[nodiscard]] std::string_view bp_scope_id() const noexcept { return bp_scope_id_; }

    // Issue #2976: construction mode (immutable). No lock.
    [[nodiscard]] ScopeConcurrency concurrency() const noexcept { return mode_; }

    AgentScope(const AgentScope&) = delete;
    AgentScope& operator=(const AgentScope&) = delete;
    AgentScope(AgentScope&&) = delete;
    AgentScope& operator=(AgentScope&&) = delete;

    // Issue #2782: true while the bound Scheduler is still alive.
    [[nodiscard]] bool scheduler_alive() const noexcept { return sched_ != nullptr; }

    // Spawn a new agent under this scope. Pushes the handle to the back;
    // reference remains valid until the scope is destroyed.
    //
    // Issue #2229: also stores a copy of the spec in specs_ so
    // watch_all(stall_timeout_ms, AgentFailurePolicy) with
    // on_stall == RestartN can re-spawn the body under the same
    // AgentSpec / name rules. restart_counts_ / consecutive_stall_counts_
    // are parallel vectors for per-handle supervision state.
    // Issue #2782: if Scheduler was destroyed, pushes a failed handle
    // (ok=false, error set) and bumps agent_scope_scheduler_dangling_total.
    AgentHandle& spawn(AgentSpec spec) {
        ScopeEnterGuard g(this, "spawn");
        // Issue #2946: production concurrent hard deny — do not mutate
        // handles_ (return thread-local failed handle).
        // 2026-09-02: Soft-mode collision (metric-only, #2399) now ALSO
        // skips handles_ mutation. The old Soft behavior ("method body
        // continues") let a concurrent spawn push_back while join_all was
        // iterating std::span(handles_) in join_agents — vector
        // reallocation dangled the span and SIGSEGV'd in
        // join_keepalive_helper (test_orch_agent_batch #2399 AC2). The
        // misuse metric still bumps (inside try_enter); spawn still
        // returns; callers MUST check handle.ok (same contract as #2946).
        if (g.denied_hard() || g.soft_collision) {
            // Issue #3366: HardDeny thread_local handle. NOT pushed to
            // handles_ (so iterators / join_all / cancel_all by index skip
            // it). id stays 0, ok=false, error="...#2946" — callers MUST
            // check handle.ok before any operation; orch:scope-spawn now
            // mirrors the orch:spawn-agent typed reject hash on this path
            // (ok=#f + quota_exceeded + quota_dimension + deny-class + error).
            // Returning a reference (not a copy) is intentional — matches
            // the live-handle arm below so callers can read .ok / .id /
            // .error / .quota_dimension uniformly. Callers that mistakenly
            // try to join / index this handle will see id=0 and
            // join_one(0) / cancel_one(0) would no-op against the empty
            // handles_ vector.
            thread_local AgentHandle failed;
            failed = AgentHandle{};
            failed.ok = false;
            failed.error = "AgentScope concurrent hard deny (#2946)";
            failed.name = spec.name;
            return failed;
        }
        if (!sched_) {
            g_orch_module_stats.agent_scope_scheduler_dangling_total.fetch_add(
                1, std::memory_order_relaxed);
            AgentHandle failed;
            failed.ok = false;
            failed.error = "AgentScope: Scheduler destroyed (#2782)";
            failed.name = spec.name;
            handles_.push_back(std::move(failed));
            specs_.push_back(std::move(spec));
            restart_counts_.push_back(0);
            consecutive_stall_counts_.push_back(0);
            return handles_.back();
        }
        // Issue #3015: production multi-Scope — fill empty bp_scope_id
        // from this scope's session-local key so a storm in A cannot
        // process-bucket-poison B. Soft / sandbox=off / explicit id
        // (including "-" process-bucket sentinel) are left alone.
        if (spec.bp_scope_id.empty() && production_scope_bp_inherit()) {
            spec.bp_scope_id = bp_scope_id_;
            g_orch_module_stats.spawn_bp_scope_inherited_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
        }
        // Issue #3497: production + same name already in handles_ with
        // Reclaimed-pending flags → typed deny, do not emplace (ghost
        // handle would steal first-match find). Soft / Off: skip the
        // walk (one production_defaults load). Empty name still appends.
        if (aura::compiler::typed_audit::production_defaults_active() && !spec.name.empty()) {
            for (const auto& hp : handles_) {
                if (hp.name == spec.name &&
                    (hp.must_wait_reclaimed || hp.reclaimed_deferred_cleanup)) {
                    thread_local AgentHandle failed;
                    failed = AgentHandle{};
                    failed.ok = false;
                    failed.error = "AgentScope: name-reuse-while-reclaimed-pending (#3497)";
                    failed.name = spec.name;
                    return failed;
                }
            }
        }
        handles_.emplace_back(spawn_agent_with_mailbox(*sched_, spec));
        // Copy the spec for re-spawn. AgentSpec's body is a
        // std::function (cheap to copy; the body closure is shared
        // by handle, so the new spawn reuses the same closure
        // instance). specs_ / restart_counts_ / consecutive_stall_counts_
        // stay aligned with handles_ via push_back in the same order.
        specs_.push_back(spec);
        restart_counts_.push_back(0);
        consecutive_stall_counts_.push_back(0);
        return handles_.back();
    }

    // Issue #3250: adopt a bare (name-table) handle without restart
    // fuel. RestartN on this slot is skipped (observable); production
    // degrades to Cancel. Prefer spawn(spec) when RestartN must replay.
    // Tests only — not a second registry / cross-scope restart map.
    AgentHandle& adopt_handle_without_spec_for_test(AgentHandle h) {
        ScopeEnterGuard g(this, "adopt_handle_without_spec_for_test");
        if (g.denied_hard()) {
            thread_local AgentHandle failed;
            failed = AgentHandle{};
            failed.ok = false;
            failed.error = "AgentScope concurrent hard deny (#2946)";
            failed.name = h.name;
            return failed;
        }
        handles_.push_back(std::move(h));
        specs_.push_back(AgentSpec{}); // empty body — not restartable
        restart_counts_.push_back(0);
        consecutive_stall_counts_.push_back(0);
        return handles_.back();
    }

    // Issue #2537: create a child AgentScope owned by this scope.
    // Shares the parent Scheduler. Child's parent() returns this.
    // Cancel/destroy on the parent propagates to all descendants.
    // Caller must serialize access (same single-owner model as spawn).
    // Issue #2782: if parent Scheduler is dead, still creates a child
    // bound to a null scheduler (fail-closed ops); bumps dangling total.
    AgentScope& spawn_child() {
        ScopeEnterGuard g(this, "spawn_child");
        // Issue #2946: concurrent hard deny — do not push children_.
        // Return *this as fail-closed stub (caller must not treat as child).
        // Issue #3444: Aura orch:scope-child detects the stub via
        // `&child == &parent` and returns ok=#f (never ok=#t on *this).
        if (g.denied_hard())
            return *this;
        if (!sched_) {
            g_orch_module_stats.agent_scope_scheduler_dangling_total.fetch_add(
                1, std::memory_order_relaxed);
            // Child without a live Scheduler — construct via a temporary
            // path is impossible without a reference; use a detaching
            // ctor that leaves sched_ null (no observer register).
            auto child =
                std::unique_ptr<AgentScope>(new AgentScope(nullptr, AgentScopeOptions{mode_}));
            child->parent_ = this;
            children_.push_back(std::move(child));
            return *children_.back();
        }
        auto child = std::make_unique<AgentScope>(*sched_, AgentScopeOptions{mode_});
        child->parent_ = this;
        children_.push_back(std::move(child));
        return *children_.back();
    }

    // Issue #2777: parent/is_root/child_count/child_at take ScopeEnterGuard
    // so concurrent ~AgentScope / spawn_child is detected (not silent).
    [[nodiscard]] AgentScope* parent() const noexcept {
        ScopeEnterGuard g(this, "parent");
        return parent_;
    }
    [[nodiscard]] bool is_root() const noexcept {
        ScopeEnterGuard g(this, "is_root");
        return parent_ == nullptr;
    }
    [[nodiscard]] std::size_t child_count() const noexcept {
        ScopeEnterGuard g(this, "child_count");
        return children_.size();
    }

    // Indexed child access (throws std::out_of_range if i >= child_count()).
    [[nodiscard]] AgentScope& child_at(std::size_t i) {
        ScopeEnterGuard g(this, "child_at");
        return *children_.at(i);
    }
    [[nodiscard]] const AgentScope& child_at(std::size_t i) const {
        ScopeEnterGuard g(this, "child_at");
        return *children_.at(i);
    }

    // Issue #3496: this layer + descendants have no live body and no
    // Reclaimed-pending flags. Aura orch:scope-join-all uses this before
    // drop_agent_scope on the root. join_all itself stays local (join the
    // layer you addressed). Soft: two bool loads per handle, no intern.
    [[nodiscard]] bool tree_settled() const noexcept {
        ScopeEnterGuard g(this, "tree_settled");
        if (g.denied_hard())
            return false; // fail-closed: do not drop a contended tree
        return tree_settled_unlocked_();
    }

    // Issue #3444: walk "0" / "0/1" / "root" via child_at. Empty and
    // "root" resolve to *this (omit-path = today's root). Invalid
    // segment or HardDeny → nullptr. Not a second Evaluator map.
    [[nodiscard]] AgentScope* resolve_scope_path(std::string_view path) noexcept {
        ScopeEnterGuard g(this, "resolve_scope_path");
        if (g.denied_hard())
            return nullptr;
        if (path.empty() || path == "root")
            return this;
        AgentScope* cur = this;
        std::size_t pos = 0;
        while (pos < path.size()) {
            if (path[pos] == '/') {
                ++pos;
                continue;
            }
            const auto slash = path.find('/', pos);
            const auto seg = path.substr(
                pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
            pos = slash == std::string_view::npos ? path.size() : slash + 1;
            if (seg.empty() || seg == "root")
                continue;
            std::size_t idx = 0;
            bool digits = false;
            for (char c : seg) {
                if (c < '0' || c > '9')
                    return nullptr;
                digits = true;
                const auto next = idx * 10u + static_cast<std::size_t>(c - '0');
                if (next < idx)
                    return nullptr;
                idx = next;
            }
            if (!digits)
                return nullptr;
            if (idx >= cur->child_count())
                return nullptr;
            cur = &cur->child_at(idx);
        }
        return cur;
    }
    [[nodiscard]] const AgentScope* resolve_scope_path(std::string_view path) const noexcept {
        return const_cast<AgentScope*>(this)->resolve_scope_path(path);
    }

    // Join all live handles. Mirrors join_agents (#2082/#2153/#3050):
    // on non-Ok, cancel + secondary drain (default 2s, JoinPolicy.drain_ms)
    // before per-handle reservation release. Release is idempotent (#2009).
    // Issue #3050: returned JoinResult is the batch aggregate; per-handle
    // Reclaimed vs Done cleanup / must_wait_reclaimed is decided inside
    // join_agents (authoritative flags live on AgentHandle; Aura
    // orch:scope-resolve reads them). Issue #2782: Scheduler dead →
    // release reservations only (no fiber join).
    // Issue #3051: C++ join_all does **not** auto-inject a wait deadline
    // (Soft / JoinPolicy default stays #3012). Aura orch:scope-join-all
    // applies kProductionWaitReclaimedMsDefault per must_wait handle.
    // Issue #3052: optional AgentFailurePolicy.on_join_fail after the
    // batch join. Issue #3208: omitting the policy (nullopt) injects
    // Cancel under production; Soft / explicit ReportOnly stay ReportOnly.
    [[nodiscard]] serve::JoinResult join_all(std::optional<std::uint64_t> timeout_ms = {}) {
        return join_all(JoinPolicy{.primary_ms = timeout_ms, .drain_ms = kDefaultJoinDrainMs});
    }

    // Issue #2153: full JoinPolicy (primary + drain_ms).
    // Issue #3052: fail.on_join_fail after per-handle local status
    // is stamped (RestartN / Throttle / Cancel / ReportOnly).
    // Issue #3208: nullopt = unset (production default Cancel);
    // passing AgentFailurePolicy honors on_join_fail even if ReportOnly.
    [[nodiscard]] serve::JoinResult
    join_all(JoinPolicy policy, std::optional<AgentFailurePolicy> fail = std::nullopt) {
        ScopeEnterGuard g(this, "join_all(policy)");
        if (handles_.empty()) {
            serve::JoinResult r;
            r.status = serve::JoinStatus::Invalid;
            return r;
        }
        if (!sched_) {
            g_orch_module_stats.agent_scope_scheduler_dangling_total.fetch_add(
                1, std::memory_order_relaxed);
            release_handles_no_join_();
            serve::JoinResult r;
            r.status = serve::JoinStatus::Invalid;
            return r;
        }
        auto jr = join_agents(std::span<AgentHandle>(handles_), policy);
        apply_on_join_fail_unlocked_(fail ? &*fail : nullptr);
        return jr;
    }

    // Issue #3208: last join_all effective on_join_fail + count of
    // handles that took a non-ReportOnly action (Cancel/RestartN/Throttle).
    [[nodiscard]] AgentFailureAction last_on_join_fail_effective() const noexcept {
        return last_on_join_fail_effective_;
    }
    [[nodiscard]] std::uint32_t last_join_fail_action_taken() const noexcept {
        return last_join_fail_action_taken_;
    }
    // Issue #3250: last join_all RestartN fuel vs skip (this call).
    [[nodiscard]] std::uint32_t last_restart_attempted() const noexcept {
        return last_restart_attempted_;
    }
    [[nodiscard]] std::uint32_t last_restart_skipped_no_spec() const noexcept {
        return last_restart_skipped_no_spec_;
    }
    [[nodiscard]] std::uint32_t last_restart_ok() const noexcept { return last_restart_ok_; }

    // Best-effort cancel request on all live fibers. Bounded cost; does
    // NOT wait. Use join_all afterwards to drain. Safe to call multiple
    // times (request_cancel is idempotent).
    //
    // Issue #2537: cancels child scopes first (top-down), then local
    // handles. Does not join/drain — callers (or ~AgentScope) drain.
    //
    // Issue #2781: hierarchy walk uses cancel_all_unlocked_ on children
    // so recursive cancel does NOT re-enter ScopeEnterGuard per child.
    // Calling public cancel_all on each child re-took the guard and could
    // false-positive agent_scope_concurrent_misuse_total (and abort under
    // AURA_AGENT_SCOPE_CONCURRENT_ABORT=1) even on a single-thread tree
    // walk. Caller of the root cancel_all still serializes via this
    // scope's enter; children inherit that serial contract.
    void cancel_all() noexcept {
        ScopeEnterGuard g(this, "cancel_all");
        cancel_all_unlocked_(/*from_hierarchy=*/false);
    }

    // Issue #2161: batch liveness watch over scope handles.
    // For each handle: watch_agent_liveness (same Closed rules when
    // keepalive_interval_ms==0). No process-global registry.
    // StallPolicy::Cancel cancels only Stalled fibers (Done/Alive untouched).
    [[nodiscard]] ScopeWatchResult watch_all(std::uint32_t stall_timeout_ms = 0,
                                             StallPolicy policy = StallPolicy::Cancel) {
        ScopeEnterGuard g(this, "watch_all");
        ScopeWatchResult r;
        const bool cancel_on_stall = (policy == StallPolicy::Cancel);
        for (auto& h : handles_) {
            auto wr = watch_agent_liveness(h, stall_timeout_ms, cancel_on_stall);
            switch (wr.status) {
                case KeepaliveWatchStatus::Alive:
                    ++r.alive;
                    break;
                case KeepaliveWatchStatus::Stalled:
                    ++r.stalled;
                    if (wr.cancelled)
                        ++r.cancelled;
                    break;
                case KeepaliveWatchStatus::Done:
                    ++r.done;
                    break;
                case KeepaliveWatchStatus::Closed:
                    ++r.closed;
                    break;
            }
        }
        return r;
    }

    // Convenience overload: bool cancel_on_stall maps to StallPolicy.
    [[nodiscard]] ScopeWatchResult watch_all(std::uint32_t stall_timeout_ms, bool cancel_on_stall) {
        return watch_all(stall_timeout_ms,
                         cancel_on_stall ? StallPolicy::Cancel : StallPolicy::ReportOnly);
    }

    // Issue #2229: full supervision policy surface. Replaces the
    // binary StallPolicy with AgentFailureAction (ReportOnly /
    // Cancel / RestartN) and adds a circuit-like consecutive-stall
    // limit + optional restart backoff. RestartN path: stop helper
    // → cancel body → join drain (via #2227 hard-reclaim) →
    // optional backoff → spawn replacement under the same AgentSpec
    // → replace handle in the scope vector → bump restart count.
    // Once restart_counts_[i] >= max_restarts OR
    // consecutive_stall_counts_[i] >= consecutive_stall_limit, the
    // scope force-downgrades to Cancel (request_cancel already
    // invoked by watch_agent_liveness with cancel_on_stall=true)
    // and bumps agent_restart_exhausted_total. Default policy
    // (Cancel / max_restarts=0 / consecutive_stall_limit=3) matches
    // the pre-#2229 StallPolicy::Cancel behaviour for callers
    // adopting AgentFailurePolicy incrementally.
    [[nodiscard]] ScopeWatchResult watch_all(std::uint32_t stall_timeout_ms,
                                             const AgentFailurePolicy& policy) {
        ScopeEnterGuard g(this, "watch_all(policy)");
        ScopeWatchResult r;
        // Map AgentFailureAction::ReportOnly / Cancel to the binary
        // watch_agent_liveness cancel_on_stall flag. RestartN uses
        // Cancel internally too (the re-spawn path runs after the
        // existing cancel completes).
        const bool cancel_on_stall = (policy.on_stall != AgentFailureAction::ReportOnly);
        for (std::size_t i = 0; i < handles_.size(); ++i) {
            auto& h = handles_[i];
            auto wr = watch_agent_liveness(h, stall_timeout_ms, cancel_on_stall);
            switch (wr.status) {
                case KeepaliveWatchStatus::Alive:
                    ++r.alive;
                    // Reset consecutive_stall on healthy (fresh heartbeat).
                    consecutive_stall_counts_[i] = 0;
                    break;
                case KeepaliveWatchStatus::Stalled: {
                    ++r.stalled;
                    if (wr.cancelled)
                        ++r.cancelled;
                    ++consecutive_stall_counts_[i];
                    g_orch_module_stats.agent_consecutive_stall_total.fetch_add(
                        1, std::memory_order_relaxed);
                    // Decide between RestartN and the fallback Cancel /
                    // ReportOnly per the policy + circuit-like cap.
                    const bool circuit_open =
                        consecutive_stall_counts_[i] >= policy.consecutive_stall_limit;
                    const bool within_max_restarts = restart_counts_[i] < policy.max_restarts;
                    const bool should_restart = (policy.on_stall == AgentFailureAction::RestartN) &&
                                                !circuit_open && within_max_restarts;
                    if (should_restart) {
                        // Re-spawn path: stop helper, cancel body,
                        // join drain (via #2227 hard-reclaim), optional
                        // backoff, then spawn replacement under the
                        // same AgentSpec and replace the handle.
                        ++r.restart_attempted;
                        stop_keepalive_helper(h);
                        if (h.fiber && !h.fiber->is_done()) {
                            h.fiber->request_cancel();
                            if (auto* sched = h.fiber->owner_sched()) {
                                sched->note_orphan_fiber(h.fiber, /*hard_deadline_ms=*/50);
                            }
                        }
                        // Issue #3250: no copyable specs_ body → skip
                        // (observable); production Cancel already fired
                        // via cancel_on_stall. Soft: no extra atomic.
                        if (restart_spec_missing_(i)) {
                            ++r.restart_skipped_no_spec;
                            note_restart_skipped_no_spec_(h, /*cancel=*/false);
                        } else if (try_restart_from_spec_(i, policy)) {
                            ++r.restart_ok;
                        }
                    } else if ((policy.on_stall == AgentFailureAction::RestartN) &&
                               (!within_max_restarts || circuit_open)) {
                        // Cancel path: request_cancel already invoked
                        // by watch_agent_liveness (cancel_on_stall=true).
                        // Bump exhausted on the max-restarts OR
                        // circuit-open signal so Agent frameworks can
                        // branch on the supervision outcome.
                        g_orch_module_stats.agent_restart_exhausted_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    // ReportOnly path: nothing extra — aggregate count
                    // already bumped via r.stalled.
                    break;
                }
                case KeepaliveWatchStatus::Done:
                    ++r.done;
                    consecutive_stall_counts_[i] = 0;
                    break;
                case KeepaliveWatchStatus::Closed:
                    ++r.closed;
                    break;
            }
        }

        // Issue #2887 / #2948: after stall pass, optionally degrade BP-hot
        // producers already in this scope. Admit gate still soft-rejects
        // *new* attach_mailbox spawns (#2228/#2535); this path converges
        // existing producers. Default on_backpressure=ReportOnly → no-op
        // (AC1). Scope-local only — no process-global registry (AC4).
        // Threshold via SSOT resolve_bp_threshold: policy 0 → process
        // default (NOT always-reject — that is #2591 spec-only).
        if (policy.on_backpressure != AgentFailureAction::ReportOnly) {
            const auto thr_override = policy.bp_threshold == 0
                                          ? std::optional<std::uint64_t>{}
                                          : std::optional<std::uint64_t>{policy.bp_threshold};
            // scope_id empty at resolve — threshold does not depend on
            // scope; gauges loaded below with load_mailbox_bp_recent.
            const auto thr_d = resolve_bp_threshold(thr_override, /*scope_id=*/{},
                                                    /*policy_zero_means_process_default=*/true);
            g_orch_module_stats.bp_threshold_resolve_total.fetch_add(1, std::memory_order_relaxed);
            const auto thr = thr_d.threshold;
            if (thr > 0) {
                // Scope BP recent: max across handle specs' bp_scope_id
                // via load_mailbox_bp_recent (same as spawn admit, #2948
                // AC4). Multi-tenant isolation: hot gauge A does not
                // force degrade on a scope that only uses B.
                std::uint64_t scope_bp_recent = 0;
                bool any_named_scope = false;
                for (const auto& sp : specs_) {
                    if (sp.bp_scope_id.empty())
                        continue;
                    any_named_scope = true;
                    const auto v = load_mailbox_bp_recent(sp.bp_scope_id);
                    if (v > scope_bp_recent)
                        scope_bp_recent = v;
                }
                if (!any_named_scope) {
                    scope_bp_recent = load_mailbox_bp_recent(/*scope_id=*/{});
                }
                if (scope_bp_recent >= thr) {
                    for (std::size_t i = 0; i < handles_.size(); ++i) {
                        auto& h = handles_[i];
                        // Only mailbox-holding live handles (producers /
                        // consumers that already admit-passed). Done /
                        // invalid / no-mailbox skip — zero cost.
                        if (!h.ok || !h.mailbox)
                            continue;
                        if (h.fiber && h.fiber->is_done())
                            continue;
                        // Apply action. Throttle is cooperative only
                        // (helper_stop; no request_cancel) per AC3.
                        if (policy.on_backpressure == AgentFailureAction::Throttle) {
                            stop_keepalive_helper(h);
                            g_orch_module_stats.agent_bp_throttle_total.fetch_add(
                                1, std::memory_order_relaxed);
                            g_orch_module_stats.agent_bp_degrade_total.fetch_add(
                                1, std::memory_order_relaxed);
                            ++r.bp_throttled;
                            ++r.bp_degraded;
                            continue;
                        }
                        // Cancel / RestartN: stop helper + request_cancel.
                        stop_keepalive_helper(h);
                        if (h.fiber && !h.fiber->is_done()) {
                            h.fiber->request_cancel();
                            if (auto* sched = h.fiber->owner_sched()) {
                                sched->note_orphan_fiber(h.fiber, /*hard_deadline_ms=*/50);
                            }
                        }
                        g_orch_module_stats.agent_bp_cancel_total.fetch_add(
                            1, std::memory_order_relaxed);
                        g_orch_module_stats.agent_bp_degrade_total.fetch_add(
                            1, std::memory_order_relaxed);
                        ++r.bp_cancelled;
                        ++r.bp_degraded;
                        ++r.cancelled;

                        // Optional RestartN (capped like on_stall path).
                        if (policy.on_backpressure == AgentFailureAction::RestartN) {
                            const bool within_max = restart_counts_[i] < policy.max_restarts;
                            if (within_max) {
                                ++r.restart_attempted;
                                if (restart_spec_missing_(i)) {
                                    ++r.restart_skipped_no_spec;
                                    note_restart_skipped_no_spec_(h, /*cancel=*/false);
                                } else if (try_restart_from_spec_(i, policy)) {
                                    ++r.restart_ok;
                                }
                            } else {
                                g_orch_module_stats.agent_restart_exhausted_total.fetch_add(
                                    1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }
        }
        return r;
    }

    // Issue #2777: size/empty/handles take ScopeEnterGuard (#2399 incomplete
    // on read path). spans are only valid while the owner serializes further
    // mutate — concurrent spawn may reallocate after return (detect via guard
    // on the call, not a transactional lock).
    [[nodiscard]] std::size_t size() const noexcept {
        ScopeEnterGuard g(this, "size");
        return handles_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        ScopeEnterGuard g(this, "empty");
        return handles_.empty();
    }

    // Read-only access (for advanced supervisor logic + tests).
    [[nodiscard]] std::span<const AgentHandle> handles() const noexcept {
        ScopeEnterGuard g(this, "handles");
        return std::span<const AgentHandle>(handles_);
    }

    // Mutable access for tests / advanced supervisors (watch_agent_liveness).
    [[nodiscard]] std::span<AgentHandle> handles_mut() noexcept {
        ScopeEnterGuard g(this, "handles_mut");
        return std::span<AgentHandle>(handles_);
    }

    // Issue #2751: session-scoped agent directory snapshot (read-only,
    // best-effort at call time). Walks this scope's handles_ and,
    // when filter.include_descendants, child scopes (#2537). Never
    // process-wide — only agents owned by this scope tree.
    // Issue #2777: ScopeEnterGuard + copy-local handles under guard so
    // concurrent ~AgentScope / spawn is detected and vector walk is not
    // silent UB. Not a full transaction with concurrent spawn (AC2).
    // Soft / empty scope → empty entries, scopes_visited >= 1 when this
    // scope exists.
    [[nodiscard]] AgentDirectorySnapshot
    directory_snapshot(const AgentDirectoryFilter& filter = {}) const {
        ScopeEnterGuard g(this, "directory_snapshot");
        AgentDirectorySnapshot snap;
        snap.schema = kAgentDirectoryIssue;
        collect_directory_(snap, filter, /*path=*/"");
        return snap;
    }

    // Issue #2926: session-local live resolve by name (no process-global
    // AgentRegistry). Best-effort against current handles_ (+ optional
    // descendant scopes under #2537). Concurrent spawn may race (same
    // serial-owner model as directory_snapshot). Returns nullptr on miss.
    // After join_all, handles may remain with fiber done — find still
    // returns them (caller reads status via fiber/is_done); after the
    // per-Evaluator scope slot is dropped, Aura resolve returns not-found.
    // Issue #3442: orch:agent-send / recv / ask / agent-join fall back
    // to this find after a name-table miss (same Evaluator, no put).
    [[nodiscard]] AgentHandle* find(std::string_view name,
                                    bool include_descendants = true) noexcept {
        ScopeEnterGuard g(this, "find");
        return find_unlocked_(name, include_descendants);
    }
    [[nodiscard]] const AgentHandle* find(std::string_view name,
                                          bool include_descendants = true) const noexcept {
        ScopeEnterGuard g(this, "find");
        return find_unlocked_(name, include_descendants);
    }

    // Supervision root: cancel + best-effort drain + release before
    // destruction. join_agents handles cancel+drain on non-Ok internally
    // (#2082), and per-handle release_agent_memory_reservation is
    // idempotent so concurrent ~AgentHandle + scope destruction is safe.
    // Issue #2399: same-thread re-entry of cancel_all/join_all under the
    // enter guard is allowed (depth); concurrent ~AgentScope from another
    // thread is detected as misuse.
    //
    // Issue #2537 destroy order (documented AC2):
    //   1. cancel_all — top-down (children, then local handles)
    //   2. children_.clear() — each child dtor drains its tree bottom-up
    //   3. join local handles (default drain_ms, #2153)
    // Issue #2782: unregister observer if Scheduler still alive; if
    // Scheduler already destroyed (sched_ null), skip fiber join and
    // only release reservations (fiber* already nulled by observer).
    ~AgentScope() {
        ScopeEnterGuard g(this, "~AgentScope");
        // Issue #3337: production teardown frees this scope's named BP
        // gauge so sequential tenant churn does not saturate the 256-cap
        // map. Soft / empty / "-" : no extra lock/atomic.
        (void)aura::orch::maybe_erase_scope_bp_gauge_on_teardown(bp_scope_id_);
        if (sched_) {
            sched_->unregister_agent_scope_observer(this);
            // Leave sched_ non-null for cancel/join below; observer
            // unregistered so ~Scheduler will not re-notify this scope.
        }
        cancel_all();
        // Drain descendants before this scope's handles (bottom-up).
        children_.clear();
        if (handles_.empty())
            return;
        if (!sched_) {
            release_handles_no_join_();
            return;
        }
        // Issue #2153: destructor uses default drain_ms (kDefaultJoinDrainMs).
        (void)join_agents(
            std::span<AgentHandle>(handles_),
            JoinPolicy{.primary_ms = kDefaultJoinDrainMs, .drain_ms = kDefaultJoinDrainMs});
    }

private:
    // Issue #2782: construct with null Scheduler (parent already dead).
    // No observer registration.
    explicit AgentScope(std::nullptr_t, AgentScopeOptions opts = {}) noexcept
        : sched_(nullptr)
        , mode_(opts.concurrency)
        , bp_scope_id_(make_agent_scope_bp_id()) {}

    // Issue #2782: Scheduler dtor callback — null sched_ + fiber pointers
    // before owned fibers are destroyed (prevents UAF).
    static void on_scheduler_destroyed_trampoline_(void* cookie) noexcept {
        if (auto* self = static_cast<AgentScope*>(cookie))
            self->on_scheduler_destroyed_();
    }

    void on_scheduler_destroyed_() noexcept {
        // Called from ~Scheduler under no AgentScope enter guard. Null
        // pointers first; metric bump for dashboards / ASan-free tests.
        sched_ = nullptr;
        g_orch_module_stats.agent_scope_scheduler_invalidated_total.fetch_add(
            1, std::memory_order_relaxed);
        for (auto& h : handles_) {
            h.fiber = nullptr;
            h.keepalive_helper = nullptr;
            // Keep mailbox / liveness shared_ptrs (heap-owned, safe).
        }
        // Children registered independently; ~Scheduler notifies each.
    }

    void release_handles_no_join_() noexcept {
        for (auto& h : handles_) {
            h.fiber = nullptr;
            h.keepalive_helper = nullptr;
            release_agent_memory_reservation(h);
        }
    }
    // Issue #2537 / #2781: cancel body without ScopeEnterGuard.
    // from_hierarchy=true when entered via a parent's unlocked walk
    // (bumps agent_scope_hierarchy_cancel_total for dashboards).
    // Recurses into children unlocked — parent cancel_all already holds
    // the root enter; per-child enter was the #2781 false-positive source.
    void cancel_all_unlocked_(bool from_hierarchy) noexcept {
        if (from_hierarchy) {
            g_orch_module_stats.agent_scope_hierarchy_cancel_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        for (auto& c : children_) {
            if (!c)
                continue;
            // Issue #2976: MutexGuarded child — parent-before-child lock
            // (parent already holds its mutex via cancel_all enter).
            // SingleOwner children stay unlocked (#2781 no false-positive).
            if (c->mode_ == ScopeConcurrency::MutexGuarded) {
                ScopeEnterGuard cg(c.get(), "cancel_all_unlocked");
                c->cancel_all_unlocked_(/*from_hierarchy=*/true);
            } else {
                c->cancel_all_unlocked_(/*from_hierarchy=*/true);
            }
        }
        // Issue #2782: null fiber after Scheduler destroy — skip cancel.
        for (auto& h : handles_) {
            if (h.fiber && !h.fiber->is_done())
                h.fiber->request_cancel();
        }
    }

    // Issue #2399 / #2777 / #2946: RAII enter/leave for concurrent misuse.
    // Same-thread re-entry increments depth (no metric). Concurrent enter
    // from another thread bumps agent_scope_concurrent_misuse_total;
    // production HardDeny (#2946) also bumps hard_deny_total and sets
    // denied_hard so mutators skip handle mutation; Soft continues the
    // method body (metric-only detect, no lock). Env=1 HardAbort aborts.
    // Zero cost beyond one atomic CAS when free. const-friendly:
    // owner_tid_/enter_depth_ are mutable (#2777 reads).
    //
    // Issue #2781: hierarchy cancel does NOT use this guard on children
    // (see cancel_all_unlocked_). Direct cancel_all / ~AgentScope still
    // enter once on the scope being cancelled.
    struct ScopeEnterGuard {
        const AgentScope* self = nullptr;
        std::unique_lock<std::recursive_mutex> lk; // empty unless MutexGuarded
        bool holds = false;
        bool hard_denied = false; // #2946 production concurrent hard deny
        // Soft-mode concurrent collision (try_enter returned false without
        // HardDeny/HardAbort — metric-only detect). Mutators must treat this
        // like denied_hard and skip handles_ mutation: join_all hands
        // std::span(handles_) to join_agents, so a concurrent push_back
        // reallocating the vector is a UAF (2026-09-02 CI: SIGSEGV in
        // join_keepalive_helper via #2399 AC2 concurrent spawn).
        bool soft_collision = false;
        ScopeEnterGuard(const AgentScope* s, const char* site) noexcept
            : self(s) {
            if (!self)
                return;
            // Issue #2976: MutexGuarded serializes via recursive_mutex.
            // SingleOwner stays zero-lock (try_enter misuse detect only).
            if (self->mode_ == ScopeConcurrency::MutexGuarded) {
                try {
                    lk = std::unique_lock<std::recursive_mutex>(self->api_mu_);
                } catch (...) {
                    // [SILENCE-PRIM-#615] lock failure is fail-closed (hard deny).
                    hard_denied = true;
                    return;
                }
                g_orch_module_stats.agent_scope_mutex_guarded_enter_total.fetch_add(
                    1, std::memory_order_relaxed);
                holds = true;
                return;
            }
            holds = self->try_enter(site, &hard_denied);
            soft_collision = !holds && !hard_denied;
        }
        ~ScopeEnterGuard() noexcept {
            if (self && self->mode_ == ScopeConcurrency::MutexGuarded)
                return; // lk unlocks
            if (holds && self)
                self->leave();
        }
        [[nodiscard]] bool denied_hard() const noexcept { return hard_denied; }
        ScopeEnterGuard(const ScopeEnterGuard&) = delete;
        ScopeEnterGuard& operator=(const ScopeEnterGuard&) = delete;
    };

    // Returns true if this thread holds ownership (caller must leave).
    // Returns false on concurrent misuse. When out_hard_denied is set
    // and policy is HardDeny, *out_hard_denied=true (mutators must not
    // mutate). Soft concurrent: false + hard_denied=false (legacy body
    // may still run). HardAbort: does not return.
    bool try_enter(const char* site, bool* out_hard_denied = nullptr) const noexcept {
        if (out_hard_denied)
            *out_hard_denied = false;
        const auto tid = std::this_thread::get_id();
        std::thread::id expected{}; // default-constructed = unowned
        if (owner_tid_.compare_exchange_strong(expected, tid, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
            enter_depth_.store(1, std::memory_order_relaxed);
            return true;
        }
        if (expected == tid) {
            // Same-thread re-entry (~AgentScope → cancel_all → join_all).
            enter_depth_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Another thread owns the scope — concurrent misuse.
        g_orch_module_stats.agent_scope_concurrent_misuse_total.fetch_add(
            1, std::memory_order_relaxed);
        // Issue #2777: directory_snapshot-specific concurrent metric.
        if (site && std::strcmp(site, "directory_snapshot") == 0) {
            g_orch_module_stats.directory_snapshot_concurrent_total.fetch_add(
                1, std::memory_order_relaxed);
        }
        const auto pol = resolve_agent_scope_concurrent_policy();
        if (pol == AgentScopeConcurrentPolicy::HardAbort) {
            std::fprintf(stderr,
                         "FATAL: AgentScope concurrent misuse at %s "
                         "(AURA_AGENT_SCOPE_CONCURRENT_ABORT=1, #2399/#2777/#2946)\n",
                         site ? site : "?");
            std::abort();
        }
        if (pol == AgentScopeConcurrentPolicy::HardDeny) {
            // Issue #2946: production hard deny — structured fail.
            g_orch_module_stats.agent_scope_concurrent_hard_deny_total.fetch_add(
                1, std::memory_order_relaxed);
            if (out_hard_denied)
                *out_hard_denied = true;
            return false;
        }
        // SoftMetric: detect only; caller may still run method body.
        return false;
    }

    void leave() const noexcept {
        const auto d = enter_depth_.fetch_sub(1, std::memory_order_relaxed);
        if (d == 1) {
            owner_tid_.store(std::thread::id{}, std::memory_order_release);
        }
    }

    // Issue #2926: name walk without taking a second enter on *this*
    // (caller already holds ScopeEnterGuard). Children take their own enter.
    [[nodiscard]] AgentHandle* find_unlocked_(std::string_view name,
                                              bool include_descendants) noexcept {
        for (auto& h : handles_) {
            if (h.name == name) {
                // Issue #3564: non-dtor recycle — Scope holds the handle
                // until tree_settled drop, so #3529 dtor never runs.
                (void)aura::orch::maybe_force_release_reclaimed_quota(h);
                return &h;
            }
        }
        if (!include_descendants)
            return nullptr;
        for (auto& c : children_) {
            if (!c)
                continue;
            ScopeEnterGuard cg(c.get(), "find");
            if (auto* p = c->find_unlocked_(name, /*include_descendants=*/true))
                return p;
        }
        return nullptr;
    }
    [[nodiscard]] const AgentHandle* find_unlocked_(std::string_view name,
                                                    bool include_descendants) const noexcept {
        for (const auto& h : handles_) {
            if (h.name == name)
                return &h;
        }
        if (!include_descendants)
            return nullptr;
        for (const auto& c : children_) {
            if (!c)
                continue;
            ScopeEnterGuard cg(c.get(), "find");
            if (const auto* p = c->find_unlocked_(name, /*include_descendants=*/true))
                return p;
        }
        return nullptr;
    }

    // Issue #2751 / #2777: collect into snap. Caller must hold ScopeEnterGuard
    // on *this*. Local handles are copied to entry rows under the guard
    // (string copies of name/status — not a live span into handles_).
    // Child scopes are walked under their own enter (recursive snapshot).
    void collect_directory_(AgentDirectorySnapshot& snap, const AgentDirectoryFilter& filter,
                            const std::string& path) const {
        ++snap.scopes_visited;
        // Snapshot handle count once; iterate by index (stable under serial
        // owner; concurrent misuse already metered if another thread entered).
        const std::size_t n = handles_.size();
        for (std::size_t hi = 0; hi < n; ++hi) {
            if (hi >= handles_.size())
                break; // concurrent shrink (misuse path) — best-effort stop
            const auto& h = handles_[hi];
            AgentDirectoryEntry e;
            e.name = h.name;
            e.id = h.id;
            e.ok = h.ok;
            e.scope_path = path.empty() ? std::string("root") : path;
            // Issue #3527: project handle pending flags (always; no intern).
            e.reclaimed_deferred = h.reclaimed_deferred_cleanup;
            e.must_wait_reclaimed = h.must_wait_reclaimed;
            if (!h.ok) {
                e.status = "spawn-failed";
            } else if (!h.fiber) {
                e.status = "unknown";
            } else if (h.reclaimed_deferred_cleanup ||
                       (h.fiber->is_reclaimed() && !h.fiber->is_done())) {
                // Align with orch:scope-resolve (#3050): Reclaimed-but-not-
                // done is not "alive" — name-table put is still blocked.
                e.status = "reclaimed";
            } else if (h.fiber->is_done()) {
                e.status = "done";
            } else if (h.fiber->is_cancel_requested()) {
                e.status = "cancelled";
            } else {
                e.status = "alive";
            }
            // Issue #3220 / #3527: lifecycle whenever pending flags are
            // set under production (not only auto-wait Timeout). Soft:
            // skip string (zero intern). Bools above still populate.
            if ((h.must_wait_reclaimed || h.reclaimed_deferred_cleanup) &&
                aura::compiler::typed_audit::production_defaults_active())
                e.lifecycle = "reclaimed-pending";
            else if (h.body_acquire_rejected() && e.status == "alive" &&
                     aura::compiler::typed_audit::production_defaults_active())
                e.lifecycle = "body-not-run"; // Issue #3251
            if (filter.alive_only && e.status != "alive")
                continue;
            if (!filter.name_prefix.empty()) {
                if (e.name.size() < filter.name_prefix.size() ||
                    e.name.compare(0, filter.name_prefix.size(), filter.name_prefix) != 0)
                    continue;
            }
            snap.entries.push_back(std::move(e));
        }
        if (!filter.include_descendants)
            return;
        // Snapshot child count; each child directory_snapshot takes its own
        // ScopeEnterGuard (detect concurrent child teardown).
        const std::size_t cn = children_.size();
        for (std::size_t i = 0; i < cn; ++i) {
            if (i >= children_.size() || !children_[i])
                continue;
            const std::string child_path = path.empty() || path == "root"
                                               ? std::to_string(i)
                                               : (path + "/" + std::to_string(i));
            // Child walk under child's enter: merge into same snap.
            children_[i]->merge_directory_under_guard_(snap, filter, child_path);
        }
    }

    // Issue #3052: honor AgentFailurePolicy::on_join_fail after
    // join_agents stamped last_join_status. Caller holds ScopeEnterGuard.
    // Reclaimed + deferred / still-running is never restart/cancel fuel (#2661).
    // Issue #3208: nullptr = unset → resolve_on_join_fail (production Cancel).
    void apply_on_join_fail_unlocked_(const AgentFailurePolicy* explicit_policy) noexcept {
        const auto action = resolve_on_join_fail(explicit_policy);
        last_on_join_fail_effective_ = action;
        last_join_fail_action_taken_ = 0;
        last_restart_attempted_ = 0;
        last_restart_skipped_no_spec_ = 0;
        last_restart_ok_ = 0;
        AgentFailurePolicy policy{};
        if (explicit_policy)
            policy = *explicit_policy;
        const auto n = handles_.size();
        for (std::size_t i = 0; i < n; ++i) {
            auto& h = handles_[i];
            const bool reclaimed_live = h.reclaimed_deferred_cleanup ||
                                        (h.fiber && h.fiber->is_reclaimed() && !h.fiber->is_done());
            if (reclaimed_live)
                continue; // AC2 / #2661
            const auto st = h.last_join_status;
            if (st != serve::JoinStatus::Timeout && st != serve::JoinStatus::Cancelled)
                continue;
            g_orch_module_stats.agent_join_fail_total.fetch_add(1, std::memory_order_relaxed);
            if (action == AgentFailureAction::ReportOnly)
                continue;
            if (action == AgentFailureAction::Cancel) {
                if (h.fiber && !h.fiber->is_done())
                    h.fiber->request_cancel();
                g_orch_module_stats.agent_join_fail_action_cancel_total.fetch_add(
                    1, std::memory_order_relaxed);
                ++last_join_fail_action_taken_;
                continue;
            }
            if (action == AgentFailureAction::Throttle) {
                stop_keepalive_helper(h);
                ++last_join_fail_action_taken_;
                continue;
            }
            if (action != AgentFailureAction::RestartN)
                continue;
            if (i >= restart_counts_.size())
                continue;
            const bool within = restart_counts_[i] < policy.max_restarts;
            if (!within) {
                g_orch_module_stats.agent_restart_exhausted_total.fetch_add(
                    1, std::memory_order_relaxed);
                continue;
            }
            ++last_restart_attempted_;
            // Issue #3250: no copyable specs_ body → skip (not silent).
            // Production degrades to Cancel; Soft: zero extra.
            if (restart_spec_missing_(i)) {
                ++last_restart_skipped_no_spec_;
                note_restart_skipped_no_spec_(h, /*cancel=*/true);
                if (aura::compiler::typed_audit::production_defaults_active())
                    ++last_join_fail_action_taken_;
                continue;
            }
            if (try_restart_from_spec_(i, policy)) {
                ++last_restart_ok_;
                ++last_join_fail_action_taken_;
            }
        }
    }

    // Issue #3250: RestartN fuel is AgentScope::spawn specs_ with a
    // copyable body. Missing slot / empty body is not restartable.
    [[nodiscard]] bool restart_spec_missing_(std::size_t i) const noexcept {
        return i >= specs_.size() || !agent_spec_restartable(specs_[i]);
    }

    // Soft: silent skip (zero extra atomics). Production: bump skip
    // + optional Cancel so RestartN is never a silent no-op.
    void note_restart_skipped_no_spec_(AgentHandle& h, bool cancel) noexcept {
        if (!aura::compiler::typed_audit::production_defaults_active())
            return;
        g_orch_module_stats.agent_restart_skipped_no_spec_total.fetch_add(
            1, std::memory_order_relaxed);
        if (cancel && h.fiber && !h.fiber->is_done()) {
            h.fiber->request_cancel();
            g_orch_module_stats.agent_join_fail_action_cancel_total.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    // Spawn replacement under stored spec. Caller already checked
    // within max_restarts and spec present. Returns true on re-spawn.
    bool try_restart_from_spec_(std::size_t i, const AgentFailurePolicy& policy) noexcept {
        if (!sched_) {
            g_orch_module_stats.agent_scope_scheduler_dangling_total.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }
        if (policy.restart_backoff_ms > 0)
            fiber_sleep_ms(policy.restart_backoff_ms);
        handles_[i] = spawn_agent_with_mailbox(*sched_, specs_[i]);
        if (i < consecutive_stall_counts_.size())
            consecutive_stall_counts_[i] = 0;
        ++restart_counts_[i];
        g_orch_module_stats.agent_restart_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Issue #2777: child subtree collect with ScopeEnterGuard on the child.
    void merge_directory_under_guard_(AgentDirectorySnapshot& snap,
                                      const AgentDirectoryFilter& filter,
                                      const std::string& path) const {
        ScopeEnterGuard g(this, "directory_snapshot");
        collect_directory_(snap, filter, path);
    }

    // Issue #3496: caller holds ScopeEnterGuard on *this*.
    [[nodiscard]] bool tree_settled_unlocked_() const noexcept {
        for (const auto& hp : handles_) {
            if ((hp.fiber && !hp.fiber->is_done()) || hp.must_wait_reclaimed ||
                hp.reclaimed_deferred_cleanup)
                return false;
        }
        for (const auto& c : children_) {
            if (!c)
                continue;
            ScopeEnterGuard cg(c.get(), "tree_settled");
            if (cg.denied_hard() || !c->tree_settled_unlocked_())
                return false;
        }
        return true;
    }

    // Issue #2782: nullable; nulled by Scheduler observer before fiber teardown.
    serve::Scheduler* sched_ = nullptr;
    std::vector<AgentHandle> handles_;
    // Issue #2229: parallel vectors for the RestartN supervision
    // policy. All three stay aligned with handles_ via push_back in
    // spawn (same order). specs_ preserves the original AgentSpec
    // for re-spawn under the same name / body / mailbox config;
    // restart_counts_ / consecutive_stall_counts_ track per-handle
    // supervision state for the circuit-like consecutive_stall_limit
    // + max_restarts cap.
    std::vector<AgentSpec> specs_;
    std::vector<std::uint32_t> restart_counts_;
    std::vector<std::uint32_t> consecutive_stall_counts_;
    // Issue #2537: explicit tree links — no process-global registry.
    // parent_ is non-owning (set only by parent's spawn_child).
    // children_ owns descendants; cleared in ~AgentScope after cancel.
    AgentScope* parent_ = nullptr;
    std::vector<std::unique_ptr<AgentScope>> children_;
    // Issue #2976: construction mode. SingleOwner = zero lock.
    ScopeConcurrency mode_ = ScopeConcurrency::SingleOwner;
    // Issue #3015: session-local BP admit key (as:<seq>). Not a registry.
    std::string bp_scope_id_{};
    // Issue #3208: last join_all resolve (unset vs explicit).
    AgentFailureAction last_on_join_fail_effective_ = AgentFailureAction::ReportOnly;
    std::uint32_t last_join_fail_action_taken_ = 0;
    // Issue #3250: last join_all RestartN fuel vs skip.
    std::uint32_t last_restart_attempted_ = 0;
    std::uint32_t last_restart_skipped_no_spec_ = 0;
    std::uint32_t last_restart_ok_ = 0;
    // Taken only when mode_ == MutexGuarded. recursive so ~AgentScope
    // → cancel_all → join_all same-thread re-entry does not deadlock.
    mutable std::recursive_mutex api_mu_;
    // Issue #2399: single-owner detection (not a mutex).
    // owner_tid_ empty = free; depth tracks same-thread re-entry.
    // Unused on the MutexGuarded path (lock serializes instead).
    mutable std::atomic<std::thread::id> owner_tid_{};
    mutable std::atomic<std::uint32_t> enter_depth_{0};
};

// Issue #2588: per-Evaluator scope map (Session-local, NOT process-static).
// Lazy-created on first scope-spawn, destroyed when the Evaluator's scope
// is empty (cancel-all / join-all) or the scope is explicitly dropped.
// The map is process-level storage keyed by Evaluator* (typed as void* to
// avoid circular include with compiler/evaluator.h) — the SCOPE itself
// is per-Evaluator; the storage is just a convenience container for
// Aura prims to find / own the lifetime. NOT a global agent registry
// (MVP linter scripts/coverage/checks/check_orch_mvp_scope.py still guards
// AgentRegistry / global_agent_registry symbols).
inline std::unordered_map<void*, std::unique_ptr<AgentScope>>& g_evaluator_agent_scopes() noexcept {
    static std::unordered_map<void*, std::unique_ptr<AgentScope>> m;
    return m;
}

// Issue #2588 / #2782: get or create the scope for the given Evaluator.
// Lazy allocation on first scope-spawn; subsequent calls return the
// existing scope. Scheduler is observed (not owned): if Scheduler is
// destroyed first, AgentScope ops fail-closed (no UAF). Prefer
// destroying AgentScope before Scheduler at session boundaries.
inline AgentScope& get_or_create_agent_scope(void* ev_key, serve::Scheduler& sched) {
    auto& m = g_evaluator_agent_scopes();
    auto it = m.find(ev_key);
    if (it == m.end()) {
        auto scope = std::make_unique<AgentScope>(sched);
        auto* raw = scope.get();
        m.emplace(ev_key, std::move(scope));
        return *raw;
    }
    return *it->second;
}

// Issue #2588: find an existing scope for the given Evaluator (key).
// Returns nullptr if no scope has been created yet.
inline AgentScope* find_agent_scope(void* ev_key) noexcept {
    auto& m = g_evaluator_agent_scopes();
    auto it = m.find(ev_key);
    return it == m.end() ? nullptr : it->second.get();
}

// Issue #2588: drop the scope for the given Evaluator (key). Returns true
// if a scope was dropped. ~AgentScope is invoked (cancel + drain + reservation
// release), so per-handle no-leak (#2155 / #2009) holds.
inline bool drop_agent_scope(void* ev_key) noexcept {
    auto& m = g_evaluator_agent_scopes();
    return m.erase(ev_key) > 0;
}

// Issue #2588 / #2778: process-wide reset for tests / session boundary
// (cancels + drains every scope, then clears the evaluator scope map).
// Also clears g_scope_bp_map (#2633 residual: gauges were insert-only
// and never freed — long-running multi-tenant hosts hit the 256 cap
// and silently lost scope isolation). Returns the count of AgentScopes
// dropped (BP map clear is side-effect; size available via
// scope_bp_map_size_for_test).
inline std::size_t reset_all_agent_scopes_for_test() noexcept {
    auto& m = g_evaluator_agent_scopes();
    const auto n = m.size();
    m.clear();
    // Issue #2778: free scope-local BP gauges so reset is a true
    // session boundary (not just AgentScope tree).
    (void)reset_scope_bp_map_for_test();
    return n;
}

// Issue #3206: production + explicit Cancel / JoinDrain residual action.
// Soft / Report / Defer: observe-only (one production_defaults load on
// the residual path). Uses existing cancel_all + join_all (#2661 no
// early free). Session-local AgentScope only.
inline const char* apply_residual_reclaim_action(AgentScope& scope,
                                                 const WorkflowFailurePolicy& w) noexcept {
    using P = ResidualReclaimPreference;
    if (w.residual == P::Report || w.residual == P::Defer)
        return "observe";
    if (!aura::compiler::typed_audit::production_defaults_active())
        return "observe";
    JoinPolicy jp;
    jp.primary_ms = 0;
    jp.drain_ms = kResidualJoinDrainMs;
    if (w.residual == P::Cancel) {
        scope.cancel_all();
        (void)scope.join_all(jp);
        g_orch_module_stats.workflow_residual_cancel_total.fetch_add(1, std::memory_order_relaxed);
        return "cancel";
    }
    if (w.residual == P::JoinDrain) {
        (void)scope.join_all(jp);
        g_orch_module_stats.workflow_residual_join_drain_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
        return "join-drain";
    }
    return "observe";
}

// Issue #2852: apply_workflow body — defined here (after AgentScope is
// fully defined) so it can call scope.watch_all. Declaration is in
// agent_spawn.h (forward decl + simplified ApplyWorkflowResult to break
// the circular include between agent_spawn.h and agent_scope.h).
//   - Phase A: parallel_intend with to_parallel_policy(w)
//   - Phase B: scope.watch_all with to_agent_policy(w) (when watch_scope)
//   - Phase C: residual observe; production + explicit Cancel/JoinDrain
//              cancel_all / join_all (Issue #3206). Soft/Report observe-only.
//   - additive: workflow_apply_total bumps once per call
// No #2661 early-free. No AgentRegistry / process-global map.
[[nodiscard]] inline ApplyWorkflowResult
apply_workflow(serve::Scheduler& sched, AgentScope& scope,
               std::span<const serve::parallel_orch::TaskSpec> tasks,
               const WorkflowFailurePolicy& w, std::uint32_t stall_timeout_ms,
               bool watch_scope) noexcept {
    ApplyWorkflowResult out;
    // Phase A — batch under composed batch policy.
    out.batch = serve::parallel_orch::parallel_intend(sched, tasks, to_parallel_policy(w));
    // Phase B — scope watch under composed agent policy (optional).
    if (watch_scope) {
        out.scope_watch_called = true;
        auto wres = scope.watch_all(stall_timeout_ms, to_agent_policy(w));
        out.scope_stalled = static_cast<int>(wres.stalled);
        out.scope_alive = static_cast<int>(wres.alive);
        out.scope_done = static_cast<int>(wres.done);
    }
    // Phase C — residual observe; production + explicit Cancel/JoinDrain
    // act via cancel_all / short join_all (#3206). Soft/Report observe-only.
    const bool batch_residual = out.batch.status != serve::parallel_orch::BatchStatus::Ok;
    const bool scope_residual = out.scope_stalled > 0;
    if (batch_residual || scope_residual) {
        note_workflow_residual_reclaim_under_policy(w);
        out.residual_observed = true;
        out.residual_action = apply_residual_reclaim_action(scope, w);
        out.residual_acted = (out.residual_action[0] != 'o'); // not "observe"
    }
    // Additive apply counter (AC3) — once per call regardless of outcome.
    g_orch_module_stats.workflow_apply_total.fetch_add(1, std::memory_order_relaxed);
    return out;
}

// Issue #2974: multi-stage workflow — ordered stages over parallel_intend
// + optional scope.watch_all + residual observe-only (#2661 unchanged).
//   Stage[i]: parallel_intend(tasks_i, batch_i)
//          → optional watch_all(watch_i)
//          → on non-Ok + stop_on_batch_fail: skip remaining stages
// Additive: workflow_run_total once per call; workflow_stage_fail_total
// per failed stage. No AgentRegistry / saga / WAL.
[[nodiscard]] inline WorkflowRunResult run_workflow(serve::Scheduler& sched, AgentScope& scope,
                                                    std::span<const WorkflowStage> stages,
                                                    ResidualReclaimPreference residual) noexcept {
    WorkflowRunResult out;
    g_orch_module_stats.workflow_run_total.fetch_add(1, std::memory_order_relaxed);
    WorkflowFailurePolicy observe{};
    observe.residual = residual;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        const auto& st = stages[i];
        ApplyWorkflowResult stage;
        // Phase A — batch under the stage ParallelPolicy (preserves
        // max_concurrency / timeout_ms / FailurePolicy fields).
        stage.batch = serve::parallel_orch::parallel_intend(sched, st.tasks, st.batch);
        // Phase B — optional scope watch under the stage AgentFailurePolicy.
        if (st.watch_scope) {
            stage.scope_watch_called = true;
            auto wres = scope.watch_all(st.stall_timeout_ms, st.watch);
            stage.scope_stalled = static_cast<int>(wres.stalled);
            stage.scope_alive = static_cast<int>(wres.alive);
            stage.scope_done = static_cast<int>(wres.done);
        }
        // Phase C — residual observe; production + explicit Cancel/JoinDrain
        // act (#3206). Soft/Report still observe-only (#2661).
        const bool batch_fail = workflow_stage_failed(stage.batch.status);
        const bool scope_residual = stage.scope_stalled > 0;
        if (batch_fail || scope_residual) {
            note_workflow_residual_reclaim_under_policy(observe);
            stage.residual_observed = true;
            out.residual_observed = true;
            stage.residual_action = apply_residual_reclaim_action(scope, observe);
            stage.residual_acted = (stage.residual_action[0] != 'o');
            if (stage.residual_acted) {
                out.residual_acted = true;
                out.residual_action = stage.residual_action;
            }
        }
        if (batch_fail) {
            ++out.stages_failed;
            g_orch_module_stats.workflow_stage_fail_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++out.stages_ok;
        }
        out.stages.push_back(std::move(stage));
        if (batch_fail && st.stop_on_batch_fail) {
            out.stopped_at = static_cast<std::uint32_t>(i + 1);
            break;
        }
    }
    return out;
}

// Issue #3125: cross-scope directory merge. Walks an explicit span of
// AgentScope* sources (caller-owned list — no global registry walk),
// applies a single CrossScopeFilter, and returns one CrossScopeSnapshot.
// Per source: 1) source_path = s->bp_scope_id() (session-local, stable),
// 2) if filter.source_scope_paths non-empty, drop sources not in list,
// 3) take directory_snapshot({}) (no per-scope filter — merge applies
// after), 4) for each entry apply alive_only / name_prefix /
// dedup_by_name, 5) stamp source_path + source_seq on survivors.
// Bumps OrchModuleStats.cross_scope_directory_total / entries_total /
// sources_total for dashboard adoption tracking. Not transactional —
// best-effort at call time (matches AgentDirectorySnapshot #2751).
// Threading: same single-owner model as #2751 directory_snapshot.
// Each per-source directory_snapshot() takes its own ScopeEnterGuard
// (#2777), so concurrent caller + ~AgentScope on the SAME source is
// detected. Cross-source: caller must serialize access to all sources
// (matches AgentScope's single-owner model per scope).
[[nodiscard]] inline CrossScopeSnapshot cross_scope_directory(std::span<AgentScope* const> sources,
                                                              CrossScopeFilter filter = {}) {
    CrossScopeSnapshot out;
    out.sources_count = sources.size();

    auto source_allowed = [&](const std::string& source_path) {
        if (filter.source_scope_paths.empty())
            return true;
        return std::find(filter.source_scope_paths.begin(), filter.source_scope_paths.end(),
                         source_path) != filter.source_scope_paths.end();
    };

    std::unordered_set<std::string> seen_names;

    for (std::size_t i = 0; i < sources.size(); ++i) {
        AgentScope* s = sources[i];
        if (s == nullptr)
            continue; // null scope — silently skip (best-effort)
        const std::string source_path(s->bp_scope_id());
        if (!source_allowed(source_path))
            continue; // whole source filtered out by allow-list

        auto snap = s->directory_snapshot({}); // no per-scope filter
        out.scopes_visited += snap.scopes_visited;

        for (const auto& e : snap.entries) {
            if (filter.alive_only && e.status != "alive") {
                ++out.entries_dropped;
                continue;
            }
            if (!filter.name_prefix.empty() &&
                e.name.compare(0, filter.name_prefix.size(), filter.name_prefix) != 0) {
                ++out.entries_dropped;
                continue;
            }
            if (filter.dedup_by_name && !seen_names.insert(e.name).second) {
                ++out.entries_dropped;
                continue;
            }
            CrossScopeEntry ce;
            ce.name = e.name;
            ce.id = e.id;
            ce.status = e.status;
            ce.scope_path = e.scope_path;
            ce.source_path = source_path;
            ce.source_seq = i;
            ce.lifecycle = e.lifecycle;
            ce.ok = e.ok;
            ce.reclaimed_deferred = e.reclaimed_deferred;
            ce.must_wait_reclaimed = e.must_wait_reclaimed;
            out.entries.push_back(std::move(ce));
        }
    }

    // OrchModuleStats mirrors for query:orch-module-stats facade (#3125).
    auto& os = g_orch_module_stats;
    os.cross_scope_directory_total.fetch_add(1, std::memory_order_relaxed);
    os.cross_scope_directory_entries_total.fetch_add(out.entries.size(), std::memory_order_relaxed);
    os.cross_scope_directory_sources_total.fetch_add(sources.size(), std::memory_order_relaxed);

    return out;
}

} // namespace aura::orch

#endif // AURA_ORCH_AGENT_SCOPE_H