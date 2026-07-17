// evaluator_gc.cpp — P1-i: GC root flush, sweep, and pair compaction
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "messaging_bridge.h"
#include "serve/gc_coordinator.h"
#include "core/gc_hooks.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

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

// Issue #206: Evaluator::compact_pairs. Compacts the
// pairs_ arena, building a remap table for stable id
// resolution.
//
// Algorithm: linear scan, copy live pairs to the front,
// build pair_remap_[old_idx] = new_idx. Dead pairs (not
// in `live_mask`) are skipped and their old index gets
// remap to -1.
//
// Returns the number of pairs after compact.
//
// The remap table is sized to the OLD pairs_ size, even
// after compact (which shrinks pairs_). This is by
// design: a stale id (e.g., from a saved
// MutationRecord) might still be in [0, old_size). The
// remap tells us if that id is live (and what its new
// index is) or freed (-1).
std::int64_t Evaluator::compact_pairs(const std::vector<bool>& live_mask) {
    const std::size_t n_old = pairs_.size();
    pair_remap_.clear();
    pair_remap_.reserve(n_old);
    // Build a new vector with only the live pairs. Use
    // move-semantics to avoid copies where possible.
    std::pmr::vector<Pair> new_pairs{&runtime_resource_};
    new_pairs.reserve(n_old); // upper bound
    std::int64_t new_idx = 0;
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
    return static_cast<std::int64_t>(pairs_.size());
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
    // No lock — called at safepoint time. Returns upper bound.
    std::size_t n = string_heap_.size() + pairs_.size();
    for (const auto& [id, _] : closures_) {
        if (static_cast<std::uint64_t>(id) < gc_safe_closure_id_) {
            ++n;
        }
    }
    return n;
}

bool Evaluator::validate_linear_ownership_state(std::uint8_t linear_state,
                                                std::uint64_t frame_version,
                                                std::uint64_t current_version,
                                                std::uint64_t bridge_epoch,
                                                std::uint64_t current_bridge_epoch) noexcept {
    // Issue #1515: untracked (0) always ok; Moved (4) is a terminal
    // state that fails EnvFrame / bridge coordination checks so GC
    // and runtime never treat moved values as live roots.
    if (linear_state == 0)
        return true;
    if (linear_state == 4) // Moved — never a valid live ownership root
        return false;
    if (frame_version < current_version)
        return false;
    if (bridge_epoch != 0 && bridge_epoch != current_bridge_epoch)
        return false;
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
        m->linear_violations_caught_total.fetch_add(1, std::memory_order_relaxed);
        m->linear_deopt_on_mismatch_total.fetch_add(1, std::memory_order_relaxed);
        m->linear_gc_safepoint_violations.fetch_add(1, std::memory_order_relaxed);
        m->linear_typed_mutate_safe_fallbacks.fetch_add(1, std::memory_order_relaxed);
        m->linear_postmutate_escape_violations_prevented_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
        // Issue #1515 / #763: GC-compiler correlation counter.
        m->linear_ownership_gc_violations_prevented_total.fetch_add(1, std::memory_order_relaxed);
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
    std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
    std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
    for (const auto& [id, cl] : closures_) {
        (void)id;
        if (cl.bridge_epoch == 0)
            continue;
        if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size())
            continue;
        const auto& fr = env_frames_[cl.env_id];
        // EnvFrame.version_ × bridge_epoch coordination (Issue #1515).
        // (IRClosure.env_version is dual-checked on the IR apply path
        // in #1513; TW Closure map uses frame.version_ here.)
        if (!validate_linear_ownership_state(1, fr.version_, current_ver, cl.bridge_epoch,
                                             current_bridge)) {
            violation = true;
            break;
        }
    }
    record_linear_gc_probe(*this, violation, nullptr);

    // Issue #1473: also force validate_or_refresh on pinned StableNodeRefs
    // at GC safepoint. The closure/probe above covers linear ownership,
    // but pinned StableNodeRefs (boundary_pinned=true) need a separate
    // refresh sweep — otherwise they can outlive their captured
    // generation through a GC compact. Walk cow_boundary_pinned_refs_
    // under the mutex and bump stable_ref_validations_at_gc_safepoint.
    if (auto* ws = workspace_flat()) {
        std::size_t validated = 0;
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        for (auto& pinned : cow_boundary_pinned_refs_) {
            if (pinned.validate_or_refresh(*ws))
                ++validated;
        }
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->stable_ref_validations_at_gc_safepoint.fetch_add(validated,
                                                                std::memory_order_relaxed);
    }
}

void Evaluator::resync_linear_jit_gc_roots_after_invalidate() noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    if (m)
        m->linear_jit_gc_root_resync_total.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::int64_t> closure_roots;
    std::vector<std::int64_t> env_roots;
    collect_compiler_managed_gc_roots(closure_roots, env_roots, current_bridge_epoch());
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

void Evaluator::probe_linear_ownership_on_fiber_steal() noexcept {
    const auto current_ver = defuse_version_snapshot();
    const auto current_bridge = current_bridge_epoch();
    bool violation = false;
    {
        // Scoped locks: release before #1543 audit (collect takes
        // closures_mtx_ again; shared_mutex is not recursive).
        std::shared_lock<std::shared_mutex> env_lock(env_frames_mtx_);
        std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
        for (const auto& [id, cl] : closures_) {
            (void)id;
            if (cl.bridge_epoch == 0)
                continue;
            if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size())
                continue;
            const auto& fr = env_frames_[cl.env_id];
            if (!validate_linear_ownership_state(1, fr.version_, current_ver, cl.bridge_epoch,
                                                 current_bridge)) {
                violation = true;
                break;
            }
        }
    }
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    std::atomic<std::uint64_t>* site = m ? &m->linear_steal_enforced : nullptr;
    record_linear_gc_probe(*this, violation, site);
    // Issue #673: cross-module correlation — when the steal
    // probe actually caught a violation, bump the
    // "ownership-violation-during-steal" correlation counter.
    // Safe to call with compiler_metrics_ == nullptr (no-op).
    if (violation) {
        bump_runtime_observability_steal_ownership_violation_correlated();

        // Issue #1525: fiber-steal path is a multi-fiber race surface.
        if (m) {
            m->multifiber_mutate_races_detected_total.fetch_add(1, std::memory_order_relaxed);
            m->multifiber_safe_fallback_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Issue #1473: force validate_or_refresh on pinned StableNodeRefs
    // at fiber-steal time.
    if (auto* ws = workspace_flat()) {
        std::size_t validated = 0;
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        for (auto& pinned : cow_boundary_pinned_refs_) {
            if (pinned.validate_or_refresh(*ws))
                ++validated;
        }
        if (auto* mm = static_cast<CompilerMetrics*>(compiler_metrics_))
            mm->stable_ref_validations_at_steal.fetch_add(validated, std::memory_order_relaxed);
    }
    // Issue #1543: fiber-steal mutation path audit.
    (void)run_linear_gc_root_audit(kLinearGcRootAuditFiberSteal);
}

void Evaluator::collect_compiler_managed_gc_roots(std::vector<std::int64_t>& closure_roots_out,
                                                  std::vector<std::int64_t>& env_roots_out,
                                                  std::uint64_t current_bridge_epoch) const {
    std::shared_lock<std::shared_mutex> lock(closures_mtx_);
    for (const auto& [id, cl] : closures_) {
        if (cl.bridge_epoch != 0 && cl.bridge_epoch != current_bridge_epoch) {
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

void* Evaluator::compact_sweep(void* sweep_buffers) {
    // The opaque pointer is aura::serve::GCSweepBuffers* (set by
    // the serve_async.cpp callback or directly by the GC collector
    // test). Cast is safe because both the message-bridge caller
    // and the direct test pass a real GCSweepBuffers.
    auto* marks = static_cast<aura::serve::GCSweepBuffers*>(sweep_buffers);
    if (!marks)
        return nullptr;

    // Issue #1489: skip destructive reclaim while a PanicCheckpoint
    // recovery window is open (process-wide gc_hooks depth or live
    // panic_safe_source_ on this evaluator). Re-pin path remains
    // available via on_arena_compact_hook after the window closes.
    if (aura::gc_hooks::gc_deferred_for_pending_panic() || has_panic_checkpoint()) {
        aura::gc_hooks::note_gc_sweep_skipped_pending_panic();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->gc_blocked_by_panic_total.fetch_add(1, std::memory_order_relaxed);
        bump_gc_blocked_by_pending_panic();
        using SweepResult = aura::messaging::GCSweepResultMsg;
        auto* result = new SweepResult();
        result->closures_freed = 0;
        result->strings_freed = 0;
        result->pairs_freed = 0;
        result->fiber_results_freed = 0;
        return result;
    }

    std::lock_guard<std::mutex> lock(heap_mutex());
    // Issue #963: allocate the shared GCSweepResultMsg layout from
    // messaging_bridge.h (single definition; serve_async deletes it).
    using SweepResult = aura::messaging::GCSweepResultMsg;
    static_assert(sizeof(SweepResult) == 4 * sizeof(std::size_t),
                  "GCSweepResultMsg must be 4×size_t");
    auto* result = new SweepResult();

    // 1. closures_ — erase unmarked entries.
    //    This is the main leak-reduction path: each closure holds
    //    an arena-allocated flat, pool, and env that can be
    //    significant memory.
    if (marks->closure_marks) {
        std::size_t before = closures_.size();
        for (auto it = closures_.begin(); it != closures_.end();) {
            int64_t id = static_cast<int64_t>(it->first);
            if (!marks->closure_marks->test(id)) {
                it = closures_.erase(it);
            } else {
                ++it;
            }
        }
        result->closures_freed = before - closures_.size();
    }

    // 2. string_heap_ — report dead count, no compaction.
    //    Compaction requires remapping all references that hold
    //    a string index (Pair car/cdr, EvalValue String tag,
    //    Closure params, etc.). Until that work lands, the heap
    //    keeps stale entries but the GC metric tells the caller
    //    how much pressure exists.
    if (marks->string_marks) {
        result->strings_freed = marks->string_marks->count_dead();
    }

    // 3. pairs_ — same. report dead count.
    if (marks->pair_marks) {
        result->pairs_freed = marks->pair_marks->count_dead();
    }

    // 4. fiber_results — owned by s_fiber_results_ (TU-local). The
    //    GC sweep handles those separately when the
    //    message-bridge registers a fiber_result sweep callback.
    //    We report 0 here so the totals add up correctly.
    result->fiber_results_freed = 0;

    return result;
}

} // namespace aura::compiler
