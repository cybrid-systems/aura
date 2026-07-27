// provenance_tracker.hh — Issues #1180/#1500/#1564/#1630/#1877/#2037:
// full StableNodeRef provenance enforcement surface (header form for
// evaluator + tests). Complements FlatAST::StableNodeRef; does not replace it.
//
// Issue #1877 / #2037 contract (mutate hotpaths):
//   MacroIntroduced hygiene → provenance stamp + FailOnStale under sandbox
//   Strict (no silent restamp in multi-tenant AI self-modify).
//   Structural mutates that can touch MacroIntroduced nodes
//   (mutate:replace-pattern, mutate:query-and-replace, replace-subtree, …)
//   MUST either:
//     (a) fail closed under hygiene-protected (default; no :allow-macro?), OR
//     (b) when allowed, stamp provenance via record_macro_hygiene_provenance
//         and under Strict apply FailOnStale (validate; refuse silent restamp).
//   Replacement roots of MacroIntroduced matches propagate the marker so
//   hygiene survives query → mutate → re-query closed loops.

#ifndef AURA_CORE_PROVENANCE_TRACKER_HH
#define AURA_CORE_PROVENANCE_TRACKER_HH

#include <atomic>
#include <cstdint>

namespace aura::core::provenance {

inline constexpr int kProvenanceTrackerPhase = 3; // #1630 mandate full provenance
inline constexpr int kProvenanceTrackerIssue = 1630;
// Issue #2056: mandate tenant_id + provenance stamp on StableNodeRef create/rebind.
inline constexpr int kStableRefTenantMandateIssue = 2056;
// Issue #2125: stamp isolation principal on all FlatAST capture factories
// (make_ref / make_safe_ref / capture_for_fiber / make_ref_in_layer /
// make_ref_from_gen) so non-batch paths carry tenant provenance.
inline constexpr int kStableRefTenantCaptureIssue = 2125;
// Issue #2186: force all EDSL StableNodeRef / node-handle consumption
// through validate_or_refresh / ensure_valid_or_refresh (silent-stale
// zero-tolerance on query/mutate hot paths).
inline constexpr int kEdslValidateOrRefreshIssue = 2186;

// Policy for ensure_valid_or_refresh (AC).
enum class AutoRefreshPolicy : std::uint8_t {
    Off = 0,
    AutoRefreshOnBoundary = 1, // default production: refresh when stale
    FailOnStale = 2,           // validate only; no restamp
};

// Process-wide enforcement counters (#1564 AC3).
struct ProvenanceEnforcementMetrics {
    std::atomic<std::uint64_t> stable_ref_auto_refresh_total{0};
    std::atomic<std::uint64_t> stable_ref_epoch_fence_hit_total{0};
    std::atomic<std::uint64_t> cross_layer_provenance_mismatch_total{0};
    std::atomic<std::uint64_t> ensure_valid_calls_total{0};
    std::atomic<std::uint64_t> ensure_valid_success_total{0};
    std::atomic<std::uint64_t> ensure_valid_fail_total{0};
    std::atomic<std::uint64_t> fiber_id_mismatch_total{0};
    std::atomic<std::uint64_t> policy_enforced_total{0};
    std::atomic<std::uint64_t> hot_path_auto_refresh_total{0};
    // Issue #1630 AC counters (aliases for Agent dashboards).
    std::atomic<std::uint64_t> boundary_pinned_auto_restamp_total{0};
    std::atomic<std::uint64_t> cross_cow_provenance_enforced_total{0};
    // Issue #1877: hygiene-protected / MacroIntroduced gates stamped into
    // provenance tracker (audit log + StableNodeRef-style record).
    std::atomic<std::uint64_t> macro_hygiene_provenance_hits_total{0};
    // Issue #1877: Strict sandbox engaged FailOnStale provenance policy.
    std::atomic<std::uint64_t> fail_on_stale_strict_sandbox_total{0};
    // Issue #2056: tenant stamp mandate + cross-tenant ref use denials.
    std::atomic<std::uint64_t> stable_ref_tenant_stamp_total{0};
    std::atomic<std::uint64_t> stable_ref_cross_tenant_deny_total{0};
    std::atomic<std::uint64_t> stable_ref_tenant_preserved_on_refresh_total{0};
    // Issue #2125: stamps applied by FlatAST capture factories under an
    // active isolation principal (make_ref family / children_stable).
    std::atomic<std::uint64_t> stable_ref_tenant_stamp_capture_total{0};
    // Optional: isolation enabled + principal set but ref.tenant_id still 0
    // at a use-site check (soft counter; no default deny — AC5 permissive).
    std::atomic<std::uint64_t> stable_ref_tenant_stamp_zero_rejected_total{0};
    // Issue #2026: linear ownership × provenance consistency closed-loop.
    std::atomic<std::uint64_t> linear_provenance_checks_total{0};
    std::atomic<std::uint64_t> linear_provenance_ok_total{0};
    std::atomic<std::uint64_t> linear_provenance_mismatch_total{0};
    std::atomic<std::uint64_t> linear_provenance_moved_live_total{0};
    std::atomic<std::uint64_t> linear_provenance_incomplete_total{0};
    std::atomic<std::uint64_t> linear_provenance_deopt_total{0};
    std::atomic<std::uint64_t> linear_provenance_steal_checks_total{0};
    std::atomic<std::uint64_t> linear_provenance_gc_checks_total{0};
};

inline ProvenanceEnforcementMetrics& g_provenance_enforcement() noexcept {
    static ProvenanceEnforcementMetrics m;
    return m;
}

inline void record_auto_refresh(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_auto_refresh_total.fetch_add(n,
                                                                       std::memory_order_relaxed);
}
inline void record_epoch_fence_hit(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_epoch_fence_hit_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_cross_layer_mismatch(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().cross_layer_provenance_mismatch_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_ensure_valid_call() noexcept {
    g_provenance_enforcement().ensure_valid_calls_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_ensure_valid_success() noexcept {
    g_provenance_enforcement().ensure_valid_success_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_ensure_valid_fail() noexcept {
    g_provenance_enforcement().ensure_valid_fail_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_fiber_id_mismatch() noexcept {
    g_provenance_enforcement().fiber_id_mismatch_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_policy_enforced() noexcept {
    g_provenance_enforcement().policy_enforced_total.fetch_add(1, std::memory_order_relaxed);
}
inline void record_hot_path_auto_refresh(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().hot_path_auto_refresh_total.fetch_add(n, std::memory_order_relaxed);
}
inline void record_boundary_pinned_auto_restamp(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().boundary_pinned_auto_restamp_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_cross_cow_provenance_enforced(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().cross_cow_provenance_enforced_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_macro_hygiene_provenance_hit(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().macro_hygiene_provenance_hits_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_fail_on_stale_strict_sandbox(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().fail_on_stale_strict_sandbox_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_stable_ref_tenant_stamp(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_tenant_stamp_total.fetch_add(n,
                                                                       std::memory_order_relaxed);
}
inline void record_stable_ref_cross_tenant_deny(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_cross_tenant_deny_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_stable_ref_tenant_preserved_on_refresh(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_tenant_preserved_on_refresh_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_stable_ref_tenant_stamp_capture(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_tenant_stamp_capture_total.fetch_add(
        n, std::memory_order_relaxed);
}
inline void record_stable_ref_tenant_stamp_zero_rejected(std::uint64_t n = 1) noexcept {
    g_provenance_enforcement().stable_ref_tenant_stamp_zero_rejected_total.fetch_add(
        n, std::memory_order_relaxed);
}

// Issue #2125: process-wide isolation capture principal for FlatAST
// make_ref family. Written by WorkspaceIsolationPolicy::set_current_tenant
// (and clear_for_test). Zero = isolation off / unset principal → capture
// leaves tenant_id 0 (permissive). Hot-path atomic; no mutex.
inline std::atomic<std::uint64_t>& g_isolation_capture_tenant() noexcept {
    static std::atomic<std::uint64_t> t{0};
    return t;
}
inline void set_isolation_capture_tenant(std::uint64_t tid) noexcept {
    g_isolation_capture_tenant().store(tid, std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t isolation_capture_tenant() noexcept {
    return g_isolation_capture_tenant().load(std::memory_order_relaxed);
}

// Issue #2037: process-wide mutate hotpath hygiene restamp / fail counters
// (mirrored into CompilerMetrics when available).
inline std::atomic<std::uint64_t>& g_hygiene_mutate_restamp_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline std::atomic<std::uint64_t>& g_hygiene_mutate_fail_on_stale_total() noexcept {
    static std::atomic<std::uint64_t> v{0};
    return v;
}
inline void record_hygiene_mutate_restamp(std::uint64_t n = 1) noexcept {
    g_hygiene_mutate_restamp_total().fetch_add(n, std::memory_order_relaxed);
}
inline void record_hygiene_mutate_fail_on_stale(std::uint64_t n = 1) noexcept {
    g_hygiene_mutate_fail_on_stale_total().fetch_add(n, std::memory_order_relaxed);
    record_fail_on_stale_strict_sandbox(n);
}

// Issue #2026: linear ownership state codes (mirror linear_rt without
// depending on the evaluator module).
inline constexpr std::uint8_t kLinearUntracked = 0;
inline constexpr std::uint8_t kLinearOwned = 1;
inline constexpr std::uint8_t kLinearBorrowed = 2;
inline constexpr std::uint8_t kLinearMutBorrowed = 3;
inline constexpr std::uint8_t kLinearMoved = 4;

// Result of validate_linear_provenance (shared by steal / GC / IR / boundary).
struct LinearProvenanceResult {
    bool ok = true;
    bool force_deopt = false; // mismatch severe enough to drop/deopt
    const char* reason = nullptr;
};

// Issue #2103 / #2182 / #2207 / #2222: process-wide linear enforce mode
// for IR hot path + shared validate_linear_provenance callers.
//
// ── Decision table (Issue #2222; Agent closed-loop policy) ─────────────
//   production_defaults (Restricted/Strict sandbox)  → process Strict
//   AURA_SANDBOX=off / set_linear_enforce_mode(Soft)  → process Soft
//   inside MutationBoundary (fiber-local depth > 0)   → effective Strict
//     (when g_force_strict_on_mutation_boundary, default on)
//   AURA_LINEAR_ENFORCE=strict|soft                  → process override
//   #2108 composite escape hard-block                → always on (belt)
//
// Production default is Strict (#2207): incomplete forensic trail on
// Move/Borrow/MutBorrow/Drop or dual-path materialize hard-fails (no
// silent half-state continue). Soft is an explicit opt-in for
// experiments / unit Soft-path tests via set_linear_enforce_mode(Soft)
// or AURA_LINEAR_ENFORCE=soft / AURA_SANDBOX=off (security_defaults).
// Steal/GC may still pass require_complete=true regardless of Soft for
// live-root safety.
//
// Issue #2222: MutationBoundary forces *effective* Strict for the
// calling fiber without flipping the process-wide Soft mode (multi-
// fiber safe). Composite #2108 escape hard-block remains independent.
enum class LinearEnforceMode : std::uint8_t {
    Soft = 0,
    Strict = 1,
};

// Issue #2207 AC1: process-wide default Strict (was Soft under #2103).
inline std::atomic<std::uint32_t> g_linear_enforce_mode{
    static_cast<std::uint32_t>(LinearEnforceMode::Strict)};
// Strict hard-fail samples (incomplete trail or other require_complete fail).
// Issue #2207 AC4: also exposed as linear_provenance_hard_fail_total.
inline std::atomic<std::uint64_t> g_linear_strict_hard_fail_total{0};
// Soft incomplete samples that continued (require_complete=false path).
inline std::atomic<std::uint64_t> g_linear_soft_incomplete_continue_total{0};

// Issue #2222: force effective Strict while MutationBoundary holds
// (fiber-local depth). Default on so Soft process mode still early-
// detects incomplete linear×provenance under Agent mutate.
inline std::atomic<std::uint32_t> g_force_strict_on_mutation_boundary{1};
// Outermost boundary enter that forced Soft process → effective Strict.
inline std::atomic<std::uint64_t> g_linear_enforce_mode_forced_boundary_total{0};
// Fiber-local nesting depth for boundary Strict hold.
inline thread_local int s_linear_enforce_boundary_strict_depth = 0;

// Issue #2222 stamp for query surfaces / Agent discovery.
inline constexpr int kLinearEnforceBoundaryAlignIssue = 2222;

// Issue #2207: AC4 public alias for Strict hard-fail counter.
[[nodiscard]] inline std::atomic<std::uint64_t>&
linear_provenance_hard_fail_total_atomic() noexcept {
    return g_linear_strict_hard_fail_total;
}

[[nodiscard]] inline LinearEnforceMode linear_enforce_mode() noexcept {
    return static_cast<LinearEnforceMode>(g_linear_enforce_mode.load(std::memory_order_relaxed));
}

inline void set_linear_enforce_mode(LinearEnforceMode m) noexcept {
    g_linear_enforce_mode.store(static_cast<std::uint32_t>(m), std::memory_order_relaxed);
}

inline void set_force_strict_on_mutation_boundary(bool on) noexcept {
    g_force_strict_on_mutation_boundary.store(on ? 1u : 0u, std::memory_order_relaxed);
}
[[nodiscard]] inline bool force_strict_on_mutation_boundary() noexcept {
    return g_force_strict_on_mutation_boundary.load(std::memory_order_relaxed) != 0;
}

// Issue #2222: MutationBoundaryGuard enter — arm fiber-local Strict hold.
// Returns true when this call armed a new Soft→Strict force (outermost
// under process Soft). Nested enters only bump depth.
inline bool mutation_boundary_push_linear_enforce_strict() noexcept {
    if (!force_strict_on_mutation_boundary())
        return false;
    bool forced = false;
    if (s_linear_enforce_boundary_strict_depth == 0 &&
        linear_enforce_mode() != LinearEnforceMode::Strict) {
        g_linear_enforce_mode_forced_boundary_total.fetch_add(1, std::memory_order_relaxed);
        forced = true;
    }
    ++s_linear_enforce_boundary_strict_depth;
    return forced;
}

// Issue #2222: MutationBoundaryGuard exit — drop one Strict-hold level.
inline void mutation_boundary_pop_linear_enforce_strict() noexcept {
    if (s_linear_enforce_boundary_strict_depth <= 0)
        return;
    --s_linear_enforce_boundary_strict_depth;
}

[[nodiscard]] inline bool linear_enforce_boundary_strict_active() noexcept {
    return s_linear_enforce_boundary_strict_depth > 0;
}

// Effective mode: boundary hold wins over process Soft (#2222).
[[nodiscard]] inline LinearEnforceMode linear_enforce_effective_mode() noexcept {
    if (s_linear_enforce_boundary_strict_depth > 0)
        return LinearEnforceMode::Strict;
    return linear_enforce_mode();
}

// IR hot path / dual-path apply: true when Strict mode is active OR
// fiber is under MutationBoundary Strict hold (#2222).
// Steal/GC enforce paths that always require complete pass true explicitly.
[[nodiscard]] inline bool linear_enforce_require_complete() noexcept {
    if (s_linear_enforce_boundary_strict_depth > 0)
        return true;
    return linear_enforce_mode() == LinearEnforceMode::Strict;
}

// Test harness Soft opt-in: Soft-mode unit tests force Soft explicitly
// (AC3). Does NOT change the production process default (Strict).
inline void reset_linear_enforce_mode_for_test() noexcept {
    g_linear_enforce_mode.store(static_cast<std::uint32_t>(LinearEnforceMode::Soft),
                                std::memory_order_relaxed);
    g_linear_strict_hard_fail_total.store(0, std::memory_order_relaxed);
    g_linear_soft_incomplete_continue_total.store(0, std::memory_order_relaxed);
    g_force_strict_on_mutation_boundary.store(1, std::memory_order_relaxed);
    g_linear_enforce_mode_forced_boundary_total.store(0, std::memory_order_relaxed);
    s_linear_enforce_boundary_strict_depth = 0;
}

// Restore production default Strict after Soft unit suites (AC1).
inline void restore_linear_enforce_production_default_for_test() noexcept {
    g_linear_enforce_mode.store(static_cast<std::uint32_t>(LinearEnforceMode::Strict),
                                std::memory_order_relaxed);
    g_force_strict_on_mutation_boundary.store(1, std::memory_order_relaxed);
    s_linear_enforce_boundary_strict_depth = 0;
}

// Issue #2026: unified linear ownership + provenance consistency check.
//
// Policy (shared across fiber-steal, GC safepoint, MutationBoundary failure,
// and IR executor linear ops):
//   - Untracked: always ok (no linear root)
//   - Moved as a live root: mismatch + force_deopt (use-after-move)
//   - Owned/Borrowed/MutBorrowed with stale frame_version: mismatch + deopt
//   - bridge_epoch != 0 and != current: mismatch + deopt (steal/GC domain)
//   - Tracked linear with both provenance_id==0 and mutation_id==0:
//     incomplete forensic trail → bump incomplete; force_deopt when
//     require_complete=true (steal/GC enforce paths pass true; IR
//     Move/Borrow/Drop + dual-path apply use linear_enforce_require_complete()
//     — production Strict #2207 / #2103, Soft only explicit opt-in)
//
// node_id is for audit/forensics (env_id or AST node); 0 when unavailable.
[[nodiscard]] inline LinearProvenanceResult
validate_linear_provenance(std::uint8_t linear_state, std::uint32_t node_id = 0,
                           std::uint32_t provenance_id = 0, std::uint64_t mutation_id = 0,
                           std::uint64_t frame_version = 0, std::uint64_t current_version = 0,
                           std::uint64_t bridge_epoch = 0, std::uint64_t current_bridge_epoch = 0,
                           bool require_complete = false) noexcept {
    (void)node_id;
    auto& m = g_provenance_enforcement();
    m.linear_provenance_checks_total.fetch_add(1, std::memory_order_relaxed);
    LinearProvenanceResult r;

    if (linear_state == kLinearUntracked) {
        m.linear_provenance_ok_total.fetch_add(1, std::memory_order_relaxed);
        return r;
    }

    // Moved must never remain a live GC/steal root.
    if (linear_state == kLinearMoved) {
        r.ok = false;
        r.force_deopt = true;
        r.reason = "Moved linear live root";
        m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_moved_live_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
        m.cross_layer_provenance_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        return r;
    }

    // EnvFrame version drift (steal / mutate concurrent with GC).
    if (current_version != 0 && frame_version != 0 && frame_version < current_version) {
        r.ok = false;
        r.force_deopt = true;
        r.reason = "linear EnvFrame version stale";
        m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
        return r;
    }

    // Bridge epoch drift across COW / compact / steal.
    if (bridge_epoch != 0 && current_bridge_epoch != 0 && bridge_epoch != current_bridge_epoch) {
        r.ok = false;
        r.force_deopt = true;
        r.reason = "linear bridge_epoch mismatch";
        m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
        m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
        return r;
    }

    // Tracked linear without forensic provenance (incomplete chain).
    if (provenance_id == 0 && mutation_id == 0) {
        m.linear_provenance_incomplete_total.fetch_add(1, std::memory_order_relaxed);
        if (require_complete) {
            r.ok = false;
            r.force_deopt = true;
            r.reason = "linear provenance incomplete";
            m.linear_provenance_mismatch_total.fetch_add(1, std::memory_order_release);
            m.linear_provenance_deopt_total.fetch_add(1, std::memory_order_release);
            // Issue #2103: Strict / require_complete hard-fail sample.
            g_linear_strict_hard_fail_total.fetch_add(1, std::memory_order_relaxed);
            return r;
        }
        // Soft (#2103): incomplete forensic trail — metric only, continue.
        g_linear_soft_incomplete_continue_total.fetch_add(1, std::memory_order_relaxed);
    }

    m.linear_provenance_ok_total.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// Completeness ratio in basis points (0–10000): ok / checks.
[[nodiscard]] inline std::uint64_t linear_provenance_consistency_bp() noexcept {
    auto& m = g_provenance_enforcement();
    const auto c = m.linear_provenance_checks_total.load(std::memory_order_relaxed);
    const auto ok = m.linear_provenance_ok_total.load(std::memory_order_relaxed);
    return c > 0 ? (ok * 10000u) / c : 10000u;
}

// Issue #1877: last MacroIntroduced hygiene → provenance stamp so truncated
// blame chains can append a hygiene frame (traceable under AI self-modify).
struct HygieneProvenanceStamp {
    std::uint32_t node_id = 0;
    std::uint64_t tenant_id = 0;
    std::uint64_t source_mutation_id = 0;
    std::uint32_t fiber_id = 0;
    std::uint64_t seq = 0;
};

// Validate tenant_id against current principal (hot-path helper for #1877).
// Zero on either side is treated as "unset" (compatible with legacy refs).
[[nodiscard]] inline bool tenant_ids_compatible(std::uint64_t ref_tenant,
                                                std::uint64_t current_tenant) noexcept {
    if (ref_tenant == 0 || current_tenant == 0)
        return true;
    return ref_tenant == current_tenant;
}

// Issue #2056: central StableNodeRef tenant + fiber stamp (layout fields only).
// Called from Evaluator::stamp_stable_ref / create helpers. Does not touch
// gen/wrap/cow — those stay FlatAST capture responsibilities.
// `tenant_id` may be 0 (unset / single-tenant). Always writes tenant_id so
// create paths never leave a stale foreign stamp when principal is 0.
template <typename StableRefT>
inline void stamp_stable_ref_fields(StableRefT& ref, std::uint64_t tenant_id,
                                    std::uint32_t fiber_id = 0) noexcept {
    ref.tenant_id = tenant_id;
    if (fiber_id != 0 && ref.fiber_id == 0)
        ref.fiber_id = fiber_id;
    record_stable_ref_tenant_stamp();
}

// Issue #2125: stamp from isolation capture principal when active.
// No-op when principal is 0 (isolation off / unset) so raw make_ref
// stays unstamped for single-tenant and #2056 "capability-only" paths.
// Shares stamp_stable_ref_fields with Evaluator::stamp_stable_ref /
// pin_node_for_atomic_batch (defense-in-depth on the batch path).
template <typename StableRefT>
inline bool maybe_stamp_stable_ref_isolation_tenant(StableRefT& ref,
                                                    std::uint32_t fiber_id = 0) noexcept {
    const auto tid = isolation_capture_tenant();
    if (tid == 0)
        return false;
    stamp_stable_ref_fields(ref, tid, fiber_id);
    record_stable_ref_tenant_stamp_capture();
    return true;
}

// Forward decl so reset can clear last_hygiene on the process-wide tracker.
struct ProvenanceTracker;
inline ProvenanceTracker& g_provenance_tracker() noexcept;

struct ProvenanceStatsSnapshot {
    std::uint64_t auto_refresh = 0;
    std::uint64_t epoch_fence_hit = 0;
    std::uint64_t cross_layer_mismatch = 0;
    std::uint64_t ensure_calls = 0;
    std::uint64_t ensure_success = 0;
    std::uint64_t ensure_fail = 0;
    std::uint64_t fiber_mismatch = 0;
    std::uint64_t policy_enforced = 0;
    std::uint64_t hot_path_refresh = 0;
    std::uint64_t boundary_pinned_auto_restamp = 0;
    std::uint64_t cross_cow_provenance_enforced = 0;
    std::uint64_t macro_hygiene_provenance_hits = 0;
    std::uint64_t fail_on_stale_strict_sandbox = 0;
    // Issue #2056
    std::uint64_t tenant_stamps = 0;
    std::uint64_t cross_tenant_denies = 0;
    std::uint64_t tenant_preserved_on_refresh = 0;
    // Issue #2125
    std::uint64_t tenant_stamp_capture = 0;
    std::uint64_t tenant_stamp_zero_rejected = 0;
    int phase = kProvenanceTrackerPhase;
    int issue = kProvenanceTrackerIssue;
};

[[nodiscard]] inline ProvenanceStatsSnapshot snapshot_provenance_enforcement() noexcept {
    auto& m = g_provenance_enforcement();
    return ProvenanceStatsSnapshot{
        m.stable_ref_auto_refresh_total.load(std::memory_order_relaxed),
        m.stable_ref_epoch_fence_hit_total.load(std::memory_order_relaxed),
        m.cross_layer_provenance_mismatch_total.load(std::memory_order_relaxed),
        m.ensure_valid_calls_total.load(std::memory_order_relaxed),
        m.ensure_valid_success_total.load(std::memory_order_relaxed),
        m.ensure_valid_fail_total.load(std::memory_order_relaxed),
        m.fiber_id_mismatch_total.load(std::memory_order_relaxed),
        m.policy_enforced_total.load(std::memory_order_relaxed),
        m.hot_path_auto_refresh_total.load(std::memory_order_relaxed),
        m.boundary_pinned_auto_restamp_total.load(std::memory_order_relaxed),
        m.cross_cow_provenance_enforced_total.load(std::memory_order_relaxed),
        m.macro_hygiene_provenance_hits_total.load(std::memory_order_relaxed),
        m.fail_on_stale_strict_sandbox_total.load(std::memory_order_relaxed),
        m.stable_ref_tenant_stamp_total.load(std::memory_order_relaxed),
        m.stable_ref_cross_tenant_deny_total.load(std::memory_order_relaxed),
        m.stable_ref_tenant_preserved_on_refresh_total.load(std::memory_order_relaxed),
        m.stable_ref_tenant_stamp_capture_total.load(std::memory_order_relaxed),
        m.stable_ref_tenant_stamp_zero_rejected_total.load(std::memory_order_relaxed),
        kProvenanceTrackerPhase,
        kProvenanceTrackerIssue,
    };
}

// Lightweight tracker (Phase 2): mutation_id + epoch fence bookkeeping.
// FlatAST::StableNodeRef remains the production handle; this tracks policy metrics.
struct ProvenanceTracker {
    std::uint64_t records = 0;
    std::uint64_t validations = 0;
    std::uint64_t dirty_propagations = 0;
    AutoRefreshPolicy policy = AutoRefreshPolicy::AutoRefreshOnBoundary;
    // Issue #1877: last hygiene stamp lives on the process-wide tracker so
    // module TUs (type_checker) and non-module TUs (tests/audit) share it.
    HygieneProvenanceStamp last_hygiene{};

    void record_mutation() noexcept { ++records; }
    void set_policy(AutoRefreshPolicy p) noexcept { policy = p; }
    [[nodiscard]] AutoRefreshPolicy get_policy() const noexcept { return policy; }

    // mutation_id check (legacy scaffold API, still used by demos).
    [[nodiscard]] bool validate_mutation_id(std::uint64_t captured,
                                            std::uint64_t current_source) const noexcept {
        ++const_cast<ProvenanceTracker*>(this)->validations;
        return captured == 0 || captured == current_source;
    }

    // Epoch fence: true if still fresh (captured == current or captured==0 legacy).
    [[nodiscard]] bool epoch_fence_ok(std::uint64_t captured_epoch,
                                      std::uint64_t current_epoch) const noexcept {
        if (captured_epoch == 0)
            return true; // unstamped / legacy
        return captured_epoch == current_epoch;
    }
};

// Namespace-scope inline (not function-local static) so module TUs that
// include this header in the global module fragment share one instance
// with non-module TUs (tests / typed_mutation_audit). Issue #1877.
inline ProvenanceTracker g_provenance_tracker_storage{};

inline ProvenanceTracker& g_provenance_tracker() noexcept {
    return g_provenance_tracker_storage;
}

// Alias onto process-wide tracker (module-safe shared state).
inline HygieneProvenanceStamp& g_last_hygiene_provenance_stamp() noexcept {
    return g_provenance_tracker().last_hygiene;
}

// Stamp hygiene violation into process-wide provenance tracker + last stamp.
// tenant_id from workspace_isolation / CapabilityGrant principal.
// Issue #1877: called from TypedMutationAudit hygiene gate so both audit
// trail and provenance tracker see MacroIntroduced blocks.
inline void record_macro_hygiene_provenance(std::uint32_t node_id, std::uint64_t tenant_id = 0,
                                            std::uint64_t mutation_id = 0,
                                            std::uint32_t fiber_id = 0) noexcept {
    auto& tr = g_provenance_tracker();
    tr.record_mutation();
    record_macro_hygiene_provenance_hit();
    auto& s = tr.last_hygiene;
    s.node_id = node_id;
    s.tenant_id = tenant_id;
    s.source_mutation_id = mutation_id;
    s.fiber_id = fiber_id;
    ++s.seq;
}

inline void reset_provenance_enforcement_for_test() noexcept {
    auto& m = g_provenance_enforcement();
    m.stable_ref_auto_refresh_total.store(0, std::memory_order_relaxed);
    m.stable_ref_epoch_fence_hit_total.store(0, std::memory_order_relaxed);
    m.cross_layer_provenance_mismatch_total.store(0, std::memory_order_relaxed);
    m.ensure_valid_calls_total.store(0, std::memory_order_relaxed);
    m.ensure_valid_success_total.store(0, std::memory_order_relaxed);
    m.ensure_valid_fail_total.store(0, std::memory_order_relaxed);
    m.fiber_id_mismatch_total.store(0, std::memory_order_relaxed);
    m.policy_enforced_total.store(0, std::memory_order_relaxed);
    m.hot_path_auto_refresh_total.store(0, std::memory_order_relaxed);
    m.boundary_pinned_auto_restamp_total.store(0, std::memory_order_relaxed);
    m.cross_cow_provenance_enforced_total.store(0, std::memory_order_relaxed);
    m.macro_hygiene_provenance_hits_total.store(0, std::memory_order_relaxed);
    m.fail_on_stale_strict_sandbox_total.store(0, std::memory_order_relaxed);
    m.stable_ref_tenant_stamp_total.store(0, std::memory_order_relaxed);
    m.stable_ref_cross_tenant_deny_total.store(0, std::memory_order_relaxed);
    m.stable_ref_tenant_preserved_on_refresh_total.store(0, std::memory_order_relaxed);
    m.stable_ref_tenant_stamp_capture_total.store(0, std::memory_order_relaxed);
    m.stable_ref_tenant_stamp_zero_rejected_total.store(0, std::memory_order_relaxed);
    set_isolation_capture_tenant(0);
    m.linear_provenance_checks_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_ok_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_mismatch_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_moved_live_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_incomplete_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_deopt_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_steal_checks_total.store(0, std::memory_order_relaxed);
    m.linear_provenance_gc_checks_total.store(0, std::memory_order_relaxed);
    g_provenance_tracker().last_hygiene = {};
}

} // namespace aura::core::provenance

#endif // AURA_CORE_PROVENANCE_TRACKER_HH
