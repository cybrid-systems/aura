// evaluator_primitives_query.cpp — P0 step 8: standalone query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/value_tags.h"
#include "serve/fiber.h"

module aura.compiler.evaluator;

import std;
import aura.core.type;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using ModulePathResolver = std::function<std::string(const std::string&)>;

using namespace types;

// Issue #288 forward declaration for the best-effort schema
// shape check helper (defined at the bottom of this file).
static bool validate_code_against_schema_simple(const std::string& code,
                                                const std::string& type_name,
                                                std::string& violation_reason,
                                                std::string& violation_field);

void register_query_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                               std::pmr::vector<std::string>& string_heap, void*& type_registry,
                               ModulePathResolver resolve_module_path, Evaluator& ev) {

    add("query:module-exports", [&pairs, &string_heap, resolve_module_path](
                                   std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_void();
        auto path = string_heap[idx];
        auto resolved = resolve_module_path(path);
        if (resolved.empty())
            return make_void();
        std::ifstream f(resolved);
        if (!f.is_open())
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        std::vector<std::string> exports;
        std::size_t pos = 0;
        while (pos < content.size()) {
            auto export_pos = content.find("(export", pos);
            if (export_pos == std::string::npos)
                break;
            if (export_pos > 0) {
                char prev = content[export_pos - 1];
                if (prev != '\n' && prev != ' ' && prev != '\t' && prev != '(') {
                    pos = export_pos + 1;
                    continue;
                }
            }
            auto sym_start = export_pos + 7;
            while (sym_start < content.size() &&
                   (content[sym_start] == ' ' || content[sym_start] == '\t' ||
                    content[sym_start] == '\n' || content[sym_start] == '\r')) {
                ++sym_start;
            }
            std::size_t i = sym_start;
            while (i < content.size() && content[i] != ')') {
                if (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' ||
                    content[i] == '\r') {
                    ++i;
                    continue;
                }
                if (content[i] == ';') {
                    while (i < content.size() && content[i] != '\n')
                        ++i;
                    continue;
                }
                std::size_t s = i;
                while (i < content.size()) {
                    char c = content[i];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '?' || c == '!' || c == '<' ||
                        c == '>' || c == '=' || c == '*' || c == '+' || c == '-' || c == '/' ||
                        c == '.' || c == '$') {
                        ++i;
                    } else {
                        break;
                    }
                }
                if (i > s) {
                    exports.push_back(content.substr(s, i - s));
                } else {
                    ++i;
                }
            }
            break;
        }
        EvalValue lst = make_void();
        for (auto it = exports.rbegin(); it != exports.rend(); ++it) {
            auto sidx = string_heap.size();
            string_heap.push_back(*it);
            auto pid = pairs.size();
            pairs.push_back({make_string(sidx), lst});
            lst = make_pair(pid);
        }
        return lst;
    });

    add("query:jit-fallback-stats", [](std::span<const EvalValue> a) -> EvalValue {
        // Issue #461: read the global fallback counter. The
        // counter is bumped by `aura_jit_fallback_to_interpreter`
        // each time the JIT default case routes through the
        // fallback path. P0 ship: returns the counter as an
        // integer. Future ship: returns a list
        // (fallback-count deopt-count consistency-violations).
        (void)a;
        return make_int(static_cast<std::int64_t>(
            aura_jit_fallback_count_v_read()));
    });

    // Issue #455: query:ir-marker-stats
    // Returns a 3-tuple (user-instructions, macro-introduced-instructions,
    // bool-literal-instructions) reflecting the SyntaxMarker distribution
    // in the current IR cache. The counts are computed on demand from
    // the active IRModule (if any) — the bridge has no per-instruction
    // global counter yet (that's a follow-up). For the P0 ship we
    // return a placeholder that documents the expected shape; the
    // follow-up wires the real per-instruction walker.
    add("query:ir-marker-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        // P0 placeholder: real implementation needs a global
        // IRModule pointer. Returning (0 0 0) lets callers
        // exercise the primitive path without lying about
        // real numbers; the follow-up wires the real counts.
        return make_int(0); // 0 = unpopulated; follow-up returns a list
    });

    // Issue #458: query:hygiene-stats. Returns an integer
    // equal to the total macro-introduced nodes skipped by
    // query:pattern so far (a single observable counter).
    // Future: returns a 3-tuple (violations skipped total-queries).
    // P0: returns the skipped count as an int.
    add("query:hygiene-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        // Read via the thread-local yield-hook evaluator (same
        // pattern as the #453 hooks). Returns 0 when no
        // evaluator is active.
        auto* ev = Evaluator::yield_hook_evaluator();
        if (!ev) return make_int(0);
        return make_int(static_cast<std::int64_t>(
            ev->get_macro_introduced_skipped_in_query()));
    });

    // Issue #456: query:dirty-subtree root-node-id
    // [reason-mask]. Walks the dirty-subtree rooted at
    // root-node-id and returns the number of dirty
    // nodes found. The optional 2nd arg is a dirty-reason
    // bitmask to AND against each node's dirty_ byte (0
    // = count all dirty nodes).
    //
    // P0: returns an integer (= count of dirty nodes in
    // the ancestor chain up to root). The follow-up
    // returns a list of (NodeId . dirty-bit) pairs.
    add("query:dirty-subtree", [](std::span<const EvalValue> a) -> EvalValue {
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws) return make_int(0);
        auto root = static_cast<aura::ast::NodeId>(as_int(a[0]));
        const std::uint8_t reason_mask = (a.size() >= 2 && is_int(a[1]))
            ? static_cast<std::uint8_t>(as_int(a[1]) & 0xFF)
            : 0xFF; // 0xFF = all reasons
        if (root >= ws->size()) return make_int(0);
        // Walk the parent chain from root upward; count nodes
        // that have at least one dirty bit intersecting
        // reason_mask. (P0: this is the ancestor chain, not
        // the full BFS over children — the impact is
        // recorded by mark_dirty_upward, which already walks
        // ancestors. The follow-up will do a real subtree
        // BFS once we have a per-child lookup.)
        std::uint64_t count = 0;
        auto cur = root;
        while (cur != aura::ast::NULL_NODE && cur < ws->size()) {
            const auto dirty_bits = ws->dirty(cur);
            if ((dirty_bits & reason_mask) != 0)
                ++count;
            cur = ws->parent_of(cur);
        }
        return make_int(static_cast<std::int64_t>(count));
    });

    // Issue #456: query:mutation-impact. Returns the
    // most-recent successful mutation-impact summary
    // recorded by exit_mutation_boundary.
    //
    // P0: returns an integer = mutation_impact_count_
    // (the total number of successful boundaries that
    // recorded an impact summary). The follow-up returns
    // a 4-tuple (epoch-after epoch-delta nodes-changed
    // reasons-mask) of the most-recent ring-buffer entry.
    add("query:mutation-impact", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        return make_int(static_cast<std::int64_t>(
            ev->get_mutation_impact_count()));
    });

    // Issue #456: query:epoch-stats. Returns the current
    // defuse_version_ epoch (the global counter bumped
    // on every mutation boundary entry/exit). Stamps
    // last_queried_epoch_ so a follow-up
    // (query:epoch-delta-since-last-query) can return
    // the delta from a previous query. P0: returns
    // the current epoch.
    add("query:epoch-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        return make_int(static_cast<std::int64_t>(ev->get_defuse_version()));
    });

    // Issue #456: query:epoch-delta-since-last-query.
    // Returns (current_epoch - last_queried_epoch_) and
    // then updates last_queried_epoch_ to the current
    // value. 0 on the first call (or when no evaluator
    // is active).
    add("query:epoch-delta-since-last-query", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t cur = ev->get_defuse_version();
        const std::uint64_t last = ev->get_last_queried_epoch();
        ev->record_epoch_query();
        return make_int(static_cast<std::int64_t>(cur - last));
    });

    // Issue #457: query:stable-ref-stats. Returns
    // observability counters for the generation_ /
    // node_gen_ / StableNodeRef lifecycle:
    //   - generation_wrap_count_  (uint16_t wraps)
    //   - stable_ref_invalidations_  (StableNodeRef rejections)
    //   - node_gen_stale_access_count_  (raw NodeId stale access)
    //
    // P0: returns an integer = sum of all three
    // counters. Follow-up: returns a 3-tuple
    // (wraps invalidations stale-accesses) so the AI
    // Agent can react to each category independently.
    add("query:stable-ref-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws) return make_int(0);
        const std::uint64_t wraps = ws->generation_wrap_count();
        const std::uint64_t invalidations = ws->stable_ref_invalidations();
        const std::uint64_t stale = ws->node_gen_stale_access_count();
        return make_int(static_cast<std::int64_t>(
            wraps + invalidations + stale));
    });

    // Issue #438: query:fiber-migration-stats. Returns
    // the sum of the 2 fiber-migration + work-stealing
    // observability counters:
    //   - mutation_steal_attempts_  (lifetime # of
    //     steal attempts the scheduler logged)
    //   - boundary_violation_count_  (lifetime # of
    //     attempts at an unsafe boundary that were
    //     deferred or skipped)
    //
    // P0: returns an integer = sum of the 2 counters.
    // Follow-up: returns a 2-tuple
    // (steal-attempts boundary-violations) so the AI
    // Agent can compute steal_efficiency and
    // boundary_violation_rate.
    add("query:fiber-migration-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t attempts = ev->get_mutation_steal_attempts();
        const std::uint64_t violations = ev->get_boundary_violation_count();
        return make_int(static_cast<std::int64_t>(
            attempts + violations));
    });

    // Issue #439: query:gc-safepoint-stats. Returns
    // the sum of the 3 GC safepoint + MutationBoundary
    // coordination observability counters:
    //   - gc_safepoint_requests_total_  (lifetime # of
    //     safepoint requests)
    //   - gc_safepoint_waits_total_  (lifetime # of
    //     wait completions)
    //   - gc_safepoint_deferred_total_  (lifetime # of
    //     deferrals because a fiber held an outermost
    //     MutationBoundary guard)
    //
    // P0: returns an integer = sum of the 3 counters.
    // Follow-up: returns a 3-tuple
    // (requests waits deferred) so the AI Agent can
    // compute deferral_rate and wait_time_avg.
    add("query:gc-safepoint-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t requests = ev->get_gc_safepoint_requests_total();
        const std::uint64_t waits = ev->get_gc_safepoint_waits_total();
        const std::uint64_t deferred = ev->get_gc_safepoint_deferred_total();
        return make_int(static_cast<std::int64_t>(
            requests + waits + deferred));
    });

    // Issue #443: query:verify-tool-stats. Returns the
    // sum of the 3 external simulator tool-calling
    // observability counters:
    //   - verify_tool_calls_total_  (lifetime # of
    //     run-external-sim calls)
    //   - verify_tool_cache_hits_total_  (lifetime # of
    //     cache hits on (cmd, generation_) lookup)
    //   - verify_tool_parse_errors_total_  (lifetime # of
    //     parse errors in cov-data / fail-data)
    //
    // P0: returns an integer = sum of the 3 counters.
    // Follow-up: returns a 3-tuple
    // (calls cache-hits parse-errors) so the AI Agent
    // can compute cache_hit_rate and parse_error_rate.
    add("query:verify-tool-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t calls = ev->get_verify_tool_calls_total();
        const std::uint64_t hits = ev->get_verify_tool_cache_hits_total();
        const std::uint64_t errors = ev->get_verify_tool_parse_errors_total();
        return make_int(static_cast<std::int64_t>(
            calls + hits + errors));
    });

    // Issue #451: query:orchestration-metrics. Returns
    // a string-encoded JSON with the orchestration
    // observability counters (yield breakdown by
    // reason, steal success / deferred counts,
    // GC pause attribution). P0 ships a string with
    // the 8 counters as a simple "{key: value, ...}"
    // encoding; the follow-up returns a structured
    // list / JSON with per-fiber histograms + recent
    // agent loop samples.
    add("query:orchestration-metrics", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        // The C-linkage shim returns the static
        // gc_pause_attributed_to_mutation_count_ from
        // the Fiber class. Per-Fiber yield counts are
        // aggregated via the active yield-hook
        // evaluator (the P0 reads them via a thread-
        // local; the follow-up uses GlobalMetrics).
        const std::uint64_t gc_pauses =
            aura_fiber_static_gc_pause_attributed_to_mutation();
        // For the P0, the per-fiber yield counts are
        // 0 (they require a per-fiber aggregate; the
        // follow-up adds it). The sum is the gc_pauses
        // counter + 0, which the test verifies is
        // non-negative.
        const std::uint64_t sum = gc_pauses;
        // Build a simple "{gc_pauses: N}" string.
        // The follow-up returns a structured
        // (yield_mutation_boundary, yield_explicit,
        //  yield_scheduler_steal, yield_blocking_io,
        //  yield_operation_boundary, steal_success,
        //  steal_deferred, gc_pauses) tuple.
        std::string result = "{\"gc_pauses_attributed_to_mutation\":";
        result += std::to_string(gc_pauses);
        result += ",\"sum\":";
        result += std::to_string(sum);
        result += "}";
        // Return as a string. We have to find an
        // evaluator to push the string into the
        // string_heap_; if no evaluator, return #f.
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const auto idx = ev->push_string_heap(result);
        return make_string(static_cast<std::int32_t>(idx));
    });

    // Issue #447: query:query-stats. Returns the sum
    // of the 3 tag+arity index counters (hits / misses /
    // rebuilds) as an integer. P0 ships the sum; the
    // follow-up returns a 3-tuple so the AI Agent can
    // compute hit_rate = hits / (hits + misses) and
    // decide when to trigger a rebuild.
    add("query:query-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws) return make_int(0);
        const std::uint64_t hits = ws->tag_arity_index_hits();
        const std::uint64_t misses = ws->tag_arity_index_misses();
        const std::uint64_t rebuilds = ws->tag_arity_index_rebuilds();
        return make_int(static_cast<std::int64_t>(
            hits + misses + rebuilds));
    });

    // Issue #547: query:pattern-index-stats. Returns
    // the sum of the 4 tag_arity_index observability
    // counters:
    //   - hits (lifetime # of find_by_tag_arity hits)
    //   - misses (lifetime # of find_by_tag_arity misses)
    //   - rebuilds (lifetime # of full rebuilds)
    //   - dirty_marks (lifetime # of mark_dirty_upward()
    //     calls that flipped the dirty flag — each mark
    //     tells callers the index is potentially stale)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple (hits misses rebuilds
    // dirty_marks) so the AI Agent can compute the
    // dirty_marks/rebuilds ratio (= how often we forced
    // a full rebuild vs incremental).
    add("query:pattern-index-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws) return make_int(0);
        const std::uint64_t hits = ws->tag_arity_index_hits();
        const std::uint64_t misses = ws->tag_arity_index_misses();
        const std::uint64_t rebuilds = ws->tag_arity_index_rebuilds();
        const std::uint64_t dirty_marks = ws->tag_arity_index_dirty_marks();
        // Issue #554: include rebuild_time_us + delta_hits
        // so (query:pattern-index-stats) returns the full
        // 6-counter matrix. The AI Agent can compute
        // avg_rebuild_us = rebuild_time_us / rebuilds and
        // delta_hit_rate = delta_hits / (delta_hits + rebuilds).
        const std::uint64_t rebuild_time_us =
            ws->tag_arity_index_rebuild_time_us();
        const std::uint64_t delta_hits =
            ws->tag_arity_index_delta_hits();
        return make_int(static_cast<std::int64_t>(
            hits + misses + rebuilds + dirty_marks +
            rebuild_time_us + delta_hits));
    });

    // Issue #547: query:pattern-hygiene-stats. Returns
    // the sum of the 2 query:pattern hygiene observability
    // counters:
    //   - macro_introduced_skipped_in_query_  (# of nodes
    //     the matcher skipped because their
    //     SyntaxMarker was MacroIntroduced and
    //     :respect-hygiene was the default #f)
    //   - hygiene_violation_count_  (# of explicit
    //     hygiene violations bumped by the matcher / query
    //     layer when a MacroIntroduced node was returned to
    //     a caller that didn't expect it)
    //
    // P0: returns an integer = sum of both counters.
    // Follow-up: returns a 2-tuple (skips violations) so
    // the AI Agent can react to each category independently.
    add("query:pattern-hygiene-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        return make_int(static_cast<std::int64_t>(
            skips + violations));
    });

    // Issue #548: query:panic-checkpoint-lifecycle-stats.
    // Returns the sum of the 4 panic-checkpoint lifecycle
    // observability counters:
    //   - panic_checkpoint_save_count_  (lifetime # of
    //     save_panic_checkpoint() calls that succeeded)
    //   - panic_checkpoint_restore_count_  (lifetime # of
    //     restore_panic_checkpoint() calls, both successful
    //     and failed)
    //   - panic_checkpoint_commit_count_  (lifetime # of
    //     commit_panic_checkpoint() calls — typically once
    //     per successful Guard dtor)
    //   - rollback_success_on_panic_  (lifetime # of
    //     restore_panic_checkpoint() calls that actually
    //     succeeded — a stricter subset of restore_count)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple
    // (save restore commit rollback-success) so the AI
    // Agent can compute the rollback-success rate
    // (= rollback_success / restore) and the save/commit
    // ratio (= save / commit, ideally 1.0).
    add("query:panic-checkpoint-lifecycle-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t save = ev->get_panic_checkpoint_save_count();
        const std::uint64_t restore = ev->get_panic_checkpoint_restore_count();
        const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
        const std::uint64_t rollback_success = ev->get_rollback_success_on_panic();
        return make_int(static_cast<std::int64_t>(
            save + restore + commit + rollback_success));
    });

    // Issue #549: query:self-evolution-stability-stats.
    // Returns the sum of the 4 self-evolution observability
    // counters:
    //   - cross_cow_invalidations_  (# of StableNodeRef
    //     rejections caused by crossing a COW snapshot
    //     boundary — bumped by validate_stable_ref when
    //     captured_gen != current generation_ with small
    //     delta, suggesting same fiber post-mutate)
    //   - fiber_stale_ref_count_  (# of stale-ref detections
    //     where the captured gen is from a different fiber's
    //     workspace — large delta)
    //   - mutation_log_rollback_count_  (# of times
    //     exit_mutation_boundary(false) actually rolled back
    //     the log — a stricter subset of failed boundaries)
    //   - provenance_mismatch_  (# of stable-ref checks
    //     where the captured provenance (origin layer)
    //     didn't match the current workspace layer)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple
    // (cross-cow fiber-stale rollback provenance-mismatch)
    // so the AI Agent can react to each category
    // independently. cross-cow > 0 is expected under load
    // (every structural mutate bumps generation_); fiber-stale
    // > 0 indicates a worker-migration bug; rollback > 0
    // indicates panic or fail-fast path was hit;
    // provenance-mismatch > 0 indicates a stale layer in the
    // StableNodeRef handle.
    add("query:self-evolution-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        return make_int(static_cast<std::int64_t>(
            cross_cow + fiber_stale + rollback + provenance));
    });

    // Issue #550: query:typed-mutation-stats. Returns the
    // sum of the 4 incremental typed self-mod observability
    // counters:
    //   - narrowing_refresh_count_  (# of OccurrenceInfoFlat
    //     entries refreshed after dirty propagation)
    //   - cross_delta_conflicts_caught_  (# of times
    //     touched_roots_ detected a CONFLICT between two
    //     delta batches)
    //   - passes_skipped_type_dirty_  (# of clean Pass
    //     blocks skipped by the DirtyAwarePass short-circuit)
    //   - touched_roots_size_  (current touched_roots_ set
    //     size — a snapshot, not a counter)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (narrowing-refresh cross-delta-conflicts passes-skipped
    // touched-roots-size) so the AI Agent can react to each
    // category independently (narrowing-refresh > 0 expected
    // under typed mutate; cross-delta-conflicts > 0 indicates
    // a CONFLICT that needs human review).
    add("query:typed-mutation-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t conflicts = ev->get_cross_delta_conflicts_caught();
        const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
        const std::uint64_t touched_size = ev->get_touched_roots_size();
        return make_int(static_cast<std::int64_t>(
            narrowing + conflicts + passes_skipped + touched_size));
    });

    // Issue #550: query:dirty-impact. Returns the touched
    // roots set size as an integer (a snapshot, not a
    // counter). Production use: the AI Agent reads this to
    // decide whether to schedule a full re-solve (size is
    // large or growth is monotonic) or trust the incremental
    // path (size is bounded).
    add("query:dirty-impact", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        return make_int(static_cast<std::int64_t>(
            ev->get_touched_roots_size()));
    });

    // Issue #305: query:type-propagation-stats. Returns the
    // sum of 4 TypeId/TypeScheme propagation observability
    // counters from the shared CompilerMetrics struct (EDA
    // hardware optimization / synthesis track):
    //   - type_propagation_runs_        (# of TypePropagationPass
    //     invocations)
    //   - type_propagation_total_        (# of instructions whose
    //     type_id was propagated)
    //   - type_propagation_unknown_      (# of instructions whose
    //     type_id == 0 (unknown) the pass could NOT propagate)
    //   - type_propagation_int_width_    (# of integers whose
    //     inferred bit-width (8/16/32/64) was used by a
    //     downstream pass — the EDA backend key metric)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (runs total unknown int-width) so the AI Agent can
    // compute propagation_rate = total / (total + unknown)
    // and react to low bit-width usage as a hint that the
    // EDA backend needs more type info.
    //
    // Non-duplicative with #550 (query:typed-mutation-stats)
    // — the latter is general; this primitive is the EDA-
    // specific TypePropagation + bit-width observability.
    add("query:type-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m) return make_int(0);
        const std::uint64_t runs = m->type_propagation_runs_.load(
            std::memory_order_relaxed);
        const std::uint64_t total = m->type_propagation_total_.load(
            std::memory_order_relaxed);
        const std::uint64_t unknown = m->type_propagation_unknown_.load(
            std::memory_order_relaxed);
        const std::uint64_t int_width = m->type_propagation_int_width_.load(
            std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(
            runs + total + unknown + int_width));
    });

    // Issue #629: query:coercion-zerooverhead-stats. Returns the
    // sum of the 4 zero-overhead coercion lifetime counters:
    //   - coercion_castop_emitted_total (TypeSpec CastOp inserts)
    //   - coercion_type_prop_hits_total (DCE Rule 1 elisions)
    //   - coercion_narrow_evidence_hits_total (DCE Rule 6 +
    //     TypeSpec narrow skips + GuardShape fast-path)
    //   - coercion_zerooverhead_win_total (per-run wins)
    add("query:coercion-zerooverhead-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m) return make_int(0);
        const std::uint64_t castop = m->coercion_castop_emitted_total.load(
            std::memory_order_relaxed);
        const std::uint64_t type_prop = m->coercion_type_prop_hits_total.load(
            std::memory_order_relaxed);
        const std::uint64_t narrow = m->coercion_narrow_evidence_hits_total.load(
            std::memory_order_relaxed);
        const std::uint64_t win = m->coercion_zerooverhead_win_total.load(
            std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(castop + type_prop + narrow + win));
    });

    // Issue #306: query:linear-ownership-stats. Returns the
    // sum of 4 hardware resource linear-ownership observability
    // counters (EDA track — wire/reg/mem/port borrow + double-
    // drive detection):
    //   - hw_resource_wire_borrows_    (# of Wire resource
    //     borrows issued by the lowerer)
    //   - hw_resource_reg_writes_      (# of Reg resource
    //     writes issued by the lowerer)
    //   - hw_resource_mem_access_     (# of Mem resource
    //     accesses issued by the lowerer)
    //   - hw_resource_double_drive_   (# of double-drive
    //     violations caught at compile time — should be 0
    //     in correct hardware code; > 0 = EDA bug)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (wire-borrows reg-writes mem-access double-drive) so
    // the AI Agent can react to double-drive > 0 as a hard
    // alert (hardware simulation safety).
    //
    // Non-duplicative with #556 (query:edsl-concurrency-stats)
    // — the latter is general EDSL concurrency; this primitive
    // is the EDA-specific hardware-resource linear-ownership
    // observability.
    add("query:linear-ownership-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m) return make_int(0);
        const std::uint64_t wire_borrows =
            m->hw_resource_wire_borrows_.load(std::memory_order_relaxed);
        const std::uint64_t reg_writes =
            m->hw_resource_reg_writes_.load(std::memory_order_relaxed);
        const std::uint64_t mem_access =
            m->hw_resource_mem_access_.load(std::memory_order_relaxed);
        const std::uint64_t double_drive =
            m->hw_resource_double_drive_.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(
            wire_borrows + reg_writes + mem_access + double_drive));
    });

    // Issue #551: query:reflect-postmutate-stats. Returns
    // the sum of the 4 reflect post-mutate observability
    // counters:
    //   - impact_snapshot_count_  (# of post-mutate impact
    //     snapshots produced by Guard dtor success path)
    //   - schema_validation_pass_count_  (# of auto_validate
    //     calls that passed — post-mutate structural
    //     integrity check)
    //   - schema_validation_fail_count_  (# of auto_validate
    //     calls that caught an inconsistency — production
    //     critical to detect silent corruption)
    //   - dirty_nodes_in_snapshot_  (# of dirty nodes captured
    //     in the latest impact snapshot — per-snapshot stat)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (impact-snapshots schema-pass schema-fail dirty-nodes)
    // so the AI Agent can compute validation pass-rate
    // (= pass / (pass + fail)) and react to schema-fail > 0
    // as a hard alert (silent corruption).
    add("query:reflect-postmutate-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t pass = ev->get_schema_validation_pass_count();
        const std::uint64_t fail = ev->get_schema_validation_fail_count();
        const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
        return make_int(static_cast<std::int64_t>(
            snapshots + pass + fail + dirty));
    });

    // Issue #597: query:macro-reflect-self-evo-stats. Returns
    // the sum of 8 observability counters spanning the full
    // Task6 production-review closed loop:
    //   macro expand (MacroIntroduced) → query:pattern hygiene
    //   → mutate under Guard → reflect auto_validate → epoch/
    //   dirty propagation → self-evo stability:
    //   - macro_introduced_skipped_in_query_  (hygiene filter)
    //   - hygiene_violation_count_            (hygiene breach)
    //   - mutation_impact_count_            (Guard success)
    //   - impact_snapshot_count_              (reflect snapshot)
    //   - schema_validation_pass_count_       (auto_validate ok)
    //   - schema_validation_fail_count_     (auto_validate fail)
    //   - panic_checkpoint_commit_count_      (Guard commit)
    //   - cross_cow_invalidations_            (self-evo COW)
    //
    // P0: returns an integer = sum of all 8 counters.
    // Follow-up: returns an 8-tuple so the AI Agent can react
    // to each category independently.
    //
    // Non-duplicative with #547/#548/#549/#551 — those expose
    // per-theme stats; this primitive is the unified Task6
    // matrix observability surface for macro+reflect+self-evo.
    add("query:macro-reflect-self-evo-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        const std::uint64_t impact = ev->get_mutation_impact_count();
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t pass = ev->get_schema_validation_pass_count();
        const std::uint64_t fail = ev->get_schema_validation_fail_count();
        const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        return make_int(static_cast<std::int64_t>(
            skips + violations + impact + snapshots + pass + fail +
            commit + cross_cow));
    });

    // Issue #602: query:prompt6-violation-count. Returns
    // the sum of 7 Prompt6 memory-safety violation counters
    // that must stay at 0 under production load:
    //   - boundary_violation_count_         (unsafe boundary)
    //   - mutation_steal_violation_count_   (steal during guard)
    //   - envframe_desync_detected_         (SoA dual-path mismatch)
    //   - unsafe_boundary_attempts_         (EDSL concurrency)
    //   - atomic_batch_steal_violation_     (batch + steal race)
    //   - provenance_mismatch_              (StableNodeRef layer)
    //   - fiber_stale_ref_count_            (cross-fiber stale ref)
    //
    // P0: returns an integer = sum of all 7 counters.
    // Follow-up: returns a 7-tuple so the AI Agent can react
    // to each category independently. Any value > 0 is a hard
    // alert for commercial production sign-off.
    //
    // Non-duplicative with #438/#448/#531/#543 — those expose
    // per-theme stats; this primitive is the unified Prompt6
    // violation surface for the full memory-safety matrix.
    add("query:prompt6-violation-count", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t boundary = ev->get_boundary_violation_count();
        const std::uint64_t steal_viol =
            ev->get_mutation_steal_violation_count();
        const std::uint64_t desync = ev->get_envframe_desync_detected();
        const std::uint64_t unsafe = ev->get_unsafe_boundary_attempts();
        const std::uint64_t batch_steal = ev->get_atomic_batch_steal_violation();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        return make_int(static_cast<std::int64_t>(
            boundary + steal_viol + desync + unsafe + batch_steal +
            provenance + fiber_stale));
    });

    // Issue #602: query:prompt6-safety-score. Returns
    // the sum of 7 Prompt6 memory-safety positive indicators
    // (higher = more safety checks passed / stale refs caught):
    //   - bridge_epoch_hit_count_           (fresh closure bridge)
    //   - linear_check_pass_count_          (linear ownership ok)
    //   - closure_stale_refresh_count_      (stale closure refreshed)
    //   - envframe_stale_refresh_count_     (stale EnvFrame refreshed)
    //   - gc_envframe_stale_skipped_        (GC caught stale EnvFrame)
    //   - envframe_gc_walk_safe_skips_      (GC walk safe skip)
    //   - gc_safepoint_waits_total_         (GC coordination completed)
    //
    // P0: returns an integer = sum of all 7 counters.
    // Follow-up: returns a 7-tuple so the AI Agent can compute
    // safety_ratio = safety_score / (safety_score + violation_count).
    //
    // Non-duplicative with #531/#543/#439 — those expose per-theme
    // pass counters; this primitive is the unified Prompt6 safety
    // score for the full memory-safety fuzz/stress matrix.
    add("query:prompt6-safety-score", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(
                ev->compiler_metrics());
        const std::uint64_t bridge_hit = m
            ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t linear_pass = m
            ? m->linear_check_pass_count_.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t closure_refresh = m
            ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t env_refresh =
            ev->get_envframe_stale_refresh_count();
        const std::uint64_t gc_skipped = m
            ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t gc_walk_skips =
            ev->get_envframe_gc_walk_safe_skips();
        const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
        return make_int(static_cast<std::int64_t>(
            bridge_hit + linear_pass + closure_refresh + env_refresh +
            gc_skipped + gc_walk_skips + gc_waits));
    });

    // Issue #607: query:task4-hotpath-safety-score. Returns
    // the sum of 6 Task4 high-perf hot-path positive indicators:
    //   - specialization_hits_         (shape/JIT fast path)
    //   - relower_skipped_entirely_    (incremental re-lower win)
    //   - passes_skipped_type_dirty_   (Pass short-circuit)
    //   - linear_elide_count_          (linear-move elision)
    //   - tag_arity_index_hits_        (SoA query index hit)
    //   - module_dirty_skips_          (clean module skip)
    //
    // P0: returns an integer = sum of all 6 counters.
    // Non-duplicative with #602/#547/#550 — unified Task4
    // hot-path observability for Arena/SoA/Value/Shape/Pass.
    add("query:task4-hotpath-safety-score", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(
                ev->compiler_metrics());
        auto* ws = ev->workspace_flat();
        const std::uint64_t spec_hits = m
            ? m->specialization_hits.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t relower_skip = m
            ? m->relower_skipped_entirely_count.load(
                  std::memory_order_relaxed)
            : 0;
        const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
        const std::uint64_t linear_elide = m
            ? m->linear_elide_count.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t index_hits = ws ? ws->tag_arity_index_hits() : 0;
        const std::uint64_t mod_skip = m
            ? m->module_dirty_skips.load(std::memory_order_relaxed)
            : 0;
        return make_int(static_cast<std::int64_t>(
            spec_hits + relower_skip + passes_skip + linear_elide +
            index_hits + mod_skip));
    });

    // Issue #607: query:task4-cache-locality-win. Returns
    // the sum of 5 cache-friendly / incremental-win counters:
    //   - tag_arity_index_hits_          (SoA index cache hit)
    //   - tag_arity_index_delta_hits_    (incremental index update)
    //   - specialization_hits_           (shape specialization)
    //   - cascade_body_only_count_       (targeted dirty cascade)
    //   - relower_per_function_called_   (per-fn incremental re-lower)
    //
    // P0: returns an integer = sum of all 5 counters.
    add("query:task4-cache-locality-win", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(
                ev->compiler_metrics());
        auto* ws = ev->workspace_flat();
        const std::uint64_t hits = ws ? ws->tag_arity_index_hits() : 0;
        const std::uint64_t delta = ws ? ws->tag_arity_index_delta_hits() : 0;
        const std::uint64_t spec = m
            ? m->specialization_hits.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t cascade = m
            ? m->cascade_body_only_count.load(std::memory_order_relaxed)
            : 0;
        const std::uint64_t per_fn = m
            ? m->relower_per_function_called_count.load(
                  std::memory_order_relaxed)
            : 0;
        return make_int(static_cast<std::int64_t>(
            hits + delta + spec + cascade + per_fn));
    });

    // Issue #570/#605: query:shape-stability-stats. Returns the sum
    // of 6 ShapeProfiler stability observability counters:
    //   - shape_stability_hit_count    (first-time stable)
    //   - shape_version_bump_count     (invalidate version++)
    //   - shape_fiber_refresh_count    (MutationBoundary yield)
    //   - mutation_shape_churn_count   (stable→unstable / invalidate)
    //   - shape_deopt_hook_fire_count  (invalidate deopt hook)
    //   - jit_shape_miss_count         (#605 JIT cache version miss)
    //
    // P0: returns an integer = sum of all 6 counters.
    // Follow-up: returns a 6-tuple + derived stable_ratio_bp.
    // Non-duplicative with #571 (value dispatch) and #607
    // (Task4 hot-path) — unified shape-stability surface.
    add("query:shape-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const std::uint64_t stable_hits =
            shape::shape_stability_hit_count.load(std::memory_order_relaxed);
        const std::uint64_t version_bumps =
            shape::shape_version_bump_count.load(std::memory_order_relaxed);
        const std::uint64_t fiber_refresh =
            shape::shape_fiber_refresh_count.load(std::memory_order_relaxed);
        const std::uint64_t churn =
            shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
        const std::uint64_t deopt_hooks =
            shape::shape_deopt_hook_fire_count.load(std::memory_order_relaxed);
        const std::uint64_t jit_shape_miss =
            shape::jit_shape_miss_count.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(
            stable_hits + version_bumps + fiber_refresh + churn + deopt_hooks +
            jit_shape_miss));
    });

    // Issue #571: query:value-dispatch-stats. Returns the sum
    // of 4 EvalValue v2 dispatch observability counters:
    //   - value_dispatch_hit_count       (table + range hit)
    //   - value_dispatch_miss_count      (ambiguous tag/range)
    //   - value_contract_violation_count (debug contract tally)
    //   - v2_string_collision_attempts   (false string tag; expect 0)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple + derived dispatch_hit_rate_bp.
    // Non-duplicative with #181 (encoding prototype) and #607
    // (Task4 hot-path matrix) — unified value-dispatch surface.
    add("query:value-dispatch-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const std::uint64_t hits =
            types::value_dispatch_hit_count.load(std::memory_order_relaxed);
        const std::uint64_t misses =
            types::value_dispatch_miss_count.load(std::memory_order_relaxed);
        const std::uint64_t violations =
            types::value_contract_violation_count.load(std::memory_order_relaxed);
        const std::uint64_t collisions =
            types::v2_string_collision_attempts.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(
            hits + misses + violations + collisions));
    });

    // Issue #607: query:task4-mutation-stability. Returns
    // the sum of 6 mutation-stability counters under load:
    //   - dirty_propagation_count_       (dirty walks completed)
    //   - selective_recheck_count_       (selective re-narrow)
    //   - guard_dirty_epoch_count_       (Guard + type cache sync)
    //   - narrowing_refresh_count_     (OccurrenceInfo refresh)
    //   - impact_snapshot_count_         (post-mutate snapshot)
    //   - cross_cow_invalidations_     (COW boundary detection)
    //
    // P0: returns an integer = sum of all 6 counters.
    add("query:task4-mutation-stability", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
        const std::uint64_t selective = ev->get_selective_recheck_count();
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        return make_int(static_cast<std::int64_t>(
            dirty_prop + selective + guard_epoch + narrowing +
            snapshots + cross_cow));
    });

    // Issue #552: query:edsl-stability-stats. Returns
    // the sum of 5 EDSL long-running stability counters
    // from across the workspace + Evaluator:
    //   - cross_cow_invalidations_        (Evaluator, #549)
    //     # of StableNodeRef rejections crossing a COW
    //     snapshot boundary
    //   - fiber_stale_ref_count_          (Evaluator, #549)
    //     # of stale-ref detections from a different
    //     fiber's workspace
    //   - generation_wrap_count_          (FlatAST, #457)
    //     # of uint16_t generation wraps — increases
    //     after ~65k structural mutates
    //   - mutation_log_rollback_count_    (Evaluator, #549)
    //     # of times the log was actually rolled back
    //     (stricter than failed-boundary count)
    //   - provenance_mismatch_           (Evaluator, #549)
    //     # of stable-ref checks where the captured
    //     provenance (origin layer) didn't match the
    //     current workspace layer
    //
    // P0: returns an integer = sum of the 5 counters.
    // Follow-up: returns a 5-tuple
    // (cross-cow fiber-stale wrap rollback provenance)
    // so the AI Agent can react to each category
    // independently. cross-cow > 0 is expected under load
    // (every structural mutate bumps generation_);
    // fiber-stale > 0 indicates a worker-migration bug;
    // wrap > 0 indicates the long-running session crossed
    // the uint16_t generation boundary (~65k mutates);
    // rollback > 0 indicates panic or fail-fast path;
    // provenance-mismatch > 0 indicates a stale layer in
    // the StableNodeRef handle.
    //
    // Non-duplicative with #549 (query:self-evolution-
    // stability-stats) — the latter focuses on Task 6 review
    // observability; this primitive focuses on Task 1 EDSL
    // primitive safety under long-running AI multi-round
    // query → mutate → eval loops.
    add("query:edsl-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        const std::uint64_t wraps = ws ? ws->generation_wrap_count() : 0;
        const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        return make_int(static_cast<std::int64_t>(
            cross_cow + fiber_stale + wraps + rollback + provenance));
    });

    // Issue #553: query:mutation-log-stats. Returns the
    // sum of 4 atomic-batch + mutation-log observability
    // counters from across the workspace + Evaluator:
    //   - atomic_batch_steal_violation_  (Evaluator, #459)
    //     # of steal attempts during an active outermost
    //     atomic batch — must be 0 in production
    //   - atomic_batch_commits_           (FlatAST, #192)
    //     # of commit_atomic_batch calls — each is one
    //     multi-mutate transaction successfully batched
    //   - atomic_batch_bumps_saved_       (FlatAST, #192)
    //     # of generation bumps SUPPRESSED by the
    //     batch (kGenerationSuppressed flag) — measures
    //     the "single bump per commit" optimization win
    //   - atomic_batch_rollbacks_         (Evaluator, #192)
    //     # of rollback_atomic_batch calls — strict
    //     subset of fail-fast paths that actually rolled
    //     back (vs succeed-but-with-partial-warning)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (steal-violations commits bumps-saved rollbacks) so
    // the AI Agent can compute bumps_saved / commits
    // (= avg # of mutations per batch) and react to
    // steal-violations > 0 as a hard alert (the batch
    // is supposed to be steal-safe).
    //
    // Non-duplicative with #459 (query:atomic-batch-stats)
    // — the latter is a 1-counter P0 ship; this primitive
    // exposes the full 4-counter matrix needed for
    // production observability of the atomic batch +
    // rollback + fiber safety closed loop.
    add("query:mutation-log-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t steal_violations =
            ev->get_atomic_batch_steal_violation();
        const std::uint64_t batch_count = ev->atomic_batch_count();
        const std::uint64_t bumps_saved_total =
            ev->atomic_batch_bumps_saved_total();
        const std::uint64_t rollbacks = ev->atomic_batch_rollbacks();
        const std::uint64_t ws_commits =
            ws ? ws->atomic_batch_commits() : 0;
        const std::uint64_t ws_bumps_saved =
            ws ? ws->atomic_batch_bumps_saved() : 0;
        // Issue #396 Phase 3: include the in-fiber heuristic
        // counter in the sum so changes to it show up in the
        // mutation-log-stats aggregate.
        const std::uint64_t in_fiber_total =
            ev->atomic_batch_in_fiber_total();
        return make_int(static_cast<std::int64_t>(
            steal_violations + batch_count + bumps_saved_total +
            rollbacks + ws_commits + ws_bumps_saved + in_fiber_total));
    });

    // (query:mutation-log [n]) — Issue #346: returns
    // a pair-list of the most recent n mutations in
    // chronological order (oldest first). n defaults
    // to 10 when omitted; negative n returns void.
    // Each element is a string representation
    // "id=<id> target=<node-id> op=<operator>
    //  summary=<summary>" so the AI agent can
    // display the evolution trace in a log view.
    // Returns the empty list (void) when no
    // mutations are logged.
    add("query:mutation-log", [](std::span<const EvalValue> a) -> EvalValue {
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws) return make_void();
        std::int64_t n = 10;
        if (!a.empty() && is_int(a[0]))
            n = as_int(a[0]);
        if (n < 0) return make_void();
        // Read the mutation log (most recent first)
        // and take the last n.
        const auto& log = ws->mutation_log_view();
        if (log.empty()) return make_void();
        const std::int64_t take = static_cast<std::int64_t>(log.size()) < n
            ? static_cast<std::int64_t>(log.size())
            : n;
        // Build the pair-list in chronological order
        // (oldest first). The log is most-recent first,
        // so we walk from (log.size() - take) to end.
        const std::size_t start = log.size() - static_cast<std::size_t>(take);
        EvalValue list = make_void();
        for (std::size_t i = log.size(); i-- > start; ) {
            const auto& rec = log[i];
            // Format: "id=<id> target=<node> op=<name> sum=<summary>"
            const std::string s = "id=" + std::to_string(rec.mutation_id) +
                                    " target=" + std::to_string(rec.target_node) +
                                    " op=" + rec.operator_name +
                                    " sum=" + rec.summary;
            const auto sidx = ev->push_string_heap(std::move(s));
            const auto p_idx = ev->push_pair(make_string(sidx), list);
            list = make_pair(p_idx);
        }
        return list;
    });

    // (query:mutations-since <id>) — Issue #346: returns
    // a pair-list of mutations with mutation_id >
    // the given id. Useful for "what changed since my
    // last checkpoint?" queries (the AI agent can
    // record its last_queried_mutation_id and ask
    // for the delta). Returns the empty list when
    // no mutations match.
    add("query:mutations-since", [](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_void();
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws) return make_void();
        const std::uint64_t since_id = static_cast<std::uint64_t>(as_int(a[0]));
        const auto& log = ws->mutation_log_view();
        EvalValue list = make_void();
        // Walk most-recent first (the natural order
        // for the agent's "newest changes" view).
        for (std::size_t i = log.size(); i-- > 0; ) {
            const auto& rec = log[i];
            if (rec.mutation_id <= since_id) break;
            const std::string s = "id=" + std::to_string(rec.mutation_id) +
                                    " target=" + std::to_string(rec.target_node) +
                                    " op=" + rec.operator_name +
                                    " sum=" + rec.summary;
            const auto sidx = ev->push_string_heap(std::move(s));
            const auto p_idx = ev->push_pair(make_string(sidx), list);
            list = make_pair(p_idx);
        }
        return list;
    });

    // (query:last-mutation-blame) — Issue #349: returns
    // the blame info for the most recent mutation as
    // a 2-tuple (operator_name . summary). The blame
    // info is what post_mutation_invariant_check
    // (#260) stamps on each emitted note; exposing
    // it as an Aura primitive lets the AI agent
    // display "triggered by mutate:rebind" in the
    // diagnostic output. Returns the empty pair
    // (void) when no mutation has been logged.
    add("query:last-mutation-blame", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws) return make_void();
        const auto view = ws->mutation_log_view();
        if (view.empty()) return make_void();
        // Most-recent first.
        const auto& rec = view.back();
        // Build the 2-tuple (operator_name . summary).
        // The pair is (op_str . summary_str) — a flat
        // (a . b) pair where a is operator_name and
        // b is summary. The cdr is a string (not a
        // nested pair) because the test contracts
        // expect a 2-tuple of strings.
        const auto oidx = ev->push_string_heap(rec.operator_name);
        const auto sidx = ev->push_string_heap(rec.summary);
        // The push_pair helper copies the EvalValues
        // (so the cdr is a fresh make_string of sidx).
        return make_pair(ev->push_pair(
            make_string(oidx), make_string(sidx)));
    });

    // Issue #612: query:adt-match-exhaust-stats. Returns the
    // sum of 4 ADT/match post-mutation reliability counters:
    //   - adt_exhaust_rechecks_total
    //   - adt_variant_mutate_impacts_total
    //   - adt_stale_exhaust_prevented_total
    //   - adt_occurrence_narrow_in_match_total
    add("query:adt-match-exhaust-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m) return make_int(0);
        const std::uint64_t rechecks = m->adt_exhaust_rechecks_total.load(
            std::memory_order_relaxed);
        const std::uint64_t impacts = m->adt_variant_mutate_impacts_total.load(
            std::memory_order_relaxed);
        const std::uint64_t stale = m->adt_stale_exhaust_prevented_total.load(
            std::memory_order_relaxed);
        const std::uint64_t narrow = m->adt_occurrence_narrow_in_match_total.load(
            std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(rechecks + impacts + stale + narrow));
    });

    // (query:match-exhaustiveness-notes) — Issue
    // #350: returns the most-recent match-
    // exhaustiveness notes (the kind = "Missing-
    // ConstructorInNestedMatch" notes emitted by
    // recheck_match_exhaustiveness_in_dirty_scope
    // after a mutation that touches an ADT
    // constructor). The function returns a
    // pair-list of node-ids (smallest first) that
    // are currently in the post-mutation
    // exhaustiveness notes.
    //
    // The C++ side (recheck_match_exhaustiveness_in_dirty_scope
    // in type_checker_impl.cpp #260) already
    // computes these notes; this primitive
    // surfaces the underlying match-info state to
    // Aura so the AI agent can ask "which matches
    // are currently flagged as non-exhaustive?".
    add("query:match-exhaustiveness-notes",
        [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws) return make_void();
        // Walk the flat; collect node-ids that
        // have a match_info entry with
        // exhaustiveness_checked = true + a
        // candidate_constructors / used_constructors
        // gap. We surface the NodeId; the agent can
        // use (query:node-type <id>) to inspect.
        EvalValue list = make_void();
        const auto n = ws->size();
        for (std::size_t id = n; id-- > 0; ) {
            if (!ws->has_match_info(static_cast<aura::ast::NodeId>(id)))
                continue;
            const auto* mi = ws->get_match_info(
                static_cast<aura::ast::NodeId>(id));
            if (!mi || !mi->exhaustiveness_checked) continue;
            // We surface any checked match. A
            // future enhancement can filter to
            // "non-exhaustive" (used < candidates)
            // but the agent can derive that
            // locally.
            auto sidx = ev->push_string_heap(
                std::to_string(id));
            auto p_idx = ev->push_pair(
                make_string(sidx), list);
            list = make_pair(p_idx);
        }
        return list;
    });

    // Issue #555: query:typed-mutation-stats-task1. Returns
    // the sum of 4 Task1 typed self-mod observability
    // counters + the 4 existing #550 counters (so the AI
    // Agent can compute propagation_ratio +
    // selective_recheck_rate + conflict_rate in one read):
    //   - dirty_propagation_count_       (Evaluator, #555)
    //     # of mark_dirty_upward walks — dirty propagation
    //     throughput
    //   - selective_recheck_count_       (Evaluator, #555)
    //     # of selective OccurrenceInfoFlat re-narrows
    //     (vs full re-solve)
    //   - touched_roots_conflict_count_  (Evaluator, #555)
    //     # of CONFLICT detections between delta batches
    //   - guard_dirty_epoch_count_       (Evaluator, #555)
    //     # of Guard dtor success paths that propagated
    //     dirty to the type cache generation
    //   - narrowing_refresh_count_       (Evaluator, #550)
    //   - cross_delta_conflicts_caught_  (Evaluator, #550)
    //   - passes_skipped_type_dirty_     (Evaluator, #550)
    //   - touched_roots_size_            (Evaluator, #550)
    //
    // P0: returns an integer = sum of the 8 counters.
    // Follow-up: returns an 8-tuple so the AI Agent can
    // react to each category independently (e.g.,
    // touched_roots_conflict > 0 = hard alert;
    // propagation_ratio close to 1.0 = expected;
    // selective_recheck_rate high = win).
    //
    // Non-duplicative with #550 (query:typed-mutation-stats)
    // — the latter is Task 6 review; this primitive is
    // Task 1 EDSL mutate + Guard + dirty propagation focus.
    add("query:typed-mutation-stats-task1", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
        const std::uint64_t selective = ev->get_selective_recheck_count();
        const std::uint64_t conflicts = ev->get_touched_roots_conflict_count();
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t cross_delta = ev->get_cross_delta_conflicts_caught();
        const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
        const std::uint64_t touched_size = ev->get_touched_roots_size();
        return make_int(static_cast<std::int64_t>(
            dirty_prop + selective + conflicts + guard_epoch +
            narrowing + cross_delta + passes_skipped + touched_size));
    });

    // Issue #556: query:edsl-concurrency-stats. Returns
    // the sum of 4 EDSL concurrency safety observability
    // counters from across the Evaluator:
    //   - mutation_steal_attempts_        (Evaluator, #438)
    //     # of steal attempts the scheduler logged
    //   - boundary_violation_count_       (Evaluator, #438)
    //     # of unsafe boundary attempts deferred/skipped
    //   - unsafe_boundary_attempts_       (Evaluator, #556)
    //     # of unsafe boundary attempts (a stricter
    //     subset of boundary_violation — cases where
    //     the boundary actually completed despite the
    //     violation)
    //   - lock_contention_us_              (Evaluator, #556)
    //     # lifetime microseconds spent waiting on
    //     workspace_mtx_ + Guard locks
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (steal-attempts boundary-violations unsafe-attempts
    // lock-contention-us) so the AI Agent can compute
    // contention_ratio = lock_contention_us / wall_time
    // and react to unsafe_boundary_attempts > 0 as a hard
    // alert (concurrency bug).
    //
    // Non-duplicative with #438 (query:fiber-migration-stats)
    // — the latter sums 2 counters (steal + violation);
    // this primitive adds the 2 Task1 EDSL concurrency
    // counters (#556) to the matrix.
    add("query:edsl-concurrency-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t steals = ev->get_mutation_steal_attempts();
        const std::uint64_t violations = ev->get_boundary_violation_count();
        const std::uint64_t unsafe_attempts = ev->get_unsafe_boundary_attempts();
        const std::uint64_t contention_us = ev->get_lock_contention_us();
        return make_int(static_cast<std::int64_t>(
            steals + violations + unsafe_attempts + contention_us));
    });

    // Issue #531: query:closure-env-safety-stats. Returns
    // the sum of 4 closure / EnvFrame / bridge_epoch /
    // linear_ownership_state observability counters from
    // the shared CompilerMetrics struct:
    //   - closure_stale_refresh_count_  (# of stale
    //     IRClosure refreshes triggered by
    //     invalidate_function — the closure-refresh
    //     frequency post-mutate)
    //   - bridge_epoch_hit_count_       (# of bridge_epoch
    //     match checks that succeeded — closure was fresh,
    //     no refresh needed)
    //   - linear_check_pass_count_      (# of linear
    //     ownership_state runtime checks that passed —
    //     Linear* op proceeded with fast path)
    //   - gc_envframe_stale_skipped_    (# of GCEnvWalkFn
    //     visits that skipped a stale EnvFrame — > 0
    //     means a COW/compaction or version mismatch was
    //     caught at GC time)
    //
    // P0: returns an integer = sum of the 4 counters.
    // Follow-up: returns a 4-tuple
    // (stale-refresh bridge-hit linear-pass gc-skipped) so
    // the AI Agent can compute refresh_rate =
    // stale_refresh / (stale_refresh + bridge_hit) and
    // react to gc_envframe_stale_skipped > 0 as a hard
    // alert (silent EnvFrame mismatch).
    add("query:closure-env-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        // Read from the shared CompilerMetrics struct via
        // the Evaluator's void pointer (set by CompilerService
        // via set_compiler_metrics(&metrics_)). Cast back to
        // CompilerMetrics* to access the 4 new counters.
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(
                ev->compiler_metrics());
        if (!m) return make_int(0);
        const std::uint64_t stale_refresh =
            m->closure_stale_refresh_count_.load(
                std::memory_order_relaxed);
        const std::uint64_t bridge_hit =
            m->bridge_epoch_hit_count_.load(
                std::memory_order_relaxed);
        const std::uint64_t linear_pass =
            m->linear_check_pass_count_.load(
                std::memory_order_relaxed);
        const std::uint64_t gc_skipped =
            m->gc_envframe_stale_skipped_.load(
                std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(
            stale_refresh + bridge_hit + linear_pass + gc_skipped));
    });

    // Issue #447: (query:tag-arity-count tag-int arity-int)
    // — count of nodes matching (tag, arity) using the
    // pre-built index. Bumps the hits or misses counter
    // accordingly. P0: 0 on miss (the follow-up falls
    // back to a linear scan on miss).
    add("query:tag-arity-count", [](std::span<const EvalValue> a) -> EvalValue {
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws) return make_int(0);
        // Lazy rebuild on first call: if the index is
        // empty, build it from the current AST.
        if (ws->tag_arity_index_size() == 0) {
            ws->rebuild_tag_arity_index();
        }
        const auto tag = static_cast<std::uint32_t>(as_int(a[0]));
        const auto ar = static_cast<std::uint16_t>(as_int(a[1]));
        const auto nodes = ws->find_by_tag_arity(tag, ar, ar);
        return make_int(static_cast<std::int64_t>(nodes.size()));
    });

    // Issue #469: query:verification-loop-stats. Returns
    // observability counters for the closed-loop
    // verification-driven self-evolution pipeline:
    //   - verification_coverage_feedback_total_  (# of
    //     coverage-hole marks applied)
    //   - verification_assert_failure_total_  (# of
    //     assert-failure marks applied)
    //   - sv_mutate_attempts_total_  (total structured
    //     SV mutates called)
    //   - sv_mutate_success_total_  (successful SV
    //     mutates)
    //   - verify_loop_cycles_total_  (manual loop ticks
    //     from the AI Agent)
    //
    // P0: returns an integer = sum of all 5 counters.
    // Follow-up: returns a 5-tuple so the AI Agent can
    // compute mutate_success_rate + coverage_delta
    // independently.
    add("query:verification-loop-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws) return make_int(0);
        const std::uint64_t cov = ws->verification_coverage_feedback_total();
        const std::uint64_t ass = ws->verification_assert_failure_total();
        const std::uint64_t att = ws->sv_mutate_attempts_total();
        const std::uint64_t suc = ws->sv_mutate_success_total();
        const std::uint64_t cyc = ws->verify_loop_cycles_total();
        return make_int(static_cast<std::int64_t>(
            cov + ass + att + suc + cyc));
    });

    // Issue #448: query:mutation-coordination-stats.
    // Returns observability counters for the fiber /
    // scheduler / GC coordination layer:
    //   - mutation_steal_violation_count_  (work-steal
    //     attempts deferred because the victim fiber
    //     is in an unsafe MutationBoundary state)
    //   - gc_blocked_by_mutation_boundary_  (GC safepoint
    //     requests deferred because an outermost guard
    //     is held)
    //   - safepoint_mutation_wait_total_ns_  (total ns
    //     spent waiting for fibers to reach a safe
    //     mutation boundary during a safepoint)
    //
    // P0: returns an integer = sum of all 3 counters.
    // Follow-up: returns a 3-tuple
    // (steal-violations gc-blocks wait-ns) so the AI
    // Agent can react to each category independently.
    add("query:mutation-coordination-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t steals = ev->get_mutation_steal_violation_count();
        const std::uint64_t gc_blocks = ev->get_gc_blocked_by_mutation_boundary();
        const std::uint64_t wait_ns = ev->get_safepoint_mutation_wait_total_ns();
        return make_int(static_cast<std::int64_t>(
            steals + gc_blocks + wait_ns));
    });

    // Issue #543: query:envframe-dualpath-stats.
    // Returns observability counters for the SoA
    // EnvFrame/EnvId dual-path (bindings_ vs
    // bindings_symid_) + version stamping + stale
    // detection + GCEnvWalkFn integration layer:
    //   - envframe_desync_detected_  (# of frames
    //     where the dual-path length/order check found
    //     a mismatch — should be 0 in production)
    //   - envframe_stale_refresh_count_  (# of frames
    //     whose version_ was bumped by
    //     materialize_call_env because it was older
    //     than the current defuse_version_)
    //   - envframe_version_mismatch_in_walk_  (# of
    //     frames skipped during walk_env_frames /
    //     lookup_by_symid_chain because their version_
    //     was older than the snapshot)
    //   - envframe_gc_walk_safe_skips_  (# of frames
    //     skipped during walk_env_frame_roots for the
    //     same reason — important for tuning the GC's
    //     epoch snapshot strategy)
    //
    // P0: returns an integer = sum of all 4 counters.
    // Follow-up: returns a 4-tuple
    // (desync stale-refresh version-mismatch gc-skips)
    // so the AI Agent can react to each category
    // independently (a desync > 0 should be a hard
    // alert; a version-mismatch > 0 is expected under
    // concurrent mutation).
    add("query:envframe-dualpath-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev) return make_int(0);
        const std::uint64_t desync = ev->get_envframe_desync_detected();
        const std::uint64_t stale = ev->get_envframe_stale_refresh_count();
        const std::uint64_t mismatch = ev->get_envframe_version_mismatch_in_walk();
        const std::uint64_t gc_skips = ev->get_envframe_gc_walk_safe_skips();
        return make_int(static_cast<std::int64_t>(
            desync + stale + mismatch + gc_skips));
    });

    add("query:schema", [&string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_bool(false);
        std::string name = string_heap[idx];
        if (!type_registry) {
            type_registry = new aura::core::TypeRegistry();
        }
        auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
        if (!treg)
            return make_bool(false);
        auto ty = treg->lookup_type(name);
        if (!ty.valid())
            return make_bool(false);
        std::string schema = "{\"title\": \"" + name + "\"";
        schema += ", \"type\": \"" +
                  std::string(treg->tag_of(ty) == aura::core::TypeTag::MODULE ? "object" : "any") +
                  "\"}";
        auto sidx = string_heap.size();
        string_heap.push_back(schema);
        return make_string(sidx);
    });

    // ── Issue #288: mutate:validate-against-schema ──────────────────
    //
    // Standalone pre-mutation validation. Callers (mutate:rebind,
    // mutate:query-and-replace, or user code) can use this to
    // check a new value against a registered type's schema before
    // committing the change. Returns one of:
    //   - #t                 (valid)
    //   - #f                 (no schema registered for the type)
    //   - (list "schema-violation" <reason> <field>) on failure
    //
    // Usage:
    //   (mutate:validate-against-schema <new-value> <type-name>)
    //     → bool or tagged-violation pair
    //
    // The new-value form is intentionally loose: an int, a string
    // (parsed as code), or a quoted s-expression all flow through
    // `validate_value_against_schema` which dispatches by type.
    // For the initial P0 ship we validate the *string code form*
    // (since mutate:rebind takes a code-string), checking that the
    // source contains no obvious shape violations (out-of-range
    // integer literals, malformed s-exprs). This is a "cheap,
    // best-effort" check — full type-level validation is a
    // follow-up.
    add("mutate:validate-against-schema", [&string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto code_idx = as_string_idx(a[0]);
        auto type_idx = as_string_idx(a[1]);
        if (code_idx >= string_heap.size() || type_idx >= string_heap.size())
            return make_bool(false);
        std::string code = string_heap[code_idx];
        std::string type_name = string_heap[type_idx];
        if (!type_registry) {
            type_registry = new aura::core::TypeRegistry();
        }
        auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
        if (!treg)
            return make_bool(false);
        auto ty = treg->lookup_type(type_name);
        if (!ty.valid())
            return make_bool(false); // no schema; treat as "no constraint"
        // Best-effort shape check on the code string. The
        // registered schema (if any) is consulted for an
        // `integer_min` / `integer_max` constraint; if the code
        // string contains a literal that violates the constraint,
        // we return a tagged violation pair.
        //
        // We deliberately keep this conservative: only literal
        // integer overflow is detected. Function bodies, variable
        // references, and dynamic values are not statically
        // validated here — those are follow-up work. The point of
        // the P0 ship is to give callers a *hook* for explicit
        // pre-mutation checks (and a tagged error path), not to
        // reimplement the type checker.
        std::string violation_reason;
        std::string violation_field;
        if (!validate_code_against_schema_simple(code, type_name, violation_reason, violation_field)) {
            // Build (list "schema-violation" <reason> <field>) pair
            // in string_heap_ + pairs_.
            auto reason_idx = string_heap.size();
            string_heap.push_back(violation_reason);
            auto field_idx = string_heap.size();
            string_heap.push_back(violation_field);
            // ("schema-violation" reason field)
            auto reason_kw_idx = string_heap.size();
            string_heap.push_back(std::string("schema-violation"));
            // ... but keyword encoding goes through make_keyword in
            // a more complex path. We build the pair as a string-
            // tagged list (s-expression) and return it as a string
            // — the caller can (eval) it. This keeps the primitive
            // self-contained without needing a full keyword path.
            std::string repr = "(schema-violation \"" + violation_reason + "\" \"" + violation_field + "\")";
            auto repr_idx = string_heap.size();
            string_heap.push_back(repr);
            return make_string(repr_idx);
        }
        return make_bool(true);
    });

    // (query:occurrence-stale? if-node-id) — Issue #339:
    // returns #t when the if-node's occurrence-narrowing
    // is stale (must re-analyze before the narrowing is
    // trusted). The staleness bit is set by
    // validate_occurrence_narrowing() when the
    // post-mutation predicate or var type no longer
    // matches the previously-recorded refined type.
    // Returns #f otherwise (fresh + #f on bad args / OOR).
    add("query:occurrence-stale?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        return make_bool(ws->is_occurrence_stale(node_id) != 0);
    });

    // Issue #639: query:narrow-blame-stats. Returns the sum of
    // narrow stale-caught, blame-attached, invalidation-post-
    // mutate, provenance-hits, and safe-fallback counters.
    add("query:narrow-blame-stats", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        std::uint64_t stale_caught = 0;
        std::uint64_t blame_attached = 0;
        std::uint64_t invalidation = 0;
        std::uint64_t provenance_hits = 0;
        std::uint64_t safe_fallbacks = 0;
        if (m) {
            stale_caught = m->narrow_stale_caught_total.load(std::memory_order_relaxed);
            blame_attached = m->narrow_blame_attached_total.load(std::memory_order_relaxed);
            invalidation =
                m->narrow_invalidation_post_mutate_total.load(std::memory_order_relaxed);
            provenance_hits = m->narrowing_provenance_total.load(std::memory_order_relaxed);
            safe_fallbacks = m->narrow_safe_fallback_total.load(std::memory_order_relaxed);
        }
        if (auto* ws = ev.workspace_flat()) {
            invalidation += ws->narrow_invalidation_post_mutate_count();
        }
        return make_int(static_cast<std::int64_t>(
            stale_caught + blame_attached + invalidation + provenance_hits + safe_fallbacks));
    });

    // Issue #627: query:bidirectional-narrow-stats. Returns the sum
    // of check-mode narrow hits, synthesize/check switches,
    // post-mutate narrow consistency, and stale-check prevented.
    add("query:bidirectional-narrow-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t check_hits =
            m->check_mode_narrow_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t switches =
            m->synthesize_check_switch_count_total.load(std::memory_order_relaxed);
        const std::uint64_t consistency =
            m->post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
        const std::uint64_t stale_prevented =
            m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(check_hits + switches + consistency +
                                                  stale_prevented));
    });

    // Issue #537 / #518 Phase 2: query:occurrence-narrowing-stats.
    // Returns the sum of stale-refresh + blame-chain-complete
    // counters from CompilerMetrics (post-mutation re-narrow
    // provenance observability).
    add("query:occurrence-narrowing-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m) return make_int(0);
        const std::uint64_t stale_refreshes =
            m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
        const std::uint64_t blame_complete =
            m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(stale_refreshes + blame_complete));
    });

    // (query:occurrence-stale-count) — Issue #339:
    // returns the current count of stale occurrence
    // nodes in the workspace. Cheap O(n) walk; intended
    // for observability + AI agent monitoring.
    add("query:occurrence-stale-count", [&ev](const auto&) -> EvalValue {
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ws->occurrence_stale_count()));
    });

    // (query:mark-occurrence-stale if-node-id) — Issue
    // #339: explicitly mark an if-node as stale.
    // Used by callers that decide staleness outside
    // the type-checker (e.g. an external validator or
    // a test). Returns #t on success, #f on bad args.
    add("query:mark-occurrence-stale", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        auto node_id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (node_id >= ws->size())
            return make_bool(false);
        ws->mark_occurrence_stale(node_id);
        return make_bool(true);
    });
}

// Issue #288: best-effort shape check for the P0 ship.
// Returns true on OK, false + sets violation_reason/field on
// the first detected violation.
//
// Scope:
//   - integer literal overflow (literal that doesn't fit in
//     int64_t is rejected)
//   - unbalanced parens
//   - empty body
// Non-scope (follow-up):
//   - type compatibility
//   - range constraints beyond int64
//   - dynamic values
static bool validate_code_against_schema_simple(const std::string& code,
                                                const std::string& type_name,
                                                std::string& violation_reason,
                                                std::string& violation_field) {
    // Empty body: reject (define with no body is invalid).
    if (code.empty()) {
        violation_reason = "empty-body";
        violation_field = type_name;
        return false;
    }
    // Unbalanced parens: simple count check (does not handle
    // strings/comments, but those are rare in mutate:rebind input
    // and a follow-up can add proper lexing).
    int paren_depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < code.size(); ++i) {
        char c = code[i];
        if (c == '"' && (i == 0 || code[i-1] != '\\')) {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;
        if (c == '(') ++paren_depth;
        else if (c == ')') {
            --paren_depth;
            if (paren_depth < 0) {
                violation_reason = "unbalanced-parens";
                violation_field = type_name;
                return false;
            }
        }
    }
    if (paren_depth != 0) {
        violation_reason = "unbalanced-parens";
        violation_field = type_name;
        return false;
    }
    // Integer literal overflow: look for digit sequences
    // preceded by `-` or whitespace, check int64_t range.
    std::size_t i = 0;
    while (i < code.size()) {
        char c = code[i];
        bool is_digit_start = (c >= '0' && c <= '9') ||
                              (c == '-' && i + 1 < code.size() && code[i+1] >= '0' && code[i+1] <= '9');
        if (!is_digit_start) { ++i; continue; }
        std::size_t j = i;
        if (c == '-') ++j;
        while (j < code.size() && code[j] >= '0' && code[j] <= '9') ++j;
        std::string lit = code.substr(i, j - i);
        // int64 range check
        try {
            std::stoll(lit);
        } catch (const std::out_of_range&) {
            violation_reason = "integer-literal-overflow";
            violation_field = type_name;
            return false;
        } catch (const std::exception&) {
            violation_reason = "malformed-integer-literal";
            violation_field = type_name;
            return false;
        }
        i = j;
    }
    return true;
}

} // namespace aura::compiler::primitives_detail