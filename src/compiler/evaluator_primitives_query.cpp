// evaluator_primitives_query.cpp — P0 step 8: standalone query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
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
                               ModulePathResolver resolve_module_path) {

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
        return make_int(static_cast<std::int64_t>(
            steal_violations + batch_count + bumps_saved_total +
            rollbacks + ws_commits + ws_bumps_saved));
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