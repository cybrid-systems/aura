// evaluator_eval_flat.cpp — P1-b: apply_closure, eval_flat, macro expansion
// aura.compiler.evaluator module partition.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "reflect/hygiene_validate.hh" // Issue #1611: MutationReflectHealth

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.evaluator_pure;
import aura.compiler.macro_expansion;
import aura.diag;
import aura.parser.parser;
import aura.compiler.soa_view;

namespace aura::compiler {

namespace primitives_detail {

    void bump_prim_error_unified_total() noexcept {
        if (auto* qev = Evaluator::get_query_evaluator()) {
            if (auto* m = static_cast<CompilerMetrics*>(qev->compiler_metrics()))
                m->prim_error_unified_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

} // namespace primitives_detail

using macro_exp::clone_macro_body;
using macro_exp::expand_inner_macros;
using macro_exp::MacroExpansionDef;

static std::unordered_map<std::string, MacroExpansionDef>
as_expansion_registry(const std::unordered_map<std::string, MacroDef>& macros) {
    std::unordered_map<std::string, MacroExpansionDef> out;
    out.reserve(macros.size());
    for (const auto& [name, md] : macros) {
        out.emplace(name, MacroExpansionDef{md.params, md.dotted, md.flat, md.pool, md.body_id});
    }
    return out;
}

using aura::compiler::pure::is_truthy;
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
using types::make_keyword;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;
// Issue #918: use aura::diag:: prefix
using aura::diag::Diagnostic;
using aura::diag::ErrorKind;

static std::string closest_match(std::string_view name, std::span<const std::string> candidates,
                                 std::size_t max_dist = 3) {
    return aura::compiler::pure::closest_match_pure(name, candidates, max_dist);
}

#define EVAL_CACHE_RETURN(expr)                                                                    \
    do {                                                                                           \
        auto _er_ = (expr);                                                                        \
        if (_er_) {                                                                                \
            f->set_cached_value(current_id, _er_->val);                                            \
        }                                                                                          \
        return _er_;                                                                               \
    } while (0)

#define EVAL_CACHE_RETURN_VAL(expr)                                                                \
    do {                                                                                           \
        auto _ev_ = (expr);                                                                        \
        f->set_cached_value(current_id, _ev_.val);                                                 \
        return _ev_;                                                                               \
    } while (0)

// Issue #739: acquire fence before epoch load so invalidate_function's
// release bump is visible to fibers executing stolen closure work.
std::uint64_t Evaluator::current_bridge_epoch() const noexcept {
    std::atomic_thread_fence(std::memory_order_acquire);
    if (compiler_metrics_) {
        auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
        m->closure_epoch_fence_enforced_total.fetch_add(1, std::memory_order_relaxed);
    }
    if (bridge_epoch_fn_ && compiler_service_) {
        return bridge_epoch_fn_(compiler_service_);
    }
    return 0;
}

static void record_epoch_stale_steal_caught(CompilerMetrics* m) {
    if (!m)
        return;
    m->epoch_stale_steal_caught.fetch_add(1, std::memory_order_relaxed);
    m->linear_violation_prevented_epoch_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #1655: file-local single-source-of-truth predicate for the
// "is this closure epoch-or-env stale?" check that gates the
// closure_epoch_mismatch_fallback metric. Mirrors the logic of
// Evaluator::closure_is_epoch_or_env_stale but lives next to both call
// sites (closure_needs_safe_fallback + the inline race-window path in
// apply_closure) so the predicate is co-located with the metric it
// guards. Excludes linear-only stale by design — that path bumps
// linear_ownership_violation_prevented instead (Issue #1478 / #1626 /
// #1660 invariant: epoch-vs-env-vs-linear metrics stay cleanly split).
//
// Issue #1485 C1 originally had closure_needs_safe_fallback track
// `bool epoch_or_env_stale` inline + a late
// `ev.closure_is_epoch_or_env_stale(cl)` invariant check. The inline
// race-window path in apply_closure (#1485 C1, #1558, #1632) inlined
// the same bridge/env checks but accidentally bumped
// closure_epoch_mismatch_fallback UNCONDITIONALLY — any future
// broadening of the race-window predicate (e.g. adding a linear arm)
// would silently inflate the metric. Extracting this helper makes the
// gate the single source of truth and prevents future drift.
static bool closure_is_epoch_stale(const Evaluator& ev, const Closure& cl) noexcept {
    if (ev.is_bridge_stale(cl.bridge_epoch, ev.current_bridge_epoch()))
        return true;
    if (cl.env_id != NULL_ENV_ID &&
        (ev.is_env_frame_invalid(cl.env_id) || ev.is_env_frame_stale(cl.env_id)))
        return true;
    return false;
}

// Issue #681 / #1287 / #1491 / #1632 / #1660: pre-call epoch/version
// enforcement for live closures held across mutate:rebind /
// invalidate_function. Dual-path apply_closure (map + bridge) MUST treat
// bridge_epoch / EnvFrame version (defuse) mismatch as mandatory safe
// fallback (no dangling flat*/pool* use after mutation).
//
// #1655: closure_is_epoch_stale (defined above) is the single source of
// truth for the closure_epoch_mismatch_fallback gate. Replaces the
// previous inline `bool epoch_or_env_stale` tracking + late
// `ev.closure_is_epoch_or_env_stale(cl)` invariant check (#1660
// redundant after #1655 extraction — the new helper is the canonical
// predicate).
static bool closure_needs_safe_fallback(const Evaluator& ev, const Closure& cl,
                                        CompilerMetrics* m) {
    bool stale = false;
    const auto cur_epoch = ev.current_bridge_epoch();
    // Bridge half — #1365 strict is_bridge_stale.
    if (ev.is_bridge_stale(cl.bridge_epoch, cur_epoch)) {
        stale = true;
        if (m) {
            m->compiler_closure_epoch_mismatch_hits.fetch_add(1, std::memory_order_relaxed);
            m->closure_bridge_epoch_safety_enforced.fetch_add(1, std::memory_order_relaxed);
            record_epoch_stale_steal_caught(m);
            // Issue #1509: concurrent steal/mutate race surface.
            m->closure_race_caught_count_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #1525: multi-fiber mutate↔eval race counter.
            m->multifiber_mutate_races_detected_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (cl.env_id != NULL_ENV_ID) {
        // EnvFrame SoA half — version_ / invalid (parent_id_ walk uses same frames).
        // Issue #1890: distinguish invalid_id / INVALID_VERSION vs version-stale
        // so metrics and fallbacks do not conflate OOB/truncated with refreshable stale.
        const bool invalid_id = ev.is_env_frame_invalid_id(cl.env_id);
        const bool terminal_invalid = !invalid_id && ev.is_env_frame_invalid(cl.env_id);
        const bool version_stale =
            !invalid_id && !terminal_invalid && ev.is_env_frame_stale(cl.env_id);
        if (invalid_id || terminal_invalid || version_stale) {
            stale = true;
            if (m) {
                m->compiler_closure_epoch_mismatch_hits.fetch_add(1, std::memory_order_relaxed);
                m->compiler_closure_envframe_stale_total.fetch_add(1, std::memory_order_relaxed);
                m->closure_bridge_epoch_safety_enforced.fetch_add(1, std::memory_order_relaxed);
                m->envframe_invalid_vs_stale_distinguished_total.fetch_add(
                    1, std::memory_order_relaxed);
                // Issue #1916 AC: dangling EnvFrame prevented at apply gate.
                // invalid_id / terminal_invalid are hard dangling; version_stale
                // is also treated as prevented-use-of-stale-env (safe fallback).
                m->dangling_env_prevented.fetch_add(1, std::memory_order_relaxed);
                m->dangling_env_prevented_apply.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Issue #1478 / #1626 / #1660 / #1916: linear post-mutate third arm
        // (distinct metric from epoch/env — linear-stale-total).
        if (!ev.linear_post_mutate_enforce(cl.env_id)) {
            stale = true;
            if (m) {
                m->linear_ownership_violation_prevented.fetch_add(1, std::memory_order_relaxed);
                m->linear_ownership_safe_fallback_total.fetch_add(1, std::memory_order_relaxed);
                m->compiler_closure_epoch_mismatch_hits.fetch_add(1, std::memory_order_relaxed);
                m->closure_bridge_epoch_safety_enforced.fetch_add(1, std::memory_order_relaxed);
                m->dangling_env_prevented.fetch_add(1, std::memory_order_relaxed);
                m->dangling_env_prevented_apply.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (stale) {
        ev.bump_compiler_root_stale_closure_detected();
        // Issue #1509: every apply_closure that observes a stale closure.
        if (m)
            m->closure_stale_apply_count_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1485 C1: top-level "we caught a stale closure before
        // dispatch" signal. Distinct from closure_stale_apply_count_total
        // (which counts every stale observation, including nested helper
        // calls); this one is bumped at apply_closure entry AFTER the
        // helper returns true, marking "this entry was stale".
        ev.bump_stale_closure_prevented();
        // Issue #1632 AC3: live_closure_stale_prevented on apply path
        // (parity with JIT walk / invalidate active-closure metrics).
        ev.bump_compiler_live_closure_stale_prevented();
        // Issue #1485 C1 / #1632 / #1655 / #1660: lifetime count of
        // safe-fallback paths after bridge_epoch / EnvFrame version_
        // mismatch. Gate on the single-source-of-truth predicate
        // (#1655) so linear-only stale fallbacks don't bleed into
        // the epoch-mismatch metric.
        // Issue #1655: gate on closure_is_epoch_stale (helper above) —
        // single source of truth for the metric. Replaces the previous
        // inline boolean tracking (a false init + three assignments
        // across the bridge / env / late-invariant branches) and the
        // now-redundant late Evaluator unified-helper invariant check
        // from the prior #1660 mandate.
        if (closure_is_epoch_stale(ev, cl))
            ev.bump_closure_epoch_mismatch_fallback();
    }
    return stale;
}

// Issue #1511: dual-check gate for every closure_bridge_ dispatch.
// Covers (1) local-map stale recovery and (2) local-miss IR bridge.
// When provenance is available (local Closure copy), enforces the same
// bridge_epoch + EnvFrame version_ contract as the tree-walker path.
// On stale: re-stamp EnvFrame version_ (so materialize won't walk a
// poisoned frame), bump metrics, then still invoke the bridge as the
// recovery path (IR re-dispatch / re-parse) — never eval dangling
// flat*/pool* via the stale local Closure.
static std::optional<EvalValue>
invoke_closure_bridge_checked(Evaluator& ev, Evaluator::ClosureBridgeFn& bridge, ClosureId cid,
                              std::span<const EvalValue> args, CompilerMetrics* metrics,
                              const Closure* provenance) {
    if (!bridge)
        return std::nullopt;
    if (metrics)
        metrics->closure_bridge_calls.fetch_add(1, std::memory_order_relaxed);

    bool stale = false;
    if (provenance) {
        // Issue #1511 / #1626: forced dual-check + linear arm —
        // same contract as closure_needs_safe_fallback.
        if (Evaluator::is_bridge_stale(provenance->bridge_epoch, ev.current_bridge_epoch()))
            stale = true;
        if (provenance->env_id != NULL_ENV_ID) {
            if (ev.is_env_frame_invalid(provenance->env_id) ||
                ev.is_env_frame_stale(provenance->env_id)) {
                stale = true;
                if (metrics)
                    metrics->compiler_closure_envframe_stale_total.fetch_add(
                        1, std::memory_order_relaxed);
            }
            // #1626: linear third arm at bridge entry (parity with map path).
            if (!ev.linear_post_mutate_enforce(provenance->env_id)) {
                stale = true;
                if (metrics)
                    metrics->linear_ownership_violation_prevented.fetch_add(
                        1, std::memory_order_relaxed);
            }
        }
    }

    if (stale) {
        if (metrics) {
            metrics->closure_bridge_fallback_stale_total.fetch_add(1, std::memory_order_relaxed);
            metrics->compiler_closure_epoch_mismatch_hits.fetch_add(1, std::memory_order_relaxed);
        }
        // EnvFrame re-stamp under the dual-check contract so a subsequent
        // materialize on a recovered path does not walk a pre-mutation frame.
        if (provenance && provenance->env_id != NULL_ENV_ID &&
            ev.is_valid_env_id(provenance->env_id)) {
            ev.refresh_stale_frame_in_walk(provenance->env_id, "closure_bridge");
        }
    }

    auto result = bridge(cid, args);

    if (stale && metrics) {
        // Safe recovery completed (bridge returned a value or nullopt —
        // either way we did not eval the stale local Closure body).
        // Only the #1511 bridge-specific counters here — the local-map
        // path already bumps compiler_closure_safe_fallbacks before
        // entering this helper (avoid double-count).
        metrics->closure_bridge_safe_fallbacks_total.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

// apply_closure — looks up closures_, foreign functions, or IR bridge
std::optional<EvalValue> Evaluator::apply_closure(ClosureId cid, std::span<const EvalValue> args) {
    // Issue #252: closure dual-path observability. Bump the
    // total counter on every call. The path-specific counters
    // (ffi / tw / bridge / ir) are bumped in each branch.
    // The IR path (runtime_closures_) bumps the ir counter
    // via the shared metrics pointer (set by service.ixx).
    // Issue #1918: EDSL apply_closure hot path uses EnvFrame SoA +
    // dual-epoch checks (no AoS pointer-chasing for live closures).
    soa_view::record_edsl_apply_soa_path();
    if (compiler_metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
        m->closure_calls_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Check for foreign function closure (cid < ffi_runtime_.func_count())
    if (cid < ffi_runtime_.func_count()) {
        if (compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
            m->closure_ffi_calls.fetch_add(1, std::memory_order_relaxed);
        }
        auto fidx = cid;
        if (fidx < ffi_runtime_.func_count()) {
            auto& ff = ffi_runtime_.func_at(static_cast<std::size_t>(fidx));
            void* fn_ptr = ff.fn_ptr;
            int ret_type = ff.ret_type;
            auto& arg_types = ff.arg_types;

            // Issue #146 Phase 5: FFI marshalling is now a pure
            // function (aura::compiler::pure::ffi_marshal_args_pure)
            // that takes all dependencies as spans. The legacy
            // stateful loop above has been replaced with a single
            // call to the pure version; the FFI dispatch (calling
            // the function pointer below) stays in this function
            // because it depends on the per-FFI function pointer
            // from ffi_runtime_.
            std::span<const std::string> string_heap_view(string_heap_.data(), string_heap_.size());
            std::span<void* const> opaque_heap_view(opaque_heap_.data(), opaque_heap_.size());
            auto marshalled = aura::compiler::pure::ffi_marshal_args_pure(
                args, arg_types, string_heap_view, opaque_heap_view);
            const auto& i6 = marshalled.i_vals;
            const auto& d6 = marshalled.d_vals;
            // s6 and str_bufs are not used directly (we only
            // need the void*; s_vals already points into str_bufs
            // for lifetime).

            std::int64_t result_i = 0;
            double result_f = 0.0;

            if (marshalled.any_float) {
                auto f_fn =
                    reinterpret_cast<double (*)(double, double, double, double, double, double)>(
                        fn_ptr);
                result_f = f_fn(d6[0], d6[1], d6[2], d6[3], d6[4], d6[5]);
                if (ret_type == 2)
                    return types::make_float(result_f);
                if (ret_type == 1)
                    return types::make_int(static_cast<std::int64_t>(result_f));
                return types::make_float(result_f);
            } else {
                auto i_fn =
                    reinterpret_cast<std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t,
                                                      std::int64_t, std::int64_t, std::int64_t)>(
                        fn_ptr);
                result_i = i_fn(i6[0], i6[1], i6[2], i6[3], i6[4], i6[5]);
                if (ret_type == 2)
                    return types::make_float(std::bit_cast<double>(result_i));
                if (ret_type == 3 && result_i != 0) {
                    // String return: char* → string_heap
                    auto s = reinterpret_cast<const char*>(static_cast<std::intptr_t>(result_i));
                    // Issue #1397: size() read + emplace_back() must be atomic
                    // (fiber:spawn shares Evaluator across threads).
                    {
                        std::lock_guard lock(alloc_storage_lock_);
                        auto sidx = string_heap_.size();
                        string_heap_.emplace_back(s ? s : "");
                        return types::make_string(sidx);
                    }
                }
                if (ret_type == 4) {
                    // Opaque: store pointer in opaque_heap_, return OpaqueRef
                    auto oi = opaque_heap_.size();
                    opaque_heap_.push_back(reinterpret_cast<void*>(result_i));
                    return types::make_opaque(oi);
                }
                return types::make_int(result_i);
            }
        }
        return std::nullopt;
    }

    // Try tree-walker closures first
    // Issue #145 P0 follow-up: shared lock on closures_mtx_
    // while we look up AND copy the closure. The fiber
    // thread holds this lock for the duration of the copy
    // (microseconds), so the main thread's mutations don't
    // race with the lookup. Without this lock, the fiber
    // thread can see a half-modified Closure (the hash
    // table node's key/value pair can be in an inconsistent
    // state during insert/erase on the main thread).
    Closure cl_copy;
    bool cl_found = false;
    {
        std::shared_lock<std::shared_mutex> rlock(closures_mtx_);
        auto it = closures_.find(cid);
        if (it != closures_.end()) {
            cl_copy = it->second;
            cl_found = true;
        }
    }
    if (cl_found) {
        CompilerMetrics* metrics =
            compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_) : nullptr;
        if (metrics)
            metrics->closure_tw_calls.fetch_add(1, std::memory_order_relaxed);
        // Issue #1926: refuse tombstoned snapshots (move/free/GC raced
        // after map copy). Lifetime_version==0 ⇒ pointees may dangle.
        if (!cl_copy.lifetime_valid_for_views()) {
            g_closure_view_dangling_prevented_total.fetch_add(1, std::memory_order_relaxed);
            if (metrics) {
                metrics->closure_view_dangling_prevented_total.store(
                    g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics->closure_stale_returns.fetch_add(1, std::memory_order_relaxed);
            }
            bump_compiler_root_dangling_prevented();
            return std::nullopt;
        }
        // Issue #1926: revalidate under lock before materialize — map entry
        // may have been erased/tombstoned after the snapshot copy.
        if (!revalidate_closure_snapshot(cid, cl_copy)) {
            if (metrics) {
                metrics->closure_view_dangling_prevented_total.store(
                    g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics->closure_view_invalid_access_total.store(
                    g_closure_view_invalid_access_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics->closure_stale_returns.fetch_add(1, std::memory_order_relaxed);
            }
            bump_compiler_root_dangling_prevented();
            return std::nullopt;
        }
        // Issue #681: epoch + EnvFrame version pre-check before
        // materialize_call_env (live closure across post-mutate inval).
        if (closure_needs_safe_fallback(*this, cl_copy, metrics)) {
            if (metrics) {
                metrics->compiler_closure_safe_fallbacks.fetch_add(1, std::memory_order_relaxed);
                // Issue #1509: safe fallback apply (bridge re-path or refuse).
                metrics->closure_safe_fallback_apply_count_total.fetch_add(
                    1, std::memory_order_relaxed);
                // Issue #1525: multi-fiber safe fallback tally.
                metrics->multifiber_safe_fallback_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Issue #1511: dual-check + EnvFrame re-stamp at bridge entry.
            if (auto bridged = invoke_closure_bridge_checked(*this, closure_bridge_, cid, args,
                                                             metrics, &cl_copy))
                return bridged;
            if (metrics)
                metrics->closure_stale_returns.fetch_add(1, std::memory_order_relaxed);
            bump_compiler_root_dangling_prevented();
            return std::nullopt;
        }
        // Issue #145 Phase 2.3 — materialize the call env from
        // the SoA arena (env_frames_[cl.env_id]) -- legacy cl.env pointer path removed in P0.
        // materialize_call_env now takes env_frames_mtx_
        // internally (Issue #145 P0 follow-up — env_frames_
        // deque map-array reallocation race under fiber:spawn).
        Env ne = materialize_call_env(cl_copy);
        ne.set_primitives(&primitives_);
        // materialize_call_env returns a default Env (owner_=nullptr) for
        // no-capture closures; set owner so eval_flat's primitive dispatch
        // (eval_env.owner()->bump_primitive_call_count()) has a valid this.
        ne.set_owner(this);

        // Issue #145 / #1482 restore: set the pool so bind_symid can
        // mirror lambda params into the string-keyed bindings_ array
        // (so body code's lookup(name) still finds them). Prefer the
        // closure-captured pool; fall back to canonical_pool().
        if (cl_copy.pool)
            ne.set_pool(cl_copy.pool);
        else if (auto* cp = canonical_pool())
            ne.set_pool(cp);
        if (cl_copy.dotted) {
            // Dotted rest param: bind named params, collect rest into list
            std::size_t named_count = cl_copy.params.size() - 1;
            for (std::size_t i = 0; i < named_count && i < args.size(); ++i)
                ne.bind_symid(cl_copy.params[i], args[i]); // Issue #145: SymId
            // Collect remaining args into a pair list for the rest param
            types::EvalValue rest = make_void();
            for (std::size_t i = args.size(); i > named_count; --i) {
                auto pid = pairs_.size();
                pairs_.push_back({args[i - 1], rest});
                rest = make_pair(pid);
            }
            ne.bind_symid(cl_copy.params.back(), rest);
        } else {
            for (std::size_t i = 0; i < cl_copy.params.size() && i < args.size(); ++i)
                ne.bind_symid(cl_copy.params[i], args[i]); // Issue #145: SymId
        }
        if (cl_copy.flat) {
            // Issue #223: check if the closure's bridge is stale
            // (arena was reset, or major mutation invalidated the
            // captured flat*/pool*). If stale, the flat*/pool*
            // are dangling — invalidate the closure (return
            // nullopt) so the caller can re-bridge from the new
            // arena. The body_source re-parse fallback is a
            // future slice (requires parser integration).
            {
                // Issue #1287 / #1558 / #1655: race-window dual-check after
                // materialize_call_env — never eval through stale flat*/pool*
                // or EnvFrame after concurrent mutate / steal / GC compact.
                // (closure_needs_safe_fallback already ran; mutation may race
                // between that check and eval_flat.)
                //
                // Issue #1655: race-window path now uses closure_is_epoch_stale
                // (file-local helper) as the single source of truth for the
                // epoch-stale predicate. Replaces the previous inline
                // `bridge_stale || env_stale` check which left
                // closure_epoch_mismatch_fallback unconditional (bug: future
                // broadening of the predicate — e.g. adding a linear arm —
                // would silently inflate the metric). The body still gates
                // the envframe per-cause bump on env_stale separately to
                // preserve per-cause granularity.
                const bool env_stale =
                    cl_copy.env_id != NULL_ENV_ID &&
                    (is_env_frame_invalid(cl_copy.env_id) || is_env_frame_stale(cl_copy.env_id));
                // Issue #1926: second revalidate after materialize (GC/erase race).
                if (!revalidate_closure_snapshot(cid, cl_copy)) {
                    if (metrics) {
                        metrics->compiler_closure_safe_fallbacks.fetch_add(
                            1, std::memory_order_relaxed);
                        metrics->closure_view_dangling_prevented_total.store(
                            g_closure_view_dangling_prevented_total.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
                        metrics->closure_stale_apply_count_total.fetch_add(
                            1, std::memory_order_relaxed);
                        metrics->closure_race_caught_count_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    bump_stale_closure_prevented();
                    bump_compiler_root_dangling_prevented();
                    return std::nullopt;
                }
                if (closure_is_epoch_stale(*this, cl_copy)) {
                    if (metrics) {
                        metrics->compiler_closure_safe_fallbacks.fetch_add(
                            1, std::memory_order_relaxed);
                        metrics->closure_bridge_epoch_safety_enforced.fetch_add(
                            1, std::memory_order_relaxed);
                        if (env_stale)
                            metrics->compiler_closure_envframe_stale_total.fetch_add(
                                1, std::memory_order_relaxed);
                        metrics->closure_stale_apply_count_total.fetch_add(
                            1, std::memory_order_relaxed);
                        metrics->closure_safe_fallback_apply_count_total.fetch_add(
                            1, std::memory_order_relaxed);
                        metrics->closure_race_caught_count_total.fetch_add(
                            1, std::memory_order_relaxed);
                        metrics->multifiber_mutate_races_detected_total.fetch_add(
                            1, std::memory_order_relaxed);
                        metrics->multifiber_safe_fallback_total.fetch_add(
                            1, std::memory_order_relaxed);
                        record_epoch_stale_steal_caught(metrics);
                    }
                    // Issue #1485 / #1558 / #1632: race-window safety net for dual-epoch.
                    bump_stale_closure_prevented();
                    bump_closure_epoch_mismatch_fallback();
                    bump_compiler_live_closure_stale_prevented();
                    // Issue #1511: dual-check + EnvFrame re-stamp at bridge entry.
                    if (auto bridged = invoke_closure_bridge_checked(*this, closure_bridge_, cid,
                                                                     args, metrics, &cl_copy))
                        return bridged;
                    if (metrics)
                        metrics->closure_stale_returns.fetch_add(1, std::memory_order_relaxed);
                    return std::nullopt;
                }
            }
            if (metrics)
                metrics->bridge_epoch_hit_count_.fetch_add(1, std::memory_order_relaxed);
            auto r = eval_flat(*cl_copy.flat, *cl_copy.pool, cl_copy.body_id, ne);
            if (r)
                return *r;
        }
        return std::nullopt;
    }

    // Try IR bridge (local-map miss). Issue #1511: still go through
    // dual-check helper (provenance=null → no local epoch to check;
    // bridge_calls metric still bumped; IR path has its own checks).
    if (closure_bridge_) {
        CompilerMetrics* metrics =
            compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_) : nullptr;
        return invoke_closure_bridge_checked(*this, closure_bridge_, cid, args, metrics,
                                             /*provenance=*/nullptr);
    }

    return std::nullopt;
}

// ── ast_to_data: convert AST subtree to EvalValue data ───────
EvalValue Evaluator::ast_to_data(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                 aura::ast::NodeId nid) {
    if (nid == ast::NULL_NODE)
        return make_void();
    auto v = flat.get(nid);
    // Local helper: build (cons "fn-name" args)
    auto cd = [&](const std::string& fn, const EvalValue& args) -> EvalValue {
        // Issue #1397: size() read + push_back() + index return must be
        // atomic (fiber:spawn shares Evaluator across threads).
        std::lock_guard lock(alloc_storage_lock_);
        auto fi = string_heap_.size();
        string_heap_.push_back(fn);
        auto pi = pairs_.size();
        pairs_.push_back({types::make_string(fi), args});
        return types::make_pair(pi);
    };

    switch (v.tag) {
        case ast::NodeTag::LiteralInt:
            return make_int(v.int_value);
        case ast::NodeTag::LiteralFloat:
            return make_float(v.float_value);
        case ast::NodeTag::LiteralString: {
            auto name = std::string(pool.resolve(v.sym_id));
            auto idx = string_heap_.size();
            string_heap_.push_back(std::move(name));
            return make_string(idx);
        }
        case ast::NodeTag::Variable: {
            auto name = std::string(pool.resolve(v.sym_id));
            // Issue #231: dedup by content so two quote literals
            // of the same symbol produce the same string heap
            // index. Without this, `(eq? 'eda:module 'eda:module)`
            // returns #f because each quote creates a fresh
            // string heap entry. Use short_str_cache_ for short
            // symbols (the common case for symbols with colon
            // prefixes like eda:module), and a heap scan for
            // longer ones. Symbols are usually < 32 chars so the
            // linear scan is bounded.
            if (name.size() <= 6) {
                auto it = short_str_cache_.find(name);
                if (it != short_str_cache_.end())
                    return it->second;
            } else {
                for (std::size_t i = 0; i < string_heap_.size(); ++i) {
                    if (string_heap_[i] == name)
                        return make_string(static_cast<std::int64_t>(i));
                }
            }
            auto idx = string_heap_.size();
            string_heap_.push_back(std::move(name));
            auto val = make_string(static_cast<std::int64_t>(idx));
            if (name.size() <= 6)
                short_str_cache_[name] = val;
            return val;
        }
        case ast::NodeTag::Call: {
            // Issue #1397: size() read + push_back() in the per-iter
            // loop must be atomic to keep the returned pair_idx stable
            // across concurrent fiber:spawn workers.
            std::lock_guard lock(alloc_storage_lock_);
            EvalValue tail = make_void();
            for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
                auto item = ast_to_data(flat, pool, *it);
                auto pair_idx = pairs_.size();
                pairs_.push_back(Pair{std::move(item), tail});
                tail = make_pair(pair_idx);
            }
            return tail;
        }
        case ast::NodeTag::Begin: {
            std::lock_guard lock(alloc_storage_lock_);
            EvalValue tail = make_void();
            for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
                auto item = ast_to_data(flat, pool, *it);
                auto pair_idx = pairs_.size();
                pairs_.push_back(Pair{std::move(item), tail});
                tail = make_pair(pair_idx);
            }
            return cd("begin", tail);
        }
        case ast::NodeTag::IfExpr: {
            auto cond = v.children.size() > 0 ? ast_to_data(flat, pool, v.child(0)) : make_void();
            auto then_b = v.children.size() > 1 ? ast_to_data(flat, pool, v.child(1)) : make_void();
            auto else_b = v.children.size() > 2 ? ast_to_data(flat, pool, v.child(2)) : make_void();
            // Issue #1397: lock across the two push_backs + make_pair calls
            // so the tail indices the IfExpr chain uses are stable.
            std::lock_guard lock(alloc_storage_lock_);
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({then_b, else_b});
            tail = make_pair(pairs_.size());
            pairs_.push_back({cond, tail});
            return cd("if", tail);
        }
        case ast::NodeTag::Lambda: {
            // Issue #1397: each push_back in the per-param loop + final body
            // push_back is atomic for stable returned pair index.
            std::lock_guard lock(alloc_storage_lock_);
            EvalValue params_tail = make_void();
            for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
                auto pname = std::string(pool.resolve(*it));
                auto pidx = string_heap_.size();
                string_heap_.push_back(pname);
                auto pair_idx = pairs_.size();
                pairs_.push_back({make_string(pidx), params_tail});
                params_tail = make_pair(pair_idx);
            }
            auto body = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({params_tail, body});
            return cd("lambda", tail);
        }
        case ast::NodeTag::Define: {
            auto name_str = std::string(pool.resolve(v.sym_id));
            // Issue #1397: size() + push_back for both string_heap_ and pairs_
            // must be atomic so the symbol / value pair has stable indices.
            std::lock_guard lock(alloc_storage_lock_);
            auto nidx = string_heap_.size();
            string_heap_.push_back(name_str);
            auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({make_string(nidx), val});
            return cd("define", tail);
        }
        case ast::NodeTag::DefineType: {
            auto type_name = pool.resolve(v.sym_id);
            auto tnidx = string_heap_.size();
            string_heap_.push_back(std::string(type_name));
            EvalValue params_tail = make_void();
            for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
                auto pname = std::string(pool.resolve(*it));
                auto pidx = string_heap_.size();
                string_heap_.push_back(pname);
                auto pp = pairs_.size();
                pairs_.push_back({make_string(pidx), params_tail});
                params_tail = make_pair(pp);
            }
            auto type_spec = make_pair(pairs_.size());
            pairs_.push_back({make_string(tnidx), params_tail});
            EvalValue ctors_tail = make_void();
            for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
                auto ctor_data = ast_to_data(flat, pool, *it);
                auto pp = pairs_.size();
                pairs_.push_back({ctor_data, ctors_tail});
                ctors_tail = make_pair(pp);
            }
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({type_spec, ctors_tail});
            return cd("define-type", tail);
        }
        case ast::NodeTag::Set: {
            auto name_str = std::string(pool.resolve(v.sym_id));
            auto nidx = string_heap_.size();
            string_heap_.push_back(name_str);
            auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({make_string(nidx), val});
            return cd("set!", tail);
        }
        case ast::NodeTag::Let:
        case ast::NodeTag::LetRec: {
            // The let node from add_let / parser has:
            //   sym_id = binding name (e.g. x)
            //   children = [val_node, body_node]
            // The body is always the LAST child.
            auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
            auto bname = std::string(pool.resolve(v.sym_id));
            auto bni = string_heap_.size();
            string_heap_.push_back(bname);
            auto bv =
                val_id != aura::ast::NULL_NODE ? ast_to_data(flat, pool, val_id) : make_void();
            auto body =
                body_id != aura::ast::NULL_NODE ? ast_to_data(flat, pool, body_id) : make_void();
            // Build: (cons name val) → bindings pair
            auto bp = pairs_.size();
            pairs_.push_back({make_string(bni), bv});
            auto bindings_tail = make_pair(bp);
            // Build: (cons bindings body) → complete let form
            auto full_bindings = make_pair(pairs_.size());
            pairs_.push_back({bindings_tail, body});
            auto kind = v.tag == ast::NodeTag::LetRec ? "letrec" : "let";
            return cd(kind, full_bindings);
        }
        case ast::NodeTag::Quote: {
            if (!v.children.empty()) {
                auto quoted = ast_to_data(flat, pool, v.child(0));
                auto tail = make_pair(pairs_.size());
                pairs_.push_back({quoted, make_void()});
                return cd("quote", tail);
            }
            return make_void();
        }
        case ast::NodeTag::Coercion: {
            if (!v.children.empty()) {
                auto expr = ast_to_data(flat, pool, v.child(0));
                auto tail = make_pair(pairs_.size());
                pairs_.push_back({expr, make_void()});
                return cd("cast", tail);
            }
            return make_void();
        }
        case ast::NodeTag::Pair: {
            auto car = ast_to_data(flat, pool, v.child(0));
            auto cdr_val = ast_to_data(flat, pool, v.child(1));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({cdr_val, make_void()});
            tail = make_pair(pairs_.size());
            pairs_.push_back({car, tail});
            return cd("cons", tail);
        }
        default:
            return make_void();
    }
}
// Inverse of ast_to_data. Needed so lambda bodies from macro data
// can be converted to AST for closure creation.
ast::NodeId Evaluator::data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat,
                                    aura::ast::StringPool& pool, int depth) {
    using namespace types;
    if (depth > 256)
        return ast::NULL_NODE;
    if (is_int(data)) {
        return flat.add_literal(as_int(data));
    }
    if (is_float(data)) {
        return flat.add_literal_float(as_float(data));
    }
    if (is_bool(data)) {
        auto id = flat.add_literal(as_bool(data) ? 1 : 0);
        flat.set_marker(id, ast::SyntaxMarker::BoolLiteral);
        return id;
    }
    if (is_void(data)) {
        return flat.add_literal(0); // () sentinel
    }
    if (is_string(data)) {
        auto idx = as_string_idx(data);
        if (idx < string_heap_.size()) {
            // Strings in code context are variable references
            auto name = string_heap_[idx];
            auto sid = pool.intern(name);
            return flat.add_variable(sid);
        }
        return ast::NULL_NODE;
    }
    if (is_pair(data)) {
        auto pair_idx = as_pair_idx(data);
        if (pair_idx >= pairs_.size())
            return ast::NULL_NODE;

        auto car_data = pairs_[pair_idx].car;
        auto cdr_data = pairs_[pair_idx].cdr;

        if (is_string(car_data)) {
            auto fn_idx = as_string_idx(car_data);
            auto fn_name = fn_idx < string_heap_.size() ? string_heap_[fn_idx] : "";

            // Quote: (quote expr)
            if (fn_name == "quote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto quoted = pairs_[qp].car;
                auto quoted_node = data_to_flat(quoted, flat, pool, depth + 1);
                if (quoted_node != ast::NULL_NODE)
                    return flat.add_quote(quoted_node);
                return flat.add_literal(0); // fallback
            }

            // Quasiquote: (quasiquote expr)
            if (fn_name == "quasiquote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto quoted = pairs_[qp].car;
                return data_to_flat(quoted, flat, pool, depth + 1);
            }

            // Unquote: (unquote expr) — just convert the inner expression
            if (fn_name == "unquote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto inner = pairs_[qp].car;
                return data_to_flat(inner, flat, pool, depth + 1);
            }

            // Define-type: (define-type (type-name params...) (ctor1 ctor2 ...))
            if (fn_name == "define-type") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto type_name_data = pairs_[np].car;
                    auto ctor_rest = pairs_[np].cdr;

                    aura::ast::SymId type_name_val = 0;
                    std::vector<aura::ast::SymId> params;
                    std::vector<ast::NodeId> ctors;

                    if (is_pair(type_name_data)) {
                        auto tnp = as_pair_idx(type_name_data);
                        if (is_string(pairs_[tnp].car)) {
                            auto ti = as_string_idx(pairs_[tnp].car);
                            auto ts = ti < string_heap_.size() ? string_heap_[ti] : "";
                            type_name_val = pool.intern(ts);
                            auto rest = pairs_[tnp].cdr;
                            while (is_pair(rest)) {
                                auto rp = as_pair_idx(rest);
                                if (is_string(pairs_[rp].car)) {
                                    auto pi = as_string_idx(pairs_[rp].car);
                                    auto ps = pi < string_heap_.size() ? string_heap_[pi] : "";
                                    params.push_back(pool.intern(ps));
                                }
                                rest = pairs_[rp].cdr;
                            }
                        }
                    } else if (is_string(type_name_data)) {
                        auto ti = as_string_idx(type_name_data);
                        auto ts = ti < string_heap_.size() ? string_heap_[ti] : "";
                        type_name_val = pool.intern(ts);
                    }

                    if (type_name_val != 0) {
                        auto cur = ctor_rest;
                        while (is_pair(cur)) {
                            auto cp = as_pair_idx(cur);
                            auto ctor_form = pairs_[cp].car;
                            cur = pairs_[cp].cdr;

                            if (is_pair(ctor_form)) {
                                auto ctor_pair = as_pair_idx(ctor_form);
                                auto ctor_car = pairs_[ctor_pair].car;
                                // Handle (quote Name) format
                                if (is_string(ctor_car)) {
                                    auto ci = as_string_idx(ctor_car);
                                    auto cs = ci < string_heap_.size() ? string_heap_[ci] : "";
                                    if (cs == "quote" && is_pair(pairs_[ctor_pair].cdr)) {
                                        // (quote Some) — extract "Some" from cdr
                                        auto qcdr = as_pair_idx(pairs_[ctor_pair].cdr);
                                        auto name_val = pairs_[qcdr].car;
                                        if (is_string(name_val)) {
                                            auto ni = as_string_idx(name_val);
                                            auto ns =
                                                ni < string_heap_.size() ? string_heap_[ni] : "";
                                            auto ctor_var = flat.add_variable(pool.intern(ns));
                                            ctors.push_back(flat.add_quote(ctor_var));
                                        }
                                    }
                                } else {
                                    // Direct ctor descriptor — store as-is
                                    auto ctor_node = data_to_flat(ctor_form, flat, pool, depth + 1);
                                    if (ctor_node != ast::NULL_NODE)
                                        ctors.push_back(ctor_node);
                                }
                            }
                        }
                        return flat.add_define_type(type_name_val, params, ctors);
                    }
                }
                return ast::NULL_NODE;
            }

            // Begin: (begin ...)
            if (fn_name == "begin") {
                std::vector<ast::NodeId> exprs;
                auto cur = cdr_data;
                while (is_pair(cur)) {
                    auto cp = as_pair_idx(cur);
                    auto e = data_to_flat(pairs_[cp].car, flat, pool, depth + 1);
                    if (e != ast::NULL_NODE)
                        exprs.push_back(e);
                    cur = pairs_[cp].cdr;
                }
                return flat.add_begin(exprs);
            }

            // If: (if cond then else)
            if (fn_name == "if") {
                ast::NodeId cond_node = ast::NULL_NODE, then_node = ast::NULL_NODE,
                            else_node = ast::NULL_NODE;
                if (is_pair(cdr_data)) {
                    auto cp = as_pair_idx(cdr_data);
                    cond_node = data_to_flat(pairs_[cp].car, flat, pool, depth + 1);
                    auto rest = pairs_[cp].cdr;
                    if (is_pair(rest)) {
                        auto tp = as_pair_idx(rest);
                        then_node = data_to_flat(pairs_[tp].car, flat, pool, depth + 1);
                        auto erest = pairs_[tp].cdr;
                        if (is_pair(erest)) {
                            auto ep = as_pair_idx(erest);
                            else_node = data_to_flat(pairs_[ep].car, flat, pool, depth + 1);
                        }
                    }
                }
                if (cond_node != ast::NULL_NODE && then_node != ast::NULL_NODE &&
                    else_node != ast::NULL_NODE)
                    return flat.add_if(cond_node, then_node, else_node);
                if (cond_node != ast::NULL_NODE && then_node != ast::NULL_NODE)
                    return flat.add_if(cond_node, then_node, flat.add_literal(0));
                return cond_node != ast::NULL_NODE ? cond_node : flat.add_literal(0);
            }

            // Lambda: (lambda (args) body)
            if (fn_name == "lambda") {
                if (is_pair(cdr_data)) {
                    auto params_pair = as_pair_idx(cdr_data);
                    auto params_data = pairs_[params_pair].car;
                    auto body_rest = pairs_[params_pair].cdr;

                    std::vector<ast::SymId> params;
                    auto args_data = params_data;
                    while (is_pair(args_data)) {
                        auto ap = as_pair_idx(args_data);
                        auto arg = pairs_[ap].car;
                        if (is_string(arg)) {
                            auto aidx = as_string_idx(arg);
                            auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                            params.push_back(pool.intern(astr));
                        }
                        args_data = pairs_[ap].cdr;
                    }

                    ast::NodeId body_node = ast::NULL_NODE;
                    if (is_pair(body_rest)) {
                        auto bp = as_pair_idx(body_rest);
                        body_node = data_to_flat(pairs_[bp].car, flat, pool, depth + 1);
                    }
                    if (body_node == ast::NULL_NODE)
                        body_node = flat.add_literal(0);
                    return flat.add_lambda(params, body_node);
                }
                return ast::NULL_NODE;
            }

            // Define: (define name value)
            if (fn_name == "define") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto name_data = pairs_[np].car;
                    auto val_rest = pairs_[np].cdr;

                    if (is_string(name_data) && is_pair(val_rest)) {
                        auto ni = as_string_idx(name_data);
                        auto ns = ni < string_heap_.size() ? string_heap_[ni] : "";
                        auto vp = as_pair_idx(val_rest);
                        auto val_node = data_to_flat(pairs_[vp].car, flat, pool, depth + 1);
                        return flat.add_define(pool.intern(ns), val_node);
                    }
                }
                return ast::NULL_NODE;
            }

            // Issue #150 Phase 1: performance-region / evolution-region
            // wrappers. The forms are:
            //   (performance-region (define f ...))
            //   (performance-region (lambda (x) ...))
            //   (evolution-region (define g ...))
            //   (evolution-region (lambda (x) ...))
            // The wrapper is transparent — the inner form is
            // processed normally — but it records a side-table
            // mapping the resulting function's name to the
            // specified region. The lowering pass (FlatFnBuilder
            // in lowering_impl.cpp) reads this side-table to set
            // IRFunction::region accordingly.
            //
            // If the inner form is not a define/lambda, the
            // wrapper is silently a no-op (the inner form is
            // processed; the region tag is discarded). This
            // matches the user's mental model: "annotate this
            // form with a region hint" — the hint is only useful
            // for define/lambda; for other forms it's ignored.
            if (fn_name == "performance-region" || fn_name == "evolution-region") {
                if (is_pair(cdr_data)) {
                    auto rp = as_pair_idx(cdr_data);
                    auto inner_node = data_to_flat(pairs_[rp].car, flat, pool, depth + 1);
                    if (inner_node == ast::NULL_NODE)
                        return ast::NULL_NODE;
                    // The inner form should be a Define or Lambda.
                    // If it's a Define, get the name and record
                    // (name -> region) in the side-table.
                    if (flat.get(inner_node).tag == ast::NodeTag::Define) {
                        auto sym = flat.get(inner_node).sym_id;
                        if (sym != ast::INVALID_SYM) {
                            auto region = (fn_name == "performance-region")
                                              ? std::uint8_t{1} /*Performance*/
                                              : std::uint8_t{2} /*Evolution*/;
                            flat.set_function_region(sym, region);
                        }
                    } else if (flat.get(inner_node).tag == ast::NodeTag::Lambda) {
                        // Lambdas are anonymous — use the
                        // overload-tagged setter for lambdas.
                        auto region = (fn_name == "performance-region")
                                          ? std::uint8_t{1} /*Performance*/
                                          : std::uint8_t{2} /*Evolution*/;
                        flat.set_function_region_lambda(inner_node, region);
                    }
                    return inner_node;
                }
                return ast::NULL_NODE;
            }

            // Set!: (set! name value)
            if (fn_name == "set!") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto name_val = pairs_[np].car;
                    auto val_rest = pairs_[np].cdr;
                    if (is_string(name_val) && is_pair(val_rest)) {
                        auto ni = as_string_idx(name_val);
                        auto ns = ni < string_heap_.size() ? string_heap_[ni] : "";
                        auto vp = as_pair_idx(val_rest);
                        auto val_node = data_to_flat(pairs_[vp].car, flat, pool, depth + 1);
                        return flat.add_set(pool.intern(ns), val_node);
                    }
                }
                return ast::NULL_NODE;
            }

            // Let: (let ((x val)) body)
            if (fn_name == "let") {
                // For data_to_flat, let is just a call node
                // (let ((x val)) body) is sugar and should already be expanded
                // Just treat as a general call
            }
        }

        // General function call: build Call(node func, [args...])
        auto func_node = data_to_flat(car_data, flat, pool, depth);
        if (func_node == ast::NULL_NODE)
            return ast::NULL_NODE;
        std::vector<ast::NodeId> args;
        auto cur = cdr_data;
        while (is_pair(cur)) {
            auto cp = as_pair_idx(cur);
            auto arg_data = pairs_[cp].car;
            // Direct string arguments (e.g., "hello" in quoted form) → LiteralString
            // Nested expressions → recurse normally
            if (is_string(arg_data)) {
                auto sidx = as_string_idx(arg_data);
                if (sidx < string_heap_.size()) {
                    auto name = string_heap_[sidx];
                    auto ssid = pool.intern(name);
                    // Issue #334 (Cycle 1): use add_variable
                    // instead of add_literalstring. In a macro
                    // body, strings represent identifier names
                    // (e.g. field names spliced into a lambda
                    // body), and we want runtime lookup, not a
                    // literal value. This affects ALL callers
                    // of data_to_flat, but the change is
                    // behaviorally a no-op for code that doesn't
                    // have bare-string args in non-quote
                    // positions — LiteralString args would
                    // always have been rare and unusual.
                    args.push_back(flat.add_variable(ssid));
                }
            } else {
                auto a = data_to_flat(arg_data, flat, pool, depth + 1);
                if (a != ast::NULL_NODE)
                    args.push_back(a);
            }
            cur = pairs_[cp].cdr;
        }
        return flat.add_call(func_node, args);
    }
    if (is_cell(data)) {
        // Dereference cell and convert the inner value
        auto cid = as_cell_id(data);
        if (cid < cells_.size())
            return data_to_flat(cells_[cid], flat, pool, depth + 1);
    }
    return ast::NULL_NODE;
}

// ── eval_data_as_code: evaluate macro-expanded data as code ──
// Macro bodies produce data (lists) via cons/quote chains.
// This function interprets that data as code and evaluates it.
// flat/pool are needed for lambda and define-shorthand handling.
//
// Issue #230 #2 follow-up: unwrap Quote-wrapping on set! targets.
EvalResult Evaluator::eval_data_as_code(const types::EvalValue& data, const Env& env,
                                        ast::FlatAST* flat, ast::StringPool* pool) {
    // Not a pair → literal value (number, string, bool, void)
    if (!types::is_pair(data)) {
        // Strings are literal symbols/data, return as-is
        return data;
    }

    // Pair: (fn arg1 arg2 ...) or (special-form arg ...)
    auto pair_idx = types::as_pair_idx(data);
    if (pair_idx >= pairs_.size())
        return make_void();

    auto car_val = pairs_[pair_idx].car;
    auto cdr_val = pairs_[pair_idx].cdr;

    // Handle special forms by name
    if (types::is_string(car_val)) {
        auto fn_idx = types::as_string_idx(car_val);
        auto fn_name = fn_idx < string_heap_.size() ? string_heap_[fn_idx] : "";

        // ── if: (if cond then else) ──
        if (fn_name == "if") {
            if (types::is_pair(cdr_val)) {
                auto cond_pair = types::as_pair_idx(cdr_val);
                auto cond_val = pairs_[cond_pair].car;
                auto rest = pairs_[cond_pair].cdr;
                auto cond_result = eval_data_as_code(cond_val, env, flat, pool);
                if (!cond_result)
                    return cond_result;
                if (types::is_pair(rest)) {
                    auto then_pair = types::as_pair_idx(rest);
                    auto then_val = pairs_[then_pair].car;
                    auto else_rest = pairs_[then_pair].cdr;
                    if (aura::compiler::pure::is_truthy(*cond_result)) {
                        auto r = eval_data_as_code(then_val, env, flat, pool);
                        return r;
                    } else {
                        // Evaluate else branch
                        if (types::is_pair(else_rest)) {
                            auto else_pair = types::as_pair_idx(else_rest);
                            auto else_val = pairs_[else_pair].car;
                            return eval_data_as_code(else_val, env, flat, pool);
                        }
                    }
                }
            }
            return make_void();
        }

        // ── when: (when cond body...) — like (if cond (begin body...) (void))
        if (fn_name == "when" || fn_name == "unless") {
            if (types::is_pair(cdr_val)) {
                auto cond_pair = types::as_pair_idx(cdr_val);
                auto cond_val = pairs_[cond_pair].car;
                auto body_rest = pairs_[cond_pair].cdr;
                auto cond_result = eval_data_as_code(cond_val, env, flat, pool);
                if (!cond_result)
                    return cond_result;
                if (aura::compiler::pure::is_truthy(*cond_result)) {
                    // Evaluate body expressions sequentially
                    EvalResult last = make_void();
                    while (types::is_pair(body_rest)) {
                        auto bp = types::as_pair_idx(body_rest);
                        last = eval_data_as_code(pairs_[bp].car, env, flat, pool);
                        if (!last)
                            return last;
                        body_rest = pairs_[bp].cdr;
                    }
                    return last;
                }
            }
            return make_void();
        }

        // ── lambda: (lambda (params) body) ──
        // Needs flat/pool to create an AST closure
        if (fn_name == "lambda") {
            if (!flat || !pool) {
                // Without flat/pool, we can't create closures — return as-is
                return make_void();
            }
            if (types::is_pair(cdr_val)) {
                auto params_pair = types::as_pair_idx(cdr_val);
                auto params_data = pairs_[params_pair].car; // (arg1 arg2 ...)
                auto body_rest = pairs_[params_pair].cdr;   // (body ...)

                // Extract param names
                std::vector<ast::SymId> param_syms;
                auto args_data = params_data;
                while (types::is_pair(args_data)) {
                    auto ap = types::as_pair_idx(args_data);
                    auto arg_data = pairs_[ap].car;
                    if (types::is_string(arg_data)) {
                        auto aidx = types::as_string_idx(arg_data);
                        auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                        param_syms.push_back(pool->intern(astr));
                    }
                    args_data = pairs_[ap].cdr;
                }

                // Extract and convert body data to FlatAST
                ast::NodeId body_node = ast::NULL_NODE;
                if (types::is_pair(body_rest)) {
                    auto bp = types::as_pair_idx(body_rest);
                    auto body_data = pairs_[bp].car;
                    body_node = data_to_flat(body_data, *flat, *pool);
                }
                if (body_node == ast::NULL_NODE)
                    body_node = flat->add_literal(0);

                // Create lambda node and closure
                auto lambda_id = flat->add_lambda(param_syms, body_node);
                auto cid = next_id();
                // Allocate closure body in temp arena during task context,
                // otherwise in persistent arena (module functions, while loops).
                auto* target_arena = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                if (!target_arena) {
                    return make_void();
                }
                auto cl_alloc = target_arena->allocator();
                auto* cl_flat = target_arena->create<aura::ast::FlatAST>(cl_alloc);
                // Issue #334: keep the closure's body symids in the SAME
                // pool as the macro's `pool` and the env's bindings. If
                // we use a fresh cl_pool for the cloned body, the body's
                // Variable symids and the env's symids are in different
                // pools, so Env::lookup_by_symid (the fast path) misses
                // and the body's lambda params are reported as unbound.
                // Using the macro's pool here aligns the body's Variables
                // with the env's bindings at lookup time.
                auto* cl_pool = pool;
                auto cloned_body =
                    clone_macro_body(*cl_flat, *cl_pool, *flat, *pool, body_node, nullptr,
                                     /*name_map=*/nullptr,
                                     /*cloned_marker=*/aura::ast::SyntaxMarker::User);
                // Issue #230 #2 follow-up: undo the Quote-wrap on set!
                // targets. The Quote-wrap was added to give symbol-
                // generating macros access to the literal arg value,
                // but it broke set! semantics for normal macros. The
                // pass walks the body, finds `(set! <target> <value>)`
                // calls, and replaces the <target> Quote with the
                // inner Variable. This is safe to run on every
                // macro body — bodies that don't use set! have no
                // set! calls, so the pass is a no-op for them.
                // unwrap_set_quotes removed (Quote-wrap reverted)
                cl_flat->root = cloned_body;
                // P0: register captured for SoA, no legacy env pointer in Closure.
                EnvId cap_id = alloc_env_frame_from_env(env);
                {
                    std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
                    Closure cl{"", {}, cl_flat, cl_pool, cloned_body, cap_id, false, target_arena};
                    // Issue #1365: stamp bridge_epoch at construction
                    stamp_closure_bridge_epoch(cl);
                    // Store param SymIds directly (Issue #145: SoA migration).
                    for (auto ps : param_syms)
                        cl.params.push_back(ps);
                    closures_[cid] = std::move(cl);
                }
                return make_closure(cid);
            }
            return make_void();
        }

        // ── begin: (begin expr1 expr2 ...) ──
        if (fn_name == "begin") {
            auto current = cdr_val;
            EvalResult last = make_void();
            while (types::is_pair(current)) {
                auto elem_pair = types::as_pair_idx(current);
                last = eval_data_as_code(pairs_[elem_pair].car, env, flat, pool);
                if (!last)
                    return last;
                current = pairs_[elem_pair].cdr;
            }
            return last;
        }

        // ── quote: (quote expr) ──
        if (fn_name == "quote") {
            if (types::is_pair(cdr_val)) {
                auto quote_pair = types::as_pair_idx(cdr_val);
                return pairs_[quote_pair].car; // Return the quoted value as-is
            }
            return make_void();
        }

        // ── define: (define name value) or (define (name args) body) ──
        if (fn_name == "define") {
            if (types::is_pair(cdr_val)) {
                auto name_pair = types::as_pair_idx(cdr_val);
                auto name_val = pairs_[name_pair].car;
                auto val_rest = pairs_[name_pair].cdr;

                // (define name value)
                if (types::is_string(name_val) && types::is_pair(val_rest)) {
                    auto val_pair = types::as_pair_idx(val_rest);
                    auto val = eval_data_as_code(pairs_[val_pair].car, env, flat, pool);
                    if (val) {
                        auto name_idx = types::as_string_idx(name_val);
                        auto name_str =
                            name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                        auto ci = alloc_cell(make_void());
                        const_cast<Env&>(env).bind(name_str, make_cell(ci));
                        cells_[ci] = *val;
                        return *val;
                    }
                    return val;
                }

                // (define (name args...) body) — function shorthand
                if (types::is_pair(name_val) && types::is_pair(val_rest) && flat && pool) {
                    auto fn_pair = types::as_pair_idx(name_val);
                    auto fn_name_data = pairs_[fn_pair].car;
                    auto fn_args_data = pairs_[fn_pair].cdr;

                    if (types::is_string(fn_name_data)) {
                        auto ni = types::as_string_idx(fn_name_data);
                        auto fn_str = ni < string_heap_.size() ? string_heap_[ni] : "";

                        // Extract param names from (arg1 arg2 ...)
                        std::vector<ast::SymId> param_syms;
                        auto args_data = fn_args_data;
                        while (types::is_pair(args_data)) {
                            auto ap = types::as_pair_idx(args_data);
                            auto arg_data = pairs_[ap].car;
                            if (types::is_string(arg_data)) {
                                auto aidx = types::as_string_idx(arg_data);
                                auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                                param_syms.push_back(pool->intern(astr));
                            }
                            args_data = pairs_[ap].cdr;
                        }

                        // Extract and convert body data
                        ast::NodeId body_node = ast::NULL_NODE;
                        if (types::is_pair(val_rest)) {
                            auto bp = types::as_pair_idx(val_rest);
                            auto body_data = pairs_[bp].car;
                            body_node = data_to_flat(body_data, *flat, *pool);
                        }
                        if (body_node == ast::NULL_NODE)
                            body_node = flat->add_literal(0);

                        // Create lambda node and closure
                        auto lambda_id = flat->add_lambda(param_syms, body_node);
                        auto cid = next_id();
                        auto* target = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                        auto* copied_env = copy_env(env, target);
                        Closure cl;
                        // Issue #1365: stamp first so unstamped closures cannot bypass safety
                        stamp_closure_bridge_epoch(cl);
                        for (auto ps : param_syms) {
                            cl.params.push_back(ps); // Issue #145: SymId, not string
                        }
                        cl.name = fn_str;
                        cl.flat = flat;
                        cl.pool = pool;
                        cl.body_id = body_node;
                        // P0: legacy cl.env removed. Always register in env_frames_
                        // for SoA (GC-safe, no pointer). env_id is now the only handle.
                        cl.env_id = alloc_env_frame_from_env(*copied_env);
                        cl.owner_arena = target;
                        {
                            std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
                            closures_[cid] = std::move(cl);
                        }

                        // Bind in env
                        auto ci = alloc_cell(make_void());
                        const_cast<Env&>(env).bind(fn_str, make_cell(ci));
                        cells_[ci] = make_closure(cid);
                        return make_closure(cid);
                    }
                }
            }
            return make_void();
        }

        // ── set!: (set! name value) ──
        if (fn_name == "set!") {
            if (types::is_pair(cdr_val)) {
                auto name_pair = types::as_pair_idx(cdr_val);
                auto name_val = pairs_[name_pair].car;
                auto val_rest = pairs_[name_pair].cdr;
                if (types::is_string(name_val) && types::is_pair(val_rest)) {
                    auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_rest)].car, env,
                                                 flat, pool);
                    if (val) {
                        auto name_idx = types::as_string_idx(name_val);
                        auto name_str =
                            name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                        auto cell_idx = const_cast<Env&>(env).lookup_cell_index(name_str);
                        if (cell_idx && *cell_idx < cells_.size()) {
                            cells_[*cell_idx] = *val;
                            return *val;
                        }
                    }
                }
            }
            return make_void();
        }

        // ── let: (let ((x val)) body) ──
        if (fn_name == "let") {
            if (types::is_pair(cdr_val)) {
                auto bindings_val = pairs_[types::as_pair_idx(cdr_val)].car;
                auto body_rest = pairs_[types::as_pair_idx(cdr_val)].cdr;
                // Collect bindings
                std::vector<std::pair<std::string, EvalValue>> bindings;
                auto current = bindings_val;
                while (types::is_pair(current)) {
                    auto binding_pair = pairs_[types::as_pair_idx(current)].car;
                    if (types::is_pair(binding_pair)) {
                        auto name_val = pairs_[types::as_pair_idx(binding_pair)].car;
                        auto val_expr = pairs_[types::as_pair_idx(binding_pair)].cdr;
                        if (types::is_string(name_val) && types::is_pair(val_expr)) {
                            auto name_idx = types::as_string_idx(name_val);
                            auto name_str =
                                name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                            auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_expr)].car,
                                                         env, flat, pool);
                            if (!val)
                                return val;
                            bindings.emplace_back(name_str, *val);
                        }
                    }
                    current = pairs_[types::as_pair_idx(current)].cdr;
                }
                // Create new env and bind
                Env new_env(&env);
                new_env.set_primitives(&primitives_);

                for (auto& [n, v] : bindings)
                    new_env.bind(n, v);
                // Evaluate body in new env
                auto body_current = body_rest;
                EvalResult last = make_void();
                while (types::is_pair(body_current)) {
                    auto elem_pair = types::as_pair_idx(body_current);
                    last = eval_data_as_code(pairs_[elem_pair].car, new_env, flat, pool);
                    if (!last)
                        return last;
                    body_current = pairs_[elem_pair].cdr;
                }
                return last;
            }
            return make_void();
        }

        // ── General function call ──
        // Look up the function in the environment or primitives.
        // #223 follow-up: skip primitive lookup when fn_name is
        // empty (e.g. when fn_idx was out of bounds at line 17542).
        // Without this guard, env.lookup_primitive("") triggers
        // the pre (!n.empty()) contract on Primitives::lookup.
        // The environment lookup below (lookup_by_intern) handles
        // missing names gracefully via nullopt.
        std::optional<PrimFn> prim;
        if (!fn_name.empty()) {
            prim = env.lookup_primitive(fn_name);
        }
        if (prim) {
            std::vector<EvalValue> args;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                if (!arg_val)
                    return arg_val;
                args.push_back(*arg_val);
                current = pairs_[arg_pair].cdr;
            }
            // Issue #441 / #1357: call count + optional render latency.
            return invoke_prim_with_telemetry(fn_name, [&]() { return (*prim)(args); });
        }

        // Issue #158: macro expansion in eval_data_as_code. The
        // cons-chain result of a legacy `defmacro` body is
        // re-evaluated here. If the head of the list is itself a
        // macro (e.g., the qq-built form `(bar ,x)` in a macro
        // body where `bar` is also a macro), the system needs to
        // detect this and trigger the macro path. Before this
        // fix, eval_data_as_code only checked env + primitives,
        // so `bar` (which lives in `macros_`, not env) was never
        // expanded — the cons chain produced the list `(bar <x>)`
        // but `bar` was just a symbol at re-eval time.
        if (macros_.count(fn_name)) {
            auto macro_it = macros_.find(fn_name);
            auto& md = macro_it->second;
            bool is_rest = md.dotted;
            // Rest params on legacy macros via eval_data_as_code
            // are not yet supported (same limitation as the main
            // eval_flat path). Fall through to a no-op return if
            // the macro has a dotted rest param.
            if (is_rest) {
                return make_void();
            }
            // Collect args (already data — no ast_to_data needed).
            std::vector<EvalValue> cargs;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                cargs.push_back(pairs_[arg_pair].car);
                current = pairs_[arg_pair].cdr;
            }
            // Build tail env with regular params bound.
            Env tail_env(&env);
            tail_env.set_primitives(&primitives_);

            if (md.pool)
                tail_env.set_pool(md.pool);
            for (std::size_t i = 0; i < md.params.size() && i < cargs.size(); ++i) {
                tail_env.bind(md.params[i], std::move(cargs[i]));
            }
            // Evaluate macro body (quasiquote-expanded template)
            // → produces data (a list).
            auto template_result =
                eval_flat(*md.flat, md.pool ? *md.pool : *current_pool_, md.body_id, tail_env);
            if (!template_result)
                return template_result;
            // Re-evaluate the data as code. The recursive call
            // here is what enables macro composition: the inner
            // macro's expansion is re-evaluated, which may itself
            // contain another macro call.
            return eval_data_as_code(*template_result, env, flat, pool);
        }

        // Look up in environment. Phase 2.5.0: route through
        // lookup_by_intern (SymId-first). canonical_pool() is
        // the long-lived workspace pool; env.pool_ is the
        // fallback for closures that captured a non-canonical
        // pool. Observable behavior matches env.lookup(name).
        auto env_val = env.lookup_by_intern(fn_name, canonical_pool());
        if (env_val) {
            auto fn_val = *env_val;
            // Dereference cells — needed when lookup returned cell sentinel (cells_ not set on env)
            if (types::is_cell(fn_val)) {
                auto ci = types::as_cell_id(fn_val);
                if (ci < cells_.size())
                    fn_val = cells_[ci];
            }
            if (types::is_closure(fn_val)) {
                auto cid = types::as_closure_id(fn_val);
                auto it = closures_.find(cid);
                if (it != closures_.end()) {
                    auto& cl = it->second;
                    // Evaluate args and apply
                    std::vector<EvalValue> cargs;
                    auto current = cdr_val;
                    while (types::is_pair(current)) {
                        auto arg_pair = types::as_pair_idx(current);
                        auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                        if (!arg_val)
                            return arg_val;
                        cargs.push_back(*arg_val);
                        current = pairs_[arg_pair].cdr;
                    }
                    // Create tail env and apply
                    Env tail_env = materialize_call_env(cl);
                    tail_env.set_primitives(&primitives_);

                    // Issue #145: set the pool so bind_symid can mirror
                    if (cl.pool)
                        tail_env.set_pool(cl.pool);
                    for (std::size_t i = 0; i < cargs.size() && i < cl.params.size(); ++i)
                        tail_env.bind_symid(cl.params[i], std::move(cargs[i]));
                    if (cl.body_id != aura::ast::NULL_NODE && cl.flat)
                        return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id,
                                         tail_env);
                    return make_void();
                }
            }
        }
    }

    // Not a string function name — evaluate car and cdr, apply
    auto fn = eval_data_as_code(car_val, env, flat, pool);
    if (!fn)
        return fn;
    if (types::is_closure(*fn)) {
        auto cid = types::as_closure_id(*fn);
        // Try tree-walker closure first, then IR bridge
        auto result = apply_closure(cid, {});
        if (result)
            return *result;

        // Fallback: manual closure apply via eval_flat
        auto it = closures_.find(cid);
        if (it != closures_.end()) {
            auto& cl = it->second;
            std::vector<EvalValue> cargs;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                if (!arg_val)
                    return arg_val;
                cargs.push_back(*arg_val);
                current = pairs_[arg_pair].cdr;
            }
            Env tail_env = materialize_call_env(cl);
            tail_env.set_primitives(&primitives_);

            // Issue #145: set the pool so bind_symid can mirror
            if (cl.pool)
                tail_env.set_pool(cl.pool);
            for (std::size_t i = 0; i < cargs.size() && i < cl.params.size(); ++i)
                tail_env.bind_symid(cl.params[i], std::move(cargs[i]));
            if (cl.body_id != aura::ast::NULL_NODE && cl.flat)
                return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id,
                                 tail_env);
        }
    }

    return make_void();
}
// ── Runtime type helpers for type annotation checking ────────
static aura::core::TypeTag runtime_type_tag(const types::EvalValue& v) {
    if (types::is_int(v))
        return aura::core::TypeTag::INT;
    if (types::is_float(v))
        return aura::core::TypeTag::FLOAT;
    if (types::is_bool(v))
        return aura::core::TypeTag::BOOL;
    if (types::is_string(v))
        return aura::core::TypeTag::STRING;
    if (types::is_pair(v))
        return aura::core::TypeTag::PAIR;
    if (types::is_closure(v))
        return aura::core::TypeTag::CLOSURE;
    if (types::is_vector(v))
        return aura::core::TypeTag::VECTOR;
    if (types::is_hash(v))
        return aura::core::TypeTag::HASH;
    return aura::core::TypeTag::DYNAMIC;
}

static std::string type_tag_name(aura::core::TypeTag tag) {
    switch (tag) {
        case aura::core::TypeTag::INT:
            return "Int";
        case aura::core::TypeTag::FLOAT:
            return "Float";
        case aura::core::TypeTag::BOOL:
            return "Bool";
        case aura::core::TypeTag::STRING:
            return "String";
        case aura::core::TypeTag::PAIR:
            return "Pair";
        case aura::core::TypeTag::CLOSURE:
            return "Closure";
        case aura::core::TypeTag::VECTOR:
            return "Vector";
        case aura::core::TypeTag::HASH:
            return "Hash";
        default:
            return "Dynamic";
    }
}

// Issue #146 Phase 2: thin legacy wrapper around the pure
// `coerce_value_pure` in `aura::compiler::pure`. Mirrors the
// legacy bool API; the underlying Result<void> is discarded.
// The 1 existing call site (L18281) keeps using the bool
// interface; new code can use `coerce_value_pure` directly
// for monadic composition.
static bool coerce_value(types::EvalValue& val, aura::core::TypeTag from, aura::core::TypeTag to,
                         std::pmr::vector<std::string>& heap) {
    return aura::compiler::pure::coerce_value_pure(val, from, to, heap).has_value();
}

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────

// Issue #236: helper implementations for mutate:atomic-batch.
// The existing atomic-batch (line ~9071) called the sub-primitive
// via primitives_.lookup which re-enters MutationBoundaryGuard
// in each sub-op, deadlocking on the non-recursive shared_mutex.
// These helpers do the same work WITHOUT the guard — the batch's
// outer guard already holds the lock for the entire batch body.
//
// MVP scope: :rebind works for the "old Define exists" path.
// :replace-value and :tweak-literal are stubs (error-out) so
// they fail fast rather than deadlocking the batch. Follow-up
// to extract those internals.
EvalResult Evaluator::eval_flat_apply_mutate_rebind(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch,
                                   "batch :rebind requires name and code (string args)"});
    if (!workspace_flat_ || !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :rebind: no workspace loaded"});
    auto name_idx = as_string_idx(a[0]);
    auto code_idx = as_string_idx(a[1]);
    if (name_idx >= string_heap_.size() || code_idx >= string_heap_.size())
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :rebind: string index out of range"});
    auto& flat = *workspace_flat_;
    auto name = string_heap_[name_idx];
    auto sym = canonical_pool()->intern(name);
    aura::ast::NodeId old_define = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
            old_define = id;
            break;
        }
    }
    if (old_define == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :rebind: no existing Define for '" + name +
                "' (new-binding path not yet supported; use standalone mutate:rebind)"});
    // Issue #1685 / #1687: snapshot + re-resolve after parse_to_flat.
    const auto size_before_parse = static_cast<std::size_t>(flat.size());
    auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
    if (!pr.success || pr.root == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::ParseError,
                                                      "batch :rebind: parse failed for new code"});
    old_define = flat.resolve_define_after_parse(sym, old_define, size_before_parse);
    if (old_define == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError,
            "batch :rebind: define not found after parse for '" + name + "'"});
    aura::ast::NodeId new_value = pr.root;
    auto root_v = flat.get(pr.root);
    if (root_v.tag == aura::ast::NodeTag::Define) {
        if (root_v.children.empty())
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, "batch :rebind: define form has no body"});
        new_value = root_v.child(0);
    }
    std::string summary = (a.size() > 2 && is_string(a[2])) ? string_heap_[as_string_idx(a[2])]
                                                            : "batch rebind " + name;
    auto old_v = flat.get(old_define);
    auto old_value_node = old_v.children.empty() ? aura::ast::NULL_NODE : old_v.child(0);
    auto mid = flat.add_mutation_with_rollback(
        old_define, "batch-rebind", std::string("Define:") + name, std::string("Define:") + name,
        summary, aura::ast::MutationStatus::Committed, 0,
        static_cast<std::uint64_t>(old_value_node), static_cast<std::uint64_t>(new_value), true);
    flat.set_child(old_define, 0, new_value);
    // Issue #493: fast dirty path (early-exit fixed point, #471).
    flat.mark_dirty_upward_fast(old_define);
    return make_int(static_cast<std::int64_t>(mid));
}

EvalResult Evaluator::eval_flat_apply_mutate_replace_value(std::span<const types::EvalValue> a) {
    if (a.size() < 3 || !is_int(a[0]) || !is_string(a[2]))
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :replace-value requires node-id (int), new-value, summary (string)"});
    if (!workspace_flat_)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :replace-value: no workspace loaded"});
    auto sum_idx = as_string_idx(a[2]);
    if (sum_idx >= string_heap_.size())
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :replace-value: summary index out of range"});
    auto& flat = *workspace_flat_;
    auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
    if (node >= flat.size())
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :replace-value: node out of range"});
    auto nv = flat.get(node);
    switch (nv.tag) {
        case aura::ast::NodeTag::LiteralInt: {
            if (!is_int(a[1]))
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::TypeError,
                    "batch :replace-value: LiteralInt node requires an integer value"});
            auto new_val = static_cast<std::int64_t>(as_int(a[1]));
            std::uint64_t old_val = static_cast<std::uint64_t>(nv.int_value);
            auto mid = flat.add_mutation_with_rollback(
                node, "replace-value", "Int", "Int", string_heap_[sum_idx],
                aura::ast::MutationStatus::Committed, 0, old_val,
                static_cast<std::uint64_t>(new_val), true);
            flat.set_int(node, new_val);
            flat.mark_dirty_upward_fast(node, aura::ast::FlatAST::kGeneralDirty);
            return make_int(static_cast<std::int64_t>(mid));
        }
        case aura::ast::NodeTag::LiteralFloat: {
            if (!is_float(a[1]))
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::TypeError,
                    "batch :replace-value: LiteralFloat node requires a float value"});
            double new_val = as_float(a[1]);
            std::uint64_t new_bits = std::bit_cast<std::uint64_t>(new_val);
            std::uint64_t old_bits = std::bit_cast<std::uint64_t>(nv.float_value);
            auto mid = flat.add_mutation_with_rollback(
                node, "replace-value", "Float", "Float", string_heap_[sum_idx],
                aura::ast::MutationStatus::Committed,
                static_cast<std::uint32_t>(aura::ast::MutationSoAField::FloatVal), old_bits,
                new_bits, true);
            flat.set_float(node, new_val);
            flat.mark_dirty_upward_fast(node, aura::ast::FlatAST::kGeneralDirty);
            return make_int(static_cast<std::int64_t>(mid));
        }
        case aura::ast::NodeTag::Variable:
        case aura::ast::NodeTag::LiteralString: {
            if (!is_string(a[1]))
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::TypeError,
                    "batch :replace-value: Variable/LiteralString node requires a string value"});
            auto new_sym_idx = as_string_idx(a[1]);
            if (new_sym_idx >= string_heap_.size())
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::InternalError,
                    "batch :replace-value: new value string index out of range"});
            auto new_name = string_heap_[new_sym_idx];
            auto new_sym = workspace_pool_->intern(new_name);
            std::uint64_t old_val = nv.sym_id;
            auto mid = flat.add_mutation_with_rollback(
                node, "replace-value", "Sym", "Sym", string_heap_[sum_idx],
                aura::ast::MutationStatus::Committed,
                static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId), old_val, new_sym,
                true);
            flat.set_sym(node, new_sym);
            flat.mark_dirty_upward_fast(node, aura::ast::FlatAST::kGeneralDirty);
            return make_int(static_cast<std::int64_t>(mid));
        }
        default:
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::TypeError,
                "batch :replace-value: node tag does not support value replacement (" +
                    std::to_string(static_cast<int>(nv.tag)) + ")"});
    }
}

EvalResult Evaluator::eval_flat_apply_mutate_tweak_literal(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]) || !workspace_flat_)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch,
                                   "batch :tweak-literal requires node-id (int) and delta (int)"});
    auto& flat = *workspace_flat_;
    auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
    if (node >= flat.size())
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :tweak-literal: node out of range"});
    auto delta = as_int(a[1]);
    auto v = flat.get(node);
    if (v.tag != aura::ast::NodeTag::LiteralInt)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::TypeError,
            "batch :tweak-literal: node " + std::to_string(node) + " is not a LiteralInt"});
    auto new_val = std::max<std::int64_t>(0, static_cast<std::int64_t>(v.int_value) + delta);
    auto old_val = v.int_value;
    std::string summary =
        (a.size() > 2 && is_string(a[2]))
            ? string_heap_[as_string_idx(a[2])]
            : "tweak-literal " + std::to_string(old_val) + "->" + std::to_string(new_val);
    flat.add_mutation_with_rollback(
        node, "tweak-literal", "Int", "Int", summary, aura::ast::MutationStatus::Committed, 0,
        static_cast<std::uint64_t>(old_val), static_cast<std::uint64_t>(new_val), true);
    flat.set_int(node, new_val);
    flat.mark_dirty_upward_fast(node);
    return make_int(static_cast<std::int64_t>(new_val));
}

// Issue #396 Phase 2: lockless variant of (mutate:remove-node).
// Replicates the inner logic of the wrapper primitive but skips
// the MutationBoundaryGuard + fiber-yield + read-only check (the
// outer atomic-batch guard already owns these). Called from
// (mutate:atomic-batch) for the "mutate:remove-node" sub-op.
// Issue #1688: removes target from ALL parents (DAG), same as public.
EvalResult Evaluator::eval_flat_apply_mutate_remove_node(std::span<const types::EvalValue> a) {
    if (a.empty() || !is_int(a[0]))
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch, "batch :remove-node requires a node-id (int)"});
    if (!workspace_flat_)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :remove-node: no workspace loaded"});
    auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
    auto& flat = *workspace_flat_;
    if (target >= flat.size())
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :remove-node: node ID " + std::to_string(target) +
                                       " >= flat size " + std::to_string(flat.size())});
    auto result = aura::ast::mutators::remove_node_from_all_parents(
        flat, target, [&](aura::ast::NodeId parent, std::uint32_t ci) {
            flat.add_structural_mutation_log_entry(parent, ci, target, aura::ast::NULL_NODE,
                                                   "remove-node");
        });
    if (!result)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      std::string("batch :remove-node: ") +
                                                          std::string(result.error().message)});
    return make_bool(true);
}

// Issue #396 Phase 2: lockless variant of (mutate:insert-child).
// Replicates the inner logic but skips the guard + fiber-yield
// + read-only check. Parses the code-string into the workspace
// (so all existing IDs stay valid) then routes through
// InsertChildMutator. Returns the parsed new-child NodeId.
EvalResult Evaluator::eval_flat_apply_mutate_insert_child(std::span<const types::EvalValue> a) {
    if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_string(a[2]))
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :insert-child requires parent-id, position, code-string"});
    if (!workspace_flat_ || !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :insert-child: no workspace loaded"});
    auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
    auto pos = static_cast<std::uint32_t>(as_int(a[1]));
    auto code_idx = as_string_idx(a[2]);
    if (code_idx >= string_heap_.size())
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :insert-child: string index out of range"});
    auto& flat = *workspace_flat_;
    // Issue #1690 / #1685: re-validate parent after parse append (live pre-parse).
    const auto size_before_parse = static_cast<std::size_t>(flat.size());
    if (parent == aura::ast::NULL_NODE || static_cast<std::size_t>(parent) >= size_before_parse ||
        !flat.is_live_node(parent))
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :insert-child: parent out of range"});
    auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        std::string parse_err = "batch :insert-child: parse failed";
        if (!pr.errors.empty()) {
            for (auto& e : pr.errors) {
                if (!parse_err.empty() && parse_err.back() != ':')
                    parse_err += "; ";
                parse_err += e.format();
            }
        } else if (!pr.error.empty()) {
            parse_err += ": " + pr.error;
        }
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::ParseError, parse_err});
    }
    if (static_cast<std::size_t>(parent) >= size_before_parse || !flat.is_live_node(parent))
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :insert-child: parent invalid after parse"});
    auto result = aura::ast::mutators::apply_mutation(
        flat, parent, aura::ast::mutators::InsertChildMutator{pos, pr.root});
    if (!result)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      std::string("batch :insert-child: ") +
                                                          std::string(result.error().message)});
    std::string summary = (a.size() > 3 && is_string(a[3]))
                              ? string_heap_[as_string_idx(a[3])]
                              : "insert child at " + std::to_string(pos);
    flat.add_structural_mutation_log_entry(parent, pos, aura::ast::NULL_NODE, pr.root,
                                           "insert-child");
    return make_int(static_cast<std::int64_t>(pr.root));
}

// Issue #1900: lockless variant of (mutate:set-body). Inner logic
// from the wrapper primitive stripped of Guard + fiber-yield +
// read-only + hygiene + lazy COW + dep-graph query + typecheck +
// ownership validation + mark_define_dirty_fn + repopulate_dep_graph.
EvalResult Evaluator::eval_flat_apply_mutate_set_body(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !workspace_flat_ ||
        !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :set-body requires name (string) and new-body-code (string)"});
    auto name_idx = as_string_idx(a[0]);
    auto code_idx = as_string_idx(a[1]);
    if (name_idx >= string_heap_.size() || code_idx >= string_heap_.size())
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError, "batch :set-body: string index out of range"});
    auto& flat = *workspace_flat_;
    auto name = string_heap_[name_idx];
    auto sym = canonical_pool()->intern(name);
    aura::ast::NodeId target = aura::ast::NULL_NODE;
    aura::ast::NodeId lambda_id = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
            if (v.children.size() != 1)
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::ArityMismatch, "batch :set-body: define has " +
                                                              std::to_string(v.children.size()) +
                                                              " children, expected 1"});
            lambda_id = v.child(0);
            auto lv = flat.get(lambda_id);
            if (lv.tag != aura::ast::NodeTag::Lambda)
                return std::unexpected(
                    aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                           "batch :set-body: define body is not a Lambda"});
            target = id;
            break;
        }
    }
    if (target == aura::ast::NULL_NODE)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :set-body: function \"" + name + "\" not found"});
    // Issue #1687: capture size before parse; re-resolve BOTH Define and
    // Lambda (double-stale NodeId risk, sibling of #1685 rebind path).
    const auto size_before_parse = static_cast<std::size_t>(flat.size());
    auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
    if (!pr.success || pr.root == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ParseError, "batch :set-body: parse failed for new body"});
    target = flat.resolve_define_after_parse(sym, target, size_before_parse);
    if (target == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError,
            "batch :set-body: define not found after parse for \"" + name + "\""});
    lambda_id = flat.resolve_lambda_child_of_define(target);
    if (lambda_id == aura::ast::NULL_NODE)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                   "batch :set-body: define body is not a Lambda after parse"});
    auto root_v = flat.get(pr.root);
    aura::ast::NodeId body_to_set = pr.root;
    if (root_v.tag == aura::ast::NodeTag::Define) {
        if (root_v.children.empty())
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, "batch :set-body: define form has no body"});
        body_to_set = root_v.child(0);
    }
    auto new_root_v = flat.get(body_to_set);
    if (new_root_v.tag == aura::ast::NodeTag::Lambda) {
        flat.set_child(target, 0, body_to_set);
    } else {
        flat.set_child(lambda_id, 0, body_to_set);
    }
    aura::ast::NodeId old_body =
        (flat.children(target).size() > 1) ? flat.children(target)[1] : aura::ast::NULL_NODE;
    flat.add_structural_mutation_log_entry(target, 1, old_body, body_to_set, "set-body");
    flat.mark_dirty_upward_fast(target, aura::ast::FlatAST::kGeneralDirty |
                                            aura::ast::FlatAST::kConstraintDirty);
    return make_bool(true);
}

// Issue #1900: lockless variant of (mutate:replace-pattern). Inner
// logic stripped of Guard + fiber-yield + read-only + hygiene.
// Single-subtree wildcard match (no Kleene capture substitution
// in batch context — outer batch can be re-issued with strict-arity
// flag if needed).
EvalResult Evaluator::eval_flat_apply_mutate_replace_pattern(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !workspace_flat_ ||
        !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :replace-pattern requires pattern and replacement strings"});
    auto pattern_idx = as_string_idx(a[0]);
    auto repl_idx = as_string_idx(a[1]);
    if (pattern_idx >= string_heap_.size() || repl_idx >= string_heap_.size())
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :replace-pattern: string index out of range"});
    auto& flat = *workspace_flat_;
    auto pattern_str = string_heap_[pattern_idx];
    std::string repl_template = string_heap_[repl_idx];
    std::string summary = "batch replace-pattern";
    for (std::size_t ai = 2; ai < a.size(); ++ai) {
        if (is_string(a[ai]))
            summary = string_heap_[as_string_idx(a[ai])];
    }
    auto alloc = temp_arena_->allocator();
    auto* pat_pool = temp_arena_->create<aura::ast::StringPool>(alloc);
    auto* pat_flat = temp_arena_->create<aura::ast::FlatAST>(alloc);
    auto pat_pr = aura::parser::parse_to_flat(pattern_str, *pat_flat, *pat_pool);
    if (!pat_pr.success || pat_pr.root == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ParseError, "batch :replace-pattern: pattern parse failed"});
    auto wildcard_sym = pat_pool->intern("...");
    const auto end_id = flat.size();
    int replaced_count = 0;
    flat.begin_atomic_batch();
    for (aura::ast::NodeId id = 0; id < end_id; ++id) {
        if (flat.root != aura::ast::NULL_NODE && id != flat.root &&
            flat.parent_of(id) == aura::ast::NULL_NODE && !flat.is_macro_introduced(id))
            continue;
        std::function<bool(aura::ast::NodeId, aura::ast::NodeId)> match_sub;
        match_sub = [&](aura::ast::NodeId wid, aura::ast::NodeId pid) -> bool {
            if (pid >= pat_flat->size() || wid >= flat.size())
                return false;
            auto wv = flat.get(wid);
            auto pv = pat_flat->get(pid);
            if (pv.tag == aura::ast::NodeTag::Variable && pv.sym_id == wildcard_sym)
                return true;
            if (wv.tag != pv.tag)
                return false;
            if (wv.tag == aura::ast::NodeTag::Variable ||
                wv.tag == aura::ast::NodeTag::LiteralString ||
                wv.tag == aura::ast::NodeTag::Define || wv.tag == aura::ast::NodeTag::DefineType ||
                wv.tag == aura::ast::NodeTag::DefineModule)
                if (wv.sym_id != pv.sym_id)
                    return false;
            if (wv.children.size() != pv.children.size())
                return false;
            for (std::size_t ci = 0; ci < wv.children.size(); ++ci)
                if (!match_sub(wv.child(ci), pv.child(ci)))
                    return false;
            return true;
        };
        if (!match_sub(id, pat_pr.root))
            continue;
        std::string filled_repl = repl_template;
        // Issue #1694: snapshot pre-parse size; resolve parent only in that
        // range after parse (do not treat parse-appended nodes as parents).
        const auto size_before_parse = static_cast<std::size_t>(flat.size());
        auto repl_pr = aura::parser::parse_to_flat(filled_repl, flat, *workspace_pool_);
        if (!repl_pr.success || repl_pr.root == aura::ast::NULL_NODE)
            continue;
        if (static_cast<std::size_t>(id) >= size_before_parse || !flat.is_live_node(id))
            continue;
        aura::ast::NodeId parent_id = flat.parent_of(id);
        std::uint32_t child_idx = 0;
        bool found = false;
        if (parent_id != aura::ast::NULL_NODE &&
            static_cast<std::size_t>(parent_id) < size_before_parse &&
            flat.is_live_node(parent_id)) {
            auto pv = flat.get(parent_id);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == id) {
                    child_idx = static_cast<std::uint32_t>(ci);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            // Fallback scan limited to pre-parse nodes only.
            for (aura::ast::NodeId pid = 0; static_cast<std::size_t>(pid) < size_before_parse;
                 ++pid) {
                if (!flat.is_live_node(pid))
                    continue;
                auto pv = flat.get(pid);
                for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                    if (pv.child(ci) == id) {
                        parent_id = pid;
                        child_idx = static_cast<std::uint32_t>(ci);
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        }
        if (!found)
            continue;
        flat.set_child(parent_id, child_idx, repl_pr.root);
        ++replaced_count;
    }
    if (replaced_count == 0) {
        flat.rollback_atomic_batch();
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError,
            "batch :replace-pattern: no matches found (or all parse-failed)"});
    }
    flat.commit_atomic_batch();
    invalidate_tag_arity_index();
    // Issue #1696: multi-node op — NULL_NODE sentinel, not NodeId 0.
    flat.add_mutation(aura::ast::NULL_NODE, "replace-pattern", pattern_str, repl_template, summary);
    return make_bool(true);
}

// Issue #1900: lockless variant of (mutate:replace-subtree).
// Hygiene gate kept inline (per-#142 contract).
EvalResult Evaluator::eval_flat_apply_mutate_replace_subtree(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !workspace_flat_ || !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :replace-subtree requires node-id (int) and new-code (string)"});
    auto target = static_cast<aura::ast::NodeId>(as_int(a[0]));
    auto code_idx = as_string_idx(a[1]);
    if (code_idx >= string_heap_.size())
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :replace-subtree: code string index out of range"});
    auto& flat = *workspace_flat_;
    if (target == aura::ast::NULL_NODE || target >= flat.size())
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError, "batch :replace-subtree: node-id out of range"});
    if (flat.is_macro_introduced(target))
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :replace-subtree: cannot mutate macro-introduced node"});
    auto new_code = string_heap_[code_idx];
    std::string summary =
        (a.size() > 2 && is_string(a[2])) ? string_heap_[as_string_idx(a[2])] : "replace-subtree";
    auto parent_id = flat.parent_of(target);
    std::uint32_t child_idx = 0;
    bool found_slot = false;
    if (parent_id != aura::ast::NULL_NODE && parent_id < flat.size()) {
        auto pv = flat.get(parent_id);
        for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
            if (pv.child(ci) == target) {
                child_idx = static_cast<std::uint32_t>(ci);
                found_slot = true;
                break;
            }
        }
    }
    if (!found_slot)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :replace-subtree: target has no parent slot to replace"});
    // Issue #1697 / #1685: re-validate parent/slot after parse; re-derive
    // from target if the pre-parse edge is no longer attached.
    // Use is_live_node not StableNodeRef::is_valid_in — parse_to_flat
    // restamps generations (#273 / #1699).
    const auto size_before_parse = static_cast<std::size_t>(flat.size());
    auto pr = aura::parser::parse_to_flat(new_code, flat, *workspace_pool_);
    if (!pr.success || pr.root == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::ParseError,
                                                      "batch :replace-subtree: parse failed"});
    auto parent_slot_ok = [&]() -> bool {
        if (parent_id == aura::ast::NULL_NODE ||
            static_cast<std::size_t>(parent_id) >= size_before_parse ||
            !flat.is_live_node(parent_id))
            return false;
        auto pv = flat.get(parent_id);
        return child_idx < pv.children.size() && pv.child(child_idx) == target;
    };
    if (!parent_slot_ok()) {
        if (static_cast<std::size_t>(target) >= size_before_parse || !flat.is_live_node(target))
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                       "batch :replace-subtree: target invalid after parse"});
        parent_id = flat.parent_of(target);
        if (parent_id == aura::ast::NULL_NODE ||
            static_cast<std::size_t>(parent_id) >= size_before_parse ||
            !flat.is_live_node(parent_id))
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                       "batch :replace-subtree: parent edge lost after parse"});
        // Re-scan child index among pre-parse parent children.
        child_idx = 0;
        found_slot = false;
        {
            auto pv = flat.get(parent_id);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == target) {
                    child_idx = static_cast<std::uint32_t>(ci);
                    found_slot = true;
                    break;
                }
            }
        }
        if (!found_slot || !parent_slot_ok())
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                       "batch :replace-subtree: parent invalid after parse"});
    }
    flat.set_child(parent_id, child_idx, pr.root);
    flat.mark_dirty_upward_fast(parent_id, aura::ast::FlatAST::kGeneralDirty);
    flat.add_mutation_subtree(pr.root, parent_id, child_idx, "<batch-captured>", "replace-subtree",
                              summary);
    return make_bool(true);
}

// Issue #1900: lockless variant of (mutate:splice).
EvalResult Evaluator::eval_flat_apply_mutate_splice(std::span<const types::EvalValue> a) {
    if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !workspace_flat_ || !workspace_pool_)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch,
                                   "batch :splice requires parent-id, position, code-strings..."});
    auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
    auto pos = static_cast<std::uint32_t>(as_int(a[1]));
    auto& flat = *workspace_flat_;
    // Issue #1699 / #1690: parent must stay a live pre-parse node across
    // each append in the multi-arg loop (6th capture-before-parse site).
    // Do not use StableNodeRef::is_valid_in post-parse: parse_to_flat
    // restamps all node generations (#273), which would false-fail.
    if (parent == aura::ast::NULL_NODE || parent >= flat.size() || !flat.is_live_node(parent))
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :splice: parent out of range"});
    std::vector<types::EvalValue> code_args;
    for (std::size_t i = 2; i < a.size(); ++i) {
        if (i == a.size() - 1 && i >= 3 && is_string(a[i]))
            continue;
        if (is_string(a[i]))
            code_args.push_back(a[i]);
    }
    if (code_args.empty())
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :splice: no code strings provided"});
    std::string summary = "batch splice";
    if (a.size() >= 4 && is_string(a[a.size() - 1])) {
        auto sidx = as_string_idx(a[a.size() - 1]);
        if (sidx < string_heap_.size())
            summary = string_heap_[sidx];
    }
    EvalValue result_list = make_void();
    std::uint32_t insert_pos = pos;
    for (auto& code_val : code_args) {
        auto cidx = as_string_idx(code_val);
        if (cidx >= string_heap_.size())
            continue;
        const auto size_before_parse = static_cast<std::size_t>(flat.size());
        auto pr = aura::parser::parse_to_flat(string_heap_[cidx], flat, *workspace_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            continue;
        // Parent is user-supplied insert target — no child edge to re-derive.
        if (static_cast<std::size_t>(parent) >= size_before_parse || !flat.is_live_node(parent))
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "batch :splice: parent invalid after parse"});
        flat.insert_child(parent, insert_pos, pr.root);
        flat.add_mutation(parent, "splice", std::to_string(insert_pos), string_heap_[cidx],
                          summary);
        flat.mark_dirty_upward_fast(parent);
        auto pid = pairs_.size();
        pairs_.push_back({make_int(static_cast<std::int64_t>(pr.root)), result_list});
        result_list = make_pair(pid);
        ++insert_pos;
    }
    EvalValue reversed = make_void();
    auto cur = result_list;
    while (is_pair(cur)) {
        auto idx = as_pair_idx(cur);
        if (idx >= pairs_.size())
            break;
        auto ridx = pairs_.size();
        pairs_.push_back({pairs_[idx].car, reversed});
        reversed = make_pair(ridx);
        cur = pairs_[idx].cdr;
    }
    return reversed;
}

// Issue #1900: lockless variant of (mutate:wrap).
// Issue #1700: re-validate parent_of_target after parse; resolve parents
// via parent_of (not O(N×C)); is_live_node not is_valid_in post-restamp.
EvalResult Evaluator::eval_flat_apply_mutate_wrap(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]) || !workspace_flat_ || !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :wrap requires node-id and wrapper-template (with '_' placeholder)"});
    auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
    auto tmpl_idx = as_string_idx(a[1]);
    if (tmpl_idx >= string_heap_.size())
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :wrap: template string index out of range"});
    auto& flat = *workspace_flat_;
    if (node == aura::ast::NULL_NODE || node >= flat.size() || !flat.is_live_node(node))
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :wrap: node out of range"});
    std::string summary = (a.size() > 2 && is_string(a[2])) ? string_heap_[as_string_idx(a[2])]
                                                            : "wrap node " + std::to_string(node);
    auto parent_of_target = flat.parent_of(node);
    int child_idx_in_parent = -1;
    if (parent_of_target != aura::ast::NULL_NODE && flat.is_live_node(parent_of_target)) {
        auto pv = flat.get(parent_of_target);
        for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
            if (pv.child(ci) == node) {
                child_idx_in_parent = static_cast<int>(ci);
                break;
            }
        }
    }
    if (parent_of_target == aura::ast::NULL_NODE || child_idx_in_parent < 0)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :wrap: node " + std::to_string(node) + " has no parent"});
    auto tmpl = string_heap_[tmpl_idx];
    auto sentinel_pos = tmpl.find('_');
    if (sentinel_pos == std::string::npos)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :wrap: wrapper-template must contain a '_' placeholder"});
    std::string sentinel = "__WRAP_TARGET_" + std::to_string(node) + "__";
    auto parsed_tmpl = tmpl.substr(0, sentinel_pos) + sentinel + tmpl.substr(sentinel_pos + 1);
    const auto size_before_parse = static_cast<std::size_t>(flat.size());
    auto pr = aura::parser::parse_to_flat(parsed_tmpl, flat, *workspace_pool_);
    if (!pr.success || pr.root == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ParseError, "batch :wrap: wrapper template parse failed"});
    // Re-validate target + re-derive parent edge after parse (#1700).
    if (static_cast<std::size_t>(node) >= size_before_parse || !flat.is_live_node(node))
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :wrap: target invalid after parse"});
    auto parent_slot_ok = [&]() -> bool {
        if (parent_of_target == aura::ast::NULL_NODE ||
            static_cast<std::size_t>(parent_of_target) >= size_before_parse ||
            !flat.is_live_node(parent_of_target) || child_idx_in_parent < 0)
            return false;
        auto pv = flat.get(parent_of_target);
        return static_cast<std::size_t>(child_idx_in_parent) < pv.children.size() &&
               pv.child(static_cast<std::uint32_t>(child_idx_in_parent)) == node;
    };
    if (!parent_slot_ok()) {
        parent_of_target = flat.parent_of(node);
        child_idx_in_parent = -1;
        if (parent_of_target != aura::ast::NULL_NODE &&
            static_cast<std::size_t>(parent_of_target) < size_before_parse &&
            flat.is_live_node(parent_of_target)) {
            auto pv = flat.get(parent_of_target);
            for (std::size_t ci = 0; ci < pv.children.size(); ++ci) {
                if (pv.child(ci) == node) {
                    child_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
        }
        if (!parent_slot_ok())
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "batch :wrap: parent invalid after parse"});
    }
    auto sentinel_sym = workspace_pool_->intern(sentinel);
    aura::ast::NodeId sentinel_id = aura::ast::NULL_NODE;
    aura::ast::NodeId sentinel_parent = aura::ast::NULL_NODE;
    int sentinel_child_idx = -1;
    for (aura::ast::NodeId sid = static_cast<aura::ast::NodeId>(size_before_parse);
         sid < flat.size(); ++sid) {
        if (!flat.is_live_node(sid))
            continue;
        auto sv = flat.get(sid);
        if (sv.tag == aura::ast::NodeTag::Variable && sv.sym_id == sentinel_sym) {
            sentinel_id = sid;
            sentinel_parent = flat.parent_of(sid);
            if (sentinel_parent != aura::ast::NULL_NODE && flat.is_live_node(sentinel_parent)) {
                auto p2v = flat.get(sentinel_parent);
                for (std::size_t ci = 0; ci < p2v.children.size(); ++ci) {
                    if (p2v.child(ci) == sid) {
                        sentinel_child_idx = static_cast<int>(ci);
                        break;
                    }
                }
            }
            break;
        }
    }
    if (sentinel_id == aura::ast::NULL_NODE || sentinel_parent == aura::ast::NULL_NODE ||
        sentinel_child_idx < 0 || !flat.is_live_node(sentinel_parent))
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError,
            "batch :wrap: sentinel placeholder not found in parsed wrapper"});
    flat.set_child(sentinel_parent, static_cast<std::uint32_t>(sentinel_child_idx), node);
    flat.set_child(parent_of_target, static_cast<std::uint32_t>(child_idx_in_parent), pr.root);
    flat.add_mutation(node, "wrap", parsed_tmpl, summary, summary);
    flat.mark_dirty_upward_fast(parent_of_target);
    return make_int(static_cast<std::int64_t>(pr.root));
}

// Issue #1900: lockless variant of (mutate:rename-symbol).
EvalResult Evaluator::eval_flat_apply_mutate_rename_symbol(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]) || !workspace_flat_ ||
        !workspace_pool_)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch,
                                   "batch :rename-symbol requires old-name and new-name strings"});
    auto old_name_idx = as_string_idx(a[0]);
    auto new_name_idx = as_string_idx(a[1]);
    if (old_name_idx >= string_heap_.size() || new_name_idx >= string_heap_.size())
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :rename-symbol: string index out of range"});
    auto& flat = *workspace_flat_;
    auto old_name = string_heap_[old_name_idx];
    auto new_name = string_heap_[new_name_idx];
    auto old_sym = workspace_pool_->intern(old_name);
    auto new_sym = workspace_pool_->intern(new_name);
    std::string summary = (a.size() > 2 && is_string(a[2]))
                              ? string_heap_[as_string_idx(a[2])]
                              : "rename " + old_name + " -> " + new_name;
    int count = 0;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.sym_id(id) == old_sym) {
            auto tag = flat.tag(id);
            if (tag == aura::ast::NodeTag::Variable || tag == aura::ast::NodeTag::Define ||
                tag == aura::ast::NodeTag::DefineType || tag == aura::ast::NodeTag::DefineModule ||
                tag == aura::ast::NodeTag::Let || tag == aura::ast::NodeTag::LetRec ||
                tag == aura::ast::NodeTag::Set || tag == aura::ast::NodeTag::MacroDef) {
                flat.sym_id(id) = new_sym;
                ++count;
            }
        }
    }
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        if (flat.tag(id) == aura::ast::NodeTag::Lambda) {
            count += flat.rename_param(id, old_sym, new_sym, nullptr);
        }
    }
    if (count == 0)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :rename-symbol: symbol \"" + old_name +
                                                          "\" not found in AST"});
    // Issue #1696: multi-node rename — NULL_NODE sentinel, not NodeId 0.
    flat.add_mutation(aura::ast::NULL_NODE, "rename-symbol", old_name, new_name, summary);
    return make_bool(true);
}

// Issue #1900: lockless variant of (mutate:move-node).
EvalResult Evaluator::eval_flat_apply_mutate_move_node(std::span<const types::EvalValue> a) {
    if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !workspace_flat_)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :move-node requires node-id, new-parent-id, new-position"});
    auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
    auto new_parent = static_cast<aura::ast::NodeId>(as_int(a[1]));
    auto new_pos = static_cast<std::uint32_t>(as_int(a[2]));
    auto& flat = *workspace_flat_;
    if (node >= flat.size() || new_parent >= flat.size() || node == aura::ast::NULL_NODE ||
        new_parent == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError, "batch :move-node: node or parent out of range"});
    if (node == new_parent)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError, "batch :move-node: cannot move node to itself"});
    {
        auto p = flat.parent_of(new_parent);
        while (p != aura::ast::NULL_NODE) {
            if (p == node)
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::InternalError,
                    "batch :move-node: new parent is a descendant of moved node"});
            auto next = flat.parent_of(p);
            if (next == p)
                break;
            p = next;
        }
    }
    auto cur_parent = flat.parent_of(node);
    if (cur_parent == aura::ast::NULL_NODE)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :move-node: node has no parent (possibly the root)"});
    int cur_idx = -1;
    auto cpv = flat.get(cur_parent);
    for (std::size_t ci = 0; ci < cpv.children.size(); ++ci) {
        if (cpv.child(ci) == node) {
            cur_idx = static_cast<int>(ci);
            break;
        }
    }
    if (cur_idx < 0)
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :move-node: node not found in parent's children list"});
    std::string summary = (a.size() > 3 && is_string(a[3])) ? string_heap_[as_string_idx(a[3])]
                                                            : "move node " + std::to_string(node);
    flat.set_child(cur_parent, cur_idx, aura::ast::NULL_NODE);
    flat.insert_child(new_parent, new_pos, node);
    flat.add_mutation(node, "move-node", std::to_string(cur_parent), std::to_string(new_parent),
                      summary);
    flat.mark_dirty_upward_fast(new_parent);
    return make_bool(true);
}

// Issue #1900: lockless variant of (mutate:inline-call).
EvalResult Evaluator::eval_flat_apply_mutate_inline_call(std::span<const types::EvalValue> a) {
    if (a.empty() || !is_int(a[0]) || !workspace_flat_ || !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch,
                                                      "batch :inline-call requires call-node-id"});
    auto call_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
    auto& flat = *workspace_flat_;
    if (call_id >= flat.size())
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError, "batch :inline-call: call node out of range"});
    auto cv = flat.get(call_id);
    if (cv.tag != aura::ast::NodeTag::Call || cv.children.empty())
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::TypeError,
            "batch :inline-call: node " + std::to_string(call_id) + " is not a call"});
    std::string summary = (a.size() > 1 && is_string(a[1]))
                              ? string_heap_[as_string_idx(a[1])]
                              : "inline call " + std::to_string(call_id);
    auto func_node = cv.child(0);
    auto fv = flat.get(func_node);
    aura::ast::NodeId func_body_node = aura::ast::NULL_NODE;
    std::vector<aura::ast::SymId> formal_params;
    if (fv.tag == aura::ast::NodeTag::Variable) {
        auto sym = fv.sym_id;
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym && !v.children.empty()) {
                func_body_node = v.child(0);
                break;
            }
        }
        if (func_body_node == aura::ast::NULL_NODE)
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                       "batch :inline-call: function definition not found"});
        auto bn = flat.get(func_body_node);
        if (bn.tag != aura::ast::NodeTag::Lambda)
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                       "batch :inline-call: target is not a lambda"});
        formal_params.assign(bn.params.begin(), bn.params.end());
        if (bn.children.empty())
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "batch :inline-call: lambda body is empty"});
        func_body_node = bn.child(0);
    } else if (fv.tag == aura::ast::NodeTag::Lambda) {
        formal_params.assign(fv.params.begin(), fv.params.end());
        if (fv.children.empty())
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "batch :inline-call: lambda body is empty"});
        func_body_node = fv.child(0);
    } else {
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :inline-call: target function form not supported"});
    }
    std::vector<aura::ast::NodeId> actual_args;
    for (std::size_t i = 1; i < cv.children.size(); ++i)
        actual_args.push_back(cv.child(i));
    if (formal_params.size() != actual_args.size())
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError, "batch :inline-call: parameter count mismatch"});
    // Issue #1702: capture parent edge before DFS clone; re-validate after.
    if (call_id == aura::ast::NULL_NODE || !flat.is_live_node(call_id))
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :inline-call: call node is not live"});
    auto call_parent = flat.parent_of(call_id);
    int call_idx_in_parent = -1;
    if (call_parent != aura::ast::NULL_NODE && flat.is_live_node(call_parent)) {
        auto cpv = flat.get(call_parent);
        for (std::size_t ci = 0; ci < cpv.children.size(); ++ci) {
            if (cpv.child(ci) == call_id) {
                call_idx_in_parent = static_cast<int>(ci);
                break;
            }
        }
    }
    if (call_parent == aura::ast::NULL_NODE || call_idx_in_parent < 0)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :inline-call: call has no parent"});
    const auto size_before_clone = static_cast<std::size_t>(flat.size());
    std::vector<aura::ast::NodeId> old_to_new(flat.size(), aura::ast::NULL_NODE);
    std::vector<aura::ast::NodeId> dfs = {func_body_node};
    while (!dfs.empty()) {
        auto cur = dfs.back();
        dfs.pop_back();
        if (cur >= old_to_new.size())
            old_to_new.resize(cur + 1, aura::ast::NULL_NODE);
        if (old_to_new[cur] != aura::ast::NULL_NODE)
            continue;
        auto v = flat.get(cur);
        aura::ast::NodeId new_id = aura::ast::NULL_NODE;
        switch (v.tag) {
            case aura::ast::NodeTag::LiteralInt:
                new_id = flat.add_literal(v.int_value);
                break;
            case aura::ast::NodeTag::LiteralFloat:
                new_id = flat.add_literal_float(v.float_value);
                break;
            case aura::ast::NodeTag::LiteralString:
                new_id = flat.add_literalstring(v.sym_id);
                break;
            case aura::ast::NodeTag::Variable: {
                bool is_param = false;
                for (std::size_t pi = 0; pi < formal_params.size(); ++pi) {
                    if (formal_params[pi] == v.sym_id) {
                        new_id = actual_args[pi];
                        is_param = true;
                        break;
                    }
                }
                if (!is_param)
                    new_id = flat.add_variable(v.sym_id);
                break;
            }
            default:
                new_id = flat.add_raw_node(v.tag);
                break;
        }
        if (new_id != aura::ast::NULL_NODE) {
            old_to_new[cur] = new_id;
            for (auto c : v.children) {
                if (c != aura::ast::NULL_NODE)
                    dfs.push_back(c);
            }
        }
    }
    for (std::size_t old_nid = 0; old_nid < old_to_new.size(); ++old_nid) {
        auto new_id = old_to_new[old_nid];
        if (new_id == aura::ast::NULL_NODE)
            continue;
        bool is_reused_arg = false;
        for (auto arg : actual_args) {
            if (arg == new_id) {
                is_reused_arg = true;
                break;
            }
        }
        if (is_reused_arg)
            continue;
        auto old_v = flat.get(static_cast<aura::ast::NodeId>(old_nid));
        if (old_v.tag == aura::ast::NodeTag::Lambda && !old_v.params.empty())
            flat.set_lambda_params(new_id, old_v.params, old_v.param_annotations);
        for (std::size_t ci = 0; ci < old_v.children.size(); ++ci) {
            auto old_child = old_v.child(ci);
            if (old_child == aura::ast::NULL_NODE)
                continue;
            if (old_child < old_to_new.size() && old_to_new[old_child] != aura::ast::NULL_NODE) {
                flat.set_child(new_id, static_cast<std::uint32_t>(ci), old_to_new[old_child]);
            } else {
                auto old_cv = flat.get(old_child);
                if (old_cv.tag == aura::ast::NodeTag::Variable) {
                    for (std::size_t pi = 0; pi < formal_params.size(); ++pi) {
                        if (formal_params[pi] == old_cv.sym_id) {
                            flat.set_child(new_id, static_cast<std::uint32_t>(ci), actual_args[pi]);
                            break;
                        }
                    }
                }
            }
        }
    }
    auto cloned_body = old_to_new[func_body_node];
    if (cloned_body == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::InternalError,
            "batch :inline-call: function definition not found for inlining"});
    // Issue #1702: re-validate call parent after multi-add DFS clone.
    if (static_cast<std::size_t>(call_id) >= size_before_clone || !flat.is_live_node(call_id))
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                   "batch :inline-call: call node invalid after DFS clone"});
    auto parent_slot_ok = [&]() -> bool {
        if (call_parent == aura::ast::NULL_NODE ||
            static_cast<std::size_t>(call_parent) >= size_before_clone ||
            !flat.is_live_node(call_parent) || call_idx_in_parent < 0)
            return false;
        auto cpv = flat.get(call_parent);
        return static_cast<std::size_t>(call_idx_in_parent) < cpv.children.size() &&
               cpv.child(static_cast<std::uint32_t>(call_idx_in_parent)) == call_id;
    };
    if (!parent_slot_ok()) {
        call_parent = flat.parent_of(call_id);
        call_idx_in_parent = -1;
        if (call_parent != aura::ast::NULL_NODE &&
            static_cast<std::size_t>(call_parent) < size_before_clone &&
            flat.is_live_node(call_parent)) {
            auto cpv = flat.get(call_parent);
            for (std::size_t ci = 0; ci < cpv.children.size(); ++ci) {
                if (cpv.child(ci) == call_id) {
                    call_idx_in_parent = static_cast<int>(ci);
                    break;
                }
            }
        }
        if (!parent_slot_ok())
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                       "batch :inline-call: call_parent invalid after DFS clone"});
    }
    flat.set_child(call_parent, static_cast<std::uint32_t>(call_idx_in_parent), cloned_body);
    flat.mark_dirty_upward_fast(call_parent);
    flat.add_mutation(call_id, "inline-call", summary, summary, summary);
    flat.restamp_all_node_generations();
    return make_bool(true);
}

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────
EvalResult Evaluator::eval_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                aura::ast::NodeId id, const Env& env) {
    if (compiler_metrics_) {
        static_cast<CompilerMetrics*>(compiler_metrics_)
            ->hotpath_eval_flat_calls.fetch_add(1, std::memory_order_relaxed);
    }
    // Catch bad_variant_access and return friendly error instead of crash.
    // This happens when user code passes wrong argument types to primitives.
    try {
        // TCO loop state: f/p point to the current FlatAST/Pool,
        // which may change during closure/macro tail calls.
        aura::ast::FlatAST* f = &flat;
        aura::ast::StringPool* p = &pool;
        const Env* current_env = &env;
        aura::ast::NodeId current_id = id;
        std::optional<Env> tail_env;

        // Recursion depth guard: friendly error vs segfault
        // Each eval_flat frame is large (~7–8KB of C stack on aarch64/x86_64),
        // so the default 8MB process stack only holds ~1000 frames. A guard of
        // 2000 allowed SIGSEGV before the limit tripped. Keep headroom under
        // the OS stack (~700 frames ≈ 5.5MB of eval_flat alone).
        // ── Recursion depth guard (thread_local) ────────────────────────
        // #109 (P0): the depth counter must be PER THREAD, not per Evaluator.
        // Fiber fallback (std::thread + [this] capture) shares an Evaluator
        // across N OS threads; if each thread increments a shared counter, a
        // modest amount of parallel work trips the guard even though no single
        // thread is deeply nested. That is what made
        // `tests/suite/concurrent.aura` T7-T10 (orch:parallel 5-way, nested
        // spawn+join) flaky with the old shared counter.
        //
        // The shared eval_depth_ member is still used below for auto-gc-temp
        // sampling and auto-gc cooldown, where "global eval activity" is the
        // intended signal (one thread's deep call can still be enough work
        // to warrant a periodic gc-temp). The two are now decoupled.
        static constexpr std::size_t MAX_C_STACK_DEPTH = 700;
        thread_local std::size_t t_c_stack_depth = 0;
        struct DepthGuard {
            std::size_t& d;
            ~DepthGuard() { --d; }
        } _dg{t_c_stack_depth};
        if (++t_c_stack_depth > MAX_C_STACK_DEPTH)
            return std::unexpected(
                Diagnostic{ErrorKind::InternalError,
                           std::format("recursion depth exceeded (>{})", MAX_C_STACK_DEPTH)});

        // ── Memory pressure auto-governance sampling (P1) ─────────
        // Every sample_every_ calls to eval_flat, recompute pressure
        // and (if policy allows) auto-trigger gc-module for the top arena.
        // Outside the hot path for typical evals (default 1-in-1000).
        // Issue #991: thread_local counters (shared Evaluator, multi-worker).
        thread_local std::size_t t_sample_counter = 0;
        thread_local std::size_t t_last_auto_gc_eval_depth = 0;
        thread_local std::string t_last_warn_level;
        if (++t_sample_counter >= memory_policy_.sample_every) {
            t_sample_counter = 0;
            sample_counter_ = t_sample_counter; // mirror for policy reset observers
            // Snapshot arena state. Inline rather than refactoring into
            // a shared helper to avoid std::function capture-lifetime issues.
            struct Snap {
                std::string name;
                double used;
                double cap;
                int pct;
            };
            std::vector<Snap> snaps;
            double total_used = 0.0, total_cap = 0.0;
            if (arena_) {
                auto s = arena_->stats();
                double u = s.used / 1048576.0;
                double c = s.capacity / 1048576.0;
                snaps.push_back({"main", u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                total_used += u;
                total_cap += c;
            }
            if (arena_group_) {
                for (auto& [full_name, stats] : arena_group_->module_stats()) {
                    auto slash = full_name.rfind('/');
                    auto short_name =
                        slash == std::string::npos ? full_name : full_name.substr(slash + 1);
                    double u = stats.used / 1048576.0;
                    double c = stats.capacity / 1048576.0;
                    snaps.push_back(
                        {short_name, u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                    total_used += u;
                    total_cap += c;
                }
            }
            int overall = total_cap > 0 ? static_cast<int>(total_used / total_cap * 100.0) : 0;
            std::string level = "low";
            if (overall >= 95)
                level = "critical";
            else if (overall >= 80)
                level = "high";
            else if (overall >= 60)
                level = "medium";

            // Log warning on level transitions (avoid spam — only log when
            // the level string changes from the last warned one).
            if (level != t_last_warn_level && (level == "high" || level == "critical")) {
                std::println(
                    std::cerr,
                    "[memory-pressure] WARNING: level={} overall-pct={} total-used={:.1f}MB", level,
                    overall, total_used);
                t_last_warn_level = level;
                last_warn_level_ = level;
            } else if (level == "low" || level == "medium") {
                t_last_warn_level = level;
                last_warn_level_ = level;
            }

            // Auto-gc: only at critical AND policy enabled AND cooldown elapsed.
            if (memory_policy_.auto_gc && level == "critical" &&
                eval_depth_ - t_last_auto_gc_eval_depth > memory_policy_.cooldown_evals) {
                // Find top arena (highest used-pct, then largest used, then name asc).
                std::string top_name;
                int top_pct = 0;
                double top_used = 0.0;
                for (auto& s : snaps) {
                    if (s.pct > top_pct || (s.pct == top_pct && s.used > top_used) ||
                        (s.pct == top_pct && s.used == top_used && s.name < top_name)) {
                        top_name = s.name;
                        top_pct = s.pct;
                        top_used = s.used;
                    }
                }
                if (!top_name.empty()) {
                    std::println(std::cerr,
                                 "[memory-pressure] AUTO-GC: freeing arena '{}' ({}% full)",
                                 top_name, top_pct);
                    gc_module(top_name);
                    t_last_auto_gc_eval_depth = eval_depth_;
                    last_auto_gc_eval_depth_ = eval_depth_;
                }
            }
        }

        while (true) {
            current_flat_ = f;
            current_pool_ = p;
            // Save the eval environment before any tail_env.emplace could corrupt current_env
            const Env& eval_env = *current_env;
            if (current_id == aura::ast::NULL_NODE)
                return EvalResult(make_void());
            // Issue #273: catch stale NodeIds before tree-walker field access.
            // Softened to runtime Diagnostic so a COW/swap-workspace boundary
            // that leaves current_id with node_gen_[id] != generation_ returns
            // a friendly InternalError instead of aborting the test binary.
            // Callers that need the debug assertion can use f->is_valid() +
            // f->is_live_node() checks at higher layers.
            if (!f->is_valid(current_id)) {
                return std::unexpected(
                    Diagnostic{ErrorKind::InternalError, "stale node id (gen mismatch)"});
            }
            if (current_id >= f->size())
                return std::unexpected(Diagnostic{ErrorKind::InternalError, "invalid node id"});
            auto v = f->get(current_id);

            // Incremental eval: if node is clean and has a cached result, reuse it.
            // Skip leaf literals (LiteralInt, LiteralFloat, LiteralString) because
            // they're always fast and the cache lookup overhead is not worth it.
            if (v.tag != aura::ast::NodeTag::LiteralInt &&
                v.tag != aura::ast::NodeTag::LiteralFloat &&
                v.tag != aura::ast::NodeTag::LiteralString &&
                v.tag != aura::ast::NodeTag::Variable && !f->is_dirty(current_id)) {
                auto cached = f->get_cached_value(current_id);
                if (cached != aura::ast::FlatAST::kNotCached) {
                    return EvalResult(EvalValue(cached));
                }
            }

            switch (v.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    // #t/#f have BoolLiteral marker — convert to Bool at runtime
                    if (v.marker == aura::ast::SyntaxMarker::BoolLiteral)
                        return make_bool(v.int_value != 0);
                    return make_int(v.int_value);
                case aura::ast::NodeTag::LiteralFloat:
                    return make_float(v.float_value);
                case aura::ast::NodeTag::LiteralString: {
                    auto raw = std::string(p->resolve(v.sym_id));
                    // Short strings: use cache to avoid duplicate heap pushes
                    if (raw.size() <= 6) {
                        auto it = short_str_cache_.find(raw);
                        if (it != short_str_cache_.end())
                            return it->second;
                        auto sid = string_heap_.size();
                        string_heap_.push_back(raw);
                        auto val = make_string(sid);
                        short_str_cache_[raw] = val;
                        return val;
                    }
                    auto sid = string_heap_.size();
                    string_heap_.push_back(std::move(raw));
                    return make_string(sid);
                }
                case aura::ast::NodeTag::Variable: {
                    auto name = p->resolve(v.sym_id);
                    // Keyword: :foo → self-evaluating keyword value (interned)
                    if (!name.empty() && name[0] == ':') {
                        auto kwstr = std::string(name);
                        std::uint64_t kidx = 0;
                        // Check if already interned
                        bool found = false;
                        for (; kidx < keyword_table_.size(); ++kidx) {
                            if (keyword_table_[kidx] == kwstr) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            kidx = keyword_table_.size();
                            keyword_table_.push_back(kwstr);
                        }
                        return make_keyword(kidx);
                    }
                    auto val = eval_env.lookup(std::string(name));
                    if (val) {
                        // Issue #229 Cycle 1 fix: dereference cell
                        // sentinel. The Define case binds the name to
                        // a cell (make_cell(ci)) for re-def support;
                        // ordinary lookups (Variable here, call sites
                        // elsewhere) should auto-deref the cell to
                        // the underlying value. Without this, simple
                        // `(define x 10) (display x)` shows
                        // `<cell[0]>` instead of `10`.
                        if (is_cell(*val)) {
                            auto ci = as_cell_id(*val);
                            if (ci < cells_.size())
                                return cells_[ci];
                        }
                        return *val;
                    }
                    std::string var_name(name);
                    if (var_name.empty()) {
                        var_name = std::format("<sym:{}>", v.sym_id);
                    }
                    std::vector<std::string> candidates;
                    {
                        const Env* e = &eval_env;
                        while (e) {
                            for (auto& b : const_cast<Env&>(*e).bindings())
                                candidates.push_back(b.first);
                            e = e->parent();
                        }
                    }
                    auto best = closest_match(var_name, candidates);
                    // Issue #79: source location from the offending node so the
                    // error report includes line:col instead of just node[id:N].
                    Diagnostic d(ErrorKind::UnboundVariable, std::move(var_name),
                                 aura::diag::SourceLocation{v.line, v.col, 0}, current_id);
                    if (!best.empty())
                        d.with_suggestion("did you mean '" + best + "'?");
                    return std::unexpected(std::move(d));
                }
                case aura::ast::NodeTag::Call: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    auto callee_id = v.child(0);
                    auto callee = f->get(callee_id);
                    // Inline lambda (arg evals are recursive; body is tail)
                    if (callee.tag == aura::ast::NodeTag::Lambda) {
                        auto pspan = callee.params;
                        bool dotted = callee.int_value != 0;
                        std::size_t named_count =
                            dotted && !pspan.empty() ? pspan.size() - 1 : pspan.size();
                        // Evaluate named args
                        std::vector<EvalValue> iargs;
                        iargs.reserve(named_count);
                        for (std::size_t i = 0; i < named_count && i + 1 < v.children.size(); ++i) {
                            auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                            if (!ar)
                                return ar;
                            iargs.push_back(*ar);
                        }
                        tail_env.emplace(&eval_env);
                        tail_env->set_primitives(&primitives_);

                        for (std::size_t i = 0; i < iargs.size(); ++i) {
                            tail_env->bind(std::string(p->resolve(pspan[i])), std::move(iargs[i]));
                        }
                        // Dotted rest: collect remaining args into a pair list
                        if (dotted && !pspan.empty()) {
                            types::EvalValue rest = make_void();
                            for (std::size_t i = v.children.size() - 1; i > named_count; --i) {
                                auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                if (!ar)
                                    return ar;
                                auto pid = pairs_.size();
                                pairs_.push_back({*ar, rest});
                                rest = make_pair(pid);
                            }
                            tail_env->bind(std::string(p->resolve(pspan.back())), rest);
                        }
                        auto body_id =
                            callee.children.empty() ? aura::ast::NULL_NODE : callee.child(0);
                        if (body_id != aura::ast::NULL_NODE)
                            return eval_flat(*f, *p, body_id, *tail_env);
                        return make_void();
                    }
                    // Macro expansion: evaluate args, bind in env, evaluate body (produces template
                    // data), then re-evaluate the data as code
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        auto macro_it = macros_.find(cname);
                        if (macro_it != macros_.end()) {
                            auto& md = macro_it->second;
                            bool is_rest = md.dotted;

                            // Issue #334: env-binding path for
                            // `define-hygienic-macro*` (md.preserved).
                            // The macro args are bound in a child env
                            // (name → ast_to_data), and the body is
                            // evaluated in that env. This makes the
                            // body's Variable refs resolve to the
                            // literal arg values directly — the same
                            // behavior as the legacy `defmacro` path.
                            // Used by symbol-generating macros like
                            // define-struct where `name` should
                            // resolve to the literal symbol and
                            // `fields` to the literal field list.
                            if (md.preserved) {
                                if (is_rest) {
                                    // Rest params on preserved
                                    // macros are not yet supported
                                    // — fall through to a "no
                                    // expansion" return.
                                    return make_void();
                                }
                                // Bind macro args as data in tail_env
                                std::size_t regular_count = md.params.size();
                                tail_env.emplace(&eval_env);
                                tail_env->set_primitives(&primitives_);
                                for (std::size_t i = 0;
                                     i < regular_count && i + 1 < v.children.size(); ++i) {
                                    tail_env->bind(md.params[i],
                                                   ast_to_data(*f, *p, v.child(i + 1)));
                                }
                                // Issue #334: use md.pool if set (the
                                // macro was defined in a different
                                // workspace), else use the current pool
                                // p. The body's Variables look up by
                                // string name in tail_env, so pool
                                // alignment doesn't matter for the
                                // Variable case (it just needs a valid
                                // pool to resolve symids into strings).
                                auto* mb_pool = md.pool ? md.pool : p;
                                auto* mb_flat = md.pool ? md.flat : f;
                                auto template_result =
                                    eval_flat(*mb_flat, *mb_pool, md.body_id, *tail_env);
                                if (!template_result)
                                    return template_result;
                                // Issue #334 (Cycle 1): convert the
                                // macro body data to FlatAST via
                                // data_to_flat. eval_data_as_code
                                // (the legacy path used here before)
                                // treats strings as literal values,
                                // so Variable references in the body
                                // become string values rather than
                                // runtime lookups — meaning
                                // symbol-generating macros that
                                // splice field names into the lambda
                                // body get the field-name strings
                                // baked into the vector instead of
                                // the call args. data_to_flat
                                // converts each string to a Variable
                                // AST node (add_variable), so the
                                // resulting lambda body has proper
                                // Variable refs that look up in the
                                // lambda's call env at call time.
                                auto ast_root = data_to_flat(*template_result, *f, *p, /*depth=*/0);
                                if (ast_root == aura::ast::NULL_NODE)
                                    return std::unexpected(aura::diag::Diagnostic{
                                        aura::diag::ErrorKind::InternalError,
                                        "data_to_flat returned NULL for env-binding macro body"});
                                return eval_flat(*f, *p, ast_root, eval_env);
                            }

                            // Issue #120: hygienic macros use clone_macro_body
                            // with a name_map (single-eval AST substitution +
                            // automatic gensym for template-introduced
                            // bindings). Non-hygienic macros keep the legacy
                            // double-eval path for backward compatibility.
                            if (md.hygienic) {
                                if (is_rest) {
                                    // Rest params on hygienic macros are
                                    // not yet supported — fall through to a
                                    // "no expansion" return.
                                    return make_void();
                                }
                                // Issue #146 follow-up: build the subst
                                // map via the pure helper. Local
                                // materialization of v.children into a
                                // vector (one alloc per macro call) keeps
                                // the call site short.
                                std::vector<aura::ast::NodeId> call_args(v.children.begin(),
                                                                         v.children.end());
                                auto subst = aura::compiler::pure::compute_macro_subst_pure(
                                    md.params, call_args, /*dotted=*/false);
                                // Clone the macro body with substitution +
                                // name_map. The cloned tree is in the
                                // *current* FlatAST (we use the target's
                                // flat = f, source = md.flat). name_map is
                                // empty initially; clone_macro_body
                                // populates it as it gensym's
                                // template-introduced bindings.
                                std::unordered_map<std::string, std::string> rename_map;
                                auto* src_pool = md.pool ? md.pool : p;
                                auto expanded = clone_macro_body(
                                    *f, *p, *md.flat, *src_pool, md.body_id, &subst, &rename_map,
                                    /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
                                if (expanded == aura::ast::NULL_NODE)
                                    return make_void();
                                // Issue #230 #2 follow-up: undo the Quote-wrap
                                // on set! targets. Same rationale as the
                                // lambda case in eval_data_as_code — the
                                // Quote-wrap helps symbol-generating macros
                                // but breaks set! semantics.
                                // unwrap_set_quotes removed (Quote-wrap reverted)
                                // Issue #121: recursively expand any nested
                                // macro calls in the cloned body using the
                                // runtime `macros_` registry. The cloned body
                                // lives in the calling flat (`*f`), but other
                                // macros (m1, m2, ...) were defined in earlier
                                // forms with their own flates — so the static
                                // `macro_expand_all` (which scans the flat)
                                // wouldn't see them. We walk the cloned tree
                                // and, for each Call whose callee is in
                                // `macros_`, recursively expand it. Bounded by
                                // a depth limit (10) to prevent infinite loops
                                // (e.g., macro X calls macro X).
                                expanded = expand_inner_macros(f, p, expanded, /*depth=*/0,
                                                               /*max_depth=*/10,
                                                               as_expansion_registry(macros_));
                                f->restamp_all_node_generations();
                                // Evaluate the cloned + inner-expanded
                                // body. eval_flat returns a runtime
                                // value (a list for cons-chain qq
                                // bodies). The list needs to be
                                // re-evaluated as code so the
                                // inner macro's result is invoked
                                // (e.g., `(bar ,x)` returns
                                // `(* x 2)`, which then evaluates
                                // as a call to `*`). This mirrors
                                // the legacy defmacro path's
                                // eval_data_as_code re-evaluation
                                // and is what enables macro
                                // composition in the hygienic
                                // case (Issue #158).
                                auto hygienic_result = eval_flat(*f, *p, expanded, eval_env);
                                if (!hygienic_result)
                                    return hygienic_result;
                                return eval_data_as_code(*hygienic_result, eval_env, f, p);
                            }

                            // Convert AST args to data (NOT evaluate — macros receive syntax)
                            // Bind regular params first (all but the last)
                            std::size_t regular_count =
                                is_rest ? md.params.size() - 1 : md.params.size();
                            tail_env.emplace(&eval_env);
                            tail_env->set_primitives(&primitives_);


                            for (std::size_t i = 0; i < regular_count && i + 1 < v.children.size();
                                 ++i) {
                                tail_env->bind(md.params[i], ast_to_data(*f, *p, v.child(i + 1)));
                            }

                            // Rest param: collect remaining args as a list
                            if (is_rest) {
                                auto& rest_name = md.params.back();
                                EvalValue rest_list = make_void();
                                for (std::size_t i = v.children.size() - 1; i >= regular_count + 1;
                                     --i) {
                                    auto item = ast_to_data(*f, *p, v.child(i));
                                    auto pid = pairs_.size();
                                    pairs_.push_back(Pair{std::move(item), rest_list});
                                    rest_list = make_pair(pid);
                                }
                                tail_env->bind(rest_name, std::move(rest_list));
                            }
                            // Evaluate macro body (quasiquote-expanded template) → produces data
                            auto template_result =
                                eval_flat(*md.flat, md.pool ? *md.pool : *p, md.body_id, *tail_env);
                            if (!template_result)
                                return template_result;
                            // Issue #334 (deferred): the simpler
                            // data_to_flat path was tried here and
                            // reverted — it can't work because the
                            // first eval_flat (line above) already
                            // fails when the macro body references
                            // lambda-local Variables (e.g. `args` of a
                            // generated `(lambda (args) ...)`) that
                            // aren't bound in tail_env at expansion
                            // time. The proper fix for #1/#2
                            // (define-struct) is the env-binding path
                            // (issue 334), not a refactor of the
                            // defmacro expansion here.
                            return eval_data_as_code(*template_result, eval_env, f, p);
                        }
                    }
                    // Built-in require: (require mod-name) — symbol, not string
                    // Phase 4: prefix by default. (require std/list) → (import "std/list" "list:")
                    //          (require std/list all:) → (import "std/list")  (backward compat)
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        if (cname == "require" && v.children.size() > 1) {
                            // Collect module names and check for all: flag
                            // (require mod1 mod2 ... all:) — all: applies to ALL modules
                            std::vector<std::string> mod_names;
                            bool use_prefix = true;
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                auto arg_v = f->get(v.child(ci));
                                if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                    auto arg_name = std::string(p->resolve(arg_v.sym_id));
                                    if (arg_name == "all:") {
                                        use_prefix = false;
                                    } else {
                                        mod_names.push_back(arg_name);
                                    }
                                } else if (arg_v.tag == aura::ast::NodeTag::LiteralString) {
                                    mod_names.push_back(std::string(p->resolve(arg_v.sym_id)));
                                }
                            }

                            // Load all modules in sequence
                            if (!arena_)
                                return make_void();
                            EvalResult last = make_void();
                            for (auto& mod_path : mod_names) {
                                // Derive prefix from module name (last path component)
                                std::string prefix;
                                if (use_prefix) {
                                    auto slash = mod_path.rfind('/');
                                    auto base = (slash == std::string::npos)
                                                    ? mod_path
                                                    : mod_path.substr(slash + 1);
                                    prefix = base + ":";
                                }

                                // Build (import "path" "prefix:") or (import "path")
                                std::string import_expr;
                                if (prefix.empty()) {
                                    import_expr = std::string("(import \"") + mod_path + "\")";
                                } else {
                                    import_expr = std::string("(import \"") + mod_path + "\" \"" +
                                                  prefix + "\")";
                                }

                                // Use temp_arena_ so (gc-temp) reclaims the
                                // parse state for each (require ...) call.
                                auto alloc = temp_arena_->allocator();
                                auto* ipool = temp_arena_->create<aura::ast::StringPool>(alloc);
                                auto* iflat = temp_arena_->create<aura::ast::FlatAST>(alloc);
                                auto pr = aura::parser::parse_to_flat(import_expr, *iflat, *ipool);
                                if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                                    return std::unexpected(Diagnostic{ErrorKind::ParseError,
                                                                      "require: internal error"});
                                }
                                iflat->root = pr.root;
                                // Pre-expand macros so import primitive is recognized
                                auto expanded_root =
                                    aura::compiler::macro_expand_all(*iflat, *ipool, iflat->root);
                                last = eval_flat(*iflat, *ipool, expanded_root, eval_env);
                                if (!last)
                                    return last;
                            }
                            return last;
                        }
                    }
                    // try/catch: (try body (catch (var) handler))
                    // body is evaluated; if it returns an error, handler is evaluated with var
                    // bound
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        // when: (when cond body...) — evaluate body only if cond is truthy
                        if (cname == "when" && v.children.size() >= 2) {
                            auto cond_id = v.child(1);
                            auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                            if (!cond_result)
                                return cond_result;
                            if (is_truthy(*cond_result)) {
                                // Evaluate all remaining children as body
                                EvalResult last = make_void();
                                for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                    last = eval_flat(*f, *p, v.child(ci), eval_env);
                                    if (!last)
                                        return last;
                                }
                                return last;
                            }
                            return make_void();
                        }
                        // unless: (unless cond body...) — evaluate body only if cond is falsy
                        if (cname == "unless" && v.children.size() >= 2) {
                            auto cond_id = v.child(1);
                            auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                            if (!cond_result)
                                return cond_result;
                            if (!is_truthy(*cond_result)) {
                                EvalResult last = make_void();
                                for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                    last = eval_flat(*f, *p, v.child(ci), eval_env);
                                    if (!last)
                                        return last;
                                }
                                return last;
                            }
                            return make_void();
                        }
                        // with-arena: (with-arena (size) body...)
                        if (cname == "with-arena" && v.children.size() >= 1) {
                            std::size_t body_start = 1;
                            if (v.children.size() >= 2) {
                                auto first_id = v.child(1);
                                auto first_v = f->get(first_id);
                                if (first_v.tag == ast::NodeTag::Call &&
                                    first_v.children.size() >= 1)
                                    body_start = 2;
                            }
                            tl_arena_push(&g_tl_arena);
                            EvalResult last_result = make_void();
                            for (std::size_t ci = body_start; ci < v.children.size(); ++ci) {
                                last_result = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!last_result)
                                    return last_result;
                            }
                            // Deep-copy result if it's an arena-allocated pair
                            if (last_result && is_pair(*last_result)) {
                                auto idx = as_pair_idx(*last_result);
                                if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
                                    auto* slot = g_pair_slots[idx];
                                    auto arena_end = g_tl_arena.base + g_tl_arena.offset;
                                    auto ptr = (uint8_t*)slot;
                                    if (g_tl_arena.base && ptr >= g_tl_arena.base &&
                                        ptr < arena_end) {
                                        auto* new_slot = (PairSlot*)std::malloc(sizeof(PairSlot));
                                        new_slot->car = slot->car;
                                        new_slot->cdr = slot->cdr;
                                        auto new_id = static_cast<int64_t>(g_pair_slots.size());
                                        g_pair_slots.push_back(new_slot);
                                        *last_result =
                                            types::make_pair(static_cast<std::uint64_t>(new_id));
                                    }
                                }
                            }
                            tl_arena_pop(&g_tl_arena);
                            return last_result;
                        }
                        // performance-region: (performance-region body...)
                        if (cname == "performance-region" && v.children.size() >= 2) {
                            EvalResult last = make_void();
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                last = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!last)
                                    return last;
                            }
                            return last;
                        }
                        // evolution-region: (evolution-region body...)
                        if (cname == "evolution-region" && v.children.size() >= 2) {
                            EvalResult last = make_void();
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                last = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!last)
                                    return last;
                            }
                            return last;
                        }
                        // with-capability: (with-capability cap-name body...)
                        // Bind capabilities as special variables in the environment.
                        if (cname == "with-capability" && v.children.size() >= 2) {
                            auto cap_id = v.child(1);
                            auto cap_result = eval_flat(*f, *p, cap_id, eval_env);
                            if (!cap_result)
                                return cap_result;
                            // Extract capability name(s)
                            std::vector<std::string> caps;
                            if (is_string(*cap_result)) {
                                auto sidx = as_string_idx(*cap_result);
                                if (sidx < string_heap_.size())
                                    caps.push_back(string_heap_[sidx]);
                            } else if (is_pair(*cap_result)) {
                                auto cidx = as_pair_idx(*cap_result);
                                while (cidx < pairs_.size()) {
                                    auto& pr = pairs_[cidx];
                                    if (is_string(pr.car)) {
                                        auto sidx2 = as_string_idx(pr.car);
                                        if (sidx2 < string_heap_.size())
                                            caps.push_back(string_heap_[sidx2]);
                                    }
                                    break;
                                }
                            }
                            // Create child env with %cap:name bindings
                            tail_env.emplace(&eval_env);
                            tail_env->set_primitives(&primitives_);

                            for (auto& cap : caps)
                                tail_env->bind("%cap:" + cap, make_bool(true));
                            // Push to capability_stack_ for capability-stack readout
                            capability_stack_.push_back(caps);
                            // Evaluate body in child env
                            EvalResult last = make_void();
                            for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                last = eval_flat(*f, *p, v.child(ci), *tail_env);
                                if (!last) {
                                    capability_stack_.pop_back();
                                    return last;
                                }
                            }
                            capability_stack_.pop_back();
                            return last;
                        }
                        // check-capability: (check-capability "Name") — look up %cap:Name binding
                        if (cname == "check-capability" && v.children.size() >= 2) {
                            auto arg_result = eval_flat(*f, *p, v.child(1), eval_env);
                            if (!arg_result)
                                return arg_result;
                            std::string cap_name;
                            if (is_string(*arg_result)) {
                                auto sidx = as_string_idx(*arg_result);
                                if (sidx < string_heap_.size())
                                    cap_name = string_heap_[sidx];
                            }
                            auto val = eval_env.lookup("%cap:" + cap_name);
                            return val.has_value() ? make_bool(true) : make_bool(false);
                        }

                        // while: (while cond body) — evaluate condition, if true evaluate body,
                        // repeat
                        if (cname == "while" && v.children.size() >= 3) {
                            // Check if args are Lambda nodes (EDSL while with closures)
                            // In that case, fall through to primitive dispatch instead
                            auto c1_node = v.child(1) < f->size() ? f->get(v.child(1)) : v;
                            auto c2_node = v.child(2) < f->size() ? f->get(v.child(2)) : v;
                            if (c1_node.tag != aura::ast::NodeTag::Lambda &&
                                c2_node.tag != aura::ast::NodeTag::Lambda) {
                                while (true) {
                                    auto cond_result = eval_flat(*f, *p, v.child(1), eval_env);
                                    if (!cond_result)
                                        return cond_result;
                                    if (!is_truthy(*cond_result))
                                        break;
                                    auto body_result = eval_flat(*f, *p, v.child(2), eval_env);
                                    if (!body_result)
                                        return body_result;
                                }
                                return make_void();
                            }
                        }

                        if (cname == "try" && v.children.size() >= 2) {
                            auto body_id = v.child(1);
                            auto result = eval_flat(*f, *p, body_id, eval_env);
                            if (result && !is_error(*result)) {
                                // Body succeeded — return result as-is
                                return result;
                            }
                            // Body errored — find catch clause (child[2] or later)
                            if (v.children.size() < 3)
                                return make_void();
                            for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                auto catch_id = v.child(ci);
                                auto cv = f->get(catch_id);
                                if (cv.tag == aura::ast::NodeTag::Call) {
                                    auto catch_fn = f->get(cv.child(0));
                                    if (catch_fn.tag == aura::ast::NodeTag::Variable &&
                                        std::string(p->resolve(catch_fn.sym_id)) == "catch") {
                                        // (catch (var) handler) — child[0]=catch, child[1]=(var),
                                        // child[2]=handler
                                        if (cv.children.size() < 3)
                                            continue;
                                        auto var_form = f->get(cv.child(1));
                                        // var_form is (var) — a Call where child[0]=Variable "var"
                                        std::string var_name;
                                        if (var_form.tag == aura::ast::NodeTag::Call &&
                                            var_form.children.size() >= 1) {
                                            auto var_node = f->get(var_form.child(0));
                                            if (var_node.tag == aura::ast::NodeTag::Variable)
                                                var_name = std::string(p->resolve(var_node.sym_id));
                                        }
                                        auto handler_id = cv.child(2);
                                        // Bind error value to var and evaluate handler
                                        Env catch_env(&eval_env);
                                        catch_env.set_primitives(&primitives_);
                                        // P0: no cells_ on Env; deref uses central cells_ or owner
                                        // walk
                                        if (!var_name.empty() && result) {
                                            catch_env.bind(var_name, *result);
                                        }
                                        return eval_flat(*f, *p, handler_id, catch_env);
                                    }
                                }
                            }
                            // No matching catch — propagate error
                            return result;
                        }
                    }

                    // and/or: short-circuit evaluation (not eager arg eval)
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        if (cname == "and" && v.children.size() >= 2) {
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                auto ar = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!ar)
                                    return ar;
                                if (!is_truthy(*ar))
                                    return *ar; // short-circuit: return falsy value
                                if (ci + 1 == v.children.size())
                                    return *ar; // last arg: return its value
                            }
                            return make_int(1); // (and) with no args → #t
                        }
                        if (cname == "or" && v.children.size() >= 2) {
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                auto ar = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!ar)
                                    return ar;
                                if (is_truthy(*ar))
                                    return *ar; // short-circuit: return first truthy value
                                if (ci + 1 == v.children.size())
                                    return *ar; // last arg: return last value (falsy)
                            }
                            return make_int(0); // (or) with no args → #f
                        }
                    }

                    // Primitive call (all arg evals are recursive)
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        // #223 follow-up: skip primitive lookup when
                        // cname is empty (e.g. when sym_id was out of
                        // bounds at the resolve() call). The
                        // environment lookup below handles missing
                        // names gracefully via nullopt.
                        std::optional<PrimFn> prim;
                        if (!cname.empty()) {
                            prim = eval_env.lookup_primitive(cname);
                        }
                        if (prim) {
                            std::vector<EvalValue> args;
                            for (std::size_t i = 1; i < v.children.size(); ++i) {
                                auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                if (!ar)
                                    return ar;
                                // Propagate error values through normal eval
                                // Note: is_string check prevents accidental collision
                                // where make_string(idx) with odd idx matches is_ref/RefError
                                // encoding
                                if (is_error(*ar) && !is_string(*ar))
                                    return ar;
                                args.push_back(*ar);
                            }
                            // Issue #441 / #1357: call count + render latency telemetry.
                            return eval_env.owner()->invoke_prim_with_telemetry(
                                cname, [&]() { return (*prim)(args); });
                        }
                    }
                    // Closure call (eval func + arg evals are recursive; body is tail)
                    auto fn = eval_flat(*f, *p, callee_id, eval_env);
                    if (!fn)
                        return fn;
                    if (is_closure(*fn)) {
                        auto cid = as_closure_id(*fn);
                        // Check for foreign function (high bit set)
                        if (cid < ffi_runtime_.func_count()) {
                            // Dispatch FFI through apply_closure
                            // (Issue #252: apply_closure increments
                            // closure_calls_total + closure_ffi_calls
                            // for the FFI branch — no double-count
                            // here because eval_flat's FFI inline path
                            // delegates the counter bumps to it.)
                            std::size_t named_count = 0;
                            std::vector<EvalValue> cargs;
                            for (std::size_t i = 0; i + 1 < v.children.size(); ++i) {
                                auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                                if (!ar)
                                    return ar;
                                cargs.push_back(*ar);
                            }
                            auto result = apply_closure(cid, cargs);
                            if (result)
                                return *result;
                            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                              "eval_flat: foreign call failed"});
                        }
                        // Issue #252: eval_flat's inline TW closure
                        // call path does NOT go through apply_closure
                        // (it inlines the body for TCO). Bump the
                        // total + TW counter here so the snapshot is
                        // consistent with apply_closure's other
                        // entry points.
                        if (compiler_metrics_) {
                            auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                            m->closure_calls_total.fetch_add(1, std::memory_order_relaxed);
                            m->closure_tw_calls.fetch_add(1, std::memory_order_relaxed);
                        }
                        Closure cl;
                        bool tw_closure = false;
                        {
                            std::shared_lock<std::shared_mutex> rlock(closures_mtx_);
                            auto it = closures_.find(cid);
                            if (it != closures_.end()) {
                                cl = it->second;
                                tw_closure = true;
                            }
                        }
                        if (!tw_closure) {
                            // IR-produced closure (e.g. lambda arg to cached define).
                            std::vector<EvalValue> cargs;
                            for (std::size_t i = 1; i < v.children.size(); ++i) {
                                auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                if (!ar)
                                    return ar;
                                if (is_error(*ar))
                                    return ar;
                                cargs.push_back(*ar);
                            }
                            auto bridged = apply_closure(cid, cargs);
                            if (bridged)
                                return *bridged;
                            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                              "eval_flat: invalid closure"});
                        }
                        // Evaluate named args for TW TCO inline path.
                        std::size_t named_count = cl.dotted && !cl.params.empty()
                                                      ? cl.params.size() - 1
                                                      : cl.params.size();
                        std::vector<EvalValue> cargs;
                        cargs.reserve(named_count);
                        for (std::size_t i = 0; i < named_count && i + 1 < v.children.size(); ++i) {
                            auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                            if (!ar)
                                return ar;
                            if (is_error(*ar))
                                return ar;
                            cargs.push_back(*ar);
                        }
                        tail_env = materialize_call_env(cl);
                        tail_env->set_primitives(&primitives_);

                        // Issue #145: set the pool so bind_symid can mirror
                        if (cl.pool)
                            tail_env->set_pool(cl.pool);
                        for (std::size_t i = 0; i < cargs.size(); ++i) {
                            tail_env->bind_symid(cl.params[i], std::move(cargs[i]));
                        }
                        // Dotted rest: collect remaining args into a pair list
                        if (cl.dotted && !cl.params.empty()) {
                            types::EvalValue rest = make_void();
                            for (std::size_t i = v.children.size() - 1; i > named_count; --i) {
                                auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                if (!ar)
                                    return ar;
                                if (is_error(*ar) && !is_string(*ar))
                                    return ar;
                                auto pid = pairs_.size();
                                pairs_.push_back({*ar, rest});
                                rest = make_pair(pid);
                            }
                            tail_env->bind_symid(cl.params.back(), rest);
                        }
                        if (cl.body_id != aura::ast::NULL_NODE)
                            return eval_flat(*cl.flat, cl.pool ? *cl.pool : *p, cl.body_id,
                                             *tail_env);
                        return make_void();
                    }
                    // Functor instantiation: callee is a %functor marker
                    if (is_string(*fn) && as_string_idx(*fn) < string_heap_.size() &&
                        string_heap_[as_string_idx(*fn)] == "%functor") {
                        auto callee_v = f->get(v.child(0));
                        if (callee_v.tag == aura::ast::NodeTag::Variable) {
                            auto tpl_name = std::string(p->resolve(callee_v.sym_id));
                            auto tpl_it = module_templates_.find(tpl_name);
                            if (tpl_it != module_templates_.end()) {
                                // 构建缓存 key: "template|arg1|arg2|..."
                                std::string cache_key = tpl_name;
                                for (std::size_t ki = 1; ki < v.children.size(); ++ki) {
                                    auto kv = f->get(v.child(ki));
                                    cache_key += "|";
                                    if (kv.tag == aura::ast::NodeTag::Variable)
                                        cache_key += std::string(p->resolve(kv.sym_id));
                                    else
                                        cache_key += "#" + std::to_string(v.child(ki));
                                }
                                auto cache_it = functor_instance_cache_.find(cache_key);
                                if (cache_it != functor_instance_cache_.end()) {
                                    return types::make_module(cache_it->second);
                                }

                                // 使用 ModuleTemplate 中缓存的参数名（避免跨 FlatAST 扫描）
                                auto& param_names = tpl_it->second.type_param_names;

                                // 创建隔离环境
                                Env mod_env(&eval_env);
                                mod_env.set_primitives(&primitives_);


                                // 绑定类型参数到环境（按原始参数名）
                                for (std::size_t ai = 1; ai < v.children.size(); ++ai) {
                                    auto arg_v = f->get(v.child(ai));
                                    std::string pname = (ai - 1 < param_names.size())
                                                            ? param_names[ai - 1]
                                                            : (":T" + std::to_string(ai - 1));
                                    if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                        // 类型参数：存为字符串（类型名）
                                        auto type_name = std::string(p->resolve(arg_v.sym_id));
                                        auto sidx = string_heap_.size();
                                        string_heap_.push_back(type_name);
                                        mod_env.bind(pname, make_string(sidx));
                                    } else {
                                        // 值参数：正常 eval
                                        auto ar = eval_flat(*f, *p, v.child(ai), eval_env);
                                        if (!ar)
                                            return ar;
                                        mod_env.bind(pname, *ar);
                                    }
                                }

                                // Capability requirement check
                                if (!tpl_it->second.cap_require.empty()) {
                                    // Find the capability argument
                                    // Capability params are stored at the end of param_names
                                    std::string provided_caps_str;
                                    for (std::size_t ai = 1; ai < v.children.size(); ++ai) {
                                        auto arg_v = f->get(v.child(ai));
                                        std::string pname = (ai - 1 < param_names.size())
                                                                ? param_names[ai - 1]
                                                                : "";
                                        // Check if this param is a cap param
                                        bool is_cap_param = false;
                                        for (auto& cp : tpl_it->second.cap_param_names) {
                                            if (cp == pname) {
                                                is_cap_param = true;
                                                break;
                                            }
                                        }
                                        if (is_cap_param) {
                                            if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                                provided_caps_str =
                                                    std::string(p->resolve(arg_v.sym_id));
                                            } else if (arg_v.tag ==
                                                       aura::ast::NodeTag::LiteralString) {
                                                provided_caps_str = "";
                                            }
                                        }
                                    }
                                    // Check if provided caps satisfy requirements
                                    // Simple string matching: "FileReadWrite" contains "FileRead"
                                    // and "FileWrite"
                                    std::vector<std::string> missing;
                                    for (auto& req : tpl_it->second.cap_require) {
                                        bool found = false;
                                        if (provided_caps_str.find(req) != std::string::npos)
                                            found = true;
                                        // Also check for "*" wildcard
                                        if (provided_caps_str == "*")
                                            found = true;
                                        if (!found)
                                            missing.push_back(req);
                                    }
                                    if (!missing.empty()) {
                                        std::string err =
                                            "functor " + tpl_name + ": missing capabilities: ";
                                        for (std::size_t mi = 0; mi < missing.size(); ++mi) {
                                            if (mi > 0)
                                                err += ", ";
                                            err += missing[mi];
                                        }
                                        auto es = string_heap_.size();
                                        string_heap_.push_back(err);
                                        auto ev = error_values_.size();
                                        error_values_.push_back(make_string(es));
                                        return make_error(ev);
                                    }
                                }

                                // Eval body by re-parsing the serialized source
                                EvalResult last = make_void();
                                auto& body_src = tpl_it->second.body_source;
                                if (!body_src.empty()) {
                                    // Parse body as a begin block so all expressions become
                                    // children
                                    std::string wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena body_arena;
                                    auto body_alloc = body_arena.allocator();
                                    aura::ast::StringPool body_pool(body_alloc);
                                    aura::ast::FlatAST body_flat(body_alloc);
                                    auto body_pr =
                                        aura::parser::parse_to_flat(wrapped, body_flat, body_pool);
                                    if (body_pr.success && body_pr.root != aura::ast::NULL_NODE) {
                                        body_flat.root = body_pr.root;
                                        auto body_v = body_flat.get(body_flat.root);
                                        if (body_v.tag == aura::ast::NodeTag::Begin) {
                                            for (auto nid : body_v.children) {
                                                auto br =
                                                    eval_flat(body_flat, body_pool, nid, mod_env);
                                                if (!br)
                                                    return br;
                                                last = *br;
                                            }
                                        } else {
                                            auto br = eval_flat(body_flat, body_pool,
                                                                body_flat.root, mod_env);
                                            if (!br)
                                                return br;
                                            last = *br;
                                        }
                                    }
                                }
                                // 实例化后生成 .aura-type 签名
                                // Extract export names from the body source
                                std::vector<std::string> export_names;
                                {
                                    std::string scan_wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena scan_arena;
                                    auto scan_alloc = scan_arena.allocator();
                                    aura::ast::StringPool scan_pool(scan_alloc);
                                    aura::ast::FlatAST scan_flat(scan_alloc);
                                    auto scan_pr = aura::parser::parse_to_flat(
                                        scan_wrapped, scan_flat, scan_pool);
                                    if (scan_pr.success && scan_pr.root != aura::ast::NULL_NODE) {
                                        scan_flat.root = scan_pr.root;
                                        auto scan_v = scan_flat.get(scan_flat.root);
                                        auto scan_children =
                                            (scan_v.tag == aura::ast::NodeTag::Begin)
                                                ? scan_v.children
                                                : std::span<const aura::ast::NodeId>(
                                                      &scan_flat.root, 1);
                                        for (auto nid : scan_children) {
                                            auto nv = scan_flat.get(nid);
                                            if (nv.tag == aura::ast::NodeTag::Export) {
                                                for (auto eid : nv.children) {
                                                    auto ev = scan_flat.get(eid);
                                                    if (ev.tag == aura::ast::NodeTag::Variable)
                                                        export_names.push_back(std::string(
                                                            scan_pool.resolve(ev.sym_id)));
                                                }
                                            }
                                        }
                                    }
                                }
                                if (!export_names.empty()) {
                                    // 对实例化后的 body 做类型推断，生成实际签名
                                    // Parse body source, type-check via TypeChecker, register
                                    // signatures
                                    aura::core::TypeRegistry tc_reg;
                                    aura::compiler::TypeChecker functor_tc(tc_reg);
                                    aura::diag::DiagnosticCollector tc_diag;

                                    std::string tc_wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena tc_arena;
                                    auto tc_alloc = tc_arena.allocator();
                                    aura::ast::StringPool tc_pool(tc_alloc);
                                    aura::ast::FlatAST tc_flat(tc_alloc);
                                    auto tc_pr =
                                        aura::parser::parse_to_flat(tc_wrapped, tc_flat, tc_pool);
                                    aura::ast::NodeId tc_root = tc_pr.root;
                                    if (tc_pr.success && tc_root != aura::ast::NULL_NODE) {
                                        tc_flat.root = tc_root;
                                        // Type-check the whole body to populate func types
                                        functor_tc.infer_flat(tc_flat, tc_pool, tc_root, tc_diag);

                                        // Scan body for export functions and extract their types
                                        for (auto& en : export_names) {
                                            std::string sig_key = cache_key + "/" + en;
                                            if (declared_type_sigs_.find(sig_key) !=
                                                declared_type_sigs_.end())
                                                continue;

                                            // Find the Define node for this export
                                            bool found = false;
                                            std::string type_str = "Any|Any";
                                            for (aura::ast::NodeId nid = 0; nid < tc_flat.size();
                                                 ++nid) {
                                                auto nv = tc_flat.get(nid);
                                                if (nv.tag == aura::ast::NodeTag::Define &&
                                                    nv.sym_id != aura::ast::INVALID_SYM &&
                                                    std::string(tc_pool.resolve(nv.sym_id)) == en &&
                                                    !nv.children.empty()) {
                                                    // Re-infer the value expression to get its type
                                                    auto val_type = functor_tc.infer_flat(
                                                        tc_flat, tc_pool, nv.child(0), tc_diag);
                                                    if (val_type.valid() && val_type.index > 0) {
                                                        // Format as type signature
                                                        auto fmt = tc_reg.format_type(val_type);
                                                        if (!fmt.empty()) {
                                                            // Convert from '->' to '|' format for
                                                            // declared_type_sigs_
                                                            auto pipe_pos = fmt.find(" -> ");
                                                            if (pipe_pos != std::string::npos) {
                                                                auto params =
                                                                    fmt.substr(0, pipe_pos);
                                                                auto ret = fmt.substr(pipe_pos + 4);
                                                                type_str = params + "|" + ret;
                                                            } else {
                                                                type_str = "|" + fmt;
                                                            }
                                                        }
                                                    }
                                                    found = true;
                                                    break;
                                                }
                                            }

                                            declared_type_sigs_[sig_key] = {
                                                .type_str = type_str,
                                                .module_file = "%functor:" + tpl_name,
                                                .resolved = found};
                                        }
                                    } else {
                                        // Fallback: Any|Any (shouldn't happen since body was parsed
                                        // earlier)
                                        for (auto& en : export_names) {
                                            std::string sig_key = cache_key + "/" + en;
                                            if (declared_type_sigs_.find(sig_key) ==
                                                declared_type_sigs_.end()) {
                                                declared_type_sigs_[sig_key] = {
                                                    .type_str = "Any|Any",
                                                    .module_file = "%functor:" + tpl_name,
                                                    .resolved = false};
                                            }
                                        }
                                    }
                                }

                                // 缓存实例化结果 — per-instance arena so the
                                // Env and any closures it owns can be freed later
                                // via gc_module(cache_key).
                                auto& inst_arena = arena_group_->module_arena(cache_key);
                                auto* cached_env = inst_arena.create<Env>(mod_env);
                                auto mod_idx = modules_.size();
                                modules_.push_back(cached_env);
                                module_cache_[cache_key] = mod_idx;
                                module_arena_ptrs_[cache_key] = &inst_arena;
                                module_names_.push_back(cache_key);
                                functor_instance_cache_[cache_key] = mod_idx;
                                return types::make_module(mod_idx);
                            }
                        }
                        return make_void();
                    }

                    // Primitive value call: callee is a PrimitiveRef (passed as value, not a
                    // Variable node)
                    if (is_primitive(*fn)) {
                        auto slot = as_primitive_slot(*fn);
                        if (slot < primitives_.slot_count()) {
                            auto prim = eval_env.lookup_primitive(primitives_.name_for_slot(slot));
                            if (prim) {
                                std::vector<EvalValue> args;
                                for (std::size_t i = 1; i < v.children.size(); ++i) {
                                    auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                    if (!ar)
                                        return ar;
                                    args.push_back(*ar);
                                }
                                // Issue #441 / #1357: call count + render latency.
                                return eval_env.owner()->invoke_prim_with_telemetry(
                                    primitives_.name_for_slot(slot),
                                    [&]() { return (*prim)(args); });
                            }
                        }
                    }
                    auto callee_name = std::string(p->resolve(callee.sym_id));
                    // Build diagnostic with appropriate suggestion (no self-move)
                    std::string suggestion;
                    if (callee_name.size() > 3 &&
                        callee_name.substr(callee_name.size() - 3) == "-fn")
                        suggestion = "if using c-func: (c-func -1 \"" +
                                     callee_name.substr(0, callee_name.size() - 3) +
                                     "\" \"(String) -> Int\")";
                    else
                        suggestion = "did you forget to define '" + callee_name + "'?";
                    return std::unexpected(
                        Diagnostic{ErrorKind::TypeError, "cannot call: " + callee_name}
                            .with_suggestion(std::move(suggestion)));
                }
                case aura::ast::NodeTag::IfExpr: {
                    if (v.children.size() < 2)
                        return EvalResult(make_void());
                    auto c = eval_flat(*f, *p, v.child(0), eval_env);
                    if (!c)
                        return c;
                    if (v.children.size() == 2) {
                        // (if cond then) — conditionally execute then-branch
                        if (is_truthy(*c)) {
                            current_id = v.child(1);
                            continue;
                        }
                        // Condition false, no else — use TCO to NULL_NODE so the
                        // while-loop guard returns void on next iteration (avoids
                        // a return path that can cause NULL_NODE in outer TCO loop
                        // when used inside rest-arg lambda bodies).
                        current_id = aura::ast::NULL_NODE;
                        continue;
                    }
                    current_id = is_truthy(*c) ? v.child(1) : v.child(2);
                    continue; // TCO: branch
                }
                case aura::ast::NodeTag::Lambda: {
                    // Capture params from FlatAST directly. Issue #145:
                    // store as SymId (SoA) — the apply_closure path now
                    // does integer-compare parameter binding instead of
                    // string-compare.
                    auto pspan = v.params;
                    std::vector<aura::ast::SymId> params;
                    params.reserve(pspan.size());
                    for (auto pid : pspan)
                        params.push_back(pid);
                    bool dotted = v.int_value != 0;
                    auto* target = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                    auto cid = next_id();
                    auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    // P0: legacy env pointer removed. Register captured env in SoA for this
                    // closure.
                    EnvId cap_id = alloc_env_frame_from_env(*current_env);
                    {
                        std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
                        Closure cl{"", std::move(params), f, p, body_id, cap_id, dotted, target};
                        // Issue #1365: stamp bridge_epoch at construction
                        stamp_closure_bridge_epoch(cl);
                        closures_[cid] = std::move(cl);
                    }
                    // Do NOT cache closure values — the closure captures the current env and a
                    // cached closure would reuse the same env on subsequent evaluations (wrong
                    // when the same Lambda node is evaluated with different captured variables).
                    return make_closure(cid);
                }
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec: {
                    bool rec = (v.tag == aura::ast::NodeTag::LetRec);
                    auto name = p->resolve(v.sym_id);
                    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
                    if (rec) {
                        // For letrec, the init value is evaluated in the new env (with cell
                        // binding)
                        tail_env.emplace(&eval_env);
                        tail_env->set_primitives(&primitives_);
                        // Issue #1482 restore: pool is required so bind_symid (and
                        // bind-with-pool) populate bindings_symid_ — the PRIMARY array
                        // copied by alloc_env_frame_from_env into the lambda capture.
                        // Without it, recursive names (e.g. fact in letrec fact) only
                        // land in string bindings_ and are dropped at capture time,
                        // yielding "unbound variable: fact" on recursive apply
                        // (gradual fact_5 / suite letrec).
                        if (p)
                            tail_env->set_pool(p);

                        // Issue #232 fix: register eval_env in env_frames_
                        // (always, not just when parent_id_ is NULL), then
                        // set tail_env's parent_id_ to eval_env's id. The
                        // materialized call env can then walk the SoA chain
                        // via lookup()'s parent_id_ fallback (added in #232
                        // commit 6e73ef2). The fix below is needed because
                        // even when eval_env.parent_id_ is non-NULL (e.g., a
                        // materialized call env has parent_id_ = top_'s id),
                        // the SoA walk needs to find the BINDINGS (e.g., 'n'
                        // from a let*), which live in eval_env but NOT in top_.
                        // Registering eval_env in env_frames_ makes those
                        // bindings visible to the SoA walk.
                        //
                        // Idempotency: skip if eval_env is already a frame
                        // (this would require tracking which envs are frames;
                        // for now, we always register, which is wasteful but
                        // correct).
                        if (eval_env.parent_id() == NULL_ENV_ID) {
                            EnvId eval_id = alloc_env_frame_from_env(eval_env);
                            const_cast<Env&>(eval_env).set_parent_id(eval_id);
                        } else {
                            // eval_env already has a parent_id_ (probably the
                            // top env). The SoA walk starts at this parent_id_
                            // which is top_ — but top_ doesn't have the let*'s
                            // bindings. Register eval_env as a NEW frame in
                            // env_frames_ (at the next index) and update its
                            // parent_id_ to the new id. The old parent_id_ is
                            // preserved on the eval_env's OWN frame.
                            EnvId new_id = alloc_env_frame_from_env(eval_env);
                            const_cast<Env&>(eval_env).set_parent_id(new_id);
                        }
                        tail_env->set_parent_id(eval_env.parent_id());

                        std::size_t ci = cells_.size();
                        cells_.push_back(make_void());
                        // SymId primary (captured by alloc_env_frame_from_env) +
                        // string mirror via bind_symid when pool_ is set. Parity
                        // with the Let path below.
                        if (v.sym_id != aura::ast::INVALID_SYM)
                            tail_env->bind_symid(v.sym_id, make_cell(ci));
                        else
                            tail_env->bind(std::string(name), make_cell(ci));
                        // Evaluate value in *tail_env (has cell binding for self-reference)
                        auto vv = eval_flat(*f, *p, val_id, *tail_env);
                        if (!vv)
                            return vv;
                        cells_[ci] = *vv;
                        // Body evaluated in *tail_env (recursive refs need the child env)
                        if (body_id != aura::ast::NULL_NODE)
                            return eval_flat(*f, *p, body_id, *tail_env);
                        return make_void();
                    } else {
                        // For let, bind directly to current eval_env (like define) to avoid
                        // creating a stack-local child env whose parent_ pointer becomes
                        // dangling when captured by a closure (bug: closure capture copies
                        // the env but parent_ still points to the original stack env).
                        auto vv = eval_flat(*f, *p, val_id, eval_env);
                        if (!vv)
                            return vv;

                        // ── Match exhaustiveness check (tree-walker path) ──
                        // At runtime we don't have static type info, so we resolve the
                        // target ADT by finding which ADT in the registry contains the
                        // first used constructor (e.g. `Cons` -> `List`). Bare-identifier
                        // candidates are only counted as used if they are real ctors of
                        // that ADT (so a variable binding doesn't false-positive).
                        if (!rec && type_registry_ && f->has_match_info(current_id)) {
                            auto* minfo = f->get_match_info(current_id);
                            if (minfo && !minfo->has_wildcard &&
                                (!minfo->used_constructors.empty() ||
                                 !minfo->candidate_constructors.empty())) {
                                auto& treg =
                                    *static_cast<aura::core::TypeRegistry*>(type_registry_);
                                // Find the target ADT by scanning for the first used_ctor
                                // or candidate_ctor (bare-id patterns).
                                const std::vector<std::string>* target_ctors = nullptr;
                                aura::core::TypeId target_tid{};
                                auto find_adt_for = [&](aura::ast::SymId sid) -> bool {
                                    auto cname = std::string(p->resolve(sid));
                                    for (std::size_t ti = 0; ti < treg.size(); ++ti) {
                                        auto tid =
                                            aura::core::TypeId{static_cast<std::uint32_t>(ti), 1};
                                        auto* c = treg.get_adt_constructors(tid);
                                        if (!c)
                                            continue;
                                        if (std::find(c->begin(), c->end(), cname) != c->end()) {
                                            target_ctors = c;
                                            target_tid = tid;
                                            return true;
                                        }
                                    }
                                    return false;
                                };
                                for (auto sid : minfo->used_constructors) {
                                    if (find_adt_for(sid))
                                        break;
                                }
                                if (!target_ctors) {
                                    for (auto sid : minfo->candidate_constructors) {
                                        if (find_adt_for(sid))
                                            break;
                                    }
                                }
                                if (target_ctors) {
                                    // Build effective used set
                                    std::vector<std::string> used_eff;
                                    used_eff.reserve(minfo->used_constructors.size() +
                                                     minfo->candidate_constructors.size());
                                    for (auto sid : minfo->used_constructors)
                                        used_eff.push_back(std::string(p->resolve(sid)));
                                    for (auto sid : minfo->candidate_constructors) {
                                        auto cname = std::string(p->resolve(sid));
                                        if (std::find(target_ctors->begin(), target_ctors->end(),
                                                      cname) != target_ctors->end())
                                            used_eff.push_back(std::move(cname));
                                    }
                                    for (auto& expected_ctor : *target_ctors) {
                                        if (std::find(used_eff.begin(), used_eff.end(),
                                                      expected_ctor) == used_eff.end()) {
                                            std::println(
                                                std::cerr,
                                                "match warning: unhandled constructor '{}' in {}",
                                                expected_ctor, treg.name_of(target_tid));
                                        }
                                    }
                                }
                            }
                        }

                        auto& me = const_cast<Env&>(eval_env);

                        auto ci = cells_.size();
                        cells_.push_back(*vv);
                        // Issue #1539: stamp Owned when init is Linear e.
                        // State lives on Env.bindings_linear_ownership_state_ and is
                        // copied into EnvFrame by alloc_env_frame_from_env (closure capture).
                        std::uint8_t lin_state = linear_rt::Untracked;
                        if (val_id != aura::ast::NULL_NODE && val_id < f->size() &&
                            f->get(val_id).tag == aura::ast::NodeTag::Linear) {
                            lin_state = linear_rt::Owned;
                        }
                        me.set_pool(p);
                        // bind_symid_with_linear_state mirrors into string bindings_
                        // when pool_ is set. Do NOT also call bind() — that would
                        // dual-write a second Untracked entry and shadow Owned
                        // linear state for (let ((x (Linear ...))) ...).
                        me.bind_symid_with_linear_state(v.sym_id, make_cell(ci), lin_state);
                        if (body_id != aura::ast::NULL_NODE)
                            return eval_flat(*f, *p, body_id, eval_env);
                        return make_void();
                    }
                }
                case aura::ast::NodeTag::DefineType: {
                    // (define-type (Name params...) (Ctor fields...) ...)
                    // Bind each constructor by evaluating an Aura lambda:
                    //   (define <Ctor> (lambda args (cons 'Ctor args)))
                    // This avoids C++ complexity and works with existing pair infrastructure.
                    auto type_name = p->resolve(v.sym_id);
                    Env& me = const_cast<Env&>(eval_env);


                    for (auto cid : v.children) {
                        if (cid >= f->size())
                            continue;
                        auto cv = f->get(cid);
                        if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty())
                            continue;
                        auto quoted = cv.child(0);
                        if (quoted >= f->size())
                            continue;
                        auto qv = f->get(quoted);
                        // Constructor data is now (cons 'ctor-name (cons 'ft1 (cons 'ft2 ...)))
                        // Extract the constructor name from the head of the list
                        std::string ctor_name;
                        aura::ast::NodeId current = quoted;
                        auto cur_v = f->get(current);
                        if (cur_v.tag == aura::ast::NodeTag::Pair) {
                            auto car_id = cur_v.child(0);
                            if (car_id < f->size()) {
                                auto car_v = f->get(car_id);
                                if (car_v.tag == aura::ast::NodeTag::Variable)
                                    ctor_name = std::string(p->resolve(car_v.sym_id));
                            }
                        }
                        if (ctor_name.empty())
                            continue;

                        // Register constructor as a primitive that creates tagged lists:
                        // (Ctor arg1 arg2 ...) → (cons 'Ctor (cons arg1 (cons arg2 ...)))
                        auto tag_slot = string_heap_.size();
                        string_heap_.push_back(ctor_name);
                        auto tag_str = make_string(tag_slot);

                        // Count fields to determine if zero-arg constructor
                        int field_count = 0;
                        {
                            aura::ast::NodeId fields_node = quoted;
                            auto fv = f->get(fields_node);
                            if (fv.tag == aura::ast::NodeTag::Pair && fv.children.size() >= 2)
                                fields_node = fv.child(1);
                            while (fields_node < f->size()) {
                                auto fnv = f->get(fields_node);
                                if (fnv.tag != aura::ast::NodeTag::Pair || fnv.children.empty())
                                    break;
                                field_count++;
                                if (fnv.children.size() >= 2)
                                    fields_node = fnv.child(1);
                                else
                                    break;
                            }
                        }
                        if (field_count == 0) {
                            me.bind(ctor_name, make_adt_zero_arg_ctor(tag_str));
                        } else {
                            register_adt_ctor(ctor_name, tag_str, field_count);
                        }
                    }
                    return EvalResult(make_void());
                }

                case aura::ast::NodeTag::Define: {
                    auto name = p->resolve(v.sym_id);
                    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    Env& me = const_cast<Env&>(eval_env);
                    // Issue #1482 restore: pool so bind dual-writes bindings_symid_
                    // (closure capture PRIMARY) + string mirror for set!/lookup.
                    if (p)
                        me.set_pool(p);

                    // Check if already bound as a cell — update existing cell to maintain
                    // sequential define chains across multiple eval calls
                    // Use lookup_binding to get the raw cell sentinel (not dereferenced value)
                    auto existing = eval_env.lookup_binding(std::string(name));
                    if (!existing && v.sym_id != aura::ast::INVALID_SYM) {
                        // SymId path: re-define after capture/materialize may only
                        // have the cell on bindings_symid_.
                        if (auto by_sym = eval_env.lookup_by_symid(v.sym_id);
                            by_sym && is_cell(*by_sym))
                            existing = by_sym;
                    }
                    if (existing && is_cell(*existing)) {
                        auto ci = as_cell_id(*existing);
                        auto vv = eval_flat(*f, *p, val_id, eval_env);
                        if (!vv)
                            return vv;
                        cells_[ci] = *vv;
                        return *vv;
                    }

                    // Create new cell binding (SymId primary when available)
                    auto ci = alloc_cell(make_void());
                    if (v.sym_id != aura::ast::INVALID_SYM)
                        me.bind_symid(v.sym_id, make_cell(ci));
                    else
                        me.bind(std::string(name), make_cell(ci));
                    auto vv = eval_flat(*f, *p, val_id, eval_env);
                    if (!vv)
                        return vv;
                    cells_[ci] = *vv;
                    return *vv;
                }
                case aura::ast::NodeTag::Begin: {
                    auto count = v.children.size();
                    if (count == 0)
                        return EvalResult(make_void());

                    // Check if there are multiple define nodes → use letrec semantics
                    // Phase 1: pre-allocate cells for all defines
                    std::vector<std::pair<std::string, aura::ast::NodeId>> letrec_defs;
                    bool has_multiple_defs = false;
                    int define_count = 0;
                    // Find last non-NULL child (NULL_NODE holes may exist from mutate:move-node)
                    aura::ast::NodeId last_expr = aura::ast::NULL_NODE;
                    for (std::size_t si = count; si > 0; --si) {
                        auto cid = v.child(si - 1);
                        if (cid != aura::ast::NULL_NODE) {
                            last_expr = cid;
                            break;
                        }
                    }
                    if (last_expr == aura::ast::NULL_NODE)
                        return EvalResult(make_void());
                    for (std::size_t i = 0; i < count; ++i) {
                        auto cid = v.child(i);
                        if (cid == aura::ast::NULL_NODE)
                            continue;
                        auto child_node = f->get(cid);
                        if (child_node.tag == aura::ast::NodeTag::Define) {
                            define_count++;
                            if (define_count > 1)
                                has_multiple_defs = true;
                            letrec_defs.push_back({std::string(p->resolve(child_node.sym_id)),
                                                   child_node.children.empty()
                                                       ? aura::ast::NULL_NODE
                                                       : child_node.child(0)});
                        }
                    }

                    // Skip NULL_NODE children (left by mutate:move-node / mutate:remove-node)
                    std::size_t effective_count = 0;
                    for (std::size_t ci = 0; ci < count; ++ci) {
                        if (v.child(ci) != aura::ast::NULL_NODE)
                            effective_count++;
                    }
                    if (effective_count < count) {
                        // Count again for the main loop using only original children
                        // We'll check each child in the loop below
                        has_multiple_defs = false;
                        define_count = 0;
                        for (std::size_t ci = 0; ci < count; ++ci) {
                            auto cid = v.child(ci);
                            if (cid == aura::ast::NULL_NODE)
                                continue;
                            auto child_node = f->get(cid);
                            if (child_node.tag == aura::ast::NodeTag::Define) {
                                define_count++;
                                if (define_count > 1)
                                    has_multiple_defs = true;
                            }
                        }
                    }

                    if (has_multiple_defs) {
                        // Phase 1: pre-allocate cells for all defines
                        // This ensures all function names are visible to each other
                        std::vector<std::size_t> cell_ids;
                        {
                            auto& mutable_env = const_cast<Env&>(eval_env);
                            // Issue #1482 restore: pool so multi-define cells land on
                            // bindings_symid_ and survive lambda capture (nested
                            // recursive helpers like seen? in std/ast-viz).
                            if (p)
                                mutable_env.set_pool(p);

                            for (auto& d : letrec_defs) {
                                auto ci = alloc_cell(make_void());
                                mutable_env.bind(d.first, make_cell(ci));
                                cell_ids.push_back(ci);
                            }
                        }
                        // Phase 2: evaluate values and set cells
                        for (std::size_t i = 0; i < letrec_defs.size(); ++i) {
                            auto& d = letrec_defs[i];
                            if (d.second != aura::ast::NULL_NODE) {
                                auto val = eval_flat(*f, *p, d.second, eval_env);
                                if (!val)
                                    return val;
                                cells_[cell_ids[i]] = *val;
                            }
                        }
                        // Phase 3: evaluate remaining (non-define) expressions
                        for (std::size_t i = 0; i < count - 1; ++i) {
                            auto cid = v.child(i);
                            if (cid == aura::ast::NULL_NODE)
                                continue;
                            auto child_node = f->get(cid);
                            if (child_node.tag == aura::ast::NodeTag::Define)
                                continue;
                            auto r = eval_flat(*f, *p, cid, eval_env);
                            if (!r)
                                return r;
                        }
                        // TCO: last expression
                        current_id = last_expr;
                        continue;
                    }

                    // Single define (or no defines) — sequential evaluation
                    for (std::size_t i = 0; i < count - 1; ++i) {
                        auto cid = v.child(i);
                        if (cid == aura::ast::NULL_NODE)
                            continue;
                        auto r = eval_flat(*f, *p, cid, eval_env);
                        if (!r)
                            return r;
                    }
                    // Find last non-NULL child
                    current_id = aura::ast::NULL_NODE;
                    for (std::size_t i = count; i > 0; --i) {
                        auto cid = v.child(i - 1);
                        if (cid != aura::ast::NULL_NODE) {
                            current_id = cid;
                            break;
                        }
                    }
                    if (current_id == aura::ast::NULL_NODE)
                        return EvalResult(make_void());
                    continue; // TCO: last expression in begin
                }
                case aura::ast::NodeTag::DefineModule: {
                    // (define-module (Name :T ...) body...)
                    // Store module template and bind Name to functor
                    auto mod_name = std::string(p->resolve(v.sym_id));
                    ModuleTemplate mt;

                    // Extract type parameter names from AST params metadata
                    // (type params = all params minus cap params)
                    auto num_cap_params = f->cap_require_count(v.id);
                    std::size_t num_type_params =
                        (num_cap_params > 0 && v.params.size() >= num_cap_params)
                            ? v.params.size() - num_cap_params
                            : v.params.size();
                    for (std::size_t i = 0; i < num_type_params; ++i) {
                        auto pid = f->param_at(v.id, i);
                        mt.type_param_names.push_back(std::string(p->resolve(pid)));
                    }
                    if (num_cap_params > 0) {
                        // cap params are at the end of the param list
                        for (std::size_t i = 0; i < num_cap_params; ++i) {
                            auto pid = f->param_at(v.id, v.params.size() - num_cap_params + i);
                            mt.cap_param_names.push_back(std::string(p->resolve(pid)));
                        }
                    }

                    // Serialize body expressions to source strings (for cross-eval instantiation)
                    // Build a node-to-source serializer using the current FlatAST
                    std::function<std::string(aura::ast::NodeId)> node_source;
                    node_source = [&](aura::ast::NodeId nid) -> std::string {
                        if (nid >= f->size() || nid == aura::ast::NULL_NODE)
                            return "";
                        auto nv = f->get(nid);
                        switch (nv.tag) {
                            case aura::ast::NodeTag::LiteralInt:
                                return std::to_string(nv.int_value);
                            case aura::ast::NodeTag::LiteralFloat:
                                return std::to_string(nv.float_value);
                            case aura::ast::NodeTag::LiteralString:
                                return "\"" + std::string(p->resolve(nv.sym_id)) + "\"";
                            case aura::ast::NodeTag::Variable:
                                return std::string(p->resolve(nv.sym_id));
                            case aura::ast::NodeTag::Quote: {
                                if (nv.children.empty())
                                    return "'()";
                                return "'" + node_source(nv.child(0));
                            }
                            case aura::ast::NodeTag::Lambda: {
                                std::string s = "(lambda (";
                                for (std::size_t pi = 0; pi < nv.params.size(); ++pi) {
                                    if (pi > 0)
                                        s += " ";
                                    s += std::string(p->resolve(nv.params[pi]));
                                }
                                s += ")";
                                if (!nv.children.empty())
                                    s += " " + node_source(nv.child(0));
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Define: {
                                std::string s = "(define";
                                if (!nv.children.empty()) {
                                    auto val_nv = f->get(nv.child(0));
                                    if (val_nv.tag == aura::ast::NodeTag::Lambda) {
                                        // Shorthand: (define (name params...) body...)
                                        s += " (" + std::string(p->resolve(nv.sym_id));
                                        for (std::size_t pi = 0; pi < val_nv.params.size(); ++pi) {
                                            s += " ";
                                            s += std::string(p->resolve(val_nv.params[pi]));
                                        }
                                        s += ")";
                                        if (!val_nv.children.empty())
                                            s += " " + node_source(val_nv.child(0));
                                    } else {
                                        s += " " + std::string(p->resolve(nv.sym_id));
                                        s += " " + node_source(nv.child(0));
                                    }
                                }
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Export: {
                                std::string s = "(export";
                                for (auto eid : nv.children) {
                                    auto ev = f->get(eid);
                                    if (ev.tag == aura::ast::NodeTag::Variable)
                                        s += " " + std::string(p->resolve(ev.sym_id));
                                }
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Call: {
                                std::string s = "(";
                                for (std::size_t ci = 0; ci < nv.children.size(); ++ci) {
                                    if (ci > 0)
                                        s += " ";
                                    s += node_source(nv.child(ci));
                                }
                                return s + ")";
                            }
                            default:
                                return "()";
                        }
                    };

                    // Serialize each body expression
                    std::string body_src;
                    for (auto cid : v.children) {
                        auto sexpr = node_source(cid);
                        if (!sexpr.empty()) {
                            if (!body_src.empty())
                                body_src += "\n";
                            body_src += sexpr;
                        }
                    }
                    mt.body_source = std::move(body_src);

                    // Scan body for `:require` directives
                    // Format: ((:require FileRead FileWrite) ...) or ((:require FileRead) ...)
                    for (auto cid : v.children) {
                        auto cv = f->get(cid);
                        if (cv.tag == aura::ast::NodeTag::Call && cv.children.size() > 0) {
                            auto callee_node = f->get(cv.child(0));
                            if (callee_node.tag == aura::ast::NodeTag::Variable ||
                                callee_node.tag == aura::ast::NodeTag::Quote) {
                                aura::ast::SymId sym =
                                    (callee_node.tag == aura::ast::NodeTag::Variable)
                                        ? callee_node.sym_id
                                        : aura::ast::INVALID_SYM;
                                std::string_view callee_name = (sym != aura::ast::INVALID_SYM)
                                                                   ? p->resolve(sym)
                                                                   : std::string_view();
                                // Check for :require or require keyword
                                if (callee_name == ":require" || callee_name == ":require-all") {
                                    // Extract required capability names from remaining children
                                    for (std::size_t ai = 1; ai < cv.children.size(); ++ai) {
                                        auto arg_node = f->get(cv.child(ai));
                                        if (arg_node.tag == aura::ast::NodeTag::Variable) {
                                            auto cap_name =
                                                std::string(p->resolve(arg_node.sym_id));
                                            // Skip duplicates
                                            bool dup = false;
                                            for (auto& r : mt.cap_require) {
                                                if (r == cap_name) {
                                                    dup = true;
                                                    break;
                                                }
                                            }
                                            if (!dup)
                                                mt.cap_require.push_back(cap_name);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    module_templates_[mod_name] = std::move(mt);

                    // Bind Name in the current env (as a cell with functor marker)
                    Env& me = const_cast<Env&>(eval_env);

                    auto ci = alloc_cell(make_void());
                    auto sidx = string_heap_.size();
                    string_heap_.push_back("%functor");
                    me.bind(mod_name, make_cell(ci));
                    cells_[ci] = make_string(sidx);
                    return make_string(sidx);
                }
                case aura::ast::NodeTag::Export: {
                    // (export sym ...) — record module API during loading
                    // No runtime effect; children are Variable nodes
                    if (!current_export_set_) {
                        current_export_set_ = std::make_unique<std::unordered_set<std::string>>();
                    }
                    for (auto cid : v.children) {
                        auto cv = f->get(cid);
                        if (cv.tag == aura::ast::NodeTag::Variable) {
                            current_export_set_->insert(std::string(p->resolve(cv.sym_id)));
                        }
                    }
                    return types::make_void();
                }
                case aura::ast::NodeTag::Set: {
                    auto name = p->resolve(v.sym_id);
                    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    auto val = eval_flat(*f, *p, val_id, eval_env);
                    if (!val)
                        return val;
                    // Use stable index instead of pointer (cells_ may reallocate)
                    auto cell_idx = eval_env.lookup_cell_index(std::string(name));
                    if (cell_idx) {
                        if (*cell_idx < cells_.size())
                            cells_[*cell_idx] = *val;
                        return *val;
                    }
                    // Fallback 1: direct binding in current env
                    for (auto& b : const_cast<Env&>(eval_env).bindings()) {
                        if (b.first == name) {
                            b.second = *val;
                            return *val;
                        }
                    }
                    // Fallback 2: scan parent envs for direct (non-cell) bindings
                    {
                        const Env* e = eval_env.parent();
                        while (e) {
                            for (auto& b : const_cast<Env&>(*e).bindings()) {
                                if (b.first == name) {
                                    b.second = *val;
                                    return *val;
                                }
                            }
                            e = e->parent();
                        }
                    }
                    // Suggest closest bound variables
                    {
                        std::vector<std::string> candidates;
                        {
                            const Env* e = &eval_env;
                            while (e) {
                                for (auto& b : const_cast<Env&>(*e).bindings())
                                    candidates.push_back(b.first);
                                e = e->parent();
                            }
                        }
                        auto best = closest_match(name, candidates);
                        // Issue #79: source location from the offending node.
                        Diagnostic d(ErrorKind::UnboundVariable, "set!: " + std::string(name),
                                     aura::diag::SourceLocation{v.line, v.col, 0}, current_id);
                        if (!best.empty())
                            d.with_suggestion("did you mean '" + best + "'?");
                        return std::unexpected(std::move(d));
                    }
                    return std::unexpected(
                        Diagnostic{ErrorKind::UnboundVariable, "set!: " + std::string(name),
                                   aura::diag::SourceLocation{v.line, v.col, 0}, current_id});
                }
                case aura::ast::NodeTag::Quote: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    // Issue #1137: quoted empty list is often LiteralInt(0) in the AST
                    // (parser uses 0 as the empty-list sentinel). Map that to void so
                    // (equal? 0 '()) / (eq? 0 '()) can distinguish fixnum 0 from nil.
                    {
                        auto qv = f->get(v.child(0));
                        if (qv.tag == aura::ast::NodeTag::LiteralInt && qv.int_value == 0 &&
                            qv.marker != aura::ast::SyntaxMarker::BoolLiteral)
                            return EvalResult(make_void());
                    }
                    return EvalResult(ast_to_data(*f, *p, v.child(0)));
                }
                case aura::ast::NodeTag::TypeAnnotation: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    auto annot_id = current_id;
                    auto child_result = eval_flat(*f, *p, v.child(0), eval_env);
                    if (!child_result)
                        return child_result;
                    // 3-arg form (: name Type val): bind the result in eval_env
                    if (v.int_value != 0) {
                        auto var_name = p->resolve(static_cast<aura::ast::SymId>(v.int_value));
                        if (!var_name.empty()) {
                            auto& me = const_cast<Env&>(eval_env);

                            auto ci = cells_.size();
                            cells_.push_back(*child_result);
                            me.bind(std::string(var_name), make_cell(ci));
                        }
                    }
                    // Runtime type check: compare value type against annotation
                    if (type_registry_ && annot_id < f->size()) {
                        auto expected_type_id = f->type_id(annot_id);
                        if (expected_type_id != 0) {
                            auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
                            auto expected_tag =
                                treg.tag_of(aura::core::TypeId{expected_type_id, 1});
                            auto actual_tag = runtime_type_tag(*child_result);
                            if (actual_tag != expected_tag &&
                                actual_tag != aura::core::TypeTag::DYNAMIC) {
                                auto& val = *child_result;
                                // Attempt coercion at runtime
                                bool coerced =
                                    coerce_value(val, actual_tag, expected_tag, string_heap_);
                                if (!coerced) {
                                    std::string expected_name(
                                        treg.format_type(aura::core::TypeId{expected_type_id, 1}));
                                    std::string actual_name = type_tag_name(actual_tag);
                                    std::println(std::cerr, "type warning: expected {}, got {}\n",
                                                 expected_name, actual_name);
                                }
                            }
                        }
                    }
                    return child_result;
                }
                case aura::ast::NodeTag::MacroDef: {
                    auto name = p->resolve(v.sym_id);
                    std::vector<std::string> param_names;
                    for (auto pn : v.params)
                        param_names.push_back(std::string(p->resolve(pn)));
                    auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    if (body_id == aura::ast::NULL_NODE)
                        return EvalResult(make_void());

                    // ── Warn: unused macro parameters ──────────────────────────
                    // Scan the body for variable references and compare with params.
                    {
                        // Collect all variable names referenced in the macro body
                        std::unordered_set<std::string> used_vars;
                        auto collect_vars = [&](this const auto& self,
                                                aura::ast::NodeId nid) -> void {
                            if (nid == aura::ast::NULL_NODE || nid >= f->size())
                                return;
                            auto nv = f->get(nid);
                            if (nv.tag == aura::ast::NodeTag::Variable &&
                                nv.sym_id != aura::ast::INVALID_SYM) {
                                used_vars.insert(std::string(p->resolve(nv.sym_id)));
                            }
                            for (auto c : nv.children)
                                self(c);
                        };
                        collect_vars(body_id);

                        int used_count = 0;
                        for (auto& pn : param_names) {
                            if (used_vars.count(pn) == 0) {
                                std::println(std::cerr,
                                             "warning: macro '{}': parameter '{}' never used",
                                             std::string(name), pn);
                            } else {
                                ++used_count;
                            }
                        }
                        if (used_count == 0) {
                            std::println(
                                std::cerr,
                                "warning: macro '{}': body does not reference any parameter",
                                std::string(name));
                        }
                    }

                    // Store macro definition with proper dotted flag
                    // Issue #120: dotted is bit 0, hygienic is bit 1 of
                    // int_val_ (encoded by add_macrodef in parser_impl.cpp).
                    bool is_dotted = (v.int_value & 1) != 0;
                    bool is_hygienic = (v.int_value & 2) != 0;
                    // Issue #230 #2: bit 2 of int_val_ flags the
                    // `define-hygienic-macro*` (preserved-params) variant.
                    bool is_preserved = (v.int_value & 4) != 0;
                    macros_[std::string(name)] = MacroDef{std::move(param_names),
                                                          is_dotted,
                                                          is_hygienic,
                                                          is_preserved,
                                                          f,
                                                          p,
                                                          body_id};
                    return EvalResult(make_void());
                }
                case aura::ast::NodeTag::Linear: {
                    // Issue #1539: Linear e evaluates the payload; binding
                    // sites (Let) stamp Owned via bind_with_linear_state.
                    if (v.children.empty())
                        return EvalResult(make_void());
                    return eval_flat(*f, *p, v.child(0), eval_env);
                }
                case aura::ast::NodeTag::Move: {
                    // Issue #1539: after reading the value, mark the source
                    // binding as Moved so linear_post_mutate_enforce can
                    // intercept subsequent use-after-move.
                    if (v.children.empty())
                        return EvalResult(make_void());
                    const auto inner = v.child(0);
                    auto result = eval_flat(*f, *p, inner, eval_env);
                    if (!result)
                        return result;
                    if (inner != aura::ast::NULL_NODE && inner < f->size() &&
                        f->get(inner).tag == aura::ast::NodeTag::Variable) {
                        (void)mark_linear_binding_moved(const_cast<Env&>(eval_env),
                                                        f->get(inner).sym_id);
                    }
                    return result;
                }
                case aura::ast::NodeTag::Borrow:
                case aura::ast::NodeTag::MutBorrow:
                case aura::ast::NodeTag::Drop: {
                    // Borrow/Drop: evaluate inner; Borrow may stamp Borrowed
                    // in a follow-up. Drop leaves Moved stamping to Move.
                    if (v.children.empty())
                        return EvalResult(make_void());
                    return eval_flat(*f, *p, v.child(0), eval_env);
                }
                default:
                    return std::unexpected(
                        Diagnostic{ErrorKind::InternalError, "eval_flat: unsupported node type"});
            }
        }
    } catch (const std::bad_alloc& e) {
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "out of memory"});
    } catch (const std::out_of_range& e) {
        return std::unexpected(Diagnostic{ErrorKind::InternalError,
                                          std::format("argument out of range: {}", e.what())});
    } catch (const std::bad_variant_access& e) {
        return std::unexpected(Diagnostic{
            ErrorKind::TypeError,
            std::format("type mismatch (wrong argument type passed to primitive): {}", e.what())});
    }
}


// Issue #165 Phase 1B: post-mutation macro re-expansion.
//
// Walks the mutation's affected subtree and re-expands any
// macro call sites it finds. This fixes the bug where
// EDSL mutations (mutate:rebind, mutate:set-body, etc.)
// leave stale macro expansions — the macro's gensym'd
// bindings may not be re-generated, and the call site
// may pick up caller's bindings that should have been
// hygiene-isolated.
//
// Algorithm (incremental — only on affected subtrees, not
// the full AST):
//   1. Compute the affected subtree using the same
//      walk pattern as affected_subtree_from_mutation
//      (Issue #148): descendants of target_node/parent_id
//      + dirty-upward ancestors.
//   2. For each node in the affected set, check if it's:
//        (a) A MacroDef — the macro body was mutated. Find
//            every Call site whose callee is this macro and
//            re-expand them.
//        (b) A Call whose callee is a known macro — the
//            call site context was mutated. Re-expand it.
//   3. For each call site, build a substitution (param →
//      arg), call clone_macro_body with fresh gensym (or
//      without, for non-hygienic macros), then run
//      expand_inner_macros on the result to handle nested
//      macros.
//   4. Set SyntaxMarker::MacroIntroduced on the new
//      expansion so the post-expansion tree is properly
//      marked for downstream consumers (type checker, IR
//      lowering, mutation operators).
//
// Returns the number of call sites re-expanded. The
// function is safe to call on any mutation record —
// bails on malformed input (NULL_NODE, out-of-range,
// empty macros_ registry).
std::size_t Evaluator::post_mutation_macro_reexpand(aura::ast::FlatAST& flat,
                                                    aura::ast::StringPool& pool,
                                                    const aura::ast::MutationRecord& rec) {
    using namespace aura::ast;

    std::size_t re_expanded = 0;
    if (rec.target_node == NULL_NODE && rec.parent_id == NULL_NODE)
        return 0;
    if (macros_.empty())
        return 0; // no macros registered, nothing to do

    // Collect affected node IDs: descendants of target_node
    // + parent_id + dirty-upward chain. This is a conservative
    // set — we may visit nodes that aren't actually affected
    // by the macro, but the re-expansion is idempotent
    // (re-expanding an already-expanded call site is a no-op
    // in effect, just reuses the same gensym).
    std::vector<NodeId> affected;
    auto add_subtree = [&](NodeId root_id) {
        if (root_id == NULL_NODE || root_id >= flat.size())
            return;
        affected.push_back(root_id);
        // BFS for descendants
        std::vector<NodeId> frontier{root_id};
        while (!frontier.empty()) {
            std::vector<NodeId> next;
            for (auto n : frontier) {
                auto v = flat.get(n);
                for (auto c : v.children) {
                    if (c != NULL_NODE && c < flat.size()) {
                        affected.push_back(c);
                        next.push_back(c);
                    }
                }
            }
            frontier = std::move(next);
        }
    };
    add_subtree(rec.target_node);
    add_subtree(rec.parent_id);

    // Climb parent_of chain for dirty-upward ancestors.
    // Safety-bounded to defend against cycles in malformed
    // FlatASTs.
    NodeId climb = rec.target_node;
    for (int i = 0; i < 256 && climb != NULL_NODE && climb < flat.size(); ++i) {
        if (auto p = flat.parent_of(climb); p != NULL_NODE && p < flat.size()) {
            affected.push_back(p);
            climb = p;
        } else {
            break;
        }
    }

    // Walk the affected set, find Call nodes whose callee is
    // a registered macro, and re-expand them.
    for (auto id : affected) {
        if (id == NULL_NODE || id >= flat.size())
            continue;
        auto v = flat.get(id);
        if (v.tag != NodeTag::Call || v.children.empty())
            continue;

        auto callee_id = v.child(0);
        if (callee_id == NULL_NODE || callee_id >= flat.size())
            continue;
        auto callee_v = flat.get(callee_id);
        if (!callee_v.has_name())
            continue;
        auto callee_name = pool.resolve(callee_v.sym_id);

        // Is the callee a registered macro?
        auto macro_it = macros_.find(std::string(callee_name));
        if (macro_it == macros_.end())
            continue;

        const auto& md = macro_it->second;

        // Build substitution: macro param names → call arg node IDs
        std::vector<aura::ast::NodeId> call_args;
        for (std::size_t i = 1; i < v.children.size(); ++i) {
            call_args.push_back(v.child(i));
        }
        auto subst_view = std::span<const aura::ast::NodeId>(call_args);

        // Compute substitution: param string → call arg node id.
        // For each param name, find the corresponding call arg
        // by position. If params has dotted-rest, the last param
        // binds to a list of remaining args.
        std::unordered_map<std::string, aura::ast::NodeId> subst_map;
        std::unordered_map<std::string, std::string> rename_map;
        for (std::size_t i = 0; i < md.params.size() && i < call_args.size(); ++i) {
            subst_map[md.params[i]] = call_args[i];
        }
        // Handle dotted-rest param (the last param absorbs remaining args)
        if (md.dotted && !md.params.empty() && call_args.size() >= md.params.size()) {
            // Build a proper-list from the remaining args by
            // consing them with add_pair. Build right-to-left
            // so we get (a . (b . (c . nil))).
            std::size_t first_rest_idx = md.params.size() - 1;
            aura::ast::NodeId list_end = aura::ast::NULL_NODE;
            for (std::size_t k = call_args.size(); k > first_rest_idx; --k) {
                std::size_t i = k - 1;
                list_end = flat.add_pair(call_args[i], list_end);
            }
            subst_map[md.params.back()] = list_end;
        }

        // Clone the macro body into the calling flat with
        // substitution + (for hygienic) name rename map.
        // For non-hygienic macros, the name_map is empty (no
        // gensym) and the params bind to call-site names (legacy
        // defmacro behavior).
        // Issue #190: pass MacroIntroduced so the cloned nodes
        // get the correct SyntaxMarker at creation time. (The
        // post-clone BFS marker-set that used to be here is now
        // redundant — clone_macro_body handles it.)
        auto* src_pool = md.pool ? md.pool : &pool;
        auto* src_flat = md.flat ? md.flat : &flat;
        auto expanded =
            clone_macro_body(flat, pool, *src_flat, *src_pool, md.body_id, &subst_map, &rename_map,
                             /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
        if (expanded == NULL_NODE)
            continue;

        // Recursively expand any nested macro calls in the
        // cloned body. Bounded by depth=10 to prevent infinite
        // loops (a macro that expands to itself).
        expanded =
            expand_inner_macros(&flat, &pool, expanded, 0, 10, as_expansion_registry(macros_));

        ++re_expanded;
    }

    return re_expanded;
}

// Issue #488 / #1611: post-mutate reflect validation hook for Guard success path.
// Integrates reflect.hh hygiene-aware MutationReflectHealth (default rejects
// MacroIntroduced schema evolution without allow_macro_mutate).
bool Evaluator::post_mutation_reflect_validate() const noexcept {
    bump_guard_panic_reflect_validate_hook();
    auto* ws = workspace_flat_;
    if (!ws || ws->size() == 0) {
        bump_schema_validation_fail_count();
        return false;
    }
    using aura::ast::FlatAST;
    using aura::ast::NodeId;

    // Issue #1611: use reflect.hh MutationReflectHealth as the
    // authoritative post-mutate hygiene probe.
    aura::reflect::MutationReflectHealth health;
    health.allow_macro_evolution = get_allow_macro_mutate();
    // Soft default: expansion dirtiness alone does not hard-fail
    // (preserve self-evo after hygienic expand). Hard reject when
    // marker consistency fails OR enforce flag is set by agent.
    health.enforce_macro_hygiene_reject = false;
    if (ws->root >= ws->size()) {
        bump_schema_validation_fail_count();
        return false;
    }
    health.generation_healthy = ws->generation_wrap_count() < 1'000'000;
    constexpr auto kExpansion =
        static_cast<std::uint8_t>(FlatAST::MacroDirtyReason::kMacroExpansion);
    // Issue #712: subtree-level reflect validation. After the
    // whole-workspace marker-consistency scan, walk MacroIntroduced
    // nodes specifically and tally three diagnostics. The walk
    // happens unconditionally (no early-out) so the counters
    // reflect the actual post-mutate state the Agent sees.
    bool called_macro_validate = false;
    std::uint64_t macro_dirty = 0;
    std::uint64_t macro_marker_mismatches = 0;
    for (NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_dirty(id))
            ++health.dirty_nodes;
        if (ws->is_macro_introduced(id)) {
            ++health.macro_markers;
            if ((ws->macro_dirty(id) & kExpansion) == 0) {
                health.marker_consistent = false;
                ++macro_marker_mismatches;
            }
            // Subtree-level: count MacroIntroduced nodes whose
            // dirty bit is set in the latest post-mutate snapshot.
            // That means: macro subtree was mutated between the
            // previous committed snapshot and now (post-mutate
            // hygiene drift). The Agent can query this counter to
            // decide whether to run a deep schema validate on the
            // affected subtree before trusting it.
            if (ws->is_dirty(id) && (ws->macro_dirty(id) & kExpansion) != 0)
                ++macro_dirty;
        }
        const auto parent = ws->parent_of(id);
        if (parent != aura::ast::NULL_NODE && parent >= ws->size())
            health.marker_consistent = false;
    }
    health.dirty_macro_nodes = macro_dirty;
    // Issue #1611: hard-reject when MacroIntroduced markers lack
    // expansion provenance (true hygiene leak) without allow flag —
    // enforce via reflect.hh policy (marker_consistent already false).
    if (macro_marker_mismatches > 0 && !health.allow_macro_evolution)
        health.enforce_macro_hygiene_reject = true;
    // Subtree-level auto_validate call count: one bump per
    // post_mutation_reflect_validate() invocation that found at
    // least one MacroIntroduced node (i.e., a macro subtree was
    // walked for reflect validation this mutation cycle).
    if (health.macro_markers > 0) {
        called_macro_validate = true;
        bump_macro_reflect_validation_calls();
    }
    if (macro_marker_mismatches > 0)
        bump_macro_reflect_schema_mismatches_caught();
    if (macro_dirty > 0)
        bump_macro_reflect_post_mutate_hygiene_drift();
    (void)called_macro_validate;
    set_dirty_nodes_in_snapshot(health.dirty_nodes);
    set_macro_markers_in_snapshot(health.macro_markers);

    // Issue #1611: single path through reflect.hh hygiene gate.
    std::string reflect_err;
    const bool ok = aura::reflect::validate_mutation_reflect_health(health, &reflect_err);
    // Also count MacroIntroduced context checks for observability.
    if (health.macro_markers > 0) {
        if (auto* m =
                compiler_metrics_ ? static_cast<CompilerMetrics*>(compiler_metrics_) : nullptr) {
            m->reflect_macro_hygiene_checks_total.fetch_add(1, std::memory_order_relaxed);
            if (!ok && !health.allow_macro_evolution)
                m->reflect_macro_hygiene_rejects_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    set_last_schema_validation_ok(ok);
    if (ok) {
        bump_schema_validation_pass_count();
        if (health.macro_markers > 0)
            bump_macro_reflect_hygiene_validation();
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflection_schema_validated_total.fetch_add(1, std::memory_order_relaxed);
            if (health.macro_markers > 0)
                m->reflection_macro_provenance_held_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #1246 Phase 1: runtime_reflect_bridge under Guard —
            // auto_validate mutated macro bodies / EDSL structs after mutate.
            m->runtime_reflect_bridge_guard.store(1, std::memory_order_relaxed);
            if (health.macro_markers > 0 || health.dirty_nodes > 0) {
                m->runtime_reflect_mutated_schema_checks.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else {
        bump_schema_validation_fail_count();
        if (compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
            m->reflection_schema_violations_total.fetch_add(1, std::memory_order_relaxed);
            if (!health.generation_healthy)
                m->reflection_stale_validation_prevented_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
            m->runtime_reflect_bridge_guard.store(1, std::memory_order_relaxed);
            m->runtime_reflect_mutated_schema_checks.fetch_add(1, std::memory_order_relaxed);
        }
    }
    (void)reflect_err;
    return ok;
}

std::int64_t Evaluator::capture_workspace_snapshot_under_lock(std::string_view name) noexcept {
    if (!workspace_flat_ || !workspace_pool_)
        return -1;
    auto src_fn = primitives_.lookup("current-source");
    if (!src_fn)
        return -1;
    auto src = (*src_fn)({});
    if (!types::is_string(src))
        return -1;
    auto src_idx = types::as_string_idx(src);
    if (src_idx >= string_heap_.size())
        return -1;
    auto source = string_heap_[src_idx];
    workspace_flat_->recycle_dead_nodes();
    FlatSnapshot fs;
    try {
        fs.flat = std::make_unique<aura::ast::FlatAST>();
        fs.pool = std::make_unique<aura::ast::StringPool>();
        *fs.flat = *workspace_flat_;
        *fs.pool = *workspace_pool_;
        fs.has_flat = true;
        fs.flat_generation = workspace_flat_->generation();
        fs.flat_size = workspace_flat_->size();
        if (workspace_tree_) {
            auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
            if (auto* node = wt->active())
                fs.cow_epoch = node->cow_epoch;
        }
    } catch (...) {
        // [SILENCE-PRIM-#615] OOM / copy failure during snapshot — keep
        // source-only snapshot (has_flat=false); same contract as
        // ast:snapshot deep-copy path (#1669 class A).
        fs.has_flat = false;
    }
    auto id = snapshot_sources_.size();
    snapshot_sources_.push_back(source);
    snapshot_names_.push_back(std::string(name));
    snapshot_flats_.push_back(std::move(fs));
    return static_cast<std::int64_t>(id);
}

bool Evaluator::restore_workspace_snapshot_under_lock(std::size_t id) noexcept {
    if (id >= snapshot_sources_.size() || workspace_read_only_ || !workspace_flat_ ||
        !workspace_pool_)
        return false;
    last_set_code_error_kind_.clear();
    last_set_code_error_msg_.clear();
    last_eval_current_result_.reset();
    if (id < snapshot_flats_.size() && snapshot_flats_[id].has_flat && snapshot_flats_[id].flat &&
        snapshot_flats_[id].pool) {
        try {
            *workspace_flat_ = *snapshot_flats_[id].flat;
            *workspace_pool_ = *snapshot_flats_[id].pool;
            workspace_flat_->recycle_dead_nodes();
            update_shared_tree_root();
            last_post_restore_report_ = workspace_flat_->validate_post_restore();
            last_post_restore_violations_ = last_post_restore_report_.violations;
            defuse_index_destroy(reinterpret_cast<void**>(&defuse_index_));
            defuse_affected_syms_.clear();
            if (mark_all_defines_dirty_fn_)
                mark_all_defines_dirty_fn_();
            if (pre_cache_workspace_defines_fn_)
                pre_cache_workspace_defines_fn_();
            return true;
        } catch (...) {
            // [SILENCE-PRIM-#615] restore_workspace_snapshot_under_lock is
            // noexcept bool API — false is the documented failure signal
            // (#1669 class A intentional-return-value).
            return false;
        }
    }
    return false;
}

Evaluator::MutationImpactEntry Evaluator::get_latest_mutation_impact_entry() const noexcept {
    const auto seq = mutation_impact_ring_seq_.load(std::memory_order_acquire);
    if (seq == 0)
        return {};
    return mutation_impact_ring_[(seq - 1) % kMutationImpactRingSize];
}

// Issue #420: end-to-end MacroIntroduced hygiene contract.
// clone_macro_body / expand_inner_macros stamp both
// SyntaxMarker::MacroIntroduced and kMacroExpansion on
// every node in an expanded subtree. Drift between the
// marker column and macro_dirty_ signals a post-split
// regression on the clone/expand → query/mutate/IR path.
void Evaluator::ensure_macro_hygiene_contract() const noexcept {
    auto* ws = workspace_flat_;
    if (!ws)
        return;
    using aura::ast::FlatAST;
    using aura::ast::NodeId;
    using aura::ast::SyntaxMarker;
    constexpr auto kExpansion =
        static_cast<std::uint8_t>(FlatAST::MacroDirtyReason::kMacroExpansion);
    for (NodeId id = 0; id < ws->size(); ++id) {
        if (!ws->is_macro_introduced(id))
            continue;
        // Caller-arg substitution slots stay User-marked but
        // may inherit kMacroExpansion from the subtree walk;
        // only verify the clone_macro_body stamp direction.
        if ((ws->macro_dirty(id) & kExpansion) == 0) {
            macro_hygiene_contract_violations_.fetch_add(1, std::memory_order_relaxed);
            // Issue #593: post-mutate IR hygiene violation tally.
            if (compiler_metrics_) {
                auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
                m->ir_hygiene_post_mutate_violation_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace aura::compiler
