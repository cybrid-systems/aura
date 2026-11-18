// service_typecheck.cpp - CompilerService type-check entry points moved
// out of the module interface (#5242). Move-only: the bodies are unchanged.

module;

#include "observability_metrics.h"
#include "core/workspace_epoch.hh" // current_mutation_epoch

module aura.compiler.service;

import std;
import aura.compiler.type_checker; // TypeCheckWrap
import aura.diag;
import aura.parser.lexer; // SourceLineOrigin (#5347)
import aura.parser.parser;

namespace aura::compiler {

CompilerService::TypecheckResult
CompilerService::typecheck_full(std::string_view input,
                                std::span<const aura::parser::SourceLineOrigin> origins) {
    TypecheckResult r;
    auto alloc = arena_.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto pr = aura::parser::parse_to_flat(input, flat, pool, origins);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        auto diag = parse_error_diag(pr);
        r.output = std::string("parse error: ") + diag.format() + "\n";
        r.has_errors = true;
        r.diagnostics.push_back(diag);
        return r;
    }
    flat.root = pr.root;

    // Use the CompilerService's persistent type_registry_ so that ADT
    // constructors registered during eval (via define-type) are visible
    // to the match exhaustiveness check.
    aura::compiler::TypeChecker tc(type_registry_);
    aura::diag::DiagnosticCollector diag;

    // Issue #79: opt into strict mode. The `typecheck` command's whole
    // purpose is to surface type errors; without strict mode, the
    // gradual-coercion path reports cross-type mismatches as `Note`
    // diagnostics that pass through `has_errors() == false` (Bug A).
    tc.set_strict(true);
    tc.set_gradual_permissiveness(gradual_permissiveness_);

    // Issue #168: gate the type cache by the global mutation
    // epoch. Any mutation since the last infer_flat bumps the
    // epoch; the type checker's cache_invalidated_ flag is
    // set on the next call, forcing re-inference of every
    // node. Coarse but provably correct for the stale-cache
    // bug class.
    tc.set_cache_epoch(aura::core::current_mutation_epoch());
    // Issue #258: plumb metrics so solve_delta timing
    // accumulates into CompilerMetrics::delta_solve_time_us.
    tc.set_metrics(&metrics_);
    // Issue #385: plumb poly metrics so register_forall
    // + instantiate_forall accumulate into
    // CompilerMetrics::poly_*_total. The setter
    // takes 3 atomic addresses (register +
    // dedup_hits + instantiate) — avoids the
    // cross-module dependency on the full
    // CompilerMetrics struct.
    type_registry_.set_poly_metrics(&metrics_.poly_register_total, &metrics_.poly_dedup_hits_total,
                                    &metrics_.poly_instantiate_total);
    // Issue #2148: precise meet lattice hits → CompilerMetrics.
    type_registry_.set_meet_precision_counter(&metrics_.meet_precision_hit_total);

    auto result = tc.infer_flat(flat, pool, pr.root, diag);

    // Issue #258: accumulate incremental typecheck stats
    // into CompilerMetrics (lifetime totals). The per-call
    // TypeChecker is short-lived, so without this
    // accumulation multi-mutation workloads would have
    // nowhere to surface the cache hit/miss ratio.
    // Wire the metrics pointer so ConstraintSystem::solve_delta
    // can also accumulate time (see type_checker_impl.cpp).
    metrics_.typecheck_cache_hits_total.fetch_add(tc.stats().cache_hits, std::memory_order_relaxed);
    metrics_.typecheck_cache_misses_total.fetch_add(tc.stats().cache_misses,
                                                    std::memory_order_relaxed);
    metrics_.typecheck_stale_cache_total.fetch_add(tc.stats().stale_cache,
                                                   std::memory_order_relaxed);
    metrics_.typecheck_gen_saved_total.fetch_add(tc.stats().gen_saved, std::memory_order_relaxed);
    // Issue #386: narrowing observability. Mirror
    // the per-call stats into the lifetime
    // CompilerMetrics counters.
    metrics_.narrowing_applied_total.fetch_add(tc.stats().narrowing_applied,
                                               std::memory_order_relaxed);
    metrics_.narrowing_skipped_total.fetch_add(tc.stats().narrowing_skipped,
                                               std::memory_order_relaxed);
    metrics_.narrowing_reanalyzed_total.fetch_add(tc.stats().narrowing_reanalyzed,
                                                  std::memory_order_relaxed);
    // Issue #340 / #1781 / #1872: predicate_memo_ lifetime totals.
    metrics_.predicate_memo_hits_total.fetch_add(tc.stats().predicate_memo_hits,
                                                 std::memory_order_relaxed);
    metrics_.predicate_memo_misses_total.fetch_add(tc.stats().predicate_memo_misses,
                                                   std::memory_order_relaxed);
    metrics_.predicate_memo_evictions_total.fetch_add(tc.stats().predicate_memo_evictions,
                                                      std::memory_order_relaxed);
    metrics_.predicate_memo_partial_evictions_total.fetch_add(
        tc.stats().predicate_memo_partial_evictions, std::memory_order_relaxed);
    // Issue #338: and/or precision.
    metrics_.and_or_meet_uses_total.fetch_add(tc.stats().and_or_meet_uses,
                                              std::memory_order_relaxed);
    metrics_.and_or_join_uses_total.fetch_add(tc.stats().and_or_join_uses,
                                              std::memory_order_relaxed);
    // Issue #434: per-node occurrence dirty recovery.
    metrics_.narrowing_dirty_recovery_total.fetch_add(tc.stats().narrowing_dirty_recovery,
                                                      std::memory_order_relaxed);
    // Issue #390: schema cache observability.
    metrics_.schema_cache_lookups_total.fetch_add(tc.stats().schema_cache_lookups,
                                                  std::memory_order_relaxed);
    metrics_.schema_cache_hits_total.fetch_add(tc.stats().schema_cache_hits,
                                               std::memory_order_relaxed);
    // Issue #387: Type Dependency Graph observability.
    // Mirror the per-call TypeChecker counters into the
    // lifetime CompilerMetrics atomics.
    metrics_.type_dep_graph_lookups.fetch_add(tc.type_dep_graph_lookups(),
                                              std::memory_order_relaxed);
    metrics_.type_dep_graph_hits.fetch_add(tc.type_dep_graph_hits(), std::memory_order_relaxed);
    metrics_.type_dep_graph_size.store(
        std::max(metrics_.type_dep_graph_size.load(std::memory_order_relaxed),
                 static_cast<std::uint64_t>(tc.type_dep_graph_size())),
        std::memory_order_relaxed);

    // Issue #116: the typecheck command doesn't proceed to
    // IR lowering (it just reports types + diagnostics), so
    // we don't need to apply the deferred CoercionMap to the
    // FlatAST. We do consume the map (via take_coercions) so
    // it doesn't leak across calls.
    (void)tc.take_coercions();

    std::string& out = r.output;
    out += "type: " + type_registry_.format_type(result) + "\n";

    for (auto& d : diag.diagnostics()) {
        r.diagnostics.push_back(d);
        if (d.kind != aura::diag::ErrorKind::Note)
            r.has_errors = true;
        out += d.format() + "\n";
    }
    if (r.diagnostics.empty())
        out += "no errors\n";

    return r;
}

std::size_t CompilerService::incremental_infer(const aura::ast::MutationRecord& rec) {
    // Issue #518: (set-code ...) via cs.eval routes through
    // workspace_flat_; cs.set_code() uses current_ast_. Prefer
    // the workspace when it carries mutations so infer_flat_partial
    // re-narrows the same flat the mutate primitive edited.
    aura::ast::FlatAST* flat = current_ast_;
    aura::ast::StringPool* pool = current_pool_;
    if (auto* ws_flat = evaluator_.workspace_flat();
        ws_flat != nullptr && !ws_flat->all_mutations().empty()) {
        flat = ws_flat;
        pool = evaluator_.workspace_pool();
    }
    if (!flat || !pool)
        return 0;
    aura::compiler::TypeChecker tc(type_registry_);
    aura::diag::DiagnosticCollector diag;
    tc.set_strict(true); // match the typecheck() default
    tc.set_gradual_permissiveness(gradual_permissiveness_);
    // Issue #168: gate by global mutation epoch (same as
    // the typecheck() path).
    tc.set_cache_epoch(aura::core::current_mutation_epoch());
    // Issue #258: plumb metrics for solve_delta timing.
    tc.set_metrics(&metrics_);
    tc.set_bidirectional_mode(bidirectional_mode_);
    // Issue #518: wire Evaluator narrowing counters to the
    // actual re-narrow path in infer_flat_partial.
    tc.set_on_narrowing_refresh([this]() { evaluator_.bump_narrowing_refresh_count(); });
    tc.set_on_selective_recheck([this]() { evaluator_.bump_selective_recheck_count(); });
    tc.set_on_touched_roots_snapshot(
        [this](std::size_t n) { evaluator_.set_touched_roots_size(n); });
    tc.set_on_cross_delta_conflict([this]() { evaluator_.bump_cross_delta_conflicts_caught(); });
    // Issue #411 fu1 follow-up #3: plumb the
    // per-DefUseIndex tracker so infer_flat_partial can
    // route through the O(uses) path when the sym is
    // registered. When the tracker is non-null AND has
    // at least one index registered, the
    // per-DefUseIndex path fires (bumps
    // per_defuse_index_used_total). When null or
    // empty, falls back to the O(n) walk (bumps
    // per_symbol_used_total). The split is
    // observable via (compile:per-symbol-reinfer-stats).
    auto* tracker_ptr = per_defuse_index_tracker_.index_count() > 0
                            ? static_cast<void*>(&per_defuse_index_tracker_)
                            : nullptr;
    // Issue #2516: dirty txn entry (invalidate → re-infer → mirror).
    auto n = tc.infer_flat_partial_with_dirty_txn(*flat, *pool, rec, diag, tracker_ptr);
    // Issue #2180: stash partial CS + occurrence vars for composite commit.
    evaluator_.stash_partial_constraint_state(static_cast<void*>(&tc));
    metrics_.typecheck_gen_saved_total.fetch_add(tc.stats().gen_saved, std::memory_order_relaxed);
    // Issue #387: Type Dependency Graph observability.
    // Mirror the per-call TypeChecker counters into the
    // lifetime CompilerMetrics atomics. The graph is
    // built by infer_flat_partial's set_type_with_gen
    // path; lookups happen during affected-set
    // computation (the actual O(affected) wiring is the
    // follow-up).
    metrics_.type_dep_graph_lookups.fetch_add(tc.type_dep_graph_lookups(),
                                              std::memory_order_relaxed);
    metrics_.type_dep_graph_hits.fetch_add(tc.type_dep_graph_hits(), std::memory_order_relaxed);
    metrics_.type_dep_graph_size.store(
        std::max(metrics_.type_dep_graph_size.load(std::memory_order_relaxed),
                 static_cast<std::uint64_t>(tc.type_dep_graph_size())),
        std::memory_order_relaxed);
    // Issue #411 follow-up #1: per-symbol / ancestor
    // path tracking. Mirror the per-call TypeChecker
    // stats into the lifetime CompilerMetrics
    // counters so the
    // (compile:per-symbol-reinfer-stats) Aura primitive
    // sees a single source of truth.
    metrics_.per_symbol_reinfer_used_total.fetch_add(tc.stats().per_symbol_used_total,
                                                     std::memory_order_relaxed);
    metrics_.per_symbol_reinfer_visited_total.fetch_add(tc.stats().per_symbol_visited_total,
                                                        std::memory_order_relaxed);
    metrics_.ancestor_reinfer_used_total.fetch_add(tc.stats().ancestor_used_total,
                                                   std::memory_order_relaxed);
    metrics_.ancestor_reinfer_visited_total.fetch_add(tc.stats().ancestor_visited_total,
                                                      std::memory_order_relaxed);
    // Issue #411 fu1 follow-up #3: per-DefUseIndex
    // path tracking. Mirror the per-call stats into
    // the lifetime CompilerMetrics counters.
    metrics_.per_defuse_index_used_total.fetch_add(tc.stats().per_defuse_index_used_total,
                                                   std::memory_order_relaxed);
    metrics_.per_defuse_index_walk_fallback_total.fetch_add(
        tc.stats().per_defuse_index_walk_fallback_total, std::memory_order_relaxed);
    // Issue #411 fu1 fu4: per-DefUseIndex visited
    // count (the O(uses) signal). Pre-fu4 this was
    // always 0 (the per-DefUseIndex path still did
    // the O(n) walk). Post-fu4 it's the real
    // wall-clock signal.
    metrics_.per_defuse_index_visited_total.fetch_add(tc.stats().per_defuse_index_visited_total,
                                                      std::memory_order_relaxed);
    // Issue #386: narrowing observability. Mirror
    // the per-call stats into the lifetime
    // CompilerMetrics counters.
    metrics_.narrowing_applied_total.fetch_add(tc.stats().narrowing_applied,
                                               std::memory_order_relaxed);
    metrics_.narrowing_skipped_total.fetch_add(tc.stats().narrowing_skipped,
                                               std::memory_order_relaxed);
    metrics_.narrowing_reanalyzed_total.fetch_add(tc.stats().narrowing_reanalyzed,
                                                  std::memory_order_relaxed);
    // Issue #340 / #1781 / #1872: predicate_memo_ lifetime totals.
    metrics_.predicate_memo_hits_total.fetch_add(tc.stats().predicate_memo_hits,
                                                 std::memory_order_relaxed);
    metrics_.predicate_memo_misses_total.fetch_add(tc.stats().predicate_memo_misses,
                                                   std::memory_order_relaxed);
    metrics_.predicate_memo_evictions_total.fetch_add(tc.stats().predicate_memo_evictions,
                                                      std::memory_order_relaxed);
    metrics_.predicate_memo_partial_evictions_total.fetch_add(
        tc.stats().predicate_memo_partial_evictions, std::memory_order_relaxed);
    // Issue #338: and/or precision.
    metrics_.and_or_meet_uses_total.fetch_add(tc.stats().and_or_meet_uses,
                                              std::memory_order_relaxed);
    metrics_.and_or_join_uses_total.fetch_add(tc.stats().and_or_join_uses,
                                              std::memory_order_relaxed);
    // Issue #434: per-node occurrence dirty recovery.
    metrics_.narrowing_dirty_recovery_total.fetch_add(tc.stats().narrowing_dirty_recovery,
                                                      std::memory_order_relaxed);
    // Issue #390: schema cache observability.
    metrics_.schema_cache_lookups_total.fetch_add(tc.stats().schema_cache_lookups,
                                                  std::memory_order_relaxed);
    metrics_.schema_cache_hits_total.fetch_add(tc.stats().schema_cache_hits,
                                               std::memory_order_relaxed);
    return n;
}

std::size_t CompilerService::auto_invoke_incremental_typecheck_for(
    aura::ast::FlatAST& flat, aura::ast::StringPool& pool, const aura::ast::MutationRecord& rec,
    std::uint64_t mutation_id_for_log, const char* source) {
    (void)source;              // was: tag for the debug log (removed in #487 fix)
    (void)mutation_id_for_log; // ditto
    if (incremental_typecheck_mode_ != IncrementalTypecheckMode::Eager)
        return 0;
    aura::compiler::TypeChecker tc(type_registry_);
    aura::diag::DiagnosticCollector diag;
    tc.set_strict(strict_mode_);
    // Match the existing incremental_infer epoch gate
    // (Issue #168): use the current mutation epoch so
    // cache entries captured under a prior epoch are
    // correctly rejected as stale.
    tc.set_cache_epoch(aura::core::current_mutation_epoch());
    // Plumb the shared metrics so the per-call engine
    // stats (cache_hits / cache_misses / stale_cache)
    // accumulate into the lifetime totals (Issue #258 /
    // #411 wiring).
    tc.set_metrics(&metrics_);
    // Issue #518: wire Evaluator narrowing counters to the
    // actual re-narrow path in infer_flat_partial.
    tc.set_on_narrowing_refresh([this]() { evaluator_.bump_narrowing_refresh_count(); });
    tc.set_on_selective_recheck([this]() { evaluator_.bump_selective_recheck_count(); });
    tc.set_on_touched_roots_snapshot(
        [this](std::size_t n) { evaluator_.set_touched_roots_size(n); });
    tc.set_on_cross_delta_conflict([this]() { evaluator_.bump_cross_delta_conflicts_caught(); });
    // Issue #411 fu1 follow-up #3: plumb the
    // per-DefUseIndex tracker (same as incremental_infer
    // above). When the tracker is non-null AND has
    // at least one index, the O(uses) per-DefUseIndex
    // path fires.
    auto* tracker_ptr2 = per_defuse_index_tracker_.index_count() > 0
                             ? static_cast<void*>(&per_defuse_index_tracker_)
                             : nullptr;
    // Issue #2516: dirty txn entry (invalidate → re-infer → mirror).
    auto n = tc.infer_flat_partial_with_dirty_txn(flat, pool, rec, diag, tracker_ptr2);
    // Issue #2262: stash partial CS for all typed_mutate paths (not only
    // incremental_infer / composite). Single long-lived fact source.
    evaluator_.stash_partial_constraint_state(static_cast<void*>(&tc));
    metrics_.typecheck_cache_hits_total.fetch_add(tc.stats().cache_hits, std::memory_order_relaxed);
    metrics_.typecheck_cache_misses_total.fetch_add(tc.stats().cache_misses,
                                                    std::memory_order_relaxed);
    metrics_.typecheck_stale_cache_total.fetch_add(tc.stats().stale_cache,
                                                   std::memory_order_relaxed);
    metrics_.typecheck_gen_saved_total.fetch_add(tc.stats().gen_saved, std::memory_order_relaxed);
    // Issue #411 follow-up #1: per-symbol / ancestor
    // path tracking. The auto-invoke path also routes
    // through infer_flat_partial, so it benefits from
    // the per-symbol optimization. Mirror the per-call
    // stats into the lifetime counters (same as
    // incremental_infer above).
    metrics_.per_symbol_reinfer_used_total.fetch_add(tc.stats().per_symbol_used_total,
                                                     std::memory_order_relaxed);
    metrics_.per_symbol_reinfer_visited_total.fetch_add(tc.stats().per_symbol_visited_total,
                                                        std::memory_order_relaxed);
    metrics_.ancestor_reinfer_used_total.fetch_add(tc.stats().ancestor_used_total,
                                                   std::memory_order_relaxed);
    metrics_.ancestor_reinfer_visited_total.fetch_add(tc.stats().ancestor_visited_total,
                                                      std::memory_order_relaxed);
    // Issue #411 fu1 follow-up #3: per-DefUseIndex
    // path tracking. Mirror the per-call stats into
    // the lifetime CompilerMetrics counters.
    metrics_.per_defuse_index_used_total.fetch_add(tc.stats().per_defuse_index_used_total,
                                                   std::memory_order_relaxed);
    metrics_.per_defuse_index_walk_fallback_total.fetch_add(
        tc.stats().per_defuse_index_walk_fallback_total, std::memory_order_relaxed);
    // Issue #411 fu1 fu4: per-DefUseIndex visited
    // count (the O(uses) signal).
    metrics_.per_defuse_index_visited_total.fetch_add(tc.stats().per_defuse_index_visited_total,
                                                      std::memory_order_relaxed);
    // Issue #386: narrowing observability. Mirror
    // the per-call stats into the lifetime
    // CompilerMetrics counters.
    metrics_.narrowing_applied_total.fetch_add(tc.stats().narrowing_applied,
                                               std::memory_order_relaxed);
    metrics_.narrowing_skipped_total.fetch_add(tc.stats().narrowing_skipped,
                                               std::memory_order_relaxed);
    metrics_.narrowing_reanalyzed_total.fetch_add(tc.stats().narrowing_reanalyzed,
                                                  std::memory_order_relaxed);
    // Issue #340 / #1781 / #1872: predicate_memo_ lifetime totals.
    metrics_.predicate_memo_hits_total.fetch_add(tc.stats().predicate_memo_hits,
                                                 std::memory_order_relaxed);
    metrics_.predicate_memo_misses_total.fetch_add(tc.stats().predicate_memo_misses,
                                                   std::memory_order_relaxed);
    metrics_.predicate_memo_evictions_total.fetch_add(tc.stats().predicate_memo_evictions,
                                                      std::memory_order_relaxed);
    metrics_.predicate_memo_partial_evictions_total.fetch_add(
        tc.stats().predicate_memo_partial_evictions, std::memory_order_relaxed);
    // Issue #338: and/or precision.
    metrics_.and_or_meet_uses_total.fetch_add(tc.stats().and_or_meet_uses,
                                              std::memory_order_relaxed);
    metrics_.and_or_join_uses_total.fetch_add(tc.stats().and_or_join_uses,
                                              std::memory_order_relaxed);
    // Issue #434: per-node occurrence dirty recovery.
    metrics_.narrowing_dirty_recovery_total.fetch_add(tc.stats().narrowing_dirty_recovery,
                                                      std::memory_order_relaxed);
    // Issue #390: schema cache observability.
    metrics_.schema_cache_lookups_total.fetch_add(tc.stats().schema_cache_lookups,
                                                  std::memory_order_relaxed);
    metrics_.schema_cache_hits_total.fetch_add(tc.stats().schema_cache_hits,
                                               std::memory_order_relaxed);
    // Issue #387: Type Dependency Graph observability.
    // Mirror the per-call TypeChecker counters into the
    // lifetime CompilerMetrics atomics.
    metrics_.type_dep_graph_lookups.fetch_add(tc.type_dep_graph_lookups(),
                                              std::memory_order_relaxed);
    metrics_.type_dep_graph_hits.fetch_add(tc.type_dep_graph_hits(), std::memory_order_relaxed);
    metrics_.type_dep_graph_size.store(
        std::max(metrics_.type_dep_graph_size.load(std::memory_order_relaxed),
                 static_cast<std::uint64_t>(tc.type_dep_graph_size())),
        std::memory_order_relaxed);
    metrics_.incremental_typecheck_auto_invocations_total.fetch_add(1, std::memory_order_relaxed);
    metrics_.incremental_typecheck_re_inferred_total.fetch_add(n, std::memory_order_relaxed);
    // Note: a previous version of this method printed a
    // debug line to stderr ("IncrementalTypecheck: ...")
    // after the re-inference. That output was being
    // captured by the EDSL test framework's stream
    // redirect, polluting the result strings of tests
    // like edsl-ir-cache:cascade-after-mutate (expected
    // "#t" got the debug line + "#t"). The line was
    // left over from the #411 ship and serves no
    // production purpose — the counters above
    // (incremental_typecheck_auto_invocations_total +
    // incremental_typecheck_re_inferred_total) are the
    // observability surface. Removed.
    return n;
}

} // namespace aura::compiler
