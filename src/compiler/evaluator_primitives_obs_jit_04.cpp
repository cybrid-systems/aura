// evaluator_primitives_obs_jit_04.cpp — Issue #909: peeled domain registration from observability
// monolith aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/value_tags.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h"
#include "jit_typed_mutation_stats.h"
#include "shape_jit_pass_closedloop_stats.h"
#include "ci_build_info.h"
#include "primitives_meta.h"
#include "primitives_detail.h"
#include "serve/metrics.h"
#include "hash_meta.h"
#include "basis_points.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.compiler.pass_manager;

extern "C" std::uint64_t aura_fiber_static_steal_outermost_mutation_boundary_total();
extern "C" std::uint64_t aura_fiber_static_steal_inner_mutation_boundary_deferred_total();
extern "C" std::uint64_t aura_fiber_static_cross_fiber_mutation_safe_steal_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_ok_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_err_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_ok_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_err_total();
extern "C" std::uint64_t aura_jit_guest_exception_bridge_total();

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

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
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #909 part 32 (orig lines 16185-16337)
void ObservabilityPrims::register_jit_p32(PrimRegistrar add, Evaluator& ev) {

    // Issue #790: query:mutate-batch-atomic-stats —
    // P0 first-class (mutate:atomic-batch body-expr
    // :snapshot? #t) primitive with pinned
    // StableNodeRef snapshot + per-boundary
    // observability + cross-fiber safety
    // (Refine/Consolidate #737/#761 non-duplicative).
    //
    // The existing #761 (query:mutate-batch-stats)
    // already surfaces the *per-batch-measurement*
    // layer: batch-count + ops-total + rollback-count
    // + ops-per-batch + bumps-saved-total + executed-
    // under-concurrent-fiber + pinned-refs-last-batch +
    // rollback-triggers (schema 761). #790 covers the
    // *cross-fiber safety + hygiene-in-batch +
    // atomic-batch primitive exposure + snapshot
    // capture + mutation-impact batch flag* specifically
    // — was a steal detected during a suppressed batch?
    // was a hygiene violation caught inside a batch?
    // is the (mutate:atomic-batch) primitive actually
    // exposed to AI? is the snapshot capture wired? is
    // the cross-fiber re-stamp active? — as separate
    // per-decision-point signals the Agent consumes to
    // decide whether to trigger mutation-impact-snapshot
    // batch_impact + cross-fiber re-stamp under
    // concurrent AI mutate.
    //
    // 2 NEW Evaluator atomics + 2 NEW bump helpers
    // + 2 NEW public accessors + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror #789).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - cross-fiber-steals-during-batch
    //       atomic_batch_cross_fiber_steals_total
    //       (# of fiber steals that fired while
    //       inside a suppressed atomic batch —
    //       counts the *observation* of a steal
    //       during batch, not the violation; bumped
    //       from
    //       Evaluator::bump_atomic_batch_cross_fiber_
    //       steal() at the planned Phase 2+
    //       restore_post_yield_or_rollback +
    //       MutationBoundaryGuard wire-up)
    //   - hygiene-violations-in-batch
    //       atomic_batch_hygiene_violations_total
    //       (# of hygiene violations detected during
    //       an atomic batch body; bumped from
    //       Evaluator::bump_atomic_batch_hygiene_
    //       violation() at the planned Phase 2+
    //       hygiene_protected_error path inside
    //       batch wire-up)
    //   - hygiene-violation-rate
    //       hardcoded 0 (Phase 2+ to derive from
    //       hygiene-violations-in-batch /
    //       batch-count × 10000; the cross-reference
    //       ratio — high = hygiene drift inside
    //       batches)
    //   - atomic-batch-primitive-active
    //       hardcoded 0 (Phase 2+ to actually expose
    //       (mutate:atomic-batch [body] :snapshot? #t)
    //       primitive per body "Implement
    //       (mutate:atomic-batch [body] :snapshot? #t)
    //       that acquires outer StructuralMutationGuard
    //       + sets suppressed_, executes body (sequence
    //       of mutate:*), on success: single bump +
    //       optional snapshot ... on fail/panic: full
    //       rollback")
    //   - snapshot-capture-active
    //       hardcoded 0 (Phase 2+ to actually capture
    //       pinned StableNodeRef snapshot per body
    //       "Capture/pin affected refs (extend
    //       SafePCVSpan or PinnedStableRefSet) during
    //       batch; expose in snapshot for post-batch
    //       validation")
    //   - cross-fiber-re-stamp-active
    //       hardcoded 0 (Phase 2+ to wire
    //       restore_post_yield_or_rollback +
    //       MutationBoundaryGuard to re-stamp
    //       generation or force refresh pinned
    //       StableRefs when inside suppressed batch
    //       per body "if inside suppressed batch,
    //       re-stamp generation or force refresh
    //       pinned StableRefs; coordinate with
    //       checkpoint_yield_boundary")
    //   - recommendation
    //       derived 0/1/2/3 from the 3 deferred
    //       flags + activity signal
    //   - schema == 790
    ObservabilityPrims::register_stats_impl(
        "query:mutate-batch-atomic-stats", [&ev](const auto&) -> EvalValue {
            const std::int64_t cross_fiber_steals =
                static_cast<std::int64_t>(ev.atomic_batch_cross_fiber_steals_total());
            const std::int64_t hygiene_violations =
                static_cast<std::int64_t>(ev.atomic_batch_hygiene_violations_total());
            // 4 hardcoded "not yet" fields for Phase 2+
            // deferred work.
            const std::int64_t hygiene_violation_rate = 0;
            const std::int64_t atomic_batch_primitive_active = 0;
            const std::int64_t snapshot_capture_active = 0;
            const std::int64_t cross_fiber_re_stamp_active = 0;
            // Recommendation: derived from the 3 deferred
            // flags + activity signal. Phase 1 only (all
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (atomic_batch_primitive_active == 1 && snapshot_capture_active == 1 &&
                cross_fiber_re_stamp_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (atomic_batch_primitive_active == 1 || snapshot_capture_active == 1 ||
                     cross_fiber_re_stamp_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (cross_fiber_steals > 0 || hygiene_violations > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no batch activity yet)
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("cross-fiber-steals-during-batch", cross_fiber_steals);
            insert_kv("hygiene-violations-in-batch", hygiene_violations);
            insert_kv("hygiene-violation-rate", hygiene_violation_rate);
            insert_kv("atomic-batch-primitive-active", atomic_batch_primitive_active);
            insert_kv("snapshot-capture-active", snapshot_capture_active);
            insert_kv("cross-fiber-re-stamp-active", cross_fiber_re_stamp_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 790);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 33 (orig lines 16338-16506)
void ObservabilityPrims::register_jit_p33(PrimRegistrar add, Evaluator& ev) {

    // Issue #791: query:workspace-closedloop-fiber-multi-agent-
    // yield-stats — P0 exhaustive fiber yield-point
    // instrumentation + automatic StableRef/dirty
    // cross-boundary propagation in all Workspace EDSL
    // primitives (query/mutate/mark_dirty/children
    // iteration) for production multi-Agent
    // orchestration (Refine/Consolidate #773/#762
    // non-duplicative).
    //
    // The existing #773 (query:workspace-closedloop-
    // fiber-eda-stats) already surfaces the *pct-derived*
    // layer: concurrent-query-mutate-success-pct +
    // cross-cow-ref-validity-pct + yield-points-hit +
    // shared-mutex-contention-ns + multi-agent-edit-
    // fidelity + stale-ref-prevented-eda-loops (schema
    // 773). #791 covers the *cross-boundary auto-
    // propagation + missed-yield negative signal*
    // specifically — were StableRefs auto-propagated
    // across COW/clone/split? were dirty bits auto-
    // propagated? were long walks catching all yield
    // points? — as separate per-decision-point signals
    // the Agent consumes to monitor Workspace
    // closed-loop production safety under concurrent
    // multi-Agent EDA verification loops.
    //
    // 3 NEW CompilerMetrics atomics + 3 NEW bump
    // helpers on Evaluator + 1 NEW primitive (hybrid
    // enforcement-side pattern, mirror #789/#790).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - autoprop-refs-total
    //       workspace_closedloop_autoprop_refs_total
    //       (# of StableRefs auto-propagated/
    //       snapshotted across workspace COW/clone/
    //       split boundaries; bumped from
    //       Evaluator::bump_workspace_closedloop_
    //       autoprop_ref() at the planned Phase 2+
    //       workspace tree + is_valid_in / WeakRef
    //       registry paths wire-up per body "On
    //       workspace COW/clone/split in primitives
    //       or WorkspaceTree, auto-propagate/snapshot
    //       active StableRef pins ... via epoch or
    //       weak registry; extend is_valid_in /
    //       mark_dirty_upward to notify cross-
    //       boundary")
    //   - autoprop-dirty-total
    //       workspace_closedloop_autoprop_dirty_total
    //       (# of dirty bits auto-propagated on
    //       workspace COW/clone/split boundaries;
    //       bumped from
    //       Evaluator::bump_workspace_closedloop_
    //       autoprop_dirty() at the planned Phase 2+
    //       mark_dirty_upward cross-boundary
    //       notification path wire-up)
    //   - missed-yield-total
    //       workspace_closedloop_missed_yield_total
    //       (# of times a long walk — pattern matcher
    //       / children_safe iteration /
    //       mark_dirty_upward on verification
    //       subtrees — missed a yield point; the
    //       negative signal — high value = yield
    //       starvation under concurrent fiber load;
    //       bumped from
    //       Evaluator::bump_workspace_closedloop_
    //       missed_yield() at the planned Phase 2+
    //       exhaustive yield instrumentation wire-up
    //       per body "Instrument all long walks ...
    //       with explicit fiber yield points or
    //       safepoint checks")
    //   - exhaustive-yield-instrumentation-active
    //       hardcoded 0 (Phase 2+ to wire Fiber::yield
    //       + check_gc_safepoint in
    //       evaluator_primitives_query.cpp +
    //       mutate.cpp + workspace paths long walks
    //       per body "Instrument all long walks
    //       (pattern matcher, children_safe iteration,
    //       mark_dirty_upward on SV verification
    //       nodes) with explicit fiber yield points
    //       or safepoint checks (Fiber::yield or
    //       check_gc_safepoint style)")
    //   - autoprop-active
    //       hardcoded 0 (Phase 2+ to wire
    //       StableRef/dirty auto-propagation across
    //       COW/clone/split boundaries per body
    //       "auto-propagate/snapshot active StableRef
    //       pins or dirty bits via epoch or weak
    //       registry; extend is_valid_in /
    //       mark_dirty_upward to notify cross-
    //       boundary"; covers the StableRef +
    //       dirty + cross-boundary validation
    //       aggregation flag)
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 791
    ObservabilityPrims::register_stats_impl(
        "query:workspace-closedloop-fiber-multi-agent-yield-stats",
        [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t autoprop_refs =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_autoprop_refs_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t autoprop_dirty =
                m ? static_cast<std::int64_t>(m->workspace_closedloop_autoprop_dirty_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t missed_yield =
                m ? static_cast<std::int64_t>(
                        m->workspace_closedloop_missed_yield_total.load(std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t exhaustive_yield_instrumentation_active = 0;
            const std::int64_t autoprop_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (all
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (exhaustive_yield_instrumentation_active == 1 && autoprop_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (exhaustive_yield_instrumentation_active == 1 || autoprop_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (autoprop_refs > 0 || autoprop_dirty > 0 || missed_yield > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no Workspace activity yet)
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("autoprop-refs-total", autoprop_refs);
            insert_kv("autoprop-dirty-total", autoprop_dirty);
            insert_kv("missed-yield-total", missed_yield);
            insert_kv("exhaustive-yield-instrumentation-active",
                      exhaustive_yield_instrumentation_active);
            insert_kv("autoprop-active", autoprop_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 791);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 34 (orig lines 16507-16674)
void ObservabilityPrims::register_jit_p34(PrimRegistrar add, Evaluator& ev) {

    // Issue #792: query:compiler-invalidate-guard-
    // steal-stats — P0 compiler-runtime integration
    // synchronization between incremental
    // invalidate_function / mutation_epoch_ and
    // EDSL/fiber MutationBoundaryGuard + steal
    // safety for live closures/Envs/GuardShape in
    // AI multi-round self-mod closed-loops
    // (Non-duplicative refinement of #783/#755/
    // #784/#787).
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror
    // #789/#790/#791). The body explicitly cites
    // 4 directly-bumpable signals the production
    // compiler-runtime sync needs to expose.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - deferred-invalidates-total
    //       compiler_invalidate_deferred_total
    //       (# of invalidate_function calls
    //       deferred when active MutationBoundary
    //       Guard depth > 0 — defer to post-yield
    //       boundary; bumped from
    //       Evaluator::bump_compiler_invalidate_
    //       deferred() at the planned Phase 2+
    //       service.ixx invalidate_function
    //       wire-up per body "Add param or query
    //       for current fiber's mutation_stack_
    //       depth ... If depth > 0 or inside Guard,
    //       defer epoch bump / re-lower to post-
    //       yield boundary or queue; expose
    //       safe_invalidate_at_outermost_boundary()")
    //   - version-refresh-hits-total
    //       compiler_version_refresh_hits_total
    //       (# of bridge_epoch / EnvFrame version_
    //       re-stamp hits on steal resume /
    //       restore_post_yield_or_rollback;
    //       bumped from
    //       Evaluator::bump_compiler_version_
    //       refresh_hit() at the planned Phase 2+
    //       evaluator_fiber_mutation.cpp +
    //       apply_closure / materialize_call_env
    //       wire-up per body "On steal resume /
    //       restore_post_yield_or_rollback (if
    //       affected by recent invalidate), force
    //       bridge_epoch / EnvFrame version_
    //       re-stamp + closure_bridge_ refresh for
    //       live IRClosure; integrate with
    //       GuardShape expected_shape re-validation")
    //   - guardshape-deopt-on-steal-total
    //       compiler_guardshape_deopt_on_steal_
    //       total (# of GuardShape deopts triggered
    //       on steal when bridge_epoch mismatch
    //       detected; bumped from
    //       Evaluator::bump_compiler_guardshape_
    //       deopt_on_steal() at the planned Phase
    //       2+ aura_jit_bridge.cpp + JIT hot-swap
    //       paths wire-up per body "During
    //       refcount swap / hot-reload, if any
    //       fiber in MutationBoundary or apply_
    //       closure active, defer or use grace +
    //       force GuardShape deopt + linear_state
    //       re-check on affected funcs; wire to
    //       mutation_epoch_")
    //   - live-closure-stale-prevented-total
    //       compiler_live_closure_stale_prevented_
    //       total (# of live IRClosure stale
    //       references prevented via closure_
    //       bridge_ refresh; bumped from
    //       Evaluator::bump_compiler_live_closure_
    //       stale_prevented() at the planned Phase
    //       2+ apply_closure dual-path + bridge_
    //       epoch check wire-up)
    //   - safe-invalidate-at-outermost-boundary-active
    //       hardcoded 0 (Phase 2+ to actually
    //       expose safe_invalidate_at_outermost_
    //       boundary() helper per body "expose
    //       safe_invalidate_at_outermost_boundary()")
    //   - steal-resume-version-refresh-active
    //       hardcoded 0 (Phase 2+ to wire force
    //       bridge_epoch / EnvFrame version_ re-
    //       stamp + closure_bridge_ refresh on
    //       steal resume)
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 792
    ObservabilityPrims::register_stats_impl(
        "query:compiler-invalidate-guard-steal-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t deferred_invalidates =
                m ? static_cast<std::int64_t>(
                        m->compiler_invalidate_deferred_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t version_refresh_hits =
                m ? static_cast<std::int64_t>(
                        m->compiler_version_refresh_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guardshape_deopt =
                m ? static_cast<std::int64_t>(
                        m->compiler_guardshape_deopt_on_steal_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t live_closure_stale_prevented =
                m ? static_cast<std::int64_t>(m->compiler_live_closure_stale_prevented_total.load(
                        std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t safe_invalidate_at_outermost_boundary_active = 0;
            const std::int64_t steal_resume_version_refresh_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (both
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (safe_invalidate_at_outermost_boundary_active == 1 &&
                steal_resume_version_refresh_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (safe_invalidate_at_outermost_boundary_active == 1 ||
                     steal_resume_version_refresh_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (deferred_invalidates > 0 || version_refresh_hits > 0 || guardshape_deopt > 0 ||
                     live_closure_stale_prevented > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no compiler-runtime sync activity yet)
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("deferred-invalidates-total", deferred_invalidates);
            insert_kv("version-refresh-hits-total", version_refresh_hits);
            insert_kv("guardshape-deopt-on-steal-total", guardshape_deopt);
            insert_kv("live-closure-stale-prevented-total", live_closure_stale_prevented);
            insert_kv("safe-invalidate-at-outermost-boundary-active",
                      safe_invalidate_at_outermost_boundary_active);
            insert_kv("steal-resume-version-refresh-active", steal_resume_version_refresh_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 792);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 35 (orig lines 16675-16832)
void ObservabilityPrims::register_jit_p35(PrimRegistrar add, Evaluator& ev) {

    // Issue #793: query:jit-aot-hotswap-fidelity-stats
    // — P0 JIT/AOT hot-swap + GuardShape + linear +
    // EnvFrame version_ consistency observability
    // (Non-duplicative consolidation/refinement of
    // #785/#787/#755).
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive (hybrid
    // enforcement-side pattern, mirror #792). The
    // body explicitly cites 4 directly-bumpable
    // fidelity signals the production JIT/AOT
    // hot-swap needs to expose.
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - deopt-forced-on-reload-total
    //       jit_deopt_forced_on_reload_total
    //       (# of GuardShape deopts forced on AOT
    //       reload / refcount swap; bumped from
    //       Evaluator::bump_jit_deopt_forced_on_
    //       reload() at the planned Phase 2+
    //       aura_jit.cpp + aura_jit_bridge.cpp
    //       hot-swap path wire-up per body "On
    //       successful refcount swap or region
    //       reload, if any active fiber holds
    //       MutationBoundary or has live
    //       GuardShape/Apply on affected func,
    //       force deopt (set generic_block) or
    //       bump shape_id / linear_state for
    //       affected IR")
    //   - linear-violation-prevented-total
    //       jit_linear_violation_prevented_total
    //       (# of linear ownership violations
    //       prevented via JIT runtime version check
    //       / MoveOp invalidation; bumped from
    //       Evaluator::bump_jit_linear_violation_
    //       prevented() at the planned Phase 2+
    //       aura_jit.cpp JIT codegen for Linear*
    //       wire-up per body "Emit additional
    //       runtime checks (version_ probe or
    //       bridge_epoch compare) before deopt
    //       decision or MoveOp")
    //   - env-version-sync-hits-total
    //       jit_env_version_sync_hits_total
    //       (# of EnvFrame::version_ sync hits
    //       triggered on JIT-executed closure
    //       steal resume / post-rollback; bumped
    //       from
    //       Evaluator::bump_jit_env_version_sync_
    //       hit() at the planned Phase 2+
    //       evaluator_fiber_mutation.cpp +
    //       apply_closure wire-up per body "On
    //       steal resume / post-rollback, for
    //       JIT-executed closures, trigger
    //       GuardShape re-evaluation or linear
    //       re-wrap if version_ or epoch drifted")
    //   - guardshape-stale-reject-total
    //       jit_guardshape_stale_reject_total
    //       (# of JIT GuardShape stale rejections
    //       caught when expected_shape / shape_id
    //       mismatch detected at apply_closure
    //       time; bumped from
    //       Evaluator::bump_jit_guardshape_stale_
    //       reject() at the planned Phase 2+
    //       ir_executor.ixx + evaluator.ixx
    //       apply_closure bridge_epoch check
    //       wire-up per body "IRInterpreter
    //       handling of GuardShape/linear +
    //       apply_closure (bridge_epoch check)")
    //   - reload-deopt-version-hooks-active
    //       hardcoded 0 (Phase 2+ to wire
    //       reload-deopt version hooks in
    //       aura_jit.cpp + aura_jit_bridge.cpp
    //       hot-swap path)
    //   - jit-emit-runtime-version-checks-active
    //       hardcoded 0 (Phase 2+ to wire additional
    //       runtime checks in JIT codegen for
    //       GuardShape / Linear* ops)
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 793
    ObservabilityPrims::register_stats_impl(
        "query:jit-aot-hotswap-fidelity-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t deopt_forced =
                m ? static_cast<std::int64_t>(
                        m->jit_deopt_forced_on_reload_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t linear_prevented =
                m ? static_cast<std::int64_t>(
                        m->jit_linear_violation_prevented_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_sync =
                m ? static_cast<std::int64_t>(
                        m->jit_env_version_sync_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guardshape_stale =
                m ? static_cast<std::int64_t>(
                        m->jit_guardshape_stale_reject_total.load(std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t reload_deopt_version_hooks_active = 0;
            const std::int64_t jit_emit_runtime_version_checks_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (both
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (reload_deopt_version_hooks_active == 1 &&
                jit_emit_runtime_version_checks_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (reload_deopt_version_hooks_active == 1 ||
                     jit_emit_runtime_version_checks_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (deopt_forced > 0 || linear_prevented > 0 || env_sync > 0 ||
                     guardshape_stale > 0)
                recommendation = 2; // Phase 1 only (atomics wired, expose/wire deferred)
            else
                recommendation = 3; // early-stage (no JIT/AOT fidelity activity yet)
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("deopt-forced-on-reload-total", deopt_forced);
            insert_kv("linear-violation-prevented-total", linear_prevented);
            insert_kv("env-version-sync-hits-total", env_sync);
            insert_kv("guardshape-stale-reject-total", guardshape_stale);
            insert_kv("reload-deopt-version-hooks-active", reload_deopt_version_hooks_active);
            insert_kv("jit-emit-runtime-version-checks-active",
                      jit_emit_runtime_version_checks_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 793);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 36 (orig lines 16833-17004)
void ObservabilityPrims::register_jit_p36(PrimRegistrar add, Evaluator& ev) {

    // Issue #794: query:full-closedloop-compiler-edsl-
    // fidelity-stats — P0 unified end-to-end
    // closed-loop fidelity measurement for the
    // integrated compiler (IR/lower/JIT) + EDSL
    // (Guard/mutate/fiber/StableRef/AOT)
    // self-evolution capability (Non-duplicative
    // to #786/#787/#755/#792/#793).
    //
    // The existing primitives surface component
    // fidelity signals individually (#786 code-as-
    // data production health + #787 end-to-end
    // fidelity under chaos + #755 concurrent
    // safety + #792 compiler invalidate sync +
    // #793 JIT/AOT hot-swap fidelity). #794 covers
    // the *cross-layer closed-loop harness*
    // fidelity signals specifically — was the
    // GuardShape deopt caught across the full
    // pipeline? was linear enforcement successful
    // across layers? was the epoch synced? was any
    // cross-layer drift detected? — as separate
    // per-decision-point signals the Agent consumes
    // to decide whether to trigger full-cycle
    // re-validation under production self-mod load.
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror
    // #792/#793).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - cross-layer-guardshape-deopt-hits-total
    //       cross_layer_guardshape_deopt_hits_total
    //       (# of times the full closed-loop harness
    //       detected GuardShape expected vs runtime
    //       shape mismatch across the full pipeline;
    //       bumped from
    //       Evaluator::bump_cross_layer_guardshape_
    //       deopt_hit() at the planned Phase 2+
    //       tests/test_full_compiler_edsl_closedloop_
    //       fidelity.cpp wire-up)
    //   - cross-layer-linear-enforce-success-total
    //       cross_layer_linear_enforce_success_total
    //       (# of times linear_ownership_state was
    //       respected across compiler + EDSL
    //       boundary; bumped from
    //       Evaluator::bump_cross_layer_linear_
    //       enforce_success() at the planned Phase
    //       2+ harness wire-up)
    //   - cross-layer-epoch-sync-total
    //       cross_layer_epoch_sync_total (# of
    //       times EnvFrame version_ + bridge_epoch
    //       were synchronized across layers; bumped
    //       from
    //       Evaluator::bump_cross_layer_epoch_sync()
    //       at the planned Phase 2+ harness
    //       wire-up)
    //   - cross-layer-drift-detections-total
    //       cross_layer_drift_detections_total
    //       (the negative signal — # of times the
    //       harness detected any cross-layer drift;
    //       high value = SLO breach; bumped from
    //       Evaluator::bump_cross_layer_drift_
    //       detection() at the planned Phase 2+
    //       harness wire-up)
    //   - full-closedloop-harness-active
    //       hardcoded 0 (Phase 2+ to actually
    //       implement tests/test_full_compiler_
    //       edsl_closedloop_fidelity.cpp per body
    //       "New harness tests/test_full_compiler_
    //       edsl_closedloop_fidelity.cpp:
    //       Implement multi-round SEVA-style loop
    //       with heavy macro/EDSL mutate under Guard
    //       + concurrent fibers + steal injection +
    //       AOT reload points; trigger compiler
    //       invalidate via mutate; assert after
    //       each cycle: GuardShape expected matches
    //       runtime shape, linear_ownership_state
    //       respected ... EnvFrame version_
    //       consistent, bridge_epoch fresh, StableRef
    //       valid, no hygiene drift, Interpreter vs
    //       JIT result identical, metrics match
    //       SLO")
    //   - slo-gate-active
    //       hardcoded 0 (Phase 2+ to wire CI gate +
    //       trend dashboard + self-heal hooks per
    //       body "Define quantitative gates
    //       (fidelity >99.5% over 10k cycles under
    //       8+ fibers + steal/AOT load; zero
    //       undetected drift; TSan/ASan clean); add
    //       CI step that runs harness and fails PR
    //       on breach; publish trend dashboard")
    //   - recommendation
    //       derived 0/1/2/3 from the 2 deferred
    //       flags + activity signal
    //   - schema == 794
    ObservabilityPrims::register_stats_impl(
        "query:full-closedloop-compiler-edsl-fidelity-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t guardshape_deopt =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_guardshape_deopt_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t linear_success =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_linear_enforce_success_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t epoch_sync =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_epoch_sync_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t drift_detections =
                m ? static_cast<std::int64_t>(
                        m->cross_layer_drift_detections_total.load(std::memory_order_relaxed))
                  : 0;
            // 2 hardcoded "not yet" flags for Phase 2+
            // deferred work.
            const std::int64_t full_closedloop_harness_active = 0;
            const std::int64_t slo_gate_active = 0;
            // Recommendation: derived from the 2 deferred
            // flags + activity signal. Phase 1 only (both
            // deferred flags == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (full_closedloop_harness_active == 1 && slo_gate_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (full_closedloop_harness_active == 1 || slo_gate_active == 1)
                recommendation = 1; // partial Phase 2+
            else if (guardshape_deopt > 0 || linear_success > 0 || epoch_sync > 0 ||
                     drift_detections > 0)
                recommendation = 2; // Phase 1 only (atomics wired, harness deferred)
            else
                recommendation = 3; // early-stage (no closed-loop fidelity activity yet)
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("cross-layer-guardshape-deopt-hits-total", guardshape_deopt);
            insert_kv("cross-layer-linear-enforce-success-total", linear_success);
            insert_kv("cross-layer-epoch-sync-total", epoch_sync);
            insert_kv("cross-layer-drift-detections-total", drift_detections);
            insert_kv("full-closedloop-harness-active", full_closedloop_harness_active);
            insert_kv("slo-gate-active", slo_gate_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 794);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 37 (orig lines 17005-17167)
void ObservabilityPrims::register_jit_p37(PrimRegistrar add, Evaluator& ev) {

    // Issue #795: query:shape-pass-hotpath-contracts-
    // stats — P0 deep hot-path Contracts + stronger
    // SoAView/ShapeStablePass Concepts +
    // ShapeProfiler JIT Epoch Sync + Dirty
    // Propagation observability (Non-duplicative
    // refinement of #768/#507/#766/#767/#741).
    //
    // The existing #768 (query:shape-pass-hotpath-
    // stats) already surfaces the 5 hot-path
    // observability counters (contract-checks-
    // hotpath / shape-stability-transitions /
    // jit-epoch-sync-hits / deopt-targeted-skips /
    // concept-violations-caught + schema 768). #795
    // covers the *deep SoA/Pass/JIT contracts +
    // stronger concepts + targeted invalidation +
    // Arena compact hook* specifically — were
    // SoAView violations caught? were
    // ShapeStablePass violations caught? was a
    // targeted deopt via #741 impact_scope used?
    // was an Arena compact on_compact_hook_
    // invoked? — as separate per-decision-point
    // signals the Agent consumes to monitor the
    // C++26 Contracts/Concepts adoption maturity
    // in the hot allocator/dispatch/SoA/shape
    // paths.
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive
    // (hybrid enforcement-side pattern, mirror
    // #789/#790/#791/#792/#793/#794).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - soa-view-violations-caught-total
    //       soa_view_violations_caught_total
    //       (# of SoAView concept static_assert
    //       violations caught at compile time /
    //       runtime; bumped from
    //       Evaluator::bump_soa_view_violations_
    //       caught() at the planned Phase 2+
    //       pass_manager.ixx + lowering/JIT
    //       run_incremental_dirty_pipeline
    //       wire-up per body "Define SoAView
    //       concept (requires const view +
    //       shape_id consult) and ShapeStablePass
    //       (requires stable_shape consult +
    //       DirtyAware); static_assert in
    //       run_incremental_dirty_pipeline")
    //   - shape-stable-pass-violations-total
    //       shape_stable_pass_violations_total
    //       (# of ShapeStablePass concept
    //       static_assert violations caught;
    //       bumped from
    //       Evaluator::bump_shape_stable_pass_
    //       violations() at the planned Phase 2+
    //       pass_manager.ixx + dominant_shape /
    //       ShapePropagationPass wire-up)
    //   - targeted-deopt-via-impact-scope-total
    //       targeted_deopt_via_impact_scope_total
    //       (# of targeted deopts via #741
    //       impact_scope instead of global
    //       invalidation; bumped from
    //       Evaluator::bump_targeted_deopt_via_
    //       impact_scope() at the planned Phase
    //       2+ shape_profiler.cpp deopt hook
    //       wire-up per body "consult DirtyAware
    //       or #741 impact_scope for targeted
    //       invalidation instead of global")
    //   - on-compact-hook-invocations-total
    //       on_compact_hook_invocations_total
    //       (# of Arena compact on_compact_hook_
    //       invocations that triggered shape_inval
    //       + dirty cascade; bumped from
    //       Evaluator::bump_on_compact_hook_
    //       invocation() at the planned Phase 2+
    //       arena.ixx + ir_soa.ixx on_compact_hook_
    //       wire-up per body "on_compact_hook_
    //       invoke with shape_inval + dirty
    //       cascade")
    //   - concepts-active
    //       hardcoded 0 (Phase 2+ to actually wire
    //       SoAView + ShapeStablePass concepts +
    //       targeted deopt via impact_scope +
    //       ShapeProfiler epoch sync all together
    //       — single flag covers all 3+ deferred
    //       wire-up areas)
    //   - recommendation
    //       derived 0/1/2/3 from the deferred flag
    //       + activity signal
    //   - schema == 795
    ObservabilityPrims::register_stats_impl(
        "query:shape-pass-hotpath-contracts-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t soa_view_violations =
                m ? static_cast<std::int64_t>(
                        m->soa_view_violations_caught_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t shape_stable_pass_violations =
                m ? static_cast<std::int64_t>(
                        m->shape_stable_pass_violations_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t targeted_deopt =
                m ? static_cast<std::int64_t>(
                        m->targeted_deopt_via_impact_scope_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t on_compact_hook =
                m ? static_cast<std::int64_t>(
                        m->on_compact_hook_invocations_total.load(std::memory_order_relaxed))
                  : 0;
            // 1 hardcoded "not yet" flag for Phase 2+
            // deferred work (covers all 3+ deferred
            // wire-up areas).
            const std::int64_t concepts_active = 0;
            // Recommendation: derived from the deferred
            // flag + activity signal. Phase 1 only
            // (deferred flag == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (concepts_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (soa_view_violations > 0 || shape_stable_pass_violations > 0 ||
                     targeted_deopt > 0 || on_compact_hook > 0)
                recommendation = 2; // Phase 1 only (atomics wired, concepts deferred)
            else
                recommendation = 3; // early-stage (no hot-path contracts activity yet)
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("soa-view-violations-caught-total", soa_view_violations);
            insert_kv("shape-stable-pass-violations-total", shape_stable_pass_violations);
            insert_kv("targeted-deopt-via-impact-scope-total", targeted_deopt);
            insert_kv("on-compact-hook-invocations-total", on_compact_hook);
            insert_kv("concepts-active", concepts_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 795);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 38 (orig lines 17168-17349)
void ObservabilityPrims::register_jit_p38(PrimRegistrar add, Evaluator& ev) {

    // Issue #796: query:ir-soa-full-migration-stats
    // — P0 end-to-end IRModuleV2 SoA full migration
    // + DirtyAware short-circuit + DepGraph
    // integration observability (Non-duplicative
    // extension of #766/#741).
    //
    // The existing #766 (query:ir-soa-migration-
    // stats) already surfaces the IR-SoA Phase 1
    // dashboard (5 NEW atomics from Phase 1 + schema
    // 766). #796 covers the *end-to-end production
    // migration* specifically — were instructions
    // emitted to IRFunctionSoA? were dirty blocks
    // skipped via DirtyAwarePass? was the JIT SoA
    // emit path exercised? was the hybrid
    // impact+dirty skip consulted? — as separate
    // per-decision-point signals the Agent consumes
    // to monitor production-grade SoA migration in
    // compiler hot paths.
    //
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive (hybrid
    // enforcement-side pattern, mirror #789/#790/
    // #791/#792/#793/#794/#795).
    //
    // Fields (7 + sentinel, 8-entry hash):
    //   - soa-instructions-emitted-total
    //       ir_soa_instructions_emitted_total (# of
    //       instructions emitted to IRFunctionSoA
    //       vs remaining AoS IRModule paths;
    //       bumped from
    //       Evaluator::bump_ir_soa_instructions_
    //       emitted() at the planned Phase 2+
    //       lowering_impl.cpp + JIT emit sites
    //       wire-up per body "Complete port of
    //       LoweringState emit, ir_executor
    //       traversal, JIT emitter to prefer
    //       IRFunctionSoA + IRInstructionView")
    //   - dirty-block-skips-total
    //       ir_soa_dirty_block_skips_total (# of
    //       blocks skipped via DirtyAwarePass +
    //       run_incremental_dirty_pipeline short-
    //       circuit; bumped from
    //       Evaluator::bump_ir_soa_dirty_block_
    //       skips() at the planned Phase 2+
    //       service.ixx invalidate_function +
    //       lowering/JIT path wire-up per body
    //       "Enforce DirtyAwarePass +
    //       run_incremental_dirty_pipeline in
    //       invalidate_function + JIT recompile")
    //   - jit-soa-time-ns-total
    //       ir_soa_jit_soa_time_ns_total (total ns
    //       spent in JIT SoA emit path — time-based
    //       signal; bumped from
    //       Evaluator::bump_ir_soa_jit_soa_time_ns()
    //       at the planned Phase 2+ aura_jit.cpp
    //       SoA emit path wire-up)
    //   - impact-dirty-hybrid-skips-total
    //       ir_soa_impact_dirty_hybrid_skips_total
    //       (# of skips via hybrid impact_scope +
    //       is_block_dirty targeting — the combined
    //       #741 + #766 short-circuit count; bumped
    //       from
    //       Evaluator::bump_ir_soa_impact_dirty_
    //       hybrid_skip() at the planned Phase 2+
    //       service.ixx invalidate_function when
    //       both DepGraph impact_scope + SoA block
    //       dirty are consulted together per body
    //       "consult ... #741 impact_scope for
    //       hybrid targeting")
    //   - clean-block-hit-rate
    //       hardcoded 0 in Phase 1 (Phase 2+ to
    //       derive from
    //       #766 ir-soa-migration-stats + dirty
    //       block counts; the cross-reference
    //       ratio — high = many clean blocks skipped
    //       via DirtyAware short-circuit)
    //   - full-soa-migration-active
    //       hardcoded 0 (Phase 2+ to actually
    //       complete the production-grade migration
    //       of LoweringState emit + ir_executor
    //       traversal + JIT emitter to prefer
    //       IRFunctionSoA + full pmr column
    //       migration + DepGraph integration —
    //       single flag covers all deferred wire-up
    //       areas)
    //   - recommendation
    //       derived 0/1/2/3 from the deferred flag
    //       + activity signal
    //   - schema == 796
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-full-migration-stats", [&ev](const auto&) -> EvalValue {
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t soa_emitted =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_instructions_emitted_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dirty_skips =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_dirty_block_skips_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t clean_block_hit_rate =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_clean_block_hit_rate_pct.load(std::memory_order_relaxed))
                  : 0;
            // #796 reuses the existing
            // ir_soa_jit_codegen_time_ns_total atomic
            // (already populated by
            // bump_ir_soa_jit_codegen_time_ns from prior
            // issue work) — the #796 primitive exposes it
            // as jit-soa-time-ns-total for the new
            // dashboard.
            const std::int64_t jit_soa_ns =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_jit_codegen_time_ns_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t pmr_utilization =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_pmr_column_utilization_pct.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t impact_dirty_skips =
                m ? static_cast<std::int64_t>(
                        m->ir_soa_impact_dirty_hybrid_skips_total.load(std::memory_order_relaxed))
                  : 0;
            // 1 hardcoded "not yet" flag for Phase 2+
            // deferred work (clean-block-hit-rate replaced
            // with existing ir_soa_clean_block_hit_rate_pct
            // atomic above; this single flag covers the
            // overall "is the full SoA migration active?"
            // status).
            const std::int64_t full_soa_migration_active = 0;
            // Recommendation: derived from the deferred
            // flag + activity signal. Phase 1 only
            // (deferred flag == 0) but with activity
            // signals from the new atomics.
            std::int64_t recommendation = 3;
            if (full_soa_migration_active == 1)
                recommendation = 0; // production-ready with all Phase 2+
            else if (soa_emitted > 0 || dirty_skips > 0 || jit_soa_ns > 0 || impact_dirty_skips > 0)
                recommendation = 2; // Phase 1 only (atomics wired, full migration deferred)
            else
                recommendation = 3; // early-stage (no IR SoA migration activity yet)
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("soa-instructions-emitted-total", soa_emitted);
            insert_kv("dirty-block-skips-total", dirty_skips);
            insert_kv("clean-block-hit-rate-pct", clean_block_hit_rate);
            insert_kv("jit-soa-time-ns-total", jit_soa_ns);
            insert_kv("pmr-column-utilization-pct", pmr_utilization);
            insert_kv("impact-dirty-hybrid-skips-total", impact_dirty_skips);
            insert_kv("full-soa-migration-active", full_soa_migration_active);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 796);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

// Issue #909 part 39 (orig lines 17350-17403)
void ObservabilityPrims::register_jit_p39(PrimRegistrar add, Evaluator& ev) {

    // Issue #809: error-handling-policy-stats — formalized exception policy + interop counters
    ObservabilityPrims::register_stats_impl(
        "query:error-handling-policy-stats", [&ev](const auto&) -> EvalValue {
            auto load = [&](auto* atomic_ptr) -> std::uint64_t {
                return atomic_ptr ? atomic_ptr->load(std::memory_order_relaxed) : 0;
            };
            CompilerMetrics* m = ev.compiler_metrics_
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics_)
                                     : nullptr;
            const std::int64_t f_interop_conversions =
                m ? static_cast<std::int64_t>(
                        m->error_policy_interop_conversions_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_contract_as_aura_error =
                m ? static_cast<std::int64_t>(m->error_policy_contract_as_aura_error_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t f_policy_doc_active = 1;
            const std::int64_t f_hot_path_uses_result = 1;
            auto* ht = FlatHashTable::create(16) /* #1141 */;
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("interop-conversions", f_interop_conversions);
            insert_kv("contract-as-aura-error", f_contract_as_aura_error);
            insert_kv("policy-doc-active", f_policy_doc_active);
            insert_kv("hot-path-uses-result", f_hot_path_uses_result);
            insert_kv("schema", 809);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
}

} // namespace aura::compiler::primitives_detail
