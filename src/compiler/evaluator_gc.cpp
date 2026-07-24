// evaluator_gc.cpp — P1-i: GC root flush, sweep, and pair compaction
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "messaging_bridge.h"
#include "serve/gc_coordinator.h"
#include "core/gc_hooks.h"
#include "core/provenance_tracker.hh" // Issue #2026: validate_linear_provenance

module aura.compiler.evaluator;

import std;
import aura.compiler.value;
import aura.core.envframe_lifetime;
import aura.core.lifetime_pin;

namespace aura::compiler {

using types::EvalValue;
// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #206 / #1757: Evaluator::compact_pairs. Compacts the
// pairs_ arena, building a remap table for stable id
// resolution.
//
// Algorithm: linear scan, copy live pairs to the front,
// build pair_remap_[old_idx] = new_idx. Dead pairs (not
// in `live_mask`) are skipped and their old index gets
// remap to -1.
//
// Returns the number of pairs after compact as std::size_t
// (Issue #1757 — matches compact_env_frames count semantic;
// was signed int64_t).
//
// The remap table is sized to the OLD pairs_ size, even
// after compact (which shrinks pairs_). This is by
// design: a stale id (e.g., from a saved
// MutationRecord) might still be in [0, old_size). The
// remap tells us if that id is live (and what its new
// index is) or freed (-1).
std::size_t Evaluator::compact_pairs(const std::vector<bool>& live_mask) {
    const std::size_t n_old = pairs_.size();
    pair_remap_.clear();
    pair_remap_.reserve(n_old);
    // Build a new vector with only the live pairs. Use
    // move-semantics to avoid copies where possible.
    std::pmr::vector<Pair> new_pairs{&runtime_resource_};
    new_pairs.reserve(n_old); // upper bound
    std::int64_t new_idx = 0; // remap entries are signed (-1 = dead)
    for (std::size_t i = 0; i < n_old; ++i) {
        // If live_mask is empty, treat all as live.
        // If live_mask is sized to n_old, use the bit.
        // If live_mask is shorter than i, treat as dead
        // (defensive).
        const bool is_live =
            live_mask.empty() ? true : (i < live_mask.size() ? live_mask[i] : false);
        if (is_live) {
            pair_remap_.push_back(new_idx);
            new_pairs.push_back(std::move(pairs_[i]));
            ++new_idx;
        } else {
            pair_remap_.push_back(-1);
        }
    }
    pairs_ = std::move(new_pairs);
    return pairs_.size();
}
// Issue #2001: Evaluator::compact_strings. Mirror of compact_pairs for
// the string_heap_ arena. Builds string_remap_ (sized to old size,
// -1 for dead) so MutationRecord / stale string indices can resolve
// post-compact. Returns # strings after compact.
//
// string_heap_ is an `std::pmr::vector<std::string>` VALUE member
// (line 4878 of evaluator.ixx), not a pointer — unlike
// PrimitiveContext::string_heap_ which is a `std::pmr::vector<std::string>*`.
// The pmr backing (runtime_resource_) is preserved on shrink/move.
std::size_t Evaluator::compact_strings(const std::vector<bool>& live_mask) {
    const std::size_t n_old = string_heap_.size();
    string_remap_.clear();
    string_remap_.reserve(n_old);
    std::pmr::vector<std::string> new_strings{&runtime_resource_};
    new_strings.reserve(n_old);
    std::int64_t new_idx = 0;
    for (std::size_t i = 0; i < n_old; ++i) {
        const bool is_live =
            live_mask.empty() ? true : (i < live_mask.size() ? live_mask[i] : false);
        if (is_live) {
            string_remap_.push_back(new_idx);
            new_strings.push_back(std::move(string_heap_[i]));
            ++new_idx;
        } else {
            string_remap_.push_back(-1);
        }
    }
    string_heap_ = std::move(new_strings);
    return string_heap_.size();
}
// ── GC root registration (Issue #113) ──────────────────────────
//
// `flush_gc_roots` walks every vector heap this Evaluator owns and
// populates the GCRootSet with the indices of all live objects. The
// GC collector calls this during its root collection phase (after
// the safepoint has stopped all fibers on this worker, so no
// concurrent mutator can run). We additionally hold `heap_mutex()`
// so a non-fiber thread in serve-async mode can't race a concurrent
// `string_heap_.push_back` (or similar) with the walk.
//
// `gc_root_count` is the cheap version: just returns the number of
// entries that WOULD be marked, without allocating the GCRootSet.
// Useful for pre-GC metrics and unit tests that want to verify the
// root set is populated without paying for the GCRootSet heap allocs.
// Issue #1864: unlike flush_gc_roots (safepoint + heap_mutex),
// gc_root_count is public outside safepoint — it takes
// shared_lock(closures_mtx_) when walking closures_.

void Evaluator::flush_gc_roots(void* root_set_out) {
    // The opaque pointer is aura::serve::GCRootSet* (set by the
    // serve_async.cpp callback). Cast is safe because the GC
    // collector passes a real GCRootSet that the messaging bridge
    // constructed in its own TU.
    auto& out = *static_cast<aura::serve::GCRootSet*>(root_set_out);

    std::lock_guard<std::mutex> lock(heap_mutex());

    // 1. string_heap_ — every slot is a root. The pool can be
    //    compacted in the sweep phase; until then, treat them all
    //    as live. Pairs can reference strings (car/cdr), so
    //    undermarking here would dangle pair fields.
    out.string_roots.reserve(out.string_roots.size() + string_heap_.size());
    for (std::size_t i = 0; i < string_heap_.size(); ++i) {
        out.string_roots.push_back(static_cast<int64_t>(i));
    }

    // 2. pairs_ — every slot is a root (cons cells are the spine
    //    of every list / tree in the heap). Stale entries from
    //    previous gc-temp cycles are the caller's responsibility
    //    to remove before GC; we mark everything.
    out.pair_roots.reserve(out.pair_roots.size() + pairs_.size());
    for (std::size_t i = 0; i < pairs_.size(); ++i) {
        out.pair_roots.push_back(static_cast<int64_t>(i));
    }

    // 3. closures_ — only roots with id < gc_safe_closure_id_ are
    //    pinned (module-level / while-loop bodies). Anything above
    //    that watermark was created inside a temp-arena intend and
    //    is safe to collect. We walk the map and emit the safe set.
    out.closure_roots.reserve(out.closure_roots.size() + closures_.size());
    for (const auto& [id, c] : closures_) {
        if (static_cast<std::uint64_t>(id) < gc_safe_closure_id_) {
            out.closure_roots.push_back(static_cast<int64_t>(id));
        }
    }

    // 4. fiber results — s_fiber_results_ is a TU-local static in
    //    evaluator_primitives_messaging.cpp (managed by fiber:join). Each live entry
    //    is a root because the value is shared between the spawned
    //    fiber and the joiner. The static map is internally
    //    synchronized by s_fiber_results_mtx_, but at the safepoint
    //    no fiber is touching it, so we walk it directly.
    //
    //    We can't see s_fiber_results_ from this method (TU-local),
    //    so we skip the fiber_result_roots field here. The
    //    message-bridge flush hook in serve_async.cpp adds those
    //    entries separately (or the GC tolerates an empty set
    //    since the value is in closures_/string_heap_ which we
    //    already marked).

    // 5. Issue #682: compiler-managed IRClosure / EnvId roots
    //    from bridge cache + persistent IR interpreters.
    if (compiler_gc_roots_fn_ && compiler_service_) {
        compiler_gc_roots_fn_(compiler_service_, root_set_out);
    }
}

std::size_t Evaluator::gc_root_count() const {
    // Issue #1864: shared_lock closures_mtx_ while iterating closures_.
    // This is a public metrics / test API (not safepoint-only); concurrent
    // register_active_closure / free can realloc or erase the map and
    // UAF an unlocked iterator. Returns an approximate upper bound —
    // string_heap_/pairs_ size() remain unlocked (heap_mutex_ covers
    // those in flush_gc_roots / compact_sweep at safepoint).
    std::size_t n = string_heap_.size() + pairs_.size();
    std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
    for (const auto& [id, _] : closures_) {
        if (static_cast<std::uint64_t>(id) < gc_safe_closure_id_) {
            ++n;
        }
    }
    return n;
}

bool Evaluator::validate_linear_ownership_state(
    std::uint8_t linear_state, std::uint64_t frame_version, std::uint64_t current_version,
    std::uint64_t bridge_epoch, std::uint64_t current_bridge_epoch,
    std::atomic<std::uint64_t>* bridge_epoch_drift_counter) noexcept {
    // Issue #1515 / #2026: route through provenance_tracker::validate_linear_provenance
    // so GC / steal / boundary / IR share one consistency policy.
    // require_complete=false here (hot dual-check); steal/GC use require_complete
    // via probe paths that call validate_linear_provenance directly.
    using aura::core::provenance::validate_linear_provenance;
    const auto r = validate_linear_provenance(
        linear_state, /*node_id=*/0, /*provenance_id=*/0, /*mutation_id=*/0, frame_version,
        current_version, bridge_epoch, current_bridge_epoch, /*require_complete=*/false);
    if (!r.ok) {
        if (bridge_epoch_drift_counter && r.reason &&
            std::string_view(r.reason).find("bridge") != std::string_view::npos)
            bridge_epoch_drift_counter->fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Preserve legacy: bridge_epoch==0 skip is already handled inside
    // validate_linear_provenance (only mismatches when both non-zero).
    return true;
}

static void record_linear_gc_probe(Evaluator& ev, bool violation,
                                   std::atomic<std::uint64_t>* site_counter) {
    if (site_counter)
        site_counter->fetch_add(1, std::memory_order_relaxed);
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    if (!m)
        return;
    m->linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
    if (violation) {
        // Issue #1867: safety-critical violation counters use release so
        // audit / stats readers that acquire-load cannot miss a just-
        // recorded violation under concurrent probe vs dashboard.
        // Success-path tallies stay relaxed (non-safety).
        m->linear_violations_caught_total.fetch_add(1, std::memory_order_release);
        m->linear_deopt_on_mismatch_total.fetch_add(1, std::memory_order_release);
        m->linear_gc_safepoint_violations.fetch_add(1, std::memory_order_release);
        m->linear_typed_mutate_safe_fallbacks.fetch_add(1, std::memory_order_release);
        m->linear_postmutate_escape_violations_prevented_total.fetch_add(1,
                                                                         std::memory_order_release);
        // Issue #1515 / #763: GC-compiler correlation counter.
        m->linear_ownership_gc_violations_prevented_total.fetch_add(1, std::memory_order_release);
    } else {
        m->linear_check_pass_count_.fetch_add(1, std::memory_order_relaxed);
        m->linear_postmutate_guard_boundary_linear_safe_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        m->linear_postmutate_env_version_sync_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #1364 / #1515: called at GC safepoint. Linear ownership probe
// coordinates EnvFrame.version_ + IRClosure.bridge_epoch against the
// live defuse/bridge snapshots so moved or epoch-stale captures cannot
// stay as GC roots.
// Mutation × safepoint contract (docs/development/safepoint-mutation.md):
//   - STW window advertised via gc_hooks::in_gc_safepoint() / ScopedSafepoint
//   - Concurrent mutate is benign: workspace_mtx_ serializes AST writes
//   - Telemetry: mutation_in_safepoint_total on MutationBoundaryGuard entry
void Evaluator::probe_linear_ownership_at_gc_safepoint() noexcept {
    const auto current_ver = defuse_version_snapshot();
    const auto current_bridge = current_bridge_epoch();
    bool violation = false;
    // Issue #1664: canonical dual-lock order is closures → env_frames
    // (matches scan_live_closures_for_linear_captures / apply_closure).
    // Pre-#1664 took env then closures — latent deadlock if either side
    // upgrades to unique while the other holds the reverse order.
    std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
    std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
    auto* m_probe = static_cast<CompilerMetrics*>(compiler_metrics_);
    auto* drift_ctr = m_probe ? &m_probe->linear_validate_bridge_epoch_drift_total : nullptr;
    using aura::core::provenance::g_provenance_enforcement;
    using aura::core::provenance::validate_linear_provenance;
    g_provenance_enforcement().linear_provenance_gc_checks_total.fetch_add(
        1, std::memory_order_relaxed);
    for (const auto& [id, cl] : closures_) {
        (void)id;
        if (cl.bridge_epoch == 0)
            continue;
        if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size())
            continue;
        const auto& fr = env_frames_[cl.env_id];
        // Issue #2026: explicit linear × provenance validation at GC.
        // Scan binding ownership states; require complete forensic trail
        // on tracked linear roots (steal/GC closed-loop).
        bool frame_has_linear = false;
        bool frame_has_moved = false;
        for (const auto s : fr.bindings_linear_ownership_state_) {
            if (s == linear_rt::Moved)
                frame_has_moved = true;
            if (s != linear_rt::Untracked)
                frame_has_linear = true;
        }
        if (frame_has_moved) {
            const auto pr = validate_linear_provenance(
                linear_rt::Moved, static_cast<std::uint32_t>(cl.env_id), 0, 0, fr.version_,
                current_ver, cl.bridge_epoch, current_bridge, /*require_complete=*/true);
            if (!pr.ok) {
                violation = true;
                break;
            }
        }
        if (frame_has_linear) {
            // Hygiene stamp provides weak mutation/provenance when present.
            const auto& hy = aura::core::provenance::g_provenance_tracker().last_hygiene;
            const auto pr = validate_linear_provenance(
                linear_rt::Owned, static_cast<std::uint32_t>(cl.env_id), hy.node_id,
                hy.source_mutation_id, fr.version_, current_ver, cl.bridge_epoch, current_bridge,
                /*require_complete=*/false);
            if (!pr.ok ||
                !validate_linear_ownership_state(1, fr.version_, current_ver, cl.bridge_epoch,
                                                 current_bridge, drift_ctr)) {
                violation = true;
                break;
            }
        } else if (!validate_linear_ownership_state(1, fr.version_, current_ver, cl.bridge_epoch,
                                                    current_bridge, drift_ctr)) {
            // Non-linear bridged closure still dual-checks epoch (legacy).
            violation = true;
            break;
        }
    }
    record_linear_gc_probe(*this, violation, nullptr);

    // Issue #1473 / #1497: force auto-restamp on pinned StableNodeRefs
    // at GC safepoint (atomic-batch + cow-boundary registries). Unified
    // helper bumps stable_ref_validations_at_gc_safepoint +
    // boundary_pinned_refresh_count + stable_ref_steal_auto_refresh_total.
    (void)auto_restamp_pinned_stable_refs_at(StableRefRefreshSite::GcSafepoint);
}

void Evaluator::resync_linear_jit_gc_roots_after_invalidate() noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    if (m)
        m->linear_jit_gc_root_resync_total.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::int64_t> closure_roots;
    std::vector<std::int64_t> env_roots;
    collect_compiler_managed_gc_roots(closure_roots, env_roots, current_bridge_epoch());
    // Issue #1638: dual-path consistency gate at GC root collection
    // (paired site at line ~346 below is the second collection point).
    // Walk the collected env_roots through
    // ensure_env_frame_dual_path_consistent so dashboards observe
    // drift at GC time (paired with the lookup/walk sites that
    // observe drift at apply time). Steady-state target 0; non-zero
    // indicates drift between compaction and the current defuse epoch.
    for (const auto eid : env_roots) {
        (void)ensure_env_frame_dual_path_consistent(static_cast<EnvId>(eid),
                                                    "collect_gc_roots_env");
    }
    // Issue #1515: every resync is a root re-registration event for the
    // linear-ownership × GC coordination surface (#763 counters).
    if (m) {
        const auto n = closure_roots.size() + env_roots.size();
        m->linear_ownership_gc_root_registrations_total.fetch_add(n == 0 ? 1 : n,
                                                                  std::memory_order_relaxed);
        m->linear_ownership_gc_env_version_resync_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #1515: unified safepoint entry — re-register linear roots under
// the live bridge_epoch, then probe EnvFrame/version ownership. Called
// from request_gc_safepoint (immediate path) and invalidate revalidate.
// Issue #1543: callers run_linear_gc_root_audit with the correct path
// tag after this returns (GcSafepoint vs Invalidate vs Manual).
void Evaluator::sync_linear_roots_and_bridge_epoch() noexcept {
    resync_linear_jit_gc_roots_after_invalidate();
    probe_linear_ownership_at_gc_safepoint();
}

// Issue #1543: path names for audit log / query surface.
std::string_view Evaluator::linear_gc_root_audit_path_name(std::uint8_t path) noexcept {
    switch (path) {
        case kLinearGcRootAuditTypedMutate:
            return "typed_mutate";
        case kLinearGcRootAuditInvalidate:
            return "invalidate_function";
        case kLinearGcRootAuditCompact:
            return "compact_env_frames";
        case kLinearGcRootAuditJitHotSwap:
            return "jit_hot_swap";
        case kLinearGcRootAuditFiberSteal:
            return "fiber_steal";
        case kLinearGcRootAuditGcSafepoint:
            return "gc_safepoint";
        case kLinearGcRootAuditManual:
            return "manual";
        default:
            return "unknown";
    }
}

// Issue #1543: GC root registration consistency audit.
// Invariants (docs/design/linear-gc-roots.md):
//   1. registrations / stale_hits / violations / resync are monotonic
//      (never decrease across audits)
//   2. env_version_resync <= registrations (each resync bumps reg ≥1)
//   3. live_roots is finite (collect_compiler_managed size)
// Bumps linear_gc_root_audit_checks_total and appends ring log entry.
bool Evaluator::run_linear_gc_root_audit(std::uint8_t path) noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    if (m)
        m->linear_gc_root_audit_checks_total.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t reg =
        m ? m->linear_ownership_gc_root_registrations_total.load(std::memory_order_relaxed) : 0;
    const std::uint64_t stale =
        m ? m->linear_ownership_gc_root_stale_hits_total.load(std::memory_order_relaxed) : 0;
    const std::uint64_t viol =
        m ? m->linear_ownership_gc_violations_prevented_total.load(std::memory_order_relaxed) : 0;
    const std::uint64_t resync =
        m ? m->linear_ownership_gc_env_version_resync_total.load(std::memory_order_relaxed) : 0;

    // Live root set under current bridge epoch (read-only collect).
    std::vector<std::int64_t> cl_roots;
    std::vector<std::int64_t> env_roots;
    collect_compiler_managed_gc_roots(cl_roots, env_roots, current_bridge_epoch());
    // Issue #1638: dual-path consistency gate at the second GC root
    // collection site (paired with the first above). See the long-form
    // comment there for the rationale.
    for (const auto eid : env_roots) {
        (void)ensure_env_frame_dual_path_consistent(static_cast<EnvId>(eid),
                                                    "collect_gc_roots_env_2");
    }
    const std::uint64_t live = cl_roots.size() + env_roots.size();

    bool ok = true;
    // Monotonicity vs previous audit snapshot.
    if (reg < linear_gc_root_audit_prev_reg_ || stale < linear_gc_root_audit_prev_stale_ ||
        viol < linear_gc_root_audit_prev_viol_ || resync < linear_gc_root_audit_prev_resync_) {
        ok = false;
    }
    // Balance: resync events never outpace registrations (each resync
    // path bumps registrations by at least 1 — see resync_linear_jit...).
    if (resync > reg)
        ok = false;

    linear_gc_root_audit_prev_reg_ = reg;
    linear_gc_root_audit_prev_stale_ = stale;
    linear_gc_root_audit_prev_viol_ = viol;
    linear_gc_root_audit_prev_resync_ = resync;

    const auto seq = linear_gc_root_audit_seq_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = linear_gc_root_audit_ring_[seq % kLinearGcRootAuditRingSize];
    slot.seq = seq;
    slot.path = path;
    slot.ok = ok ? 1 : 0;
    slot.registrations = reg;
    slot.stale_hits = stale;
    slot.violations_prevented = viol;
    slot.env_version_resync = resync;
    slot.live_roots = live;
    linear_gc_root_audit_total_.fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void Evaluator::record_linear_violation_audit(std::uint8_t path, std::uint8_t reason,
                                              std::uint32_t env_id,
                                              std::uint64_t closure_id) noexcept {
    const auto seq = linear_violation_audit_seq_.fetch_add(1, std::memory_order_relaxed);
    auto& slot = linear_violation_audit_ring_[seq % kLinearViolationAuditRingSize];
    slot.seq = seq;
    slot.path = path;
    slot.reason = reason;
    slot.epoch = current_bridge_epoch();
    slot.defuse_version = defuse_version_snapshot();
    slot.env_id = env_id;
    slot.closure_id = closure_id;
    linear_violation_audit_total_.fetch_add(1, std::memory_order_relaxed);
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->linear_violation_audit_total.fetch_add(1, std::memory_order_relaxed);
}

void Evaluator::force_drop_or_mark_invalid(ClosureId id) noexcept {
    std::unique_lock<std::shared_mutex> cl_lock(closures_mtx_);
    auto it = closures_.find(id);
    if (it == closures_.end())
        return;
    if (it->second.bridge_epoch == 0)
        return;
    it->second.bridge_epoch = 0;
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
        m->linear_force_drop_total.fetch_add(1, std::memory_order_relaxed);
        m->linear_live_closures_marked_invalid_total.fetch_add(1, std::memory_order_relaxed);
        m->compiler_closure_safe_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    record_linear_violation_audit(kLinearGcRootAuditManual, kLinearViolReasonForceDrop,
                                  static_cast<std::uint32_t>(it->second.env_id),
                                  static_cast<std::uint64_t>(id));
}

// Issue #1568: single entry for mutation / compact / JIT / fiber / Guard
// consistency — scan + enforce_all + epoch fence + GC root audit.
Evaluator::LinearBoundaryEnforceResult
Evaluator::enforce_linear_boundary_consistency(std::uint8_t path, bool mark_all_linear) noexcept {
    LinearBoundaryEnforceResult out;
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    if (m)
        m->linear_boundary_consistency_total.fetch_add(1, std::memory_order_relaxed);

    // 1) Scan live closures; force Drop (bridge_epoch=0) on linear / Moved.
    const auto scan = scan_live_closures_for_linear_captures(
        /*mark_invalid=*/true, /*only_if_moved=*/!mark_all_linear);
    out.closures_scanned = scan.examined;
    out.marked_invalid = scan.marked_invalid;
    out.moved_violations = scan.with_moved_capture;
    if (scan.with_moved_capture > 0 || scan.marked_invalid > 0)
        out.all_safe = false;
    // Provenance audit for Moved captures marked this scan.
    if (scan.with_moved_capture > 0) {
        record_linear_violation_audit(path, kLinearViolReasonMoved, 0, 0);
    }

    // 2) EnvFrame SoA sweep — use-after-move intercept.
    const auto sweep = linear_post_mutate_enforce_all();
    out.frames_checked = sweep.frames_checked;
    if (!sweep.all_safe) {
        out.all_safe = false;
        if (m)
            m->linear_ownership_violation_prevented.fetch_add(1, std::memory_order_relaxed);
        record_linear_violation_audit(path, kLinearViolReasonMoved, 0, 0);
    }

    // 3) Epoch fence: non-zero but stale bridge_epoch → force Drop.
    //    Dual-check EnvFrame.version_ under defuse snapshot.
    {
        const auto cur_bridge = current_bridge_epoch();
        const auto cur_ver = defuse_version_snapshot();
        // Lock order: closures unique → env shared (same as scan_live).
        std::unique_lock<std::shared_mutex> cl_lock(closures_mtx_);
        std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
        for (auto& [id, cl] : closures_) {
            if (cl.bridge_epoch == 0)
                continue;
            bool drop = false;
            std::uint8_t reason = kLinearViolReasonEpochStale;
            if (cl.bridge_epoch != cur_bridge) {
                drop = true;
            } else if (cl.env_id != NULL_ENV_ID && cl.env_id < env_frames_.size()) {
                const auto& fr = env_frames_[cl.env_id];
                // Stale frame version under concurrent mutate/compact.
                if (fr.version_ != INVALID_VERSION && fr.version_ < cur_ver) {
                    // Only force-drop if frame has linear state (avoid
                    // over-invalidating pure non-linear captures).
                    bool has_linear = false;
                    for (const auto s : fr.bindings_linear_ownership_state_) {
                        if (s != linear_rt::Untracked) {
                            has_linear = true;
                            break;
                        }
                    }
                    if (has_linear) {
                        drop = true;
                        reason = kLinearViolReasonEpochStale;
                    }
                }
            }
            if (!drop)
                continue;
            cl.bridge_epoch = 0;
            ++out.epoch_fence_hits;
            ++out.marked_invalid;
            out.all_safe = false;
            if (m) {
                m->linear_epoch_fence_enforce_total.fetch_add(1, std::memory_order_relaxed);
                m->linear_force_drop_total.fetch_add(1, std::memory_order_relaxed);
                m->linear_ownership_violation_prevented.fetch_add(1, std::memory_order_relaxed);
                m->linear_live_closures_marked_invalid_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            }
            // Record under locks is fine (audit ring is independent).
            const auto seq = linear_violation_audit_seq_.fetch_add(1, std::memory_order_relaxed);
            auto& slot = linear_violation_audit_ring_[seq % kLinearViolationAuditRingSize];
            slot.seq = seq;
            slot.path = path;
            slot.reason = reason;
            slot.epoch = cur_bridge;
            slot.defuse_version = cur_ver;
            slot.env_id = static_cast<std::uint32_t>(cl.env_id);
            slot.closure_id = static_cast<std::uint64_t>(id);
            linear_violation_audit_total_.fetch_add(1, std::memory_order_relaxed);
            if (m)
                m->linear_violation_audit_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // 4) GC root registration consistency audit (monotonicity + balance).
    (void)run_linear_gc_root_audit(path);

    // 5) Issue #2026: linear × provenance consistency after enforce (shared
    // with IR / steal / GC). Soft require_complete=false so boundary stays
    // non-blocking; force_deopt mismatches still mark !all_safe.
    {
        using aura::core::provenance::validate_linear_provenance;
        const auto current_ver = defuse_version_snapshot();
        const auto current_bridge = current_bridge_epoch();
        const auto& hy = aura::core::provenance::g_provenance_tracker().last_hygiene;
        std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
        std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
        for (const auto& [id, cl] : closures_) {
            (void)id;
            if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size())
                continue;
            const auto& fr = env_frames_[cl.env_id];
            for (const auto s : fr.bindings_linear_ownership_state_) {
                if (s == linear_rt::Untracked)
                    continue;
                const auto pr = validate_linear_provenance(
                    s, static_cast<std::uint32_t>(cl.env_id), hy.node_id, hy.source_mutation_id,
                    fr.version_, current_ver, cl.bridge_epoch, current_bridge,
                    /*require_complete=*/false);
                if (!pr.ok && pr.force_deopt) {
                    out.all_safe = false;
                    if (m) {
                        m->linear_deopt_on_mismatch_total.fetch_add(1, std::memory_order_relaxed);
                        m->linear_ownership_violation_prevented.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    break;
                }
            }
            if (!out.all_safe)
                break;
        }
    }

    // 6) Mirror issue metrics aliases used by query surface.
    if (m) {
        // linear_post_mutate_enforcements already bumped per-frame in enforce;
        // also bump the aggregate total once for boundary entry visibility.
        m->linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
    }
    return out;
}

// Issue #1951: post-Guard-failure linear recovery consolidation.
// Bundles the 4-step closed-loop pattern at evaluator.ixx:12800-12820
// (linear_post_mutate_enforce_all + enforce_linear_boundary_consistency
//  + walk_active_closures + guard_failure_linear_enforce_total bump)
// into one helper call. Concrete measurable reduction: 4 calls → 1 call,
// ~14 lines of repeated pattern → 1 helper invocation. Future cycles
// can extend the helper with more recovery steps without touching the
// call site (which lives in the hot ~MutationBoundaryGuard dtor path).
Evaluator::LinearPostFailureResult
Evaluator::enforce_linear_post_failure(std::uint8_t path) noexcept {
    LinearPostFailureResult out;

    // Step 1: scan + enforce post-mutation linear pipeline (no-op return).
    (void)linear_post_mutate_enforce_all();

    // Step 2: enforce linear boundary consistency (mark all linear
    // captures invalid — same path as invalidate/compact/steal).
    auto boundary = enforce_linear_boundary_consistency(path, /*mark_all_linear=*/true);
    out.frames_checked = boundary.frames_checked;
    out.closures_scanned = boundary.closures_scanned;
    out.marked_invalid = boundary.marked_invalid;
    out.epoch_fence_hits = boundary.epoch_fence_hits;
    out.moved_violations = boundary.moved_violations;
    out.all_safe = boundary.all_safe;

    // Step 3: walk live closures (metrics-only probe; deopt driven by
    // is_bridge_stale on next apply_closure / closure_needs_safe_fallback).
    walk_active_closures([](ClosureId, Closure&) {
        // no-op; ensures registry is hot for the next apply.
    });

    // Step 4: bump the guard-failure linear-enforce metric.
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->guard_failure_linear_enforce_total.fetch_add(1, std::memory_order_relaxed);

    return out;
}

// Issue #2003: EnvFrame explicit lifetime protocol — RAII guard ctor
// site. run_envframe_lifetime_guard is the host.scan_skip_freed callback
// body; it runs scan_live_closures_for_linear_captures (which skips
// already-tombstoned / erased slots per #1665) + bumps the per-site
// envframe_lifetime_guard_runs_total counter. The trampoline cast
// ctx back to Evaluator* (always set by the wire-up sites) and forwards.
void Evaluator::run_envframe_lifetime_guard(
    aura::core::envframe_lifetime::EnvFrameLifetimeSite site) noexcept {
    scan_live_closures_for_linear_captures(/*mark_invalid=*/true);
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        m->envframe_lifetime_guard_runs_total.fetch_add(1, std::memory_order_relaxed);
    }
    (void)site;
}

void Evaluator::envframe_lifetime_trampoline(
    void* ctx, aura::core::envframe_lifetime::EnvFrameLifetimeSite site) noexcept {
    auto* ev = static_cast<Evaluator*>(ctx);
    if (ev)
        ev->run_envframe_lifetime_guard(site);
}

// Helper: build an EnvFrameLifetimeHost bound to this Evaluator. The
// returned host is passed to EnvFrameLifetimeGuard's ctor; the guard's
// dtor invokes host.scan_skip_freed(ctx, site) which dispatches via
// envframe_lifetime_trampoline to run_envframe_lifetime_guard.
static aura::core::envframe_lifetime::EnvFrameLifetimeHost
make_envframe_lifetime_host(Evaluator& ev) {
    aura::core::envframe_lifetime::EnvFrameLifetimeHost h{};
    h.ctx = static_cast<void*>(&ev);
    h.scan_skip_freed = &Evaluator::envframe_lifetime_trampoline;
    return h;
}

void Evaluator::probe_linear_ownership_on_fiber_steal() noexcept {
    // Issue #2003: EnvFrame explicit lifetime protocol — guard runs
    // scan_skip_freed_closures + bumps site-tagged counter on dtor
    // (scope exit). Instantiated at function entry so its dtor runs
    // after the enforce + restamp below complete.
    aura::core::envframe_lifetime::EnvFrameLifetimeGuard envframe_guard{
        make_envframe_lifetime_host(*this),
        aura::core::envframe_lifetime::EnvFrameLifetimeSite::FiberSteal};
    // Issue #1557 / #1568: full boundary consistency (scan all linear +
    // epoch fence + enforce_all + GC root audit).
    // Issue #1664: scan_live_closures (via enforce) uses closures → env
    // lock order; do not re-acquire env then closures here.
    const auto r =
        enforce_linear_boundary_consistency(kLinearGcRootAuditFiberSteal, /*mark_all_linear=*/true);
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    std::atomic<std::uint64_t>* site = m ? &m->linear_steal_enforced : nullptr;
    // Issue #2026: post-enforce linear × provenance closed-loop at steal.
    using aura::core::provenance::g_provenance_enforcement;
    using aura::core::provenance::validate_linear_provenance;
    g_provenance_enforcement().linear_provenance_steal_checks_total.fetch_add(
        1, std::memory_order_relaxed);
    bool prov_violation = false;
    {
        const auto current_ver = defuse_version_snapshot();
        const auto current_bridge = current_bridge_epoch();
        const auto& hy = aura::core::provenance::g_provenance_tracker().last_hygiene;
        std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
        std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
        for (const auto& [id, cl] : closures_) {
            (void)id;
            if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size())
                continue;
            const auto& fr = env_frames_[cl.env_id];
            for (const auto s : fr.bindings_linear_ownership_state_) {
                if (s == linear_rt::Untracked)
                    continue;
                const auto pr = validate_linear_provenance(
                    s, static_cast<std::uint32_t>(cl.env_id), hy.node_id, hy.source_mutation_id,
                    fr.version_, current_ver, cl.bridge_epoch, current_bridge,
                    /*require_complete=*/false);
                if (!pr.ok && pr.force_deopt) {
                    prov_violation = true;
                    break;
                }
            }
            if (prov_violation)
                break;
        }
    }
    const bool any_violation = !r.all_safe || prov_violation;
    record_linear_gc_probe(*this, any_violation, site);
    if (any_violation) {
        bump_runtime_observability_steal_ownership_violation_correlated();
        if (m) {
            m->multifiber_mutate_races_detected_total.fetch_add(1, std::memory_order_relaxed);
            m->multifiber_safe_fallback_total.fetch_add(1, std::memory_order_relaxed);
            if (prov_violation)
                m->linear_deopt_on_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1473 / #1497: unified auto-restamp at fiber-steal time.
    (void)auto_restamp_pinned_stable_refs_at(StableRefRefreshSite::Steal);
    // Note: GC root audit already ran inside enforce_linear_boundary_consistency.
}

void Evaluator::collect_compiler_managed_gc_roots(std::vector<std::int64_t>& closure_roots_out,
                                                  std::vector<std::int64_t>& env_roots_out,
                                                  std::uint64_t snapshot_bridge_epoch) const {
    // Issue #1734: caller passes a safepoint snapshot of bridge_epoch.
    // A concurrent bump (commit_panic_checkpoint / compact_env_frames)
    // can advance the live epoch before we enumerate. Detect drift and
    // filter roots against the live epoch (safer for ongoing ops).
    const auto live_epoch = current_bridge_epoch();
    if (live_epoch != snapshot_bridge_epoch) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->gc_roots_bridge_epoch_drift_total.fetch_add(1, std::memory_order_relaxed);
    }
    const auto eff_epoch = live_epoch;

    std::shared_lock<std::shared_mutex> lock(closures_mtx_);
    for (const auto& [id, cl] : closures_) {
        if (cl.bridge_epoch != 0 && cl.bridge_epoch != eff_epoch) {
            bump_compiler_root_dangling_prevented();
            // Issue #1515: stale bridge_epoch root skipped during GC walk.
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->linear_ownership_gc_root_stale_hits_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            continue;
        }
        closure_roots_out.push_back(static_cast<std::int64_t>(id));
        if (cl.env_id != NULL_ENV_ID && is_valid_env_id(cl.env_id))
            env_roots_out.push_back(static_cast<std::int64_t>(cl.env_id));
    }
}

// ── GC sweep / compaction (Issue #113 Phase 3) ──────────────
//
// `compact_sweep` is called by the GC collector's `collect()`
// after the mark phase has set the live bits in `marks`. We hold
// `heap_mutex()` because the sweep runs at the safepoint but a
// non-fiber thread in serve-async mode could still touch the heaps.
//
// For `closures_` we actually erase unmarked entries — this is the
// main memory-reclamation path (closure bodies hold arena-allocated
// state). For the vector heaps, we report the dead count without
// compaction, because compaction requires remapping all
// EvalValue / pair / cell references — that's a major refactor
// tracked separately in `binary_runtime_plan.md` (the C-runtime
// equivalent) and in a future iteration of the Aura evaluator
// (likely via a generation index table).

// Forward decl: helper is `static`, defined at file scope below
// compact_sweep (per Issue #2003 — runs scan_skip_freed on dtor).
static int run_envframe_lifetime_guard_compact_sweep_helper(Evaluator& ev);

Evaluator::CompactSweepResult Evaluator::compact_sweep(void* sweep_buffers) {
    (void)run_envframe_lifetime_guard_compact_sweep_helper(*this);
    // Issue #1732: typed CompactSweepResult (by value) — no void* cast
    // at call sites. Layout is 4×size_t, matching messaging_bridge.h
    // GCSweepResultMsg for optional bridge conversion.
    static_assert(sizeof(CompactSweepResult) == 4 * sizeof(std::size_t),
                  "CompactSweepResult must be 4×size_t (#1732 / #963)");
    CompactSweepResult result{};

    // The opaque pointer is aura::serve::GCSweepBuffers* (set by
    // the serve_async.cpp callback or directly by the GC collector
    // test). Cast is safe because both the message-bridge caller
    // and the direct test pass a real GCSweepBuffers / PassThru.
    auto* marks = static_cast<aura::serve::GCSweepBuffers*>(sweep_buffers);
    if (!marks) {
        // Issue #1866: null marks is a misconfiguration (collector
        // omitted sweep buffers), not "no dead objects". Still return
        // a zeroed CompactSweepResult (#1732 by-value; never nullptr)
        // so callers can treat empty as skip — but bump a metric so
        // the silent-failure mode is observable.
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->gc_compact_sweep_null_marks_total.fetch_add(1, std::memory_order_relaxed);
        return result; // zeroed — no work
    }

    // Issue #2005: defer destructive compact while any (ffi:pin-buffer)
    // LifetimePin is live (render hotpath soft-gate binding). The pin
    // protects an FFI buffer from UAF across this sweep; deferring the
    // sweep keeps the buffer valid until the pin is released via
    // (ffi:unpin-buffer). The pin registry's LifetimePin ctor/dtor
    // already manages the defer depth via arm/release_ffi_pin_defer.
    if (aura::gc_hooks::ffi_pin_defer_active()) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            m->ffi_defer_because_pin_total.fetch_add(1, std::memory_order_relaxed);
        }
        return result; // zeroed — deferred
    }

    // Issue #1489 / #1581: skip destructive reclaim while a
    // PanicCheckpoint recovery window is open (process-wide gc_hooks
    // depth or live panic_safe_source_ on this evaluator). Re-pin
    // path remains available via on_arena_compact_hook after the
    // window closes.
    if (aura::gc_hooks::should_defer_compact_for_pending_checkpoint() || has_panic_checkpoint()) {
        aura::gc_hooks::note_gc_sweep_skipped_pending_panic();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->gc_blocked_by_panic_total.fetch_add(1, std::memory_order_relaxed);
        bump_gc_blocked_by_pending_panic();
        return result; // all zeros — reclaim skipped
    }

    std::lock_guard<std::mutex> lock(heap_mutex());

    // Issue #2000: invalidate pinned FFI buffers during sweep. Pins may
    // reference swept closure / heap state — conservatively mark them
    // dead. FFI consumers observe validate() returning false and re-pin
    // on next use (no UAF). Per-CompilerMetrics counter bump via direct
    // wire-up (lifetime_pin module can't import evaluator; same bridge
    // pattern as #1908 file-level atomic fallbacks).
    {
        const auto n_invalidated = aura::core::lifetime::invalidate_all_pins_for_arena(0);
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            if (n_invalidated > 0)
                m->lifetime_pin_invalidations_total.fetch_add(
                    static_cast<std::uint64_t>(n_invalidated), std::memory_order_relaxed);
        }
    }

    // Issue #1865: drop pair_remap_ at the start of any successful
    // sweep path. compact_pairs() builds remaps for a prior live_mask;
    // after sweep reclaims (or reports reclaimable) heap state those
    // indices can be stale. Clearing forces callers to re-derive via
    // compact_pairs / resolve_pair identity (empty → identity). Do not
    // clear on the panic-defer early return above (no heap mutation).
    pair_remap_.clear();

    // 1. closures_ — erase unmarked entries.
    //    This is the main leak-reduction path: each closure holds
    //    an arena-allocated flat, pool, and env that can be
    //    significant memory.
    if (marks->closure_marks) {
        std::size_t before = closures_.size();
        for (auto it = closures_.begin(); it != closures_.end();) {
            int64_t id = static_cast<int64_t>(it->first);
            if (!marks->closure_marks->test(id)) {
                // Issue #1888: tombstone before erase so ClosureView
                // lifetime revalidation fails on moved-from / freed sources.
                invalidate_closure_lifetime(it->second);
                it = closures_.erase(it);
            } else {
                ++it;
            }
        }
        result.closures_freed = before - closures_.size();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            m->closure_view_dangling_prevented_total.store(
                g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
    }

    // 2. string_heap_ — Issue #2001: actually compact + remap
    //    (was report-only). Builds string_remap_ (sized to old size,
    //    -1 for dead) so MutationRecord / stale string indices can
    //    resolve via resolve_string() post-compact. Pinned FFI
    //    buffers get invalidated by the lifetime_pin block above;
    //    other consumers re-validate via workspace generation bump.
    if (marks->string_marks) {
        const std::size_t n_old = string_heap_.size();
        std::vector<bool> live_mask;
        live_mask.reserve(n_old);
        for (std::size_t i = 0; i < n_old; ++i)
            live_mask.push_back(marks->string_marks->test(i));
        const std::size_t n_new = compact_strings(live_mask);
        result.strings_freed = n_old > n_new ? n_old - n_new : 0;
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            m->gc_strings_compacted_total.fetch_add(
                static_cast<std::uint64_t>(result.strings_freed), std::memory_order_relaxed);
        }
    }

    // 3. pairs_ — Issue #2001: actually compact + remap (was report-
    //    only). Uses existing compact_pairs() + pair_remap_ (cleared
    //    above so prior remaps cannot outlive this sweep).
    if (marks->pair_marks) {
        const std::size_t n_old = pairs_.size();
        std::vector<bool> live_mask;
        live_mask.reserve(n_old);
        for (std::size_t i = 0; i < n_old; ++i)
            live_mask.push_back(marks->pair_marks->test(i));
        const std::size_t n_new = compact_pairs(live_mask);
        result.pairs_freed = n_old > n_new ? n_old - n_new : 0;
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            m->gc_pairs_remapped_total.fetch_add(static_cast<std::uint64_t>(result.pairs_freed),
                                                 std::memory_order_relaxed);
        }
    }

    // 4. fiber_results — owned by s_fiber_results_ (TU-local). The
    //    GC sweep handles those separately when the
    //    message-bridge registers a fiber_result sweep callback.
    //    We report 0 here so the totals add up correctly.
    result.fiber_results_freed = 0;

    // Issue #2004: opportunistic live_compact(Soft) after a successful
    // compact_sweep. Soft-gates under render hotpath / MutationBoundary;
    // bumps generation + invalidates LifetimePins on layout change. Wired
    // here (the C++ TU with the evaluator module imported) so per-
    // CompilerMetrics atomics can be bumped directly, same pattern as
    // #1908 macro_provenance wire-up sites.
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        m->arena_live_compact_soft_count.fetch_add(1, std::memory_order_relaxed);
    }
    const aura::ast::LiveCompactResult lc =
        arena_group_ ? arena_group_->live_compact(aura::ast::LiveCompactMode::Soft)
                     : aura::ast::LiveCompactResult{};
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        m->arena_live_compact_reclaimed_bytes_total.fetch_add(lc.bytes_reclaimed,
                                                              std::memory_order_relaxed);
        m->arena_live_compact_freelist_hits_total.fetch_add(lc.slots_recycled,
                                                            std::memory_order_relaxed);
        if (lc.invalidates_pins) {
            m->arena_live_compact_gen_restamps_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return result;
}

// Issue #2003: EnvFrame explicit lifetime protocol — compact_sweep itself
// runs the guard. The guard's dtor invokes host.scan_skip_freed (which
// dispatches to run_envframe_lifetime_guard(CompactSweep)). Scope-bound
// below the result finalization so the scan runs after remap tables are
// fully populated + bridge_epoch bump committed.
static int run_envframe_lifetime_guard_compact_sweep_helper(Evaluator& ev) {
    using namespace aura::core::envframe_lifetime;
    EnvFrameLifetimeGuard guard{make_envframe_lifetime_host(ev),
                                EnvFrameLifetimeSite::CompactSweep};
    (void)guard.site();
    return 0;
}

} // namespace aura::compiler
