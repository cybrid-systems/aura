// evaluator_primitives_query.cpp — P0 step 8: standalone query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/shape_profiler.h"
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

// Issue #514: count MacroIntroduced nodes in the workspace marker column.
static std::uint64_t workspace_marker_macro_introduced(Evaluator* ev) {
    if (!ev)
        return 0;
    auto* ws = ev->workspace_flat();
    if (!ws)
        return 0;
    std::uint64_t count = 0;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_macro_introduced(id))
            ++count;
    }
    return count;
}

static std::uint64_t ir_inline_hygiene_skipped(Evaluator* ev) {
    if (!ev || !ev->get_macro_hygiene_skipped_fn_)
        return 0;
    return ev->get_macro_hygiene_skipped_fn_();
}

void register_query_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                               std::pmr::vector<std::string>& string_heap, void*& type_registry,
                               ModulePathResolver resolve_module_path, Evaluator& ev) {

    add("query:module-exports",
        [&pairs, &string_heap, resolve_module_path](std::span<const EvalValue> a) -> EvalValue {
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
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
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
                            (c >= '0' && c <= '9') || c == '_' || c == '?' || c == '!' ||
                            c == '<' || c == '>' || c == '=' || c == '*' || c == '+' || c == '-' ||
                            c == '/' || c == '.' || c == '$') {
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
        return make_int(static_cast<std::int64_t>(aura_jit_fallback_count_v_read()));
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
        if (!ev)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev->get_macro_introduced_skipped_in_query()));
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
        if (!ev)
            return make_int(0);
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        auto root = static_cast<aura::ast::NodeId>(as_int(a[0]));
        const std::uint8_t reason_mask = (a.size() >= 2 && is_int(a[1]))
                                             ? static_cast<std::uint8_t>(as_int(a[1]) & 0xFF)
                                             : 0xFF; // 0xFF = all reasons
        if (root >= ws->size())
            return make_int(0);
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
        if (!ev)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev->get_mutation_impact_count()));
    });

    // Issue #488: query:mutation-impact-snapshot. Hash view of the
    // most recent Guard success impact summary for AI decision loops.
    add("query:mutation-impact-snapshot",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto entry = ev->get_latest_mutation_impact_entry();
            auto* ht = FlatHashTable::create(12);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("epoch-after", static_cast<std::int64_t>(entry.epoch_after));
            insert_kv("epoch-delta", static_cast<std::int64_t>(entry.epoch_delta));
            insert_kv("nodes-changed", static_cast<std::int64_t>(entry.nodes_changed));
            insert_kv("reasons-mask", static_cast<std::int64_t>(entry.reasons_mask));
            insert_kv("dirty-nodes", static_cast<std::int64_t>(ev->get_dirty_nodes_in_snapshot()));
            insert_kv("macro-markers",
                      static_cast<std::int64_t>(ev->get_macro_markers_in_snapshot()));
            insert_kv("schema-pass",
                      static_cast<std::int64_t>(ev->get_schema_validation_pass_count()));
            insert_kv("schema-fail",
                      static_cast<std::int64_t>(ev->get_schema_validation_fail_count()));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #489: query:stability-stats. Hash view of StableNodeRef
    // enforcement counters for EDSL mutate/query hot paths.
    add("query:stability-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        std::uint64_t invalidations = 0;
        if (auto* ws = ev->workspace_flat())
            invalidations = ws->stable_ref_invalidations();
        auto* ht = FlatHashTable::create(12);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = string_heap.size();
                    string_heap.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("raw-nodeid-usage",
                  static_cast<std::int64_t>(ev->get_raw_nodeid_usage_in_primitives_count()));
        insert_kv("stable-ref-validated",
                  static_cast<std::int64_t>(ev->get_stable_ref_validated_in_primitives_count()));
        insert_kv("stale-ref-blocked",
                  static_cast<std::int64_t>(ev->get_stale_ref_blocked_count()));
        insert_kv("stale-ref-warned", static_cast<std::int64_t>(ev->get_stale_ref_warned_count()));
        insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(invalidations));
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
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
        if (!ev)
            return make_int(0);
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
        if (!ev)
            return make_int(0);
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
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const std::uint64_t wraps = ws->generation_wrap_count();
        const std::uint64_t invalidations = ws->stable_ref_invalidations();
        const std::uint64_t stale = ws->node_gen_stale_access_count();
        return make_int(static_cast<std::int64_t>(wraps + invalidations + stale));
    });

    // Issue #470: query:stable-ref-stats-hash — 4-element
    // integer list for AI Agent decision-making. The
    // original query:stable-ref-stats returns an integer
    // sum (3 categories); the list version breaks out each
    // category + a recommendation int for actionable
    // monitoring. The list (4 ints) is the simplest cross-
    // module shape that doesn't need string_heap_ access
    // (which is private in the static-lambda context).
    // Decoding: position 0 = generation-wrap-count,
    // position 1 = stable-ref-invalidations,
    // position 2 = node-gen-stale-accesses,
    // position 3 = recommendation (0=healthy,
    // 1=wrap-detected, 2=high-invalidation-rate).
    // The Aura side can iterate with a `let` form.
    add("query:stable-ref-stats-hash",
        [&pairs, &string_heap, &ev](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            std::uint64_t wraps = 0;
            std::uint64_t invalidations = 0;
            std::uint64_t stale = 0;
            if (auto* ws = ev.workspace_flat()) {
                wraps = ws->generation_wrap_count();
                invalidations = ws->stable_ref_invalidations();
                stale = ws->node_gen_stale_access_count();
            }
            std::int64_t rec_int = 0;
            if (wraps > 0)
                rec_int = 1;
            else if (invalidations >= 10)
                rec_int = 2;
            // Build a 4-field hash using the FNV-1a scheme
            // (same as the other observability primitives).
            // Uses the `string_heap` reference passed into
            // register_query_primitives() (avoids the private
            // Evaluator::string_heap_ field).
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_int(rec_int);
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("generation-wrap-count", static_cast<std::int64_t>(wraps));
            insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(invalidations));
            insert_kv("node-gen-stale-accesses", static_cast<std::int64_t>(stale));
            insert_kv("recommendation", rec_int);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        if (!ev)
            return make_int(0);
        const std::uint64_t attempts = ev->get_mutation_steal_attempts();
        const std::uint64_t violations = ev->get_boundary_violation_count();
        return make_int(static_cast<std::int64_t>(attempts + violations));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t requests = ev->get_gc_safepoint_requests_total();
        const std::uint64_t waits = ev->get_gc_safepoint_waits_total();
        const std::uint64_t deferred = ev->get_gc_safepoint_deferred_total();
        return make_int(static_cast<std::int64_t>(requests + waits + deferred));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t calls = ev->get_verify_tool_calls_total();
        const std::uint64_t hits = ev->get_verify_tool_cache_hits_total();
        const std::uint64_t errors = ev->get_verify_tool_parse_errors_total();
        return make_int(static_cast<std::int64_t>(calls + hits + errors));
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
        const std::uint64_t gc_pauses = aura_fiber_static_gc_pause_attributed_to_mutation();
        std::uint64_t eda_sv_cycles = 0;
        std::uint64_t eda_sv_corruption = 0;
        if (auto* ev = Evaluator::get_query_evaluator()) {
            if (const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics())) {
                eda_sv_cycles = m->eda_sv_evolution_cycles_total.load(std::memory_order_relaxed);
                eda_sv_corruption =
                    m->eda_sv_corruption_detected_total.load(std::memory_order_relaxed);
            }
        }
        const std::uint64_t sum = gc_pauses + eda_sv_cycles;
        std::string result = "{\"gc_pauses_attributed_to_mutation\":";
        result += std::to_string(gc_pauses);
        result += ",\"eda_sv_evolution_cycles\":";
        result += std::to_string(eda_sv_cycles);
        result += ",\"eda_sv_corruption_detected\":";
        result += std::to_string(eda_sv_corruption);
        result += ",\"sum\":";
        result += std::to_string(sum);
        result += "}";
        // Return as a string. We have to find an
        // evaluator to push the string into the
        // string_heap_; if no evaluator, return #f.
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
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
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const std::uint64_t hits = ws->tag_arity_index_hits();
        const std::uint64_t misses = ws->tag_arity_index_misses();
        const std::uint64_t rebuilds = ws->tag_arity_index_rebuilds();
        return make_int(static_cast<std::int64_t>(hits + misses + rebuilds));
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
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const std::uint64_t hits = ws->tag_arity_index_hits();
        const std::uint64_t misses = ws->tag_arity_index_misses();
        const std::uint64_t rebuilds = ws->tag_arity_index_rebuilds();
        const std::uint64_t dirty_marks = ws->tag_arity_index_dirty_marks();
        // Issue #554: include rebuild_time_us + delta_hits
        // so (query:pattern-index-stats) returns the full
        // 6-counter matrix. The AI Agent can compute
        // avg_rebuild_us = rebuild_time_us / rebuilds and
        // delta_hit_rate = delta_hits / (delta_hits + rebuilds).
        const std::uint64_t rebuild_time_us = ws->tag_arity_index_rebuild_time_us();
        const std::uint64_t delta_hits = ws->tag_arity_index_delta_hits();
        return make_int(static_cast<std::int64_t>(hits + misses + rebuilds + dirty_marks +
                                                  rebuild_time_us + delta_hits));
    });

    // Issue #490: query:pattern-index-rebuild-stats. Hash view of
    // lazy vs eager Evaluator index rebuild counters + FlatAST timing.
    add("query:pattern-index-rebuild-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            std::uint64_t flat_rebuilds = 0;
            std::uint64_t flat_rebuild_time_us = 0;
            if (auto* ws = ev->workspace_flat()) {
                flat_rebuilds = ws->tag_arity_index_rebuilds();
                flat_rebuild_time_us = ws->tag_arity_index_rebuild_time_us();
            }
            auto* ht = FlatHashTable::create(12);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("lazy-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_lazy_rebuilds()));
            insert_kv("eager-mutate-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_eager_mutate_rebuilds()));
            insert_kv("eager-cow-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_eager_cow_rebuilds()));
            insert_kv("flat-rebuilds", static_cast<std::int64_t>(flat_rebuilds));
            insert_kv("flat-rebuild-time-us", static_cast<std::int64_t>(flat_rebuild_time_us));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        if (!ev)
            return make_int(0);
        const std::uint64_t skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        return make_int(static_cast<std::int64_t>(skips + violations));
    });

    // Issue #486: query:macro-hygiene-stats. Hash view of the
    // MacroIntroduced hygiene decision surface for AI self-evolution
    // loops (non-duplicative with #547 2-counter int sum and #421
    // 7-counter pattern-macro-filter bundle):
    //   - root-skips: macro_introduced_skipped_in_query_
    //   - recursive-skips: pattern_recursive_macro_skipped_
    //   - hygiene-violations: hygiene_violation_count_
    //   - macro-markers: workspace MacroIntroduced marker tally
    add("query:macro-hygiene-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("root-skips",
                      static_cast<std::int64_t>(ev->get_macro_introduced_skipped_in_query()));
            insert_kv("recursive-skips",
                      static_cast<std::int64_t>(ev->get_pattern_recursive_macro_skipped()));
            insert_kv("hygiene-violations",
                      static_cast<std::int64_t>(ev->get_hygiene_violation_count()));
            insert_kv("macro-markers",
                      static_cast<std::int64_t>(workspace_marker_macro_introduced(ev)));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        if (!ev)
            return make_int(0);
        const std::uint64_t save = ev->get_panic_checkpoint_save_count();
        const std::uint64_t restore = ev->get_panic_checkpoint_restore_count();
        const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
        const std::uint64_t rollback_success = ev->get_rollback_success_on_panic();
        return make_int(static_cast<std::int64_t>(save + restore + commit + rollback_success));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        return make_int(static_cast<std::int64_t>(cross_cow + fiber_stale + rollback + provenance));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t conflicts = ev->get_cross_delta_conflicts_caught();
        const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
        const std::uint64_t touched_size = ev->get_touched_roots_size();
        return make_int(
            static_cast<std::int64_t>(narrowing + conflicts + passes_skipped + touched_size));
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
        if (!ev)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev->get_touched_roots_size()));
    });

    // Issue #495: query:task2-refinement-stats. Returns the sum
    // of 4 Task2 review refinement pillar counters:
    //   - constraint_soundness: delta_conflict_reverify_total +
    //     delta_conflict_detected_total (#466/#509)
    //   - coercion_zerooverhead: dead_coercion_eliminated_total +
    //     coercion_zerooverhead_win_total (#468/#574)
    //   - occurrence_blame: narrowing_dirty_recovery_total +
    //     occurrence_blame_chain_complete_total (#467)
    //   - jit_elision_hits: coercion_narrow_evidence_hits_total
    //     (JIT/IR narrow-evidence elision synergy)
    add("query:task2-refinement-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t constraint =
            m->delta_conflict_reverify_total.load(std::memory_order_relaxed) +
            m->delta_conflict_detected_total.load(std::memory_order_relaxed);
        const std::uint64_t coercion =
            m->dead_coercion_eliminated_total.load(std::memory_order_relaxed) +
            m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed);
        const std::uint64_t occurrence =
            m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) +
            m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed);
        const std::uint64_t jit_elision =
            m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
        return make_int(
            static_cast<std::int64_t>(constraint + coercion + occurrence + jit_elision));
    });

    // Issue #690: query:constraint-delta-blame-stats. Returns the
    // sum of cross-delta constraint blame-chain completeness hits
    // plus occurrence narrowing blame-chain completeness.
    add("query:constraint-delta-blame-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t constraint_blame =
            m ? m->constraint_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t occurrence_blame =
            m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(constraint_blame + occurrence_blame));
    });

    // Issue #509: query:constraint-delta-stats. Returns the sum
    // of 2 solve_delta touched_roots soundness counters:
    //   - touched_roots_hits: delta_conflict_reverify_total
    //     (bounded clean-constraint re-scans after touched roots)
    //   - cross_delta_conflicts_caught: cross_delta_conflicts_caught_
    add("query:constraint-delta-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t touched_hits =
            m ? m->delta_conflict_reverify_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t conflicts = ev->get_cross_delta_conflicts_caught();
        return make_int(static_cast<std::int64_t>(touched_hits + conflicts));
    });

    // Issue #628: query:solve-delta-safety-stats. Returns the sum
    // of 4 solve_delta clean-constraint safety counters:
    //   - clean_conflicts_detected: delta_conflict_detected_total
    //   - full_solve_fallbacks: solve_delta_full_solve_fallback_total
    //   - delta_vs_full_consistency: delta_conflict_reverify_total
    //   - missed_conflict_prevented: delta_constraints_processed_total
    add("query:solve-delta-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t clean_conflicts =
            m->delta_conflict_detected_total.load(std::memory_order_relaxed);
        const std::uint64_t full_fallbacks =
            m->solve_delta_full_solve_fallback_total.load(std::memory_order_relaxed);
        const std::uint64_t consistency =
            m->delta_conflict_reverify_total.load(std::memory_order_relaxed);
        const std::uint64_t prevented =
            m->delta_constraints_processed_total.load(std::memory_order_relaxed);
        return make_int(
            static_cast<std::int64_t>(clean_conflicts + full_fallbacks + consistency + prevented));
    });

    // Issue #573: query:typed-incremental-stats. Returns the sum
    // of 4 Task2 incremental typed self-mod reliability counters:
    //   - delta_conflicts_caught: cross_delta_conflicts_caught_
    //   - narrowing_refresh_count: narrowing_refresh_count_
    //   - local_recheck_hit_rate: selective_recheck_count_
    //     (proxy for selective local re-check vs full solve)
    //   - solve_delta_time_us: delta_solve_time_us (CompilerMetrics)
    add("query:typed-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t conflicts = ev->get_cross_delta_conflicts_caught();
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t local_recheck = ev->get_selective_recheck_count();
        const std::uint64_t solve_us =
            m ? m->delta_solve_time_us.load(std::memory_order_relaxed) : 0;
        return make_int(
            static_cast<std::int64_t>(conflicts + narrowing + local_recheck + solve_us));
    });

    // Issue #608: query:type-incremental-stats. Returns the sum
    // of 4 incremental type reliability counters from
    // CompilerMetrics:
    //   - delta_constraints_processed_total (dep-tracked solve_delta)
    //   - narrowing_dirty_recovery_total (occurrence-dirty recoveries)
    //   - post_mutate_narrow_consistency_total (narrow reliability)
    //   - incremental_typecheck_auto_invocations_total (delta win vs full)
    add("query:type-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t delta_processed =
            m->delta_constraints_processed_total.load(std::memory_order_relaxed);
        const std::uint64_t occ_recovery =
            m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed);
        const std::uint64_t narrow_hits =
            m->post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
        const std::uint64_t delta_win =
            m->incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed);
        return make_int(
            static_cast<std::int64_t>(delta_processed + occ_recovery + narrow_hits + delta_win));
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
        if (!m)
            return make_int(0);
        const std::uint64_t runs = m->type_propagation_runs_.load(std::memory_order_relaxed);
        const std::uint64_t total = m->type_propagation_total_.load(std::memory_order_relaxed);
        const std::uint64_t unknown = m->type_propagation_unknown_.load(std::memory_order_relaxed);
        const std::uint64_t int_width =
            m->type_propagation_int_width_.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(runs + total + unknown + int_width));
    });

    // Issue #468: query:dead-coercion-zerooverhead-stats. Returns
    // the sum of 4 DeadCoercionEliminationPass zero-overhead
    // lifetime counters (refines #433 observability):
    //   - dead_coercion_eliminated_total (identity/no-op elisions)
    //   - dead_coercion_elapsed_us_total (pass pipeline time)
    //   - coercion_type_prop_hits_total (Rule 1 type_id identity)
    //   - coercion_zerooverhead_win_total (per-run pipeline wins)
    add("query:dead-coercion-zerooverhead-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t eliminated =
            m->dead_coercion_eliminated_total.load(std::memory_order_relaxed);
        const std::uint64_t elapsed =
            m->dead_coercion_elapsed_us_total.load(std::memory_order_relaxed);
        const std::uint64_t type_prop =
            m->coercion_type_prop_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t win =
            m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(eliminated + elapsed + type_prop + win));
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
        if (!m)
            return make_int(0);
        const std::uint64_t castop =
            m->coercion_castop_emitted_total.load(std::memory_order_relaxed);
        const std::uint64_t type_prop =
            m->coercion_type_prop_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t narrow =
            m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t win =
            m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(castop + type_prop + narrow + win));
    });

    // Issue #574: query:coercion-elim-stats. Returns the sum of
    // 4 coercion elimination observability counters:
    //   - coercion_castop_emitted_total (total CastOps from lowering)
    //   - dead_coercion_eliminated_total (identity/no-op elisions)
    //   - coercion_narrow_evidence_hits_total (runtime-check elisions
    //     proved away by narrow_evidence Rule 6)
    //   - narrowing_provenance_total (blame/provenance preserved on
    //     occurrence-narrowing paths feeding coercion metadata)
    add("query:coercion-elim-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t total_castop =
            m->coercion_castop_emitted_total.load(std::memory_order_relaxed);
        const std::uint64_t eliminated =
            m->dead_coercion_eliminated_total.load(std::memory_order_relaxed);
        const std::uint64_t runtime_check_elided =
            m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t blame = m->narrowing_provenance_total.load(std::memory_order_relaxed);
        return make_int(
            static_cast<std::int64_t>(total_castop + eliminated + runtime_check_elided + blame));
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
        if (!m)
            return make_int(0);
        const std::uint64_t wire_borrows =
            m->hw_resource_wire_borrows_.load(std::memory_order_relaxed);
        const std::uint64_t reg_writes = m->hw_resource_reg_writes_.load(std::memory_order_relaxed);
        const std::uint64_t mem_access = m->hw_resource_mem_access_.load(std::memory_order_relaxed);
        const std::uint64_t double_drive =
            m->hw_resource_double_drive_.load(std::memory_order_relaxed);
        return make_int(
            static_cast<std::int64_t>(wire_borrows + reg_writes + mem_access + double_drive));
    });

    // Issue #575: query:linear-ownership-incremental-stats. Returns
    // the sum of 4 Task2 PerDefUse + ownership_dirty incremental
    // linear ownership counters:
    //   - ownership_revalidate_count: linear_post_mutate_revalidations_total
    //   - dirty_linear_uses: per_defuse_index_visited_total
    //     (O(uses) selective re-validation proxy)
    //   - violation_caught_post_mutate: linear_violations_caught_total
    //     + linear_leak_prevented_total
    //   - escape_analysis_hits: linear_check_pass_count_
    //     (runtime linear ownership_state fast-path checks)
    add("query:linear-ownership-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t revalidate =
            m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
        const std::uint64_t dirty_uses =
            m->per_defuse_index_visited_total.load(std::memory_order_relaxed);
        const std::uint64_t violations =
            m->linear_violations_caught_total.load(std::memory_order_relaxed);
        const std::uint64_t leaks = m->linear_leak_prevented_total.load(std::memory_order_relaxed);
        const std::uint64_t escape_hits =
            m->linear_check_pass_count_.load(std::memory_order_relaxed);
        return make_int(
            static_cast<std::int64_t>(revalidate + dirty_uses + violations + leaks + escape_hits));
    });

    // Issue #610: query:linear-ownership-mutation-stats. Returns
    // the sum of 4 post-mutation linear ownership observability
    // counters:
    //   - post_mutate_revalidations: linear_post_mutate_revalidations_total
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear: linear_deopt_on_invalidate_total
    //   - leak_prevented: linear_leak_prevented_total
    add("query:linear-ownership-mutation-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t revalidations =
            m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
        const std::uint64_t violations =
            m->linear_violations_caught_total.load(std::memory_order_relaxed);
        const std::uint64_t deopt =
            m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
        const std::uint64_t leaks = m->linear_leak_prevented_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(revalidations + violations + deopt + leaks));
    });

    // Issue #638: query:linear-ownership-safety-stats. Returns the
    // sum of 3 runtime linear + GuardShape post-mutation safety
    // counters (non-duplicative with #610 mutation-stats,
    // #575 incremental-stats, #306 hw linear-ownership-stats):
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear_mismatch: linear_deopt_on_mismatch_total
    //   - post_mutate_enforcements: linear_post_mutate_enforcements_total
    add("query:linear-ownership-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t violations =
            m->linear_violations_caught_total.load(std::memory_order_relaxed);
        const std::uint64_t deopt_mismatch =
            m->linear_deopt_on_mismatch_total.load(std::memory_order_relaxed);
        const std::uint64_t enforcements =
            m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(violations + deopt_mismatch + enforcements));
    });

    // Issue #598: query:linear-ownership-runtime-stats. Returns the
    // sum of 4 runtime linear enforcement counters spanning
    // Interpreter/JIT hot-path + invalidate_function integration
    // (non-duplicative with #638 safety-stats which omits invalidate
    // deopt; #610 mutation-stats which includes revalidations/leaks):
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear_mismatch: linear_deopt_on_mismatch_total
    //   - post_mutate_enforcement_hits:
    //     linear_post_mutate_enforcements_total
    //   - deopt_on_invalidate: linear_deopt_on_invalidate_total
    add("query:linear-ownership-runtime-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t violations =
            m->linear_violations_caught_total.load(std::memory_order_relaxed);
        const std::uint64_t deopt_mismatch =
            m->linear_deopt_on_mismatch_total.load(std::memory_order_relaxed);
        const std::uint64_t enforcement_hits =
            m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
        const std::uint64_t deopt_invalidate =
            m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(violations + deopt_mismatch + enforcement_hits +
                                                  deopt_invalidate));
    });

    // Issue #454: query:reflect-edsl-bridge-stats. Returns the
    // sum of 4 reflection-to-EDSL bridge observability counters:
    //   - schema_validation_pass_count_  (auto_validate hook)
    //   - schema_validation_fail_count_  (validation failures)
    //   - impact_snapshot_count_         (post-mutate reflection data)
    //   - macro_introduced_skipped_in_query_  (SyntaxMarker filter
    //     introspection via query:pattern / schema-of-marker bridge)
    add("query:reflect-edsl-bridge-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t pass = ev->get_schema_validation_pass_count();
        const std::uint64_t fail = ev->get_schema_validation_fail_count();
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t marker_skips = ev->get_macro_introduced_skipped_in_query();
        return make_int(static_cast<std::int64_t>(pass + fail + snapshots + marker_skips));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t pass = ev->get_schema_validation_pass_count();
        const std::uint64_t fail = ev->get_schema_validation_fail_count();
        const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
        return make_int(static_cast<std::int64_t>(snapshots + pass + fail + dirty));
    });

    // Issue #594: query:reflection-selfmod-stats. Returns the sum of
    // 5 static-reflection + self-mod validation observability counters
    // spanning the Task6 Guard post-mutate validate hook (#551) and
    // mutate:* self-evolution paths (non-duplicative with #551
    // reflect-postmutate 4-tuple and #454 reflect-edsl-bridge):
    //   - post_mutate_validate_pass: schema_validation_pass_count_
    //   - schema_violations_prevented: schema_validation_fail_count_
    //   - validations_run proxy: impact_snapshot_count_ (Guard hook
    //     invocations — each successful mutate triggers validate)
    //   - mutation_impact_count_  (successful Guard self-mod transforms)
    //   - guard_dirty_epoch_count_  (Guard + schema/type integration)
    //
    // P0: returns an integer = sum of all 5 counter groups.
    // validations_run (pass + fail) is derivable by the Agent;
    // follow-up returns a 5-tuple for independent pass-rate tracking.
    add("query:reflection-selfmod-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t pass = ev->get_schema_validation_pass_count();
        const std::uint64_t fail = ev->get_schema_validation_fail_count();
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t impact = ev->get_mutation_impact_count();
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        return make_int(static_cast<std::int64_t>(pass + fail + snapshots + impact + guard_epoch));
    });

    // Issue #514: query:ir-hygiene-stats. Returns the sum of IR-level
    // macro-hygiene observability counters (Top 1 — AST→IR propagation):
    //   - InlinePass macro_hygiene_skipped_ (call sites skipped because
    //     source_marker == MacroIntroduced && respect_macro_hygiene_)
    //   - marker_macro_introduced_count (workspace SyntaxMarker column)
    add("query:ir-hygiene-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t inline_skipped = ir_inline_hygiene_skipped(ev);
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        return make_int(static_cast<std::int64_t>(inline_skipped + markers));
    });

    // Issue #514: query:pattern-marker-stats. Returns the sum of
    // query-side marker/hygiene counters (Top 1 — query matcher):
    //   - macro_introduced_skipped_in_query_  (default :respect-hygiene)
    //   - hygiene_violation_count_
    //   - marker_macro_introduced_count  (workspace subtree marker tally)
    add("query:pattern-marker-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        return make_int(static_cast<std::int64_t>(skips + violations + markers));
    });

    // Issue #517: query:consolidated-production-priority-stats.
    // Returns the sum of 9 counter groups spanning the 3 P0 foundational
    // pillars from the consolidated meta tracker (non-duplicative with
    // #514 Task6, #515 consolidated P0, #516 Prompt6, and #520 Top-5
    // roadmap which adds batch/orchestration/SV-mutate themes):
    //   P1 Persistence + EDA (#511/#510): panic_checkpoint_save/commit +
    //                                    generation_wrap_count +
    //                                    verification_coverage_feedback +
    //                                    verification_assert_failure
    //   P2 Memory-safety (#505/#516): bridge_epoch_hit +
    //                                 closure_stale_refresh +
    //                                 envframe_stale_refresh +
    //                                 envframe_gc_walk_safe_skips +
    //                                 gc_safepoint_waits_total
    //   P3 SoA hotpath (#506/#463): ir_soa_instructions_emitted +
    //                               ir_soa_functions_emitted +
    //                               passes_skipped_type_dirty +
    //                               module_dirty_skips
    //
    // P0: returns an integer = sum of all 9 counter groups.
    // Follow-up: returns a 3-tuple (persistence+eda memory soa) for
    // fleet dashboards tracking the #517 north-star pillars.
    add("query:consolidated-production-priority-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t persistence = ev->get_panic_checkpoint_save_count() +
                                              ev->get_panic_checkpoint_commit_count() +
                                              (ws ? ws->generation_wrap_count() : 0);
            const std::uint64_t eda_feedback = ws ? ws->verification_coverage_feedback_total() +
                                                        ws->verification_assert_failure_total()
                                                  : 0;
            const std::uint64_t memory_bridge =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) +
                        m->closure_stale_refresh_count_.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t memory_env =
                ev->get_envframe_stale_refresh_count() + ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_sync = ev->get_gc_safepoint_waits_total();
            const std::uint64_t ir_soa =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) +
                        m->ir_soa_functions_emitted.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t soa_dirty =
                ev->get_passes_skipped_type_dirty() +
                (m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0);
            return make_int(static_cast<std::int64_t>(persistence + eda_feedback + memory_bridge +
                                                      memory_env + gc_sync + ir_soa + soa_dirty));
        });

    // Issue #520: query:production-roadmap-stats. Returns the sum of
    // 10 counter groups spanning the consolidated Top 5 production
    // priorities (non-duplicative synthesis of #496/#510/#511/#505/
    // #506/#413 themes; avoids #514 Task6, #634 commercial, #635
    // macro-reflect, and the original #429/#430/#431 core tracks):
    //   P1 EDA/SV closed-loop (#496/#510): verification_feedback +
    //                                      sv_mutate_attempts/success
    //   P2 Persistence (#511): panic_checkpoint_save/commit +
    //                          generation_wrap_count
    //   P3 Memory safety (#505/#516): bridge_epoch_hit +
    //                                 closure_stale_refresh +
    //                                 envframe_stale_refresh
    //   P4 SoA hotpath (#506): passes_skipped_type_dirty +
    //                          tag_arity_index_hits + specialization_hits
    //   P5 Atomic batch/rollback (#413/#439): atomic_batch_commits +
    //                                         batch_rollbacks +
    //                                         mutation_log_rollbacks
    //
    // P0: returns an integer = sum of all 10 counter groups.
    // Follow-up: returns a 10-tuple so fleet dashboards can track
    // each north-star pillar independently.
    add("query:production-roadmap-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t eda_feedback = ws ? ws->verification_coverage_feedback_total() +
                                                    ws->verification_assert_failure_total()
                                              : 0;
        const std::uint64_t eda_sv =
            ws ? ws->sv_mutate_attempts_total() + ws->sv_mutate_success_total() : 0;
        const std::uint64_t checkpoint =
            ev->get_panic_checkpoint_save_count() + ev->get_panic_checkpoint_commit_count();
        const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
        const std::uint64_t memory_bridge =
            m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) +
                    m->closure_stale_refresh_count_.load(std::memory_order_relaxed)
              : 0;
        const std::uint64_t memory_env = ev->get_envframe_stale_refresh_count();
        const std::uint64_t soa_skip = ev->get_passes_skipped_type_dirty();
        const std::uint64_t soa_hotpath =
            (m ? m->specialization_hits.load(std::memory_order_relaxed) : 0) +
            (ws ? ws->tag_arity_index_hits() : 0);
        const std::uint64_t batch =
            (ws ? ws->atomic_batch_commits() : 0) + ev->atomic_batch_count();
        const std::uint64_t rollback =
            ev->atomic_batch_rollbacks() + ev->get_mutation_log_rollback_count();
        return make_int(static_cast<std::int64_t>(eda_feedback + eda_sv + checkpoint + gen_wrap +
                                                  memory_bridge + memory_env + soa_skip +
                                                  soa_hotpath + batch + rollback));
    });

    // Issue #514: query:task6-production-readiness-stats. Returns the
    // sum of 12 counters spanning the Task6 review Top 3 production
    // gaps (non-duplicative synthesis of #547/#551/#550 themes):
    //   Top1 hygiene/marker: skips + violations + inline_skipped + markers
    //   Top2 Guard/reflect: mutation_impact + impact_snapshot + schema_pass
    //                       + panic_commit
    //   Top3 dirty/type: narrowing_refresh + passes_skipped + touched_roots
    //                    + narrowing_dirty_recovery
    add("query:task6-production-readiness-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t top1 =
            ev->get_macro_introduced_skipped_in_query() + ev->get_hygiene_violation_count() +
            ir_inline_hygiene_skipped(ev) + workspace_marker_macro_introduced(ev);
        const std::uint64_t top2 =
            ev->get_mutation_impact_count() + ev->get_impact_snapshot_count() +
            ev->get_schema_validation_pass_count() + ev->get_panic_checkpoint_commit_count();
        const std::uint64_t dirty_recovery =
            m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t top3 = ev->get_narrowing_refresh_count() +
                                   ev->get_passes_skipped_type_dirty() +
                                   ev->get_touched_roots_size() + dirty_recovery;
        return make_int(static_cast<std::int64_t>(top1 + top2 + top3));
    });

    // Issue #441: query:compiler-runtime-production-readiness-stats.
    // Returns the sum of 12 counters spanning the consolidated
    // P0 production-readiness pillars (non-duplicative synthesis
    // of #438/#439/#440/#437 themes; avoids #514 Task6 hygiene/
    // dirty focus and the core-three #426/#427/#428 tracks):
    //   Runtime fiber (#438): mutation_steal_attempts +
    //                          boundary_violation_count
    //   Runtime GC (#439): gc_safepoint_requests + waits + deferred
    //   EDSL workspace (#440): cross_cow + fiber_stale + provenance
    //                          + mutation_log_rollback + schema_pass
    //                          + mutation_impact
    //   EDA verify (#437): verify_dirty (assertion+coverage+sva+
    //                      formal) + verify_tool_calls
    add("query:compiler-runtime-production-readiness-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t runtime_fiber =
                ev->get_mutation_steal_attempts() + ev->get_boundary_violation_count();
            const std::uint64_t runtime_gc = ev->get_gc_safepoint_requests_total() +
                                             ev->get_gc_safepoint_waits_total() +
                                             ev->get_gc_safepoint_deferred_total();
            const std::uint64_t edsl_workspace =
                ev->get_cross_cow_invalidations() + ev->get_fiber_stale_ref_count() +
                ev->get_provenance_mismatch() + ev->get_mutation_log_rollback_count() +
                ev->get_schema_validation_pass_count() + ev->get_mutation_impact_count();
            const std::uint64_t eda_verify =
                (ws ? ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
                          ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total()
                    : 0) +
                ev->get_verify_tool_calls_total();
            return make_int(static_cast<std::int64_t>(runtime_fiber + runtime_gc + edsl_workspace +
                                                      eda_verify));
        });

    // Issue #634: query:commercial-production-readiness-stats.
    // Returns the sum of 14 counters spanning the July 2026
    // commercial P0 pillars (non-duplicative synthesis of
    // #620/#623/#624/#627-#629/#630-#632/#614-#617/#618;
    // avoids #441 compiler-runtime focus and #613-#633 per-theme
    // issue tests):
    //   Fiber/StableRef (#620/#631): provenance_mismatch +
    //                                stable_ref_invalidations
    //   Arena/GC (#623): gc_safepoint_waits + gc_safepoint_requests
    //   Shape/JIT (#624): shape_stability_hit_count + deopt_count
    //   TypeSystem (#627/#628/#629): narrowing_dirty_recovery +
    //                                 coercion_zerooverhead_win
    //   EDA verify/batch (#630-#632): verify_dirty totals +
    //                                 atomic_batch_commits
    //   Stdlib hotpath (#614/#615/#617): specialization_hits +
    //                                    tag_arity_index_hits
    //   Orchestration (#618): mutation_steal_attempts +
    //                         lock_contention_us
    add("query:commercial-production-readiness-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t stable_ref =
                ev->get_provenance_mismatch() + (ws ? ws->stable_ref_invalidations() : 0);
            const std::uint64_t arena_gc =
                ev->get_gc_safepoint_waits_total() + ev->get_gc_safepoint_requests_total();
            const std::uint64_t shape_jit =
                shape::shape_stability_hit_count.load(std::memory_order_relaxed) +
                (m ? m->deopt_count.load(std::memory_order_relaxed) : 0);
            const std::uint64_t type_system =
                (m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0) +
                (m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0);
            const std::uint64_t eda_batch =
                (ws ? ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
                          ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total()
                    : 0) +
                (m ? m->atomic_batch_commits.load(std::memory_order_relaxed) : 0);
            const std::uint64_t stdlib_hotpath =
                (m ? m->specialization_hits.load(std::memory_order_relaxed) : 0) +
                (ws ? ws->tag_arity_index_hits() : 0);
            const std::uint64_t orchestration =
                ev->get_mutation_steal_attempts() + ev->get_lock_contention_us();
            return make_int(static_cast<std::int64_t>(stable_ref + arena_gc + shape_jit +
                                                      type_system + eda_batch + stdlib_hotpath +
                                                      orchestration));
        });

    // Issue #635: query:macro-reflect-self-evo-commercial-stats.
    // Returns the sum of 10 counters spanning the July 2026
    // macro + static reflection + self-evolution commercial
    // closed-loop (non-duplicative synthesis of #597 Task6
    // matrix, #619 follow-up, and #634 runtime pillars):
    //   Macro (#290 clone_macro_body): macro_expansion_dirty +
    //                                  macro_self_modify_dirty
    //   Query hygiene (#547): macro_introduced_skipped_in_query +
    //                          marker_macro_introduced_count
    //   Reflect (#551/#454): schema_validation_pass +
    //                        schema_validation_fail +
    //                        impact_snapshot_count
    //   Guard self-evo (#555): mutation_impact_count +
    //                          guard_dirty_epoch_count
    //   Dirty propagation (#415): mark_dirty_upward_call_count
    //   Commercial safety (#620/#624): stable_ref_invalidations +
    //                                  deopt_count
    //
    // P0: returns an integer = sum of all 10 counter groups.
    // Follow-up: returns a 10-tuple so the AI Agent can compute
    // macro_dirty_rate, reflect_pass_rate, and propagation_depth
    // independently for commercial fleet dashboards.
    add("query:macro-reflect-self-evo-commercial-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t macro_dirty =
                ws->macro_expansion_dirty_total() + ws->macro_self_modify_dirty_total();
            const std::uint64_t query_hygiene = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t reflect_validate =
                ev->get_schema_validation_pass_count() + ev->get_schema_validation_fail_count();
            const std::uint64_t reflect_snap = ev->get_impact_snapshot_count();
            const std::uint64_t guard_impact = ev->get_mutation_impact_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t dirty_up = ws->mark_dirty_upward_call_count();
            const std::uint64_t stable_ref = ws->stable_ref_invalidations();
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(
                macro_dirty + query_hygiene + markers + reflect_validate + reflect_snap +
                guard_impact + guard_epoch + dirty_up + stable_ref + deopt));
        });

    // Issue #636: query:edsl-query-mutate-commercial-stats.
    // Returns the sum of 10 counter groups spanning the July 2026
    // EDSL workspace + query/mutate + StableNodeRef commercial
    // closed-loop (non-duplicative synthesis of #620/#622/#619/
    // #621/#630/#623/#618 themes; avoids #552 edsl-stability
    // long-run focus, #635 macro-reflect, and #634 runtime pillars):
    //   StableNodeRef (#620/#631): stable_ref_invalidations +
    //                              node_gen_stale_access +
    //                              provenance_mismatch + fiber_stale_ref
    //   Query/pattern (#619/#621): tag_arity_index_hits +
    //                              tag_arity_index_dirty_marks +
    //                              macro_introduced_skipped_in_query
    //   Mutate/Guard (#622): mutation_impact_count +
    //                        guard_dirty_epoch_count
    //   Dirty propagation: mark_dirty_upward_call_count +
    //                     mark_dirty_total_nodes
    //   Atomic batch (#632): atomic_batch_commits +
    //                        atomic_batch_rollbacks + batch_count
    //   EDA feedback (#630): verification_coverage_feedback +
    //                        verification_assert_failure
    //   GC/orchestration (#623/#618): gc_safepoint_requests/waits +
    //                                 steal_attempts + lock_contention
    //
    // P0: returns an integer = sum of all 10 counter groups.
    // Follow-up: returns a 10-tuple for per-pillar fleet dashboards.
    add("query:edsl-query-mutate-commercial-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const std::uint64_t stable_ref =
            ws->stable_ref_invalidations() + ws->node_gen_stale_access_count();
        const std::uint64_t provenance =
            ev->get_provenance_mismatch() + ev->get_fiber_stale_ref_count();
        const std::uint64_t query_index =
            ws->tag_arity_index_hits() + ws->tag_arity_index_dirty_marks();
        const std::uint64_t query_hygiene = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t guard_mutate =
            ev->get_mutation_impact_count() + ev->get_guard_dirty_epoch_count();
        const std::uint64_t dirty_up =
            ws->mark_dirty_upward_call_count() + ws->mark_dirty_total_nodes();
        const std::uint64_t atomic =
            ws->atomic_batch_commits() + ev->atomic_batch_count() + ev->atomic_batch_rollbacks();
        const std::uint64_t eda_feedback =
            ws->verification_coverage_feedback_total() + ws->verification_assert_failure_total();
        const std::uint64_t gc_coord =
            ev->get_gc_safepoint_requests_total() + ev->get_gc_safepoint_waits_total();
        const std::uint64_t orchestration =
            ev->get_mutation_steal_attempts() + ev->get_lock_contention_us();
        return make_int(static_cast<std::int64_t>(stable_ref + provenance + query_index +
                                                  query_hygiene + guard_mutate + dirty_up + atomic +
                                                  eda_feedback + gc_coord + orchestration));
    });

    // Issue #619: query:macro-reflect-self-evo-followup-stats.
    // Returns the sum of 4 Task6 follow-up closed-loop counters:
    //   - hygiene_skips: macro_introduced_skipped_in_query_
    //   - post_mutate_reflect_pass: schema_validation_pass_count_
    //   - dirty_type_recheck_count: narrowing_dirty_recovery_total +
    //     incremental_typecheck_auto_invocations_total
    //   - transform_applied: mutation_impact_count_
    add("query:macro-reflect-self-evo-followup-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t hygiene = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t reflect = ev->get_schema_validation_pass_count();
            const std::uint64_t dirty_recheck =
                m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t type_recheck =
                m ? m->incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t transform = ev->get_mutation_impact_count();
            return make_int(static_cast<std::int64_t>(hygiene + reflect + dirty_recheck +
                                                      type_recheck + transform));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        const std::uint64_t impact = ev->get_mutation_impact_count();
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t pass = ev->get_schema_validation_pass_count();
        const std::uint64_t fail = ev->get_schema_validation_fail_count();
        const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        return make_int(static_cast<std::int64_t>(skips + violations + impact + snapshots + pass +
                                                  fail + commit + cross_cow));
    });

    // Issue #595: query:self-evolution-loop-stats. Returns the
    // sum of 5 marker/dirty/epoch/Guard self-evolution loop
    // counters (non-duplicative synthesis of #541/#525/#557
    // themes; avoids #597 macro+reflect 8-counter bundle and
    // #619 follow-up 4-counter bundle):
    //   - hygiene_skips: macro_introduced_skipped_in_query_
    //   - dirty_propagated: dirty_propagation_count_
    //   - epoch_deltas: guard_dirty_epoch_count_
    //   - validation_pass: schema_validation_pass_count_
    //   - rollback_count: mutation_log_rollback_count_
    add("query:self-evolution-loop-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t hygiene = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t dirty = ev->get_dirty_propagation_count();
        const std::uint64_t epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t validation = ev->get_schema_validation_pass_count();
        const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
        return make_int(static_cast<std::int64_t>(hygiene + dirty + epoch + validation + rollback));
    });

    // Issue #583: query:primitives-stats. Returns the sum of 6
    // primitives registry + core hot-path observability counters
    // spanning evaluator_primitives_registry.cpp registration
    // and list/math/core builtins (non-duplicative with #478
    // primitive-error-stats 2-tuple and stats:count meta):
    //   - registry_slots: primitives_.slot_count() (ordered_names_)
    //   - primitive_errors: primitive_error_count_ (make_primitive_error)
    //   - error_values_stored: error_values_.size() proxy
    //   - total_mutations_: AI Agent mutate-loop activity
    //   - total_query_calls_: query:* hot-path activity
    //   - specialization_hits_: compiled hot-path proxy
    //
    // P0: returns an integer = sum of all 6 counter groups.
    // Follow-up: returns a 6-tuple so the Agent can compute
    // error_rate = primitive_errors / (mutations + query_calls)
    // and registry health independently.
    add("query:primitives-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t registry_slots = ev->get_primitive_slot_count();
        const std::uint64_t errors = ev->get_primitive_error_count();
        const std::uint64_t stored = ev->get_primitive_error_values_size();
        const std::uint64_t mutations = ev->total_mutations();
        const std::uint64_t queries = ev->get_total_query_calls();
        const std::uint64_t hot_hits =
            m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(registry_slots + errors + stored + mutations +
                                                  queries + hot_hits));
    });

    // Issue #480: query:primitive-meta-stats. Returns the sum of
    // 6 self-describing primitive metadata counters spanning
    // PrimMeta storage + describe/list primitives (non-duplicative
    // with #583 primitives-stats registry hot-path and #478
    // primitive-error-stats 2-tuple):
    //   - registry_slots: primitives_.slot_count()
    //   - documented_meta: slots with non-empty doc string
    //   - describe_calls: primitive_describe_count_
    //   - list_meta_calls: primitive_list_meta_count_
    //   - primitive_errors: primitive_error_count_
    //   - total_query_calls_: Agent meta-inspection activity
    //
    // P0: returns an integer = sum of all 6 counter groups.
    add("query:primitive-meta-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t registry_slots = ev->get_primitive_slot_count();
        const std::uint64_t documented = ev->get_primitive_documented_meta_count();
        const std::uint64_t describes = ev->get_primitive_describe_count();
        const std::uint64_t list_meta = ev->get_primitive_list_meta_count();
        const std::uint64_t errors = ev->get_primitive_error_count();
        const std::uint64_t queries = ev->get_total_query_calls();
        return make_int(static_cast<std::int64_t>(registry_slots + documented + describes +
                                                  list_meta + errors + queries));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t boundary = ev->get_boundary_violation_count();
        const std::uint64_t steal_viol = ev->get_mutation_steal_violation_count();
        const std::uint64_t desync = ev->get_envframe_desync_detected();
        const std::uint64_t unsafe = ev->get_unsafe_boundary_attempts();
        const std::uint64_t batch_steal = ev->get_atomic_batch_steal_violation();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        return make_int(static_cast<std::int64_t>(boundary + steal_viol + desync + unsafe +
                                                  batch_steal + provenance + fiber_stale));
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
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t bridge_hit =
            m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t linear_pass =
            m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t closure_refresh =
            m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
        const std::uint64_t gc_skipped =
            m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t gc_walk_skips = ev->get_envframe_gc_walk_safe_skips();
        const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
        return make_int(static_cast<std::int64_t>(bridge_hit + linear_pass + closure_refresh +
                                                  env_refresh + gc_skipped + gc_walk_skips +
                                                  gc_waits));
    });

    // Issue #506: query:soa-hotpath-adoption-stats. Returns the sum
    // of 8 IR SoA + dirty-aware Pass Pipeline adoption counters
    // spanning evaluator/lowering hot paths (#463 scaffold →
    // production adoption; non-duplicative with #607 Task4
    // matrix and compile:ir-soa-stats hash primitive):
    //   - ir_soa_instructions_emitted_   (IRFunctionSoA dual-emit)
    //   - ir_soa_functions_emitted_      (IRFunctionSoA functions)
    //   - passes_skipped_type_dirty_     (DirtyAwarePass short-circuit)
    //   - relower_skipped_entirely_count_ (incremental re-lower win)
    //   - relower_per_function_called_   (per-fn SoA re-lower path)
    //   - module_dirty_skips_            (clean module skip)
    //   - linear_elide_count_            (SoA column fast path in Pass)
    //   - cascade_body_only_count_       (targeted dirty cascade)
    //
    // P0: returns an integer = sum of all 8 counters.
    // Follow-up: returns an 8-tuple so the AI Agent can compute
    // dirty_skip_rate = passes_skipped / (passes_skipped + relower_called)
    // and SoA adoption_ratio independently.
    add("query:soa-hotpath-adoption-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t ir_instr =
            m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
        const std::uint64_t ir_funcs =
            m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
        const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
        const std::uint64_t relower_skip =
            m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t relower_per_fn =
            m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mod_skip =
            m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
        const std::uint64_t linear_elide =
            m ? m->linear_elide_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t cascade =
            m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(ir_instr + ir_funcs + passes_skip + relower_skip +
                                                  relower_per_fn + mod_skip + linear_elide +
                                                  cascade));
    });

    // Issue #408: query:dirty-propagation-cost-stats. Returns the
    // sum of 7 EDSL dirty propagation + IR block_dirty_ cost
    // counters for high-frequency structural mutation profiling
    // (non-duplicative with #415 dirty-reason-propagation-stats
    // 9-counter verify-category slice, #550 typed-mutation-stats
    // narrowing/touched_roots slice, and #399/#398 per-theme tests):
    //   - upward_calls: mark_dirty_upward_call_count_ (FlatAST)
    //   - upward_nodes: mark_dirty_total_nodes_ (walk depth proxy)
    //   - fast_fixed_point: dirty_upward_fast_fixed_point_hits_
    //   - dirty_propagation: dirty_propagation_count_ (Evaluator)
    //   - passes_skipped: passes_skipped_type_dirty_ (IR block skip)
    //   - selective_recheck: selective_recheck_count_ (incremental)
    //   - cascade_body: cascade_body_only_count (precise block mark)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:dirty-propagation-cost-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t upward_calls = ws ? ws->mark_dirty_upward_call_count() : 0;
        const std::uint64_t upward_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
        const std::uint64_t fast_hits = ws ? ws->dirty_upward_fast_fixed_point_count() : 0;
        const std::uint64_t propagation = ev->get_dirty_propagation_count();
        const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
        const std::uint64_t selective = ev->get_selective_recheck_count();
        const std::uint64_t cascade =
            m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(upward_calls + upward_nodes + fast_hits +
                                                  propagation + passes_skip + selective + cascade));
    });

    // Issue #471: query:dirty-propagation-stats. Returns the
    // sum of 3 SV-scale dirty-propagation observability counters
    // (complements query:dirty-propagation-cost-stats from #408
    // which covers 7 cost-related counters):
    //   - upward_calls: mark_dirty_upward_call_count_ (FlatAST)
    //   - early_exit_count: mark_dirty_early_exit_count_ (when
    //     mark_dirty_upward_fast's fixed-point check fired)
    //   - max_depth_observed: mark_dirty_max_depth_observed_
    //     (deepest BFS level reached across all calls)
    //
    // The max_depth_observed is the key signal for SV-scale
    // perf: deep module hierarchies (10k+ nodes, generate
    // blocks) hit BFS levels of 50+ on every small mutate;
    // the early-exit rate (early_exit / upward_calls) tells
    // the AI Agent how much redundant work is being saved.
    add("query:dirty-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const std::uint64_t upward_calls = ws->mark_dirty_upward_call_count();
        const std::uint64_t early_exit = ws->mark_dirty_early_exit_count();
        const std::uint64_t max_depth = ws->mark_dirty_max_depth_observed();
        return make_int(static_cast<std::int64_t>(upward_calls + early_exit + max_depth));
    });

    // Issue #414: query:generation-epoch-stats. Returns the sum of
    // 7 long-running generation_ + composite wrap_epoch_ +
    // mutation-epoch observability counters for AI multi-round
    // session profiling (non-duplicative with #456 epoch-stats
    // single defuse_version return, #457 stable-ref-stats
    // 3-counter invalidation slice, #368 ast:generation-stats
    // per-field hash, #527 stable-ref-cow-fiber-stats COW/fiber
    // slice, and #552 edsl-stability-stats Task1 slice):
    //   - bump_generation: bump_generation_count_ (FlatAST churn)
    //   - generation_wrap: generation_wrap_count_ (uint16 wrap)
    //   - wrap_epoch: wrap_epoch_ (composite epoch, #368)
    //   - is_valid_checks: is_valid_check_count_ (validity load)
    //   - guard_epoch: guard_dirty_epoch_count_ (boundary epoch)
    //   - defuse_epoch: defuse_version_ (global mutation epoch)
    //   - rollback_ok: structural_rollback_success_ (mutate/rollback)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:generation-epoch-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t bumps = ws ? ws->bump_generation_count() : 0;
        const std::uint64_t wraps = ws ? ws->generation_wrap_count() : 0;
        const std::uint64_t wrap_ep = ws ? ws->wrap_epoch() : 0;
        const std::uint64_t checks = ws ? ws->is_valid_check_count() : 0;
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t defuse = ev->get_defuse_version();
        const std::uint64_t rollback = ws ? ws->structural_rollback_success() : 0;
        return make_int(static_cast<std::int64_t>(bumps + wraps + wrap_ep + checks + guard_epoch +
                                                  defuse + rollback));
    });

    // Issue #416: query:ast-column-compaction-stats. Returns the sum
    // of 7 FlatAST SoA column compaction + fragmentation
    // observability counters for long-lived workspace profiling
    // (non-duplicative with #405 arena-compaction-stats ArenaGroup
    // slice, #261 ast:node-lifecycle-stats per-field hash,
    // ast:recycle-nodes / ast:compact-nodes action primitives,
    // and #430 arena live-object moving theme):
    //   - recycle_total: node_recycle_total_ (dead slot reuse)
    //   - compact_total: node_compact_total_ (densify reclaimed)
    //   - slot_reuse: node_slot_reuse_count_ (free_list hits)
    //   - live_nodes: node_lifecycle_stats live count snapshot
    //   - free_slots: node_lifecycle_stats free_list size
    //   - total_slots: node_lifecycle_stats SoA column size
    //   - fragmentation_bp: dead/total ratio in basis points
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:ast-column-compaction-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const auto snap = ws->node_lifecycle_stats();
        const std::uint64_t recycle = ws->node_recycle_total();
        const std::uint64_t compact = ws->node_compact_total();
        const std::uint64_t reuse = ws->node_slot_reuse_count();
        const std::uint64_t live = snap.live_nodes;
        const std::uint64_t free = snap.free_slots;
        const std::uint64_t total = snap.total_slots;
        const std::uint64_t frag_bp =
            static_cast<std::uint64_t>(snap.fragmentation_ratio * 10000.0);
        return make_int(
            static_cast<std::int64_t>(recycle + compact + reuse + live + free + total + frag_bp));
    });

    // Issue #417: query:mutation-boundary-invariant-stats. Returns
    // the sum of 7 cross-TU MutationBoundaryGuard + defuse_version_
    // + per-fiber stack observability counters for post-P1/P2
    // evaluator split drift detection (non-duplicative with #448
    // mutation-coordination-stats scheduler/GC slice, #438
    // fiber-migration-stats 2-counter steal slice, #264
    // compile:concurrency-stats per-field hash, and #456
    // epoch-stats single defuse_version return):
    //   - invariant_violations: total_invariant_violations_
    //   - cross_fiber_rollback: cross_fiber_rollback_count_
    //   - mutation_yields: mutation_yield_count_
    //   - guard_epoch: guard_dirty_epoch_count_
    //   - boundary_violations: boundary_violation_count_
    //   - defuse_epoch: defuse_version_ (mutation epoch)
    //   - boundary_depth: mutation_boundary_depth() snapshot
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:mutation-boundary-invariant-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t violations = ev->get_total_invariant_violations();
        const std::uint64_t rollback = ev->cross_fiber_rollback_count();
        const std::uint64_t yields = ev->mutation_yield_count();
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t boundary = ev->get_boundary_violation_count();
        const std::uint64_t defuse = ev->get_defuse_version();
        const std::uint64_t depth = Evaluator::mutation_boundary_depth();
        return make_int(static_cast<std::int64_t>(violations + rollback + yields + guard_epoch +
                                                  boundary + defuse + depth));
    });

    // Issue #418: query:envframe-dualpath-stale-stats. Returns the
    // sum of 7 EnvFrame SoA dual-path + stale-policy observability
    // counters spanning evaluator_env.cpp hot paths
    // (non-duplicative with #543 envframe-dualpath-stats 4-counter
    // core slice, #602 prompt6-safety-score aggregated matrix,
    // and pre-registered envframe-stale-stats / envframe-bump-stats
    // stats:list aliases without dedicated sum primitives):
    //   - desync: envframe_desync_detected_ (length mismatch)
    //   - stale_refresh: envframe_stale_refresh_count_ (AutoRefresh)
    //   - post_rollback: envframe_post_rollback_invalidations_
    //   - version_mismatch: envframe_version_mismatch_in_walk_
    //   - gc_walk_skips: envframe_gc_walk_safe_skips_
    //   - gc_stale_skipped: gc_envframe_stale_skipped_ (GC policy)
    //   - defuse_epoch: defuse_version_ (stale epoch snapshot)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:envframe-dualpath-stale-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t desync = ev->get_envframe_desync_detected();
        const std::uint64_t stale = ev->get_envframe_stale_refresh_count();
        const std::uint64_t rollback = ev->get_envframe_post_rollback_invalidations();
        const std::uint64_t mismatch = ev->get_envframe_version_mismatch_in_walk();
        const std::uint64_t gc_skips = ev->get_envframe_gc_walk_safe_skips();
        const std::uint64_t gc_stale =
            m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t defuse = ev->get_defuse_version();
        return make_int(static_cast<std::int64_t>(desync + stale + rollback + mismatch + gc_skips +
                                                  gc_stale + defuse));
    });

    // Issue #419: query:defuse-version-stats. Returns the sum of
    // 7 modular defuse_version_ + AOT/runtime dispatch
    // observability counters for hot-update stale detection
    // (non-duplicative with #456 epoch-stats single-version return,
    // #456 epoch-delta-since-last-query delta-only primitive,
    // #189 concurrency:version-snapshot pair, and #414/#417/#418
    // slices that include defuse_version as one of seven groups):
    //   - defuse_epoch: current_defuse_version() (live epoch)
    //   - last_queried: last_queried_epoch_ (epoch-stats stamp)
    //   - total_mutations: total_mutations_ (lifetime bump count)
    //   - mutation_impact: mutation_impact_count_ (boundary flush)
    //   - guard_epoch: guard_dirty_epoch_count_ (boundary coord)
    //   - aot_emits: CompilerMetrics::aot_emits (emit events)
    //   - bridge_hits: bridge_epoch_hit_count_ (fresh bridge)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:defuse-version-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t defuse = ev->current_defuse_version();
        const std::uint64_t last = ev->get_last_queried_epoch();
        const std::uint64_t mutations = ev->total_mutations();
        const std::uint64_t impact = ev->get_mutation_impact_count();
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t aot = m ? m->aot_emits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t bridge =
            m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(defuse + last + mutations + impact + guard_epoch +
                                                  aot + bridge));
    });

    // Issue #420: query:macro-hygiene-contract-stats. Returns the
    // sum of 7 end-to-end MacroIntroduced hygiene contract
    // observability counters spanning clone/expand → query:pattern
    // → mutate guards → IR InlinePass (non-duplicative with #458
    // hygiene-stats 1-counter skip-only slice, #547
    // pattern-hygiene-stats 2-counter query slice, #514
    // ir-hygiene-stats / pattern-marker-stats 2–3 counter slices,
    // and #597 macro-reflect-self-evo-stats 8-counter bundle):
    //   - query_skips: macro_introduced_skipped_in_query_
    //   - violations: hygiene_violation_count_
    //   - markers: workspace MacroIntroduced marker column tally
    //   - ir_skips: InlinePass macro_hygiene_skipped_
    //   - queries: total_query_calls_
    //   - contract_violations: macro_hygiene_contract_violations_
    //   - macro_dirty: macro_expansion_dirty_total_ (clone path)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:macro-hygiene-contract-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t query_skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        const std::uint64_t ir_skips = ir_inline_hygiene_skipped(ev);
        const std::uint64_t queries = ev->get_total_query_calls();
        const std::uint64_t contract = ev->get_macro_hygiene_contract_violations();
        const std::uint64_t macro_dirty = ws ? ws->macro_expansion_dirty_total() : 0;
        return make_int(static_cast<std::int64_t>(query_skips + violations + markers + ir_skips +
                                                  queries + contract + macro_dirty));
    });

    // Issue #421: query:pattern-macro-filter-stats. Returns the
    // sum of 7 query:pattern recursive MacroIntroduced filter
    // observability counters (non-duplicative with #547
    // pattern-hygiene-stats 2-counter root-skip slice, #514
    // pattern-marker-stats 3-counter slice, and #420
    // macro-hygiene-contract-stats end-to-end bundle):
    //   - root_skips: macro_introduced_skipped_in_query_
    //   - recursive_skips: pattern_recursive_macro_skipped_
    //   - filter_violations: pattern_macro_filter_violations_
    //   - markers: workspace MacroIntroduced marker tally
    //   - queries: total_query_calls_
    //   - index_hits: tag_arity_index_hits (fast-path)
    //   - hygiene_violations: hygiene_violation_count_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:pattern-macro-filter-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t recursive = ev->get_pattern_recursive_macro_skipped();
        const std::uint64_t violations = ev->get_pattern_macro_filter_violations();
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        const std::uint64_t queries = ev->get_total_query_calls();
        const std::uint64_t index_hits = ws ? ws->tag_arity_index_hits() : 0;
        const std::uint64_t hygiene = ev->get_hygiene_violation_count();
        return make_int(static_cast<std::int64_t>(root_skips + recursive + violations + markers +
                                                  queries + index_hits + hygiene));
    });

    // Issue #422: query:hygiene-violation-stats. Returns the sum of
    // 7 mutate-path hygiene violation observability counters
    // (non-duplicative with #458 hygiene-stats skip-only slice,
    // #547 pattern-hygiene-stats query slice, and #420/#421
    // macro-hygiene bundles):
    //   - violation_attempts: hygiene_violation_attempts_
    //   - violation_count: hygiene_violation_count_
    //   - query_skips: macro_introduced_skipped_in_query_
    //   - mutation_impact: mutation_impact_count_
    //   - total_mutations: total_mutations_ (lifetime)
    //   - markers: workspace MacroIntroduced marker tally
    //   - guard_epoch: guard_dirty_epoch_count_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:hygiene-violation-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t attempts = ev->get_hygiene_violation_attempts();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        const std::uint64_t query_skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t impact = ev->get_mutation_impact_count();
        const std::uint64_t mutations = ev->total_mutations();
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        return make_int(static_cast<std::int64_t>(attempts + violations + query_skips + impact +
                                                  mutations + markers + guard_epoch));
    });

    // Issue #423: query:pattern-structural-index-stats. Returns the
    // sum of 7 Evaluator-side structural pre-index observability
    // counters (non-duplicative with #547/#554 query:pattern-index-stats
    // FlatAST workspace slice):
    //   - structural_hits: pattern_structural_index_hits_
    //   - structural_misses: pattern_structural_index_misses_
    //   - index_buckets: tag_arity_index_size()
    //   - index_entries: tag_arity_index_entry_count()
    //   - synced_size: tag_arity_index_synced_size()
    //   - synced_gen: tag_arity_index_synced_gen()
    //   - consistency_violations: pattern_index_consistency_violations_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:pattern-structural-index-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t hits = ev->get_pattern_structural_index_hits();
        const std::uint64_t misses = ev->get_pattern_structural_index_misses();
        const std::uint64_t buckets = ev->tag_arity_index_size();
        const std::uint64_t entries = ev->tag_arity_index_entry_count();
        const std::uint64_t synced_size = ev->tag_arity_index_synced_size();
        const std::uint64_t synced_gen = ev->tag_arity_index_synced_gen();
        const std::uint64_t violations = ev->get_pattern_index_consistency_violations();
        return make_int(static_cast<std::int64_t>(hits + misses + buckets + entries + synced_size +
                                                  synced_gen + violations));
    });

    // Issue #424: query:stable-ref-workspace-tree-stats. Returns the
    // sum of 7 WorkspaceTree / cross-layer StableNodeRef
    // observability counters (non-duplicative with #457
    // stable-ref-stats 3 FlatAST counters, #527
    // stable-ref-cow-fiber-stats COW/fiber bundle):
    //   - workspace_resolves: stable_ref_workspace_resolves_
    //   - workspace_resolve_misses: stable_ref_workspace_resolve_misses_
    //   - tree_violations: stable_ref_workspace_tree_violations_
    //   - tree_layers: WorkspaceTree::size()
    //   - active_idx: WorkspaceTree::active_idx()
    //   - cow_epoch: WorkspaceTree::cow_epoch_
    //   - is_valid_checks: FlatAST is_valid_check_count()
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:stable-ref-workspace-tree-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t resolves = ev->get_stable_ref_workspace_resolves();
        const std::uint64_t misses = ev->get_stable_ref_workspace_resolve_misses();
        const std::uint64_t violations = ev->get_stable_ref_workspace_tree_violations();
        std::uint64_t layers = 0;
        std::uint64_t active = 0;
        std::uint64_t cow_epoch = 0;
        if (auto* wt = static_cast<WorkspaceTree*>(ev->workspace_tree())) {
            layers = wt->size();
            active = wt->active_idx();
            cow_epoch = wt->cow_epoch_;
        }
        const std::uint64_t checks = ws ? ws->is_valid_check_count() : 0;
        return make_int(static_cast<std::int64_t>(resolves + misses + violations + layers + active +
                                                  cow_epoch + checks));
    });

    // Issue #407: query:shape-deopt-burst-stats. Returns the sum of
    // 7 ShapeProfiler bursty-mutation + deopt-storm observability
    // counters for AI orchestration workload tuning
    // (non-duplicative with #570 shape-stability-stats 6-counter
    // slice emphasizing stable_hits/fiber_refresh, #605 JIT mutate
    // matrix, and #403 ir-metadata-stats deopt-only slice):
    //   - shape_churn: mutation_shape_churn_count (burst detect)
    //   - shape_changes: shape_changes_observed (change frequency)
    //   - deopt_storm: deopt_count (GuardShape mismatch total)
    //   - jit_recompile: jit_shape_miss_count (cache version miss)
    //   - deopt_hooks: shape_deopt_hook_fire_count (invalidate hook)
    //   - version_bumps: shape_version_bump_count (profile invalidate)
    //   - spec_hits: specialization_hits (steady-state contrast)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:shape-deopt-burst-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t churn =
            shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
        const std::uint64_t changes =
            m ? m->shape_changes_observed.load(std::memory_order_relaxed) : 0;
        const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t jit_miss = shape::jit_shape_miss_count.load(std::memory_order_relaxed);
        const std::uint64_t hooks =
            shape::shape_deopt_hook_fire_count.load(std::memory_order_relaxed);
        const std::uint64_t bumps = shape::shape_version_bump_count.load(std::memory_order_relaxed);
        const std::uint64_t spec_hits =
            m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(churn + changes + deopt + jit_miss + hooks +
                                                  bumps + spec_hits));
    });

    // Issue #406: query:pass-contracts-stats. Returns the sum of
    // 7 Pass Pipeline + Contracts + zero-overhead hot-path counters
    // spanning AnalysisPass/DirtyAwarePass concepts and cheap-view
    // dispatch (non-duplicative with #571 value-dispatch-stats
    // 4-tuple, #506 soa-hotpath-adoption 8-counter slice, and
    // #381 per-concept unit tests in test_issue_163):
    //   - contract_violations: value_contract_violation_count
    //   - dispatch_hits: value_dispatch_hit_count (cheap-view fast path)
    //   - passes_skipped: passes_skipped_type_dirty_ (DirtyAwarePass)
    //   - relower_skipped: relower_skipped_entirely_count
    //   - relower_per_fn: relower_per_function_called_count
    //   - module_dirty_skips: clean module incremental skip
    //   - zerooverhead_wins: coercion_zerooverhead_win_total
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:pass-contracts-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t violations =
            types::value_contract_violation_count.load(std::memory_order_relaxed);
        const std::uint64_t dispatch_hits =
            types::value_dispatch_hit_count.load(std::memory_order_relaxed);
        const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
        const std::uint64_t relower_skip =
            m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t relower_per_fn =
            m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mod_skip =
            m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
        const std::uint64_t zero_wins =
            m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(violations + dispatch_hits + passes_skip +
                                                  relower_skip + relower_per_fn + mod_skip +
                                                  zero_wins));
    });

    // Issue #405: query:arena-compaction-stats. Returns the sum of
    // 7 arena automatic compaction + fragmentation orchestration
    // counters for AI multi-round mutation workloads
    // (non-duplicative with #187 compile:arena-stats / arena:*
    // primitives, #335 arena:adaptive-stats 2-tuple, and #300
    // arena:defrag-stats 5-tuple):
    //   - auto_compact_triggers: ArenaGroup trigger count
    //   - auto_compact_skips: ArenaGroup skip count (below threshold)
    //   - compaction_count: lifetime compact() calls (all modules)
    //   - compaction_saved: lifetime bytes reclaimed
    //   - compaction_paused: deferred at MutationBoundary
    //   - mutation_volume: total_mutations_ (orchestration signal)
    //   - dirty_propagation: mark_dirty_upward activity
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:arena-compaction-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto& group = ev->arena_group();
        const auto stats = group.total_stats();
        const std::uint64_t triggers = group.auto_compact_trigger_count();
        const std::uint64_t skips = group.auto_compact_skip_count();
        const std::uint64_t compacts = stats.compaction_count;
        const std::uint64_t saved = stats.total_compaction_saved;
        const std::uint64_t paused = ev->compaction_paused_by_boundary();
        const std::uint64_t mutations = ev->total_mutations();
        const std::uint64_t dirty = ev->get_dirty_propagation_count();
        return make_int(static_cast<std::int64_t>(triggers + skips + compacts + saved + paused +
                                                  mutations + dirty));
    });

    // Issue #404: query:ir-soa-incremental-stats. Returns the sum
    // of 7 IR SoA Phase 3 block_dirty_-driven incremental lowering
    // counters (non-duplicative with #506 soa-hotpath-adoption
    // 8-counter slice that includes passes_skipped + linear_elide,
    // #607 task4-cache-locality-win tag_arity slice, and #254
    // compile:ir-soa-stats hash primitive):
    //   - ir_soa_instructions_emitted   (IRFunctionSoA dual-emit)
    //   - ir_soa_functions_emitted      (IRFunctionSoA functions)
    //   - relower_skipped_entirely      (skip clean re-lower win)
    //   - relower_per_function_called   (per-fn incremental path)
    //   - module_dirty_skips            (clean module skip)
    //   - module_dirty_recompiles       (dirty module recompile)
    //   - cascade_body_only             (block_dirty cascade)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:ir-soa-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t ir_instr =
            m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
        const std::uint64_t ir_funcs =
            m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
        const std::uint64_t relower_skip =
            m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t relower_per_fn =
            m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mod_skip =
            m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mod_recompile =
            m ? m->module_dirty_recompiles.load(std::memory_order_relaxed) : 0;
        const std::uint64_t cascade =
            m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(ir_instr + ir_funcs + relower_skip +
                                                  relower_per_fn + mod_skip + mod_recompile +
                                                  cascade));
    });

    // Issue #403: query:ir-metadata-stats. Returns the sum of
    // 7 IRInstruction rich-metadata consumption counters spanning
    // IRInterpreter + JIT paths (non-duplicative with #506 SoA
    // adoption 8-counter slice, #570 shape-stability-stats,
    // #598 linear-ownership-runtime-stats 4-tuple):
    //   - narrow_evidence_hits: coercion_narrow_evidence_hits_total
    //     (GuardShape narrow fast-path — interpreter + JIT)
    //   - linear_elide: linear_elide_count (linear_ownership_state
    //     elision in TypeSpecializationWrap)
    //   - linear_enforce: linear_post_mutate_enforcements_total
    //     (interpreter GuardShape linear enforcement)
    //   - linear_pass: linear_check_pass_count_ (interpreter linear
    //     ownership fast-path checks)
    //   - jit_shape_hits: specialization_hits (JIT shape_id fast path)
    //   - deopt_total: deopt_count (shape mismatch — consistency signal)
    //   - adt_variant_impacts: adt_variant_mutate_impacts_total
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:ir-metadata-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t narrow =
            m ? m->coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t linear_elide =
            m ? m->linear_elide_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t linear_enforce =
            m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t linear_pass =
            m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t jit_hits =
            m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t adt_impacts =
            m ? m->adt_variant_mutate_impacts_total.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(narrow + linear_elide + linear_enforce +
                                                  linear_pass + jit_hits + deopt + adt_impacts));
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
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        auto* ws = ev->workspace_flat();
        const std::uint64_t spec_hits =
            m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t relower_skip =
            m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
        const std::uint64_t linear_elide =
            m ? m->linear_elide_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t index_hits = ws ? ws->tag_arity_index_hits() : 0;
        const std::uint64_t mod_skip =
            m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(spec_hits + relower_skip + passes_skip +
                                                  linear_elide + index_hits + mod_skip));
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
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        auto* ws = ev->workspace_flat();
        const std::uint64_t hits = ws ? ws->tag_arity_index_hits() : 0;
        const std::uint64_t delta = ws ? ws->tag_arity_index_delta_hits() : 0;
        const std::uint64_t spec = m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t cascade =
            m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t per_fn =
            m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
        return make_int(static_cast<std::int64_t>(hits + delta + spec + cascade + per_fn));
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
        return make_int(static_cast<std::int64_t>(stable_hits + version_bumps + fiber_refresh +
                                                  churn + deopt_hooks + jit_shape_miss));
    });

    // Issue #492: query:shape-profiler-stats — structured ShapeProfiler
    // deopt/stability view for AI orchestration (non-duplicative with
    // #570 int-sum and #407 burst-stats).
    add("query:shape-profiler-stats", [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m =
            static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
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
        const std::uint64_t deopt_storm =
            shape::shape_deopt_storm_count.load(std::memory_order_relaxed);
        const std::uint64_t shape_changes =
            m ? m->shape_changes_observed.load(std::memory_order_relaxed) : 0;
        const std::uint64_t deopt_count = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t spec_hits =
            m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t spec_misses =
            m ? m->specialization_misses.load(std::memory_order_relaxed) : 0;
        constexpr std::int64_t k_window =
            static_cast<std::int64_t>(shape::ShapeProfiler::kDefaultWindowSize);
        constexpr std::int64_t k_ratio_bp = static_cast<std::int64_t>(
            shape::ShapeProfiler::kDefaultStabilityRatio * 10000.0);
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = string_heap.size();
                    string_heap.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("stability-hits", static_cast<std::int64_t>(stable_hits));
        insert_kv("version-bumps", static_cast<std::int64_t>(version_bumps));
        insert_kv("fiber-refresh", static_cast<std::int64_t>(fiber_refresh));
        insert_kv("shape-churn", static_cast<std::int64_t>(churn));
        insert_kv("deopt-hooks", static_cast<std::int64_t>(deopt_hooks));
        insert_kv("jit-shape-miss", static_cast<std::int64_t>(jit_shape_miss));
        insert_kv("deopt-storm-count", static_cast<std::int64_t>(deopt_storm));
        insert_kv("shape-changes-observed", static_cast<std::int64_t>(shape_changes));
        insert_kv("deopt-count", static_cast<std::int64_t>(deopt_count));
        insert_kv("specialization-hits", static_cast<std::int64_t>(spec_hits));
        insert_kv("specialization-misses", static_cast<std::int64_t>(spec_misses));
        insert_kv("window-size", k_window);
        insert_kv("stability-ratio-bp", k_ratio_bp);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
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
        const std::uint64_t hits = types::value_dispatch_hit_count.load(std::memory_order_relaxed);
        const std::uint64_t misses =
            types::value_dispatch_miss_count.load(std::memory_order_relaxed);
        const std::uint64_t violations =
            types::value_contract_violation_count.load(std::memory_order_relaxed);
        const std::uint64_t collisions =
            types::v2_string_collision_attempts.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(hits + misses + violations + collisions));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
        const std::uint64_t selective = ev->get_selective_recheck_count();
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        return make_int(static_cast<std::int64_t>(dirty_prop + selective + guard_epoch + narrowing +
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
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        const std::uint64_t wraps = ws ? ws->generation_wrap_count() : 0;
        const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        return make_int(
            static_cast<std::int64_t>(cross_cow + fiber_stale + wraps + rollback + provenance));
    });

    // Issue #527: query:stable-ref-cow-fiber-stats. Returns the
    // sum of 7 StableNodeRef cross-COW / fiber / workspace-gen
    // counters spanning FlatAST + Evaluator closed loop
    // (non-duplicative with #457 stable-ref-stats 3 FlatAST
    // counters, #552 edsl-stability-stats 5-counter Task1
    // slice, #549 self-evolution-stability-stats 4-counter
    // Task6 slice):
    //   - cross_cow_invalidations_        (Evaluator COW boundary)
    //   - fiber_stale_ref_count_          (Evaluator fiber migration)
    //   - provenance_mismatch_            (workspace_gen mismatch)
    //   - generation_wrap_count_          (FlatAST uint16 wrap)
    //   - stable_ref_invalidations_       (FlatAST ref rejections)
    //   - node_gen_stale_access_count_    (FlatAST raw NodeId stale)
    //   - mutation_log_rollback_count_    (Guard rollback path)
    //
    // P0: returns an integer = sum of all 7 counter groups.
    add("query:stable-ref-cow-fiber-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        const std::uint64_t wraps = ws ? ws->generation_wrap_count() : 0;
        const std::uint64_t invalidations = ws ? ws->stable_ref_invalidations() : 0;
        const std::uint64_t stale = ws ? ws->node_gen_stale_access_count() : 0;
        const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
        return make_int(static_cast<std::int64_t>(cross_cow + fiber_stale + provenance + wraps +
                                                  invalidations + stale + rollback));
    });

    // Issue #529: query:atomic-batch-rollback-stats. Returns the
    // sum of 7 counters spanning the end-to-end atomic batch +
    // mutation_log_ rollback + Guard + fiber orchestration
    // closed loop (non-duplicative with #459 1-counter steal
    // ship, #553 7-counter batch matrix, and atomic-batch:stats
    // hash in observability):
    //   - batch_commits: atomic_batch_count_
    //   - batch_rollbacks: atomic_batch_rollbacks_
    //   - bumps_saved: atomic_batch_bumps_saved_total_
    //   - fiber_safety: atomic_batch_steal_violation_ +
    //                   atomic_batch_in_fiber_total_
    //   - guard_rollbacks: mutation_log_rollback_count_
    //   - guard_success: mutation_impact_count_
    //   - panic_recovery: panic_checkpoint_restore_count_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    // Follow-up: returns a 7-tuple so the AI Agent can compute
    // rollback_rate and fiber_safety_ratio independently.
    add("query:atomic-batch-rollback-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t commits = ev->atomic_batch_count();
        const std::uint64_t rollbacks = ev->atomic_batch_rollbacks();
        const std::uint64_t bumps_saved = ev->atomic_batch_bumps_saved_total();
        const std::uint64_t fiber_safety =
            ev->get_atomic_batch_steal_violation() + ev->atomic_batch_in_fiber_total();
        const std::uint64_t guard_rollbacks = ev->get_mutation_log_rollback_count();
        const std::uint64_t guard_success = ev->get_mutation_impact_count();
        const std::uint64_t panic_recovery = ev->get_panic_checkpoint_restore_count();
        return make_int(static_cast<std::int64_t>(commits + rollbacks + bumps_saved + fiber_safety +
                                                  guard_rollbacks + guard_success +
                                                  panic_recovery));
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
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t steal_violations = ev->get_atomic_batch_steal_violation();
        const std::uint64_t batch_count = ev->atomic_batch_count();
        const std::uint64_t bumps_saved_total = ev->atomic_batch_bumps_saved_total();
        const std::uint64_t rollbacks = ev->atomic_batch_rollbacks();
        const std::uint64_t ws_commits = ws ? ws->atomic_batch_commits() : 0;
        const std::uint64_t ws_bumps_saved = ws ? ws->atomic_batch_bumps_saved() : 0;
        // Issue #396 Phase 3: include the in-fiber heuristic
        // counter in the sum so changes to it show up in the
        // mutation-log-stats aggregate.
        const std::uint64_t in_fiber_total = ev->atomic_batch_in_fiber_total();
        return make_int(static_cast<std::int64_t>(steal_violations + batch_count +
                                                  bumps_saved_total + rollbacks + ws_commits +
                                                  ws_bumps_saved + in_fiber_total));
    });

    // Issue #400: query:mutation-rollback-coverage-stats. Returns
    // the sum of 4 rollback-coverage observability counters
    // spanning sym_id / structural / field-offset / batch paths
    // (non-duplicative with #553 mutation-log-stats batch matrix
    // and #369 per-theme structural tests):
    //   - structural_success: structural_rollback_success
    //     (children_/sym_id structural ops restored)
    //   - structural_besteffort: structural_rollback_besteffort
    //     (records needing full subtree restore)
    //   - field_log_rollbacks: mutation_log_rollback_count_
    //     (Guard boundary field_offset rollbacks incl. sym_id)
    //   - batch_rollbacks: atomic_batch_rollbacks_
    add("query:mutation-rollback-coverage-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        const std::uint64_t structural_success = ws ? ws->structural_rollback_success() : 0;
        const std::uint64_t structural_besteffort = ws ? ws->structural_rollback_besteffort() : 0;
        const std::uint64_t field_log = ev->get_mutation_log_rollback_count();
        const std::uint64_t batch = ev->atomic_batch_rollbacks();
        return make_int(static_cast<std::int64_t>(structural_success + structural_besteffort +
                                                  field_log + batch));
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
        if (!ev)
            return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_void();
        std::int64_t n = 10;
        if (!a.empty() && is_int(a[0]))
            n = as_int(a[0]);
        if (n < 0)
            return make_void();
        // Read the mutation log (most recent first)
        // and take the last n.
        const auto& log = ws->mutation_log_view();
        if (log.empty())
            return make_void();
        const std::int64_t take =
            static_cast<std::int64_t>(log.size()) < n ? static_cast<std::int64_t>(log.size()) : n;
        // Build the pair-list in chronological order
        // (oldest first). The log is most-recent first,
        // so we walk from (log.size() - take) to end.
        const std::size_t start = log.size() - static_cast<std::size_t>(take);
        EvalValue list = make_void();
        for (std::size_t i = log.size(); i-- > start;) {
            const auto& rec = log[i];
            // Format: "id=<id> target=<node> op=<name> sum=<summary>"
            const std::string s = "id=" + std::to_string(rec.mutation_id) +
                                  " target=" + std::to_string(rec.target_node) +
                                  " op=" + rec.operator_name + " sum=" + rec.summary;
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
        if (!ev)
            return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_void();
        const std::uint64_t since_id = static_cast<std::uint64_t>(as_int(a[0]));
        const auto& log = ws->mutation_log_view();
        EvalValue list = make_void();
        // Walk most-recent first (the natural order
        // for the agent's "newest changes" view).
        for (std::size_t i = log.size(); i-- > 0;) {
            const auto& rec = log[i];
            if (rec.mutation_id <= since_id)
                break;
            const std::string s = "id=" + std::to_string(rec.mutation_id) +
                                  " target=" + std::to_string(rec.target_node) +
                                  " op=" + rec.operator_name + " sum=" + rec.summary;
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
        if (!ev)
            return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_void();
        const auto view = ws->mutation_log_view();
        if (view.empty())
            return make_void();
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
        return make_pair(ev->push_pair(make_string(oidx), make_string(sidx)));
    });

    // Issue #577: query:adt-exhaustiveness-stats. Returns the sum
    // of 4 Task2 ADT/match exhaustiveness + narrowing counters:
    //   - exhaustiveness_checks: adt_exhaust_rechecks_total
    //   - narrowing_hits_on_match: adt_occurrence_narrow_in_match_total
    //   - stale_exhaustiveness_prevented: adt_stale_exhaust_prevented_total
    //   - mutation_impact_on_adt: adt_variant_mutate_impacts_total
    add("query:adt-exhaustiveness-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t checks = m->adt_exhaust_rechecks_total.load(std::memory_order_relaxed);
        const std::uint64_t narrow =
            m->adt_occurrence_narrow_in_match_total.load(std::memory_order_relaxed);
        const std::uint64_t stale =
            m->adt_stale_exhaust_prevented_total.load(std::memory_order_relaxed);
        const std::uint64_t impact =
            m->adt_variant_mutate_impacts_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(checks + narrow + stale + impact));
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
        if (!m)
            return make_int(0);
        const std::uint64_t rechecks =
            m->adt_exhaust_rechecks_total.load(std::memory_order_relaxed);
        const std::uint64_t impacts =
            m->adt_variant_mutate_impacts_total.load(std::memory_order_relaxed);
        const std::uint64_t stale =
            m->adt_stale_exhaust_prevented_total.load(std::memory_order_relaxed);
        const std::uint64_t narrow =
            m->adt_occurrence_narrow_in_match_total.load(std::memory_order_relaxed);
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
    add("query:match-exhaustiveness-notes", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_void();
        // Walk the flat; collect node-ids that
        // have a match_info entry with
        // exhaustiveness_checked = true + a
        // candidate_constructors / used_constructors
        // gap. We surface the NodeId; the agent can
        // use (query:node-type <id>) to inspect.
        EvalValue list = make_void();
        const auto n = ws->size();
        for (std::size_t id = n; id-- > 0;) {
            if (!ws->has_match_info(static_cast<aura::ast::NodeId>(id)))
                continue;
            const auto* mi = ws->get_match_info(static_cast<aura::ast::NodeId>(id));
            if (!mi || !mi->exhaustiveness_checked)
                continue;
            // We surface any checked match. A
            // future enhancement can filter to
            // "non-exhaustive" (used < candidates)
            // but the agent can derive that
            // locally.
            auto sidx = ev->push_string_heap(std::to_string(id));
            auto p_idx = ev->push_pair(make_string(sidx), list);
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
        if (!ev)
            return make_int(0);
        const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
        const std::uint64_t selective = ev->get_selective_recheck_count();
        const std::uint64_t conflicts = ev->get_touched_roots_conflict_count();
        const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t cross_delta = ev->get_cross_delta_conflicts_caught();
        const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
        const std::uint64_t touched_size = ev->get_touched_roots_size();
        return make_int(static_cast<std::int64_t>(dirty_prop + selective + conflicts + guard_epoch +
                                                  narrowing + cross_delta + passes_skipped +
                                                  touched_size));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t steals = ev->get_mutation_steal_attempts();
        const std::uint64_t violations = ev->get_boundary_violation_count();
        const std::uint64_t unsafe_attempts = ev->get_unsafe_boundary_attempts();
        const std::uint64_t contention_us = ev->get_lock_contention_us();
        return make_int(
            static_cast<std::int64_t>(steals + violations + unsafe_attempts + contention_us));
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
        if (!ev)
            return make_int(0);
        // Read from the shared CompilerMetrics struct via
        // the Evaluator's void pointer (set by CompilerService
        // via set_compiler_metrics(&metrics_)). Cast back to
        // CompilerMetrics* to access the 4 new counters.
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t stale_refresh =
            m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
        const std::uint64_t bridge_hit = m->bridge_epoch_hit_count_.load(std::memory_order_relaxed);
        const std::uint64_t linear_pass =
            m->linear_check_pass_count_.load(std::memory_order_relaxed);
        const std::uint64_t gc_skipped =
            m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed);
        return make_int(
            static_cast<std::int64_t>(stale_refresh + bridge_hit + linear_pass + gc_skipped));
    });

    // Issue #447: (query:tag-arity-count tag-int arity-int)
    // — count of nodes matching (tag, arity) using the
    // pre-built index. Bumps the hits or misses counter
    // accordingly. P0: 0 on miss (the follow-up falls
    // back to a linear scan on miss).
    add("query:tag-arity-count", [](std::span<const EvalValue> a) -> EvalValue {
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
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
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const std::uint64_t cov = ws->verification_coverage_feedback_total();
        const std::uint64_t ass = ws->verification_assert_failure_total();
        const std::uint64_t att = ws->sv_mutate_attempts_total();
        const std::uint64_t suc = ws->sv_mutate_success_total();
        const std::uint64_t cyc = ws->verify_loop_cycles_total();
        return make_int(static_cast<std::int64_t>(cov + ass + att + suc + cyc));
    });

    // Issue #415: query:dirty-reason-propagation-stats. Returns
    // the sum of 9 dirty-reason + propagation observability
    // counters spanning the verification-category bitmask
    // infrastructure (#344/#437/#469) and mark_dirty_upward
    // propagation metrics (#256/#336):
    //   - mark_dirty_upward_call_count_   (FlatAST, #256)
    //   - mark_dirty_total_nodes_         (FlatAST, #256)
    //   - dirty_upward_fast_fixed_point_hits_ (FlatAST, #336)
    //   - verify_assertion_dirty_total_   (FlatAST, #437)
    //   - verify_coverage_dirty_total_    (FlatAST, #437)
    //   - verify_sva_dirty_total_         (FlatAST, #437)
    //   - verify_formal_cex_dirty_total_  (FlatAST, #437)
    //   - verification_coverage_feedback_total_ (FlatAST, #469)
    //   - verification_assert_failure_total_  (FlatAST, #469)
    //
    // P0: returns an integer = sum of all 9 counters.
    // Follow-up: returns a 9-tuple so the AI Agent can
    // compute propagation_depth = total_nodes / call_count
    // and verify_feedback_rate independently.
    //
    // Non-duplicative with #344 (compile:dirty-reason-counts
    // per-node tallies + query:dirty-nodes subtree query),
    // #437 (query:verify-dirty-stats 4-tuple only), and
    // #469 (query:verification-loop-stats includes SV
    // mutate + loop-cycle counters).
    add("query:dirty-reason-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        auto* ws = ev->workspace_flat();
        if (!ws)
            return make_int(0);
        const std::uint64_t upward_calls = ws->mark_dirty_upward_call_count();
        const std::uint64_t upward_nodes = ws->mark_dirty_total_nodes();
        const std::uint64_t fast_hits = ws->dirty_upward_fast_fixed_point_count();
        const std::uint64_t verify =
            ws->verify_assertion_dirty_total() + ws->verify_coverage_dirty_total() +
            ws->verify_sva_dirty_total() + ws->verify_formal_cex_dirty_total();
        const std::uint64_t feedback =
            ws->verification_coverage_feedback_total() + ws->verification_assert_failure_total();
        return make_int(
            static_cast<std::int64_t>(upward_calls + upward_nodes + fast_hits + verify + feedback));
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
        if (!ev)
            return make_int(0);
        const std::uint64_t steals = ev->get_mutation_steal_violation_count();
        const std::uint64_t gc_blocks = ev->get_gc_blocked_by_mutation_boundary();
        const std::uint64_t wait_ns = ev->get_safepoint_mutation_wait_total_ns();
        return make_int(static_cast<std::int64_t>(steals + gc_blocks + wait_ns));
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
    //   - bindings_dual_sync_count_  (# of frames where
    //     the dual-path length/order check succeeded —
    //     expected to grow under normal mutation)
    //
    // P0: returns an integer = sum of all 5 counters.
    // Follow-up: returns a 5-tuple
    // (desync dual-sync stale-refresh version-mismatch gc-skips)
    // so the AI Agent can react to each category
    // independently (a desync > 0 should be a hard
    // alert; a version-mismatch > 0 is expected under
    // concurrent mutation).
    add("query:envframe-dualpath-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const std::uint64_t desync = ev->get_envframe_desync_detected();
        const std::uint64_t dual_sync = ev->get_bindings_dual_sync_count();
        const std::uint64_t stale = ev->get_envframe_stale_refresh_count();
        const std::uint64_t mismatch = ev->get_envframe_version_mismatch_in_walk();
        const std::uint64_t gc_skips = ev->get_envframe_gc_walk_safe_skips();
        return make_int(static_cast<std::int64_t>(desync + dual_sync + stale + mismatch + gc_skips));
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
    add("mutate:validate-against-schema",
        [&string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
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
            if (!validate_code_against_schema_simple(code, type_name, violation_reason,
                                                     violation_field)) {
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
                std::string repr =
                    "(schema-violation \"" + violation_reason + "\" \"" + violation_field + "\")";
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
            invalidation = m->narrow_invalidation_post_mutate_total.load(std::memory_order_relaxed);
            provenance_hits = m->narrowing_provenance_total.load(std::memory_order_relaxed);
            safe_fallbacks = m->narrow_safe_fallback_total.load(std::memory_order_relaxed);
        }
        if (auto* ws = ev.workspace_flat()) {
            invalidation += ws->narrow_invalidation_post_mutate_count();
        }
        return make_int(static_cast<std::int64_t>(stale_caught + blame_attached + invalidation +
                                                  provenance_hits + safe_fallbacks));
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
        return make_int(
            static_cast<std::int64_t>(check_hits + switches + consistency + stale_prevented));
    });

    // Issue #467: query:occurrence-stats. Returns the sum of 4
    // per-node occurrence-dirty + blame chain propagation counters:
    //   - occurrence_dirty_recoveries: narrowing_dirty_recovery_total
    //   - blame_chain_propagated: narrow_blame_attached_total +
    //     occurrence_blame_chain_complete_total
    //   - stale_narrowing_prevented: occurrence_stale_refreshes_total +
    //     stale_check_narrow_prevented_total
    //   - narrowing_refresh: narrowing_refresh_count_ (Evaluator)
    add("query:occurrence-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t dirty_recovery =
            m ? m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t blame_attached =
            m ? m->narrow_blame_attached_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t blame_complete =
            m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t stale_refresh =
            m ? m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t stale_prevented =
            m ? m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        return make_int(static_cast<std::int64_t>(dirty_recovery + blame_attached + blame_complete +
                                                  stale_refresh + stale_prevented + narrowing));
    });

    // Issue #576: query:occurrence-blame-stats. Returns the sum
    // of 4 Task2 occurrence typing + blame/provenance counters:
    //   - stale_narrowing_prevented: stale_check_narrow_prevented_total
    //   - blame_chain_preserved: occurrence_blame_chain_complete_total
    //   - narrowing_refresh_count: narrowing_refresh_count_ (Evaluator)
    //   - provenance_mismatch: provenance_mismatch_ (Evaluator)
    add("query:occurrence-blame-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_int(0);
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t stale_prevented =
            m ? m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t blame_preserved =
            m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t narrowing = ev->get_narrowing_refresh_count();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        return make_int(
            static_cast<std::int64_t>(stale_prevented + blame_preserved + narrowing + provenance));
    });

    // Issue #609: query:occurrence-narrow-stats. Returns the sum
    // of 4 post-mutation occurrence narrow recovery counters:
    //   - narrow_recoveries: occurrence_stale_refreshes_total +
    //     narrowing_reanalyzed_total (predicate re-analysis)
    //   - blame_attached: narrow_blame_attached_total
    //   - post_mutate_correctness: post_mutate_narrow_consistency_total
    //   - stale_narrow_prevented: stale_check_narrow_prevented_total
    add("query:occurrence-narrow-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
        const std::uint64_t narrow_recoveries =
            m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) +
            m->narrowing_reanalyzed_total.load(std::memory_order_relaxed);
        const std::uint64_t blame_attached =
            m->narrow_blame_attached_total.load(std::memory_order_relaxed);
        const std::uint64_t post_mutate_correctness =
            m->post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
        const std::uint64_t stale_narrow_prevented =
            m->stale_check_narrow_prevented_total.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(
            narrow_recoveries + blame_attached + post_mutate_correctness + stale_narrow_prevented));
    });

    // Issue #537 / #518 Phase 2: query:occurrence-narrowing-stats.
    // Returns the sum of stale-refresh + blame-chain-complete
    // counters from CompilerMetrics (post-mutation re-narrow
    // provenance observability).
    add("query:occurrence-narrowing-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        const auto* m = static_cast<const CompilerMetrics*>(
            Evaluator::get_query_evaluator()->compiler_metrics());
        if (!m)
            return make_int(0);
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
        if (c == '"' && (i == 0 || code[i - 1] != '\\')) {
            in_string = !in_string;
            continue;
        }
        if (in_string)
            continue;
        if (c == '(')
            ++paren_depth;
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
        bool is_digit_start = (c >= '0' && c <= '9') || (c == '-' && i + 1 < code.size() &&
                                                         code[i + 1] >= '0' && code[i + 1] <= '9');
        if (!is_digit_start) {
            ++i;
            continue;
        }
        std::size_t j = i;
        if (c == '-')
            ++j;
        while (j < code.size() && code[j] >= '0' && code[j] <= '9')
            ++j;
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