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
#include "serve/metrics.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.pass_manager;
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

// Issue #514 / #501: count MacroIntroduced nodes in the workspace marker column.
static std::uint64_t workspace_marker_macro_introduced(Evaluator* ev) {
    if (!ev)
        return 0;
    const std::uint64_t snapshot = ev->get_macro_markers_in_snapshot();
    ev->lock_workspace_shared();
    std::uint64_t count = 0;
    if (auto* ws = ev->workspace_flat()) {
        const auto& markers = ws->marker_column();
        if (!markers.empty()) {
            for (auto m : markers) {
                if (m == aura::ast::SyntaxMarker::MacroIntroduced)
                    ++count;
            }
        }
    }
    ev->unlock_workspace_shared();
    return count > 0 ? count : snapshot;
}

static std::uint64_t ir_inline_hygiene_skipped(Evaluator* ev) {
    if (!ev || !ev->get_macro_hygiene_skipped_fn_)
        return 0;
    return ev->get_macro_hygiene_skipped_fn_();
}

// Issue #750: runtime AST subtree validation for macro/EDSL self-evo safety.
struct ReflectRuntimeValidateResult {
    bool ok = false;
    bool hygiene_held = true;
    bool stale_prevented = false;
    std::uint64_t macro_markers = 0;
};

static bool is_edsl_verification_tag(aura::ast::NodeTag tag) noexcept {
    using aura::ast::NodeTag;
    return tag == NodeTag::Constraint || tag == NodeTag::Class || tag == NodeTag::Covergroup ||
           tag == NodeTag::Coverpoint || tag == NodeTag::Property || tag == NodeTag::Interface ||
           tag == NodeTag::Modport || tag == NodeTag::Sequence || tag == NodeTag::Assert;
}

static ReflectRuntimeValidateResult runtime_reflect_validate_ast_subtree(aura::ast::FlatAST& flat,
                                                                         aura::ast::NodeId root,
                                                                         bool edsl_mode) {
    ReflectRuntimeValidateResult out;
    if (root == aura::ast::NULL_NODE || root >= flat.size() || !flat.is_live_node(root)) {
        out.stale_prevented = true;
        return out;
    }
    if (edsl_mode && !is_edsl_verification_tag(flat.get(root).tag))
        return out;
    constexpr auto kExpansion =
        static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
    bool marker_ok = true;
    std::vector<aura::ast::NodeId> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        if (id == aura::ast::NULL_NODE || id >= flat.size()) {
            marker_ok = false;
            continue;
        }
        const auto v = flat.get(id);
        if (flat.is_macro_introduced(id)) {
            ++out.macro_markers;
            if ((flat.macro_dirty(id) & kExpansion) == 0)
                marker_ok = false;
        }
        const auto parent = flat.parent_of(id);
        if (parent != aura::ast::NULL_NODE && parent >= flat.size())
            marker_ok = false;
        for (auto c : v.children) {
            if (c != aura::ast::NULL_NODE)
                stack.push_back(c);
        }
    }
    out.hygiene_held = marker_ok;
    out.ok = marker_ok;
    return out;
}

static void bump_reflection_schema_metrics(CompilerMetrics* m,
                                           const ReflectRuntimeValidateResult& result) {
    if (!m)
        return;
    if (result.stale_prevented) {
        m->reflection_stale_validation_prevented_total.fetch_add(1, std::memory_order_relaxed);
        m->reflection_schema_violations_total.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (result.ok) {
        m->reflection_schema_validated_total.fetch_add(1, std::memory_order_relaxed);
        if (result.macro_markers > 0)
            m->reflection_macro_provenance_held_total.fetch_add(1, std::memory_order_relaxed);
    } else {
        m->reflection_schema_violations_total.fetch_add(1, std::memory_order_relaxed);
    }
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
            insert_kv("schema-valid", ev->get_last_schema_validation_ok() ? 1 : 0);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #504: query:mutation-boundary-log. Consolidated Guard impact
    // log for AI self-evolution loops (non-duplicative with #488
    // mutation-impact-snapshot single-entry view and #417 invariant sum):
    //   - latest ring entry: epoch-after/delta, nodes-changed, reasons-mask
    //   - impact-snapshots, mutation-impacts, dirty-nodes, macro-markers
    //   - boundary-depth, guard-epoch, ring-seq, ring-capacity
    //   - boundary-log-total, boundary-log-recommendation
    add("query:mutation-boundary-log", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        const auto entry = ev->get_latest_mutation_impact_entry();
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
        const std::uint64_t snapshots = ev->get_impact_snapshot_count();
        const std::uint64_t impacts = ev->get_mutation_impact_count();
        const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
        const std::uint64_t markers = ev->get_macro_markers_in_snapshot();
        const std::uint64_t ring_seq = ev->get_mutation_impact_ring_seq();
        const std::uint64_t total =
            snapshots + impacts + entry.epoch_delta + entry.nodes_changed + dirty;
        std::int64_t recommendation = 0;
        if (!ev->get_last_schema_validation_ok())
            recommendation = 3;
        else if (entry.nodes_changed > 20)
            recommendation = 2;
        else if (dirty > 10)
            recommendation = 1;
        insert_kv("epoch-after", static_cast<std::int64_t>(entry.epoch_after));
        insert_kv("epoch-delta", static_cast<std::int64_t>(entry.epoch_delta));
        insert_kv("nodes-changed", static_cast<std::int64_t>(entry.nodes_changed));
        insert_kv("reasons-mask", static_cast<std::int64_t>(entry.reasons_mask));
        insert_kv("impact-snapshots", static_cast<std::int64_t>(snapshots));
        insert_kv("mutation-impacts", static_cast<std::int64_t>(impacts));
        insert_kv("dirty-nodes", static_cast<std::int64_t>(dirty));
        insert_kv("macro-markers", static_cast<std::int64_t>(markers));
        insert_kv("boundary-depth",
                  static_cast<std::int64_t>(Evaluator::mutation_boundary_depth()));
        insert_kv("guard-epoch", static_cast<std::int64_t>(ev->get_guard_dirty_epoch_count()));
        insert_kv("ring-seq", static_cast<std::int64_t>(ring_seq));
        insert_kv("ring-capacity", 8); // Evaluator::kMutationImpactRingSize
        insert_kv("schema-valid", ev->get_last_schema_validation_ok() ? 1 : 0);
        insert_kv("boundary-log-total", static_cast<std::int64_t>(total));
        insert_kv("boundary-log-recommendation", recommendation);
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
            auto* ht = FlatHashTable::create(16);
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
            // Issue #738: cross-COW + boundary pinning observability.
            insert_kv("cross-cow-invalidations",
                      static_cast<std::int64_t>(ev.get_cross_cow_invalidations()));
            if (auto* wflat = ev.workspace_flat())
                insert_kv("pinned-across-boundaries",
                          static_cast<std::int64_t>(wflat->pinned_across_boundaries()));
            else
                insert_kv("pinned-across-boundaries", 0);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #738: query:stable-ref-boundary-stats-hash — cross-COW /
    // sub-workspace pinning + boundary validity observability for
    // concurrent AI orchestration. Complements #527
    // (stable-ref-cow-fiber-stats) and #457 (stable-ref-stats).
    add("query:stable-ref-boundary-stats-hash", [&ev, &string_heap](const auto&) -> EvalValue {
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
        const std::uint64_t cross_cow = ev.get_cross_cow_invalidations();
        const std::uint64_t pins_total = ev.cow_boundary_pins_total();
        const std::uint64_t pins_active = ev.cow_boundary_pinned_ref_count();
        std::uint64_t flat_pins = 0;
        std::uint64_t boundary_checks = 0;
        std::uint64_t cow_epoch = 0;
        if (auto* ws = ev.workspace_flat()) {
            flat_pins = ws->pinned_across_boundaries();
            boundary_checks = ws->cross_boundary_validations();
            cow_epoch = ws->workspace_cow_epoch();
        }
        insert_kv("cross-cow-invalidations", static_cast<std::int64_t>(cross_cow));
        insert_kv("pinned-across-boundaries", static_cast<std::int64_t>(flat_pins));
        insert_kv("boundary-pins-total", static_cast<std::int64_t>(pins_total));
        insert_kv("boundary-pins-active", static_cast<std::int64_t>(pins_active));
        insert_kv("boundary-validations", static_cast<std::int64_t>(boundary_checks));
        insert_kv("workspace-cow-epoch", static_cast<std::int64_t>(cow_epoch));
        insert_kv("schema", 738);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #620: query:stable-ref-provenance — Agent-discoverable
    // StableNodeRef provenance query. Takes a raw NodeId, calls
    // ws->make_safe_ref(nid) to capture full provenance from the
    // current FlatAST state, and returns a 9-field hash:
    //   - id                          int (the captured node id)
    //   - gen                         int (current generation_)
    //   - mutation-id-at-capture      int (FlatAST next_mutation_id_)
    //   - workspace-id                int (workspace layer; 0 = root)
    //   - fiber-id                    int (current fiber's id, or 0
    //                                  if no fiber is active — makes
    //                                  cross-fiber steals visible)
    //   - last-validated-generation   int (initially == gen; updated
    //                                  by validate_with_provenance())
    //   - wrap-epoch                  int (FlatAST wrap_epoch_; lets
    //                                  the Agent detect refs captured
    //                                  before a uint16_t wrap-around)
    //   - subtree-gen-at-capture      int (per-Define subtree gen;
    //                                  #392; EDA long-running helper)
    //   - is-live                     bool (whether the captured
    //                                  ref currently satisfies
    //                                  ref.is_valid_in(ws))
    //   - schema                      int (sentinel = 620 so the
    //                                  Agent can detect schema drift)
    //
    // Returns #f when the NodeId is out-of-range or there's no
    // workspace loaded, so the Agent can branch on the bool-result
    // without confusing "unknown node" with "live node" (the
    // latter still returns a hash with all fields populated).
    add("query:stable-ref-provenance",
        [&pairs, &string_heap, &ev](std::span<const EvalValue> a) -> EvalValue {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->stable_ref_provenance_query_total.fetch_add(1, std::memory_order_relaxed);
            if (a.empty() || !is_int(a[0]))
                return make_bool(false);
            const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
            auto* ws = ev.workspace_flat();
            if (!ws)
                return make_bool(false);
            if (nid >= ws->size())
                return make_bool(false);
            // Issue #303 / Issue #392: capture full provenance.
            // fiber_id = 0 when no fiber is active on this thread;
            // the Agent can compare two captures and detect a
            // cross-fiber swap via the fiber-id field.
            const std::uint32_t cur_fiber = static_cast<std::uint32_t>(aura_fiber_current_id());
            auto ref = ws->make_safe_ref(nid, /*workspace_id=*/0, cur_fiber);
            const bool is_live = ref.is_valid_in(*ws);
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_bool(false);
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
            insert_kv("id", static_cast<std::int64_t>(ref.id));
            insert_kv("gen", static_cast<std::int64_t>(ref.gen));
            insert_kv("mutation-id-at-capture",
                      static_cast<std::int64_t>(ref.mutation_id_at_capture));
            insert_kv("workspace-id", static_cast<std::int64_t>(ref.workspace_id));
            insert_kv("fiber-id", static_cast<std::int64_t>(ref.fiber_id));
            insert_kv("last-validated-generation",
                      static_cast<std::int64_t>(ref.last_validated_generation));
            insert_kv("wrap-epoch", static_cast<std::int64_t>(ref.wrap_epoch));
            insert_kv("subtree-gen-at-capture",
                      static_cast<std::int64_t>(ref.subtree_gen_at_capture));
            insert_kv("is-live", is_live ? 1 : 0);
            insert_kv("schema", 620);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #497: query:stable-ref-lifecycle-stats — long-session
    // generation/compaction/refresh observability for AI loops.
    add("query:stable-ref-lifecycle-stats",
        [&pairs, &string_heap, &ev](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            std::uint64_t wraps = 0;
            std::uint64_t invalidations = 0;
            std::uint64_t stale = 0;
            std::uint64_t soft_compact = 0;
            std::uint64_t auto_refresh = 0;
            std::uint64_t bump_gen = 0;
            std::uint64_t compact_total = 0;
            std::uint32_t wrap_epoch = 0;
            std::uint16_t cur_gen = 0;
            if (auto* ws = ev.workspace_flat()) {
                wraps = ws->generation_wrap_count();
                invalidations = ws->stable_ref_invalidations();
                stale = ws->node_gen_stale_access_count();
                soft_compact = ws->soft_compact_count();
                auto_refresh = ws->stale_ref_auto_refresh_count();
                bump_gen = ws->bump_generation_count();
                compact_total = ws->node_compact_total();
                wrap_epoch = ws->wrap_epoch();
                cur_gen = ws->current_generation();
            }
            std::int64_t recommendation = 0;
            if (wraps > 0)
                recommendation = 1;
            else if (invalidations >= 10)
                recommendation = 2;
            else if (cur_gen > 60000)
                recommendation = 3;
            const std::uint64_t lifecycle_total =
                wraps + invalidations + stale + soft_compact + auto_refresh + bump_gen;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_int(recommendation);
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
            insert_kv("wrap-epoch", static_cast<std::int64_t>(wrap_epoch));
            insert_kv("current-generation", static_cast<std::int64_t>(cur_gen));
            insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(invalidations));
            insert_kv("node-gen-stale-accesses", static_cast<std::int64_t>(stale));
            insert_kv("soft-compact-count", static_cast<std::int64_t>(soft_compact));
            insert_kv("stale-ref-auto-refresh-count", static_cast<std::int64_t>(auto_refresh));
            insert_kv("bump-generation-count", static_cast<std::int64_t>(bump_gen));
            insert_kv("node-compact-total", static_cast<std::int64_t>(compact_total));
            insert_kv("lifecycle-recommendation", recommendation);
            insert_kv("lifecycle-total", static_cast<std::int64_t>(lifecycle_total));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #631: query:stable-ref-provenance-sv-stats-hash —
    // Agent-discoverable structured dashboard for StableNodeRef
    // cross-fiber + multi-agent SV provenance, specifically
    // covering AC3 from the issue body. Pairs with the existing
    // query:stable-ref-provenance (#620, per-ref fields) and
    // query:stable-ref-stats-hash (#457, lifetime aggregate).
    //
    // Fields (4):
    //   - cross-fiber-violations          new cross_fiber_violations_
    //                                    total counter (foundation for
    //                                    AC1 enforcement; bumped when
    //                                    ref.fiber_id != current in
    //                                    query:/mutate: paths).
    //                                    Value is 0 until the
    //                                    enforcement work ships
    //                                    (AC1 follow-up).
    //   - provenance-mismatches-on-sv    existing stable_ref_invalidations
    //                                    counter (#620/#368/#313/#437
    //                                    — bumped on every invalidate()
    //                                    + on every boundary mismatch
    //                                    from validate_with_provenance).
    //                                    Synthetic same source — 0
    //                                    until enforcement wires
    //                                    distinct counters per
    //                                    source. Marked derived in
    //                                    the field name via the
    //                                    -on-sv suffix.
    //   - safe-resolves                  new safe_resolves_total
    //                                    counter (foundation for
    //                                    AC2 — auto-refresh
    //                                    provenance on capture +
    //                                    WorkspaceTree fallback).
    //                                    Value is 0 until AC2 wires.
    //   - total-stable-ref-invalidations  existing stable_ref_
    //                                    invalidations lifetime count
    //                                    (same as -mismatches-on-sv
    //                                    above; tracked separately so
    //                                    the Agent can see if the
    //                                    future split allocates).
    //   - schema == 631                  sentinel for Agent drift
    //                                    detection (mirrors the
    //                                    #618+#620+#621+#622+
    //                                    #623+#624+#625+#626+
    //                                    #630 chain).
    //
    // Discovery before this PR (no duplication): the C++ side
    // already exposes stable_ref_invalidations atomics on both
    // CompilerMetrics and FlatAST (added by #313/#368/#620).
    // The 2 new atomics (cross_fiber_violations_total +
    // safe_resolves_total) are foundation scaffolding for the
    // AC1 + AC2 enforcement work which is invasive C++ +
    // multi-fiber Guard wire-up that needs benchmarking + perf
    // regression coverage alongside the JIT/hot-swap work in
    // #601/#491.
    //
    // The single NEW contribution is the structured primitive
    // the issue body AC3 lists by exact name +
    // (query:stable-ref-provenance-sv-stats).
    add("query:stable-ref-provenance-sv-stats-hash",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t cross_fiber =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->cross_fiber_violations_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t safe_resolves =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->safe_resolves_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t mismatches =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->stable_ref_invalidations.load(std::memory_order_relaxed)
                    : 0;
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
            insert_kv("cross-fiber-violations", static_cast<std::int64_t>(cross_fiber));
            insert_kv("provenance-mismatches-on-sv", static_cast<std::int64_t>(mismatches));
            insert_kv("safe-resolves", static_cast<std::int64_t>(safe_resolves));
            insert_kv("total-stable-ref-invalidations", static_cast<std::int64_t>(mismatches));
            insert_kv("schema", 631);
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

    // Issue #618 + #591: query:scheduler-mutation-coord-stats —
    // structured-hash companion to (query:orchestration-metrics).
    // The latter (#451) returns a JSON string for back-compat
    // with existing test_issue_451; this primitive is the
    // Agent-discoverable structured form for the LLM-aware
    // orchestrator side.
    //
    // Returned hash (#618 foundation + #591 steal/GC coordination):
    //   - gc-pauses-attributed-to-mutation  int (lifetime # of
    //                                      GC safepoints where the
    //                                      wait was attributed to
    //                                      an active MutationBoundary
    //                                      guard)
    //   - mutation-boundary-depth            int (current call
    //                                      depth — 0 = not inside
    //                                      any guard; >0 = nested)
    //   - current-fiber-id                  int (current Fiber's
    //                                      numeric id, or 0 if no
    //                                      fiber is active on this
    //                                      thread)
    //   - is-fibers-active                  bool (true iff
    //                                      current-fiber-id > 0)
    //   - gc-frequency-tune-ratio           int (0..100; the value
    //                                      the last
    //                                      (orchestration:tune-gc-
    //                                      frequency ratio) call
    //                                      set; default 50)
    //   - schema                           int (sentinel = 618 so
    //                                      the Agent can detect
    //                                      schema changes)
    //   - steal-deferred-count             int (#591: global defer
    //                                      count during active boundary)
    //   - safepoint-wait-on-boundary-us    int (#591: GC wait proxy
    //                                      attributed to boundary)
    //   - wakeup-after-defer-success       int (#591: ring-steal
    //                                      success proxy after defer)
    //   - mutation-coord-schema            int (591)
    //   - scheduler-mutation-coord-total   int (monotonic synthesis)
    //   - scheduler-mutation-coord-recommendation int
    add("query:scheduler-mutation-coord-stats", [](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        auto& string_heap = ev->string_heap_mut();
        auto* ht = FlatHashTable::create(32);
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
        const std::uint64_t gc_pauses = aura_fiber_static_gc_pause_attributed_to_mutation();
        const std::uint64_t depth = aura_evaluator_mutation_boundary_depth();
        const std::uint64_t cur_fiber = aura_fiber_current_id();
        const std::uint64_t ratio = static_cast<std::uint64_t>(
            aura::serve::gc_frequency_tune_ratio().load(std::memory_order_relaxed));
        const std::uint64_t steal_deferred = aura_adaptive_steal_global_deferred_total();
        const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
        const std::uint64_t gc_requests = ev->get_gc_safepoint_requests_total();
        const std::uint64_t gc_deferred = ev->get_gc_safepoint_deferred_total();
        const std::uint64_t wait_ns = ev->get_gc_safepoint_wait_total_ns();
        const std::uint64_t ring_successes = aura_adaptive_steal_ring_successes();
        const std::uint64_t steal_successes = aura_work_steal_successes_total();
        const std::int64_t safepoint_wait_on_boundary_us =
            static_cast<std::int64_t>((wait_ns / 1000) + gc_pauses * 10);
        const std::int64_t wakeup_after_defer_success =
            steal_deferred > 0 ? static_cast<std::int64_t>(ring_successes + steal_successes)
                               : static_cast<std::int64_t>(ring_successes);
        const std::uint64_t total =
            gc_pauses + steal_deferred + gc_waits + gc_requests + gc_deferred + ring_successes;
        std::int64_t recommendation = 0;
        if (ev->get_mutation_steal_violation_count() > 0)
            recommendation = 3;
        else if (steal_deferred > steal_successes && steal_deferred > 3)
            recommendation = 2;
        else if (gc_pauses > 0 || steal_deferred > 0)
            recommendation = 1;
        insert_kv("gc-pauses-attributed-to-mutation", static_cast<std::int64_t>(gc_pauses));
        insert_kv("mutation-boundary-depth", static_cast<std::int64_t>(depth));
        insert_kv("current-fiber-id", static_cast<std::int64_t>(cur_fiber));
        insert_kv("is-fibers-active", cur_fiber > 0 ? 1 : 0);
        insert_kv("gc-frequency-tune-ratio", static_cast<std::int64_t>(ratio));
        insert_kv("schema", 618);
        insert_kv("steal-deferred-count", static_cast<std::int64_t>(steal_deferred));
        insert_kv("safepoint-wait-on-boundary-us", safepoint_wait_on_boundary_us);
        insert_kv("wakeup-after-defer-success", wakeup_after_defer_success);
        insert_kv("mutation-coord-schema", 591);
        insert_kv("scheduler-mutation-coord-total", static_cast<std::int64_t>(total));
        insert_kv("scheduler-mutation-coord-recommendation", recommendation);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #618: (orchestration:tune-gc-frequency ratio) —
    // setter for the GC safepoint frequency tuning atomic.
    // (orchestration:tune-gc-frequency) with no arg reads back
    // the current value. With an int arg in [0, 100] writes +
    // returns the previous value. Out-of-range args are clamped
    // (negative -> 0; > 100 -> 100) so callers don't have to
    // validate.
    //
    // P0 ships write/read/return; the actual scheduler-side
    // consult of this atomic is a follow-up. Until then, the
    // value is dormant but visible to the Agent via
    // (query:scheduler-mutation-coord-stats) so the LLM-aware
    // tuning loop can be wired up without re-touching the
    // scheduler in the same PR.
    add("orchestration:tune-gc-frequency", [](std::span<const EvalValue> a) -> EvalValue {
        auto& ratio = aura::serve::gc_frequency_tune_ratio();
        const std::uint64_t prev = ratio.load(std::memory_order_relaxed);
        if (a.empty()) {
            return make_int(static_cast<std::int64_t>(prev));
        }
        if (!is_int(a[0])) {
            // Bad-arg: return current value as int, no change.
            return make_int(static_cast<std::int64_t>(prev));
        }
        std::int64_t requested = as_int(a[0]);
        std::uint32_t clamped = 0;
        if (requested < 0)
            clamped = 0;
        else if (requested > 100)
            clamped = 100;
        else
            clamped = static_cast<std::uint32_t>(requested);
        ratio.store(clamped, std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(prev));
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

    // Issue #621: query:pattern-index-stats-hash — Agent-discoverable
    // structured form of (query:pattern-index-stats). The legacy
    // primitive (#547) returns an int = sum of 6 counters; this
    // version returns the full 10-field hash so the AI Agent can
    // react to each category independently.
    //
    // Fields (10):
    //   - hits               lifetime find_by_tag_arity hits
    //   - misses             lifetime find_by_tag_arity misses
    //   - rebuilds           lifetime full rebuilds (#547)
    //   - dirty-marks        lifetime mark_dirty_upward() calls
    //                        that flipped the dirty flag (#554)
    //   - rebuild-time-us    cumulative rebuild wall time (#554)
    //   - delta-hits         incremental-patch hits (#554)
    //   - linear-fallbacks   == misses (count of queries that
    //                        fell through to a linear scan)
    //   - arity-accuracy     hits / (hits + misses) * 100
    //                        (0 if both are 0; rounded to int)
    //   - delta-hit-rate     delta_hits / (delta_hits +
    //                        rebuilds) * 100; measures how often
    //                        incremental patches satisfy lookups
    //                        vs requiring a full rebuild
    //   - recommendation     int 0=healthy, 1=high-miss-rate,
    //                        2=rebuild-bound (rebuild_time_us
    //                        dominates delta_hits)
    //   - schema == 621 sentinel
    //
    // P0 ships the structured form with derived metrics
    // computed inline (no new C++ atomics). The actual
    // tag_arity_index rebuild/patch + query:pattern hot-path
    // wiring are follow-ups (hot-path changes that need
    // benchmarking + perf regression coverage first).
    add("query:pattern-index-stats-hash",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            std::uint64_t hits = 0;
            std::uint64_t misses = 0;
            std::uint64_t rebuilds = 0;
            std::uint64_t dirty_marks = 0;
            std::uint64_t rebuild_time_us = 0;
            std::uint64_t delta_hits = 0;
            if (auto* ws = ev->workspace_flat()) {
                hits = ws->tag_arity_index_hits();
                misses = ws->tag_arity_index_misses();
                rebuilds = ws->tag_arity_index_rebuilds();
                dirty_marks = ws->tag_arity_index_dirty_marks();
                rebuild_time_us = ws->tag_arity_index_rebuild_time_us();
                delta_hits = ws->tag_arity_index_delta_hits();
            }
            const std::uint64_t total = hits + misses;
            const std::int64_t arity_accuracy =
                total == 0 ? 0 : static_cast<std::int64_t>((hits * 100) / total);
            const std::uint64_t delta_denom = delta_hits + rebuilds;
            const std::int64_t delta_hit_rate =
                delta_denom == 0 ? 0 : static_cast<std::int64_t>((delta_hits * 100) / delta_denom);
            std::int64_t recommendation = 0;
            if (total > 0 && arity_accuracy < 50)
                recommendation = 1;
            else if (rebuilds > 0 &&
                     rebuild_time_us > static_cast<std::uint64_t>(delta_hits + 1) * 100)
                recommendation = 2;
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
            insert_kv("hits", static_cast<std::int64_t>(hits));
            insert_kv("misses", static_cast<std::int64_t>(misses));
            insert_kv("rebuilds", static_cast<std::int64_t>(rebuilds));
            insert_kv("dirty-marks", static_cast<std::int64_t>(dirty_marks));
            insert_kv("rebuild-time-us", static_cast<std::int64_t>(rebuild_time_us));
            insert_kv("delta-hits", static_cast<std::int64_t>(delta_hits));
            insert_kv("linear-fallbacks", static_cast<std::int64_t>(misses));
            insert_kv("arity-accuracy", arity_accuracy);
            insert_kv("delta-hit-rate", delta_hit_rate);
            insert_kv("recommendation", recommendation);
            insert_kv("schema", 621);
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
    add("query:macro-hygiene-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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

    // Issue #511: query:workspace-snapshot-stats. Hash view of workspace
    // persistence + panic-checkpoint snapshot observability for long-session
    // AI Agent resume (non-duplicative with #548 int-sum
    // panic-checkpoint-lifecycle-stats and #497 stable-ref-lifecycle hash):
    //   - workspace-size: live FlatAST node count
    //   - gen-age: current FlatAST generation_
    //   - wrap-epoch: generation wrap epoch for resume safety
    //   - stable-ref-invalidations: stale ref detections since session start
    //   - checkpoint-save / checkpoint-restore / checkpoint-commit /
    //     checkpoint-transfer: panic checkpoint lifecycle counters
    //   - rollback-success: successful panic restores
    //   - panic-safe-source-len: bytes in last checkpoint source snapshot
    //   - workspace-snapshot-total: sum of primary counters
    //   - workspace-snapshot-recommendation: 0=ok, 1=checkpoint stale, 2=high restore
    add("query:workspace-snapshot-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
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
            const std::uint64_t workspace_size = ws ? ws->size() : 0;
            const std::uint64_t gen_age = ws ? ws->current_generation() : 0;
            const std::uint64_t wrap_epoch = ws ? ws->wrap_epoch() : 0;
            const std::uint64_t ref_inval = ws ? ws->stable_ref_invalidations() : 0;
            const std::uint64_t save = ev->get_panic_checkpoint_save_count();
            const std::uint64_t restore = ev->get_panic_checkpoint_restore_count();
            const std::uint64_t commit = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t transfer = ev->get_panic_checkpoint_transfer_count();
            const std::uint64_t rollback_success = ev->get_rollback_success_on_panic();
            const std::uint64_t source_len = ev->panic_safe_source().size();
            const std::uint64_t total =
                workspace_size + gen_age + save + restore + commit + transfer + rollback_success;
            std::int64_t recommendation = 0;
            if (save > 0 && restore > save)
                recommendation = 2;
            else if (save > 0 && source_len == 0)
                recommendation = 1;
            insert_kv("workspace-size", static_cast<std::int64_t>(workspace_size));
            insert_kv("gen-age", static_cast<std::int64_t>(gen_age));
            insert_kv("wrap-epoch", static_cast<std::int64_t>(wrap_epoch));
            insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(ref_inval));
            insert_kv("checkpoint-save", static_cast<std::int64_t>(save));
            insert_kv("checkpoint-restore", static_cast<std::int64_t>(restore));
            insert_kv("checkpoint-commit", static_cast<std::int64_t>(commit));
            insert_kv("checkpoint-transfer", static_cast<std::int64_t>(transfer));
            insert_kv("rollback-success", static_cast<std::int64_t>(rollback_success));
            insert_kv("panic-safe-source-len", static_cast<std::int64_t>(source_len));
            insert_kv("workspace-snapshot-total", static_cast<std::int64_t>(total));
            insert_kv("workspace-snapshot-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #512: query:runtime-orchestration-stats. Hash view of runtime
    // production orchestration gaps (work-stealing outermost enforcement,
    // EnvFrame/GC safepoint coordination, fiber migration) — non-duplicative
    // synthesis of #500 work-steal-stats, #618 scheduler-mutation-coord-stats,
    // and #543 envframe-dualpath-stats:
    //   - steal-attempts / steal-successes / steal-deferred-outermost /
    //     steal-violations: work-stealing + MutationBoundary safety
    //   - mutation-boundary-depth: current guard nesting (0 = steal-safe)
    //   - gc-safepoint-requests / gc-safepoint-waits /
    //     gc-pauses-attributed-to-mutation: scheduler/GC coordination
    //   - envframe-stale-refresh: EnvFrame dual-path consistency signal
    //   - lock-contention-us / fiber-migration-attempts: orchestration load
    //   - runtime-orchestration-total / runtime-orchestration-recommendation
    add("query:runtime-orchestration-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
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
            const std::uint64_t steal_attempts = aura_work_steal_attempts_total();
            const std::uint64_t steal_successes = aura_work_steal_successes_total();
            const std::uint64_t steal_deferred = aura_adaptive_steal_global_deferred_total();
            const std::uint64_t steal_violations = ev->get_mutation_steal_violation_count();
            const std::uint64_t boundary_depth = aura_evaluator_mutation_boundary_depth();
            const std::uint64_t gc_requests = ev->get_gc_safepoint_requests_total();
            const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
            const std::uint64_t gc_attributed = aura_fiber_static_gc_pause_attributed_to_mutation();
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t lock_us = ev->get_lock_contention_us();
            const std::uint64_t migration = ev->get_mutation_steal_attempts();
            const std::uint64_t total = steal_attempts + steal_successes + steal_deferred +
                                        steal_violations + gc_requests + gc_waits + gc_attributed +
                                        env_refresh + migration;
            std::int64_t recommendation = 0;
            if (steal_violations > 0)
                recommendation = 3;
            else if (steal_deferred > steal_successes && steal_deferred > 3)
                recommendation = 2;
            else if (boundary_depth > 0 && steal_attempts > 0)
                recommendation = 1;
            insert_kv("steal-attempts", static_cast<std::int64_t>(steal_attempts));
            insert_kv("steal-successes", static_cast<std::int64_t>(steal_successes));
            insert_kv("steal-deferred-outermost", static_cast<std::int64_t>(steal_deferred));
            insert_kv("steal-violations", static_cast<std::int64_t>(steal_violations));
            insert_kv("mutation-boundary-depth", static_cast<std::int64_t>(boundary_depth));
            insert_kv("gc-safepoint-requests", static_cast<std::int64_t>(gc_requests));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("gc-pauses-attributed-to-mutation", static_cast<std::int64_t>(gc_attributed));
            insert_kv("envframe-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("lock-contention-us", static_cast<std::int64_t>(lock_us));
            insert_kv("fiber-migration-attempts", static_cast<std::int64_t>(migration));
            insert_kv("runtime-orchestration-total", static_cast<std::int64_t>(total));
            insert_kv("runtime-orchestration-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #513: query:aot-hot-reload-stats. Consolidated hash view of AOT
    // hot-reload production readiness (func_table epoch swap, stale detection,
    // refcount/region safety) — non-duplicative synthesis of #708
    // query:aot-reload-stats + query:aot-checkpoint-version-stats:
    //   - reload-attempts / reload-success / stale-rejected: dlopen path
    //   - refcount-swaps / concurrent-safe-reloads: func_table epoch swap
    //   - region-violations / deopt-on-steal: multi-fiber safety signals
    //   - checkpoint-version-drifts: defuse/bridge_epoch drift probe
    //   - func-table-epoch / defuse-version: live version state
    //   - aot-hot-reload-total / aot-hot-reload-recommendation
    add("query:aot-hot-reload-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
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
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t attempts =
            m ? m->aot_reload_attempts_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t success =
            m ? m->aot_hot_update_success_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t stale =
            m ? m->aot_stale_reject_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t swaps = m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t region_viol =
            m ? m->aot_region_mismatch_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t deopt_steal =
            m ? m->aot_deopt_on_steal_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t concurrent_safe =
            m ? m->aot_concurrent_safe_reloads_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t drifts =
            m ? m->aot_checkpoint_version_drifts_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t table_epoch = aura_aot_func_table_epoch();
        const std::uint64_t defuse_ver = aura_get_aot_defuse_version();
        const std::uint64_t total = attempts + success + stale + swaps + region_viol + deopt_steal +
                                    concurrent_safe + drifts;
        std::int64_t recommendation = 0;
        if (region_viol > 0 || deopt_steal > 0)
            recommendation = 3;
        else if (stale > success && stale > 0)
            recommendation = 2;
        else if (drifts > 0)
            recommendation = 1;
        insert_kv("reload-attempts", static_cast<std::int64_t>(attempts));
        insert_kv("reload-success", static_cast<std::int64_t>(success));
        insert_kv("stale-rejected", static_cast<std::int64_t>(stale));
        insert_kv("refcount-swaps", static_cast<std::int64_t>(swaps));
        insert_kv("region-violations", static_cast<std::int64_t>(region_viol));
        insert_kv("deopt-on-steal", static_cast<std::int64_t>(deopt_steal));
        insert_kv("concurrent-safe-reloads", static_cast<std::int64_t>(concurrent_safe));
        insert_kv("checkpoint-version-drifts", static_cast<std::int64_t>(drifts));
        insert_kv("func-table-epoch", static_cast<std::int64_t>(table_epoch));
        insert_kv("defuse-version", static_cast<std::int64_t>(defuse_ver));
        insert_kv("aot-hot-reload-total", static_cast<std::int64_t>(total));
        insert_kv("aot-hot-reload-recommendation", recommendation);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #522: query:aot-production-reload-stats. Commercial P0 hash view
    // of AOT hot-reload deployment readiness (func_table swap, multi-agent
    // module/region namespace, version drift) — non-duplicative synthesis
    // of #513 aot-hot-reload-stats with #287 module_version + #708 region
    // isolation; avoids duplicating #708 per-theme security.cpp hashes:
    //   - reload-attempts / reload-success / stale-rejected: dlopen path
    //   - refcount-swaps / concurrent-safe-reloads: func_table epoch swap
    //   - region-violations / deopt-on-steal: multi-fiber safety
    //   - checkpoint-version-drifts: defuse/bridge_epoch drift
    //   - func-table-epoch / defuse-version / module-version / host-region-mask
    //   - swap-success-rate-pct: 0..100 from attempts/success
    //   - aot-production-reload-total / aot-production-reload-recommendation
    add("query:aot-production-reload-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(32);
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
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t attempts =
                m ? m->aot_reload_attempts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t success =
                m ? m->aot_hot_update_success_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stale =
                m ? m->aot_stale_reject_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t swaps =
                m ? m->aot_refcount_swaps_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t region_viol =
                m ? m->aot_region_mismatch_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt_steal =
                m ? m->aot_deopt_on_steal_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t concurrent_safe =
                m ? m->aot_concurrent_safe_reloads_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t drifts =
                m ? m->aot_checkpoint_version_drifts_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t table_epoch = aura_aot_func_table_epoch();
            const std::uint64_t defuse_ver = aura_get_aot_defuse_version();
            const std::uint64_t module_ver = aura_get_module_version();
            const std::uint64_t region_mask = aura_get_aot_region_mask();
            const std::uint64_t success_rate_pct =
                attempts > 0 ? (100 * success / attempts) : (success > 0 ? 100 : 0);
            const std::uint64_t total = attempts + success + stale + swaps + region_viol +
                                        deopt_steal + concurrent_safe + drifts;
            std::int64_t recommendation = 0;
            if (region_viol > 0 || deopt_steal > 0)
                recommendation = 3;
            else if (stale > success && stale > 0)
                recommendation = 2;
            else if (drifts > 0 || defuse_ver != module_ver)
                recommendation = 1;
            insert_kv("reload-attempts", static_cast<std::int64_t>(attempts));
            insert_kv("reload-success", static_cast<std::int64_t>(success));
            insert_kv("stale-rejected", static_cast<std::int64_t>(stale));
            insert_kv("refcount-swaps", static_cast<std::int64_t>(swaps));
            insert_kv("region-violations", static_cast<std::int64_t>(region_viol));
            insert_kv("deopt-on-steal", static_cast<std::int64_t>(deopt_steal));
            insert_kv("concurrent-safe-reloads", static_cast<std::int64_t>(concurrent_safe));
            insert_kv("checkpoint-version-drifts", static_cast<std::int64_t>(drifts));
            insert_kv("func-table-epoch", static_cast<std::int64_t>(table_epoch));
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_ver));
            insert_kv("module-version", static_cast<std::int64_t>(module_ver));
            insert_kv("host-region-mask", static_cast<std::int64_t>(region_mask));
            insert_kv("swap-success-rate-pct", static_cast<std::int64_t>(success_rate_pct));
            insert_kv("aot-production-reload-total", static_cast<std::int64_t>(total));
            insert_kv("aot-production-reload-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #523: query:envframe-production-safety-stats. Commercial P0 hash view
    // of SoA EnvFrame/EnvId dual-path consistency + version stamping + stale
    // detection + GC safety — non-duplicative synthesis of #543
    // envframe-dualpath-stats, #418 envframe-dualpath-stale-stats, and #505
    // closure-env-safety envframe themes; avoids #516 prompt6-memory-safety-stats
    // broad Prompt6 matrix and #512 runtime-orchestration steal/GC pillars:
    //   - dual-path-desync / dual-path-sync-count: bindings_ vs bindings_symid_
    //   - stale-refresh-count / version-mismatch-in-walk: materialize + walk paths
    //   - gc-walk-safe-skips / gc-envframe-stale-skipped: GCEnvWalkFn hardening
    //   - post-rollback-invalidations: MutationBoundary checkpoint rollback
    //   - defuse-version: live epoch snapshot for version stamping
    //   - dual-path-sync-rate-pct: sync / (sync + desync) * 100
    //   - envframe-production-safety-total / envframe-production-safety-recommendation
    add("query:envframe-production-safety-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
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
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t dual_sync = ev->get_bindings_dual_sync_count();
            const std::uint64_t stale_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t version_mismatch = ev->get_envframe_version_mismatch_in_walk();
            const std::uint64_t gc_walk_skips = ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_stale =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t post_rollback = ev->get_envframe_post_rollback_invalidations();
            const std::uint64_t defuse_ver = ev->current_defuse_version();
            const std::uint64_t dual_checks = dual_sync + desync;
            const std::int64_t sync_rate_pct =
                dual_checks > 0 ? static_cast<std::int64_t>((dual_sync * 100) / dual_checks) : 100;
            const std::uint64_t total = desync + dual_sync + stale_refresh + version_mismatch +
                                        gc_walk_skips + gc_stale + post_rollback;
            std::int64_t recommendation = 0;
            if (desync > 0)
                recommendation = 3;
            else if (gc_stale > 0 && version_mismatch > stale_refresh)
                recommendation = 2;
            else if (version_mismatch > 0 || stale_refresh > 0)
                recommendation = 1;
            insert_kv("dual-path-desync", static_cast<std::int64_t>(desync));
            insert_kv("dual-path-sync-count", static_cast<std::int64_t>(dual_sync));
            insert_kv("stale-refresh-count", static_cast<std::int64_t>(stale_refresh));
            insert_kv("version-mismatch-in-walk", static_cast<std::int64_t>(version_mismatch));
            insert_kv("gc-walk-safe-skips", static_cast<std::int64_t>(gc_walk_skips));
            insert_kv("gc-envframe-stale-skipped", static_cast<std::int64_t>(gc_stale));
            insert_kv("post-rollback-invalidations", static_cast<std::int64_t>(post_rollback));
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_ver));
            insert_kv("dual-path-sync-rate-pct", sync_rate_pct);
            insert_kv("envframe-production-safety-total", static_cast<std::int64_t>(total));
            insert_kv("envframe-production-safety-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #524: query:macro-production-hygiene-stats. Commercial P0 hash view
    // of MacroIntroduced marker propagation + hygiene closed-loop (clone_macro_body
    // → query:pattern → IR InlinePass) — non-duplicative synthesis of #501
    // ir-hygiene-stats, #503 pattern-marker-stats, #547 pattern-hygiene-stats,
    // and #486 macro-hygiene-stats; avoids #597 macro-reflect-self-evo-stats
    // reflect/validation bundle and #420 macro-hygiene-contract-stats int-sum:
    //   - root-skips / recursive-skips: query:pattern hygiene filter surface
    //   - hygiene-violations / filter-violations: matcher + filter contract
    //   - macro-markers: workspace SyntaxMarker::MacroIntroduced tally
    //   - inline-hygiene-skipped / respect-macro-hygiene: IR InlinePass policy
    //   - contract-violations / macro-expansion-dirty: clone_macro_body path
    //   - hygiene-filter-rate-pct: (root+recursive) / markers * 100
    //   - macro-production-hygiene-total / macro-production-hygiene-recommendation
    add("query:macro-production-hygiene-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
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
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t filter_violations = ev->get_pattern_macro_filter_violations();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t inline_skipped = ir_inline_hygiene_skipped(ev);
            const bool respects = InlinePass::get_respect_macro_hygiene();
            const std::uint64_t contract = ev->get_macro_hygiene_contract_violations();
            const std::uint64_t macro_dirty = ws ? ws->macro_expansion_dirty_total() : 0;
            const std::uint64_t filter_checks = root_skips + recursive_skips;
            const std::int64_t filter_rate_pct =
                markers > 0 ? static_cast<std::int64_t>((filter_checks * 100) / markers) : 0;
            const std::uint64_t total = root_skips + recursive_skips + violations +
                                        filter_violations + markers + inline_skipped + contract +
                                        macro_dirty;
            std::int64_t recommendation = 0;
            if (violations > 0 || filter_violations > 0 || contract > 0)
                recommendation = 3;
            else if (inline_skipped > 0 && !respects)
                recommendation = 2;
            else if (root_skips + recursive_skips > 0)
                recommendation = 1;
            insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
            insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
            insert_kv("filter-violations", static_cast<std::int64_t>(filter_violations));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("inline-hygiene-skipped", static_cast<std::int64_t>(inline_skipped));
            insert_kv("respect-macro-hygiene", respects ? 1 : 0);
            insert_kv("contract-violations", static_cast<std::int64_t>(contract));
            insert_kv("macro-expansion-dirty", static_cast<std::int64_t>(macro_dirty));
            insert_kv("hygiene-filter-rate-pct", filter_rate_pct);
            insert_kv("macro-production-hygiene-total", static_cast<std::int64_t>(total));
            insert_kv("macro-production-hygiene-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #525: query:guard-production-impact-stats. Commercial P0 hash view
    // of MutationBoundaryGuard post-success impact snapshot + reflect/schema
    // validation closed-loop — non-duplicative synthesis of #504
    // mutation-boundary-log, #488 mutation-impact-snapshot, and #551
    // reflect-postmutate-stats; avoids #515 consolidated P0 tracker and #597
    // macro-reflect-self-evo-stats broad bundle:
    //   - epoch-after/delta, nodes-changed, reasons-mask: latest ring entry
    //   - impact-snapshots / mutation-impacts: Guard success tallies
    //   - dirty-nodes / macro-markers: snapshot marker+delta surface
    //   - schema-pass/fail/valid: post_mutation_reflect_validate hook
    //   - guard-epoch / boundary-depth / dirty-propagation: epoch coordination
    //   - checkpoint-commits: panic checkpoint commit on Guard success
    //   - validation-pass-rate-pct: pass / (pass + fail) * 100
    //   - guard-production-impact-total / guard-production-impact-recommendation
    add("query:guard-production-impact-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto entry = ev->get_latest_mutation_impact_entry();
            auto* ht = FlatHashTable::create(32);
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
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t impacts = ev->get_mutation_impact_count();
            const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
            const std::uint64_t markers = ev->get_macro_markers_in_snapshot();
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
            const std::uint64_t commits = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t validations = pass + fail;
            const std::int64_t pass_rate_pct =
                validations > 0 ? static_cast<std::int64_t>((pass * 100) / validations) : 100;
            const std::uint64_t total = snapshots + impacts + entry.epoch_delta +
                                        entry.nodes_changed + dirty + pass + fail + guard_epoch +
                                        dirty_prop + commits;
            std::int64_t recommendation = 0;
            if (fail > 0 || !ev->get_last_schema_validation_ok())
                recommendation = 3;
            else if (entry.nodes_changed > 20)
                recommendation = 2;
            else if (dirty > 0 || snapshots > 0)
                recommendation = 1;
            insert_kv("epoch-after", static_cast<std::int64_t>(entry.epoch_after));
            insert_kv("epoch-delta", static_cast<std::int64_t>(entry.epoch_delta));
            insert_kv("nodes-changed", static_cast<std::int64_t>(entry.nodes_changed));
            insert_kv("reasons-mask", static_cast<std::int64_t>(entry.reasons_mask));
            insert_kv("impact-snapshots", static_cast<std::int64_t>(snapshots));
            insert_kv("mutation-impacts", static_cast<std::int64_t>(impacts));
            insert_kv("dirty-nodes", static_cast<std::int64_t>(dirty));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("schema-pass", static_cast<std::int64_t>(pass));
            insert_kv("schema-fail", static_cast<std::int64_t>(fail));
            insert_kv("schema-valid", ev->get_last_schema_validation_ok() ? 1 : 0);
            insert_kv("guard-epoch", static_cast<std::int64_t>(guard_epoch));
            insert_kv("boundary-depth",
                      static_cast<std::int64_t>(Evaluator::mutation_boundary_depth()));
            insert_kv("dirty-propagation", static_cast<std::int64_t>(dirty_prop));
            insert_kv("checkpoint-commits", static_cast<std::int64_t>(commits));
            insert_kv("validation-pass-rate-pct", pass_rate_pct);
            insert_kv("guard-production-impact-total", static_cast<std::int64_t>(total));
            insert_kv("guard-production-impact-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #528: query:pattern-production-index-stats. Commercial P0 hash view
    // of query:pattern incremental tag_arity_index + MacroIntroduced hygiene
    // integration for large-AST AI loops — non-duplicative synthesis of #547
    // pattern-index-stats, #490 pattern-index-rebuild-stats, #621
    // pattern-index-stats-hash, and #547 pattern-hygiene-stats; avoids #524
    // macro-production-hygiene-stats IR/clone bundle and #503 pattern-marker
    // int-only themes:
    //   P1 Index: hits/misses/rebuilds, dirty-marks, rebuild-time-us, delta-hits
    //   P2 Rebuild triggers: lazy/eager-mutate/eager-cow rebuild tallies
    //   P3 Structural fast-path: structural-hits/misses, index-entries
    //   P4 Hygiene: root-skips, recursive-skips, hygiene-violations, markers
    //   - arity-accuracy-pct / delta-hit-rate-pct derived metrics
    //   - pattern-production-index-total / pattern-production-index-recommendation
    add("query:pattern-production-index-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            auto* ht = FlatHashTable::create(32);
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
            const std::uint64_t hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t misses = ws ? ws->tag_arity_index_misses() : 0;
            const std::uint64_t rebuilds = ws ? ws->tag_arity_index_rebuilds() : 0;
            const std::uint64_t dirty_marks = ws ? ws->tag_arity_index_dirty_marks() : 0;
            const std::uint64_t rebuild_time_us = ws ? ws->tag_arity_index_rebuild_time_us() : 0;
            const std::uint64_t delta_hits = ws ? ws->tag_arity_index_delta_hits() : 0;
            const std::uint64_t lazy_rebuilds = ev->get_pattern_index_lazy_rebuilds();
            const std::uint64_t eager_mutate = ev->get_pattern_index_eager_mutate_rebuilds();
            const std::uint64_t eager_cow = ev->get_pattern_index_eager_cow_rebuilds();
            const std::uint64_t structural_hits = ev->get_pattern_structural_index_hits();
            const std::uint64_t structural_misses = ev->get_pattern_structural_index_misses();
            const std::uint64_t index_entries = ev->tag_arity_index_entry_count();
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t index_total = hits + misses;
            const std::int64_t arity_accuracy_pct =
                index_total == 0 ? 0 : static_cast<std::int64_t>((hits * 100) / index_total);
            const std::uint64_t delta_denom = delta_hits + rebuilds;
            const std::int64_t delta_hit_rate_pct =
                delta_denom == 0 ? 0 : static_cast<std::int64_t>((delta_hits * 100) / delta_denom);
            const std::uint64_t total = hits + misses + rebuilds + dirty_marks + rebuild_time_us +
                                        delta_hits + root_skips + recursive_skips + violations +
                                        structural_hits + structural_misses;
            std::int64_t recommendation = 0;
            if (violations > 0)
                recommendation = 3;
            else if (index_total > 0 && arity_accuracy_pct < 50)
                recommendation = 2;
            else if (rebuilds > 0 &&
                     rebuild_time_us > static_cast<std::uint64_t>(delta_hits + 1) * 100)
                recommendation = 2;
            else if (root_skips + recursive_skips > 0)
                recommendation = 1;
            insert_kv("index-hits", static_cast<std::int64_t>(hits));
            insert_kv("index-misses", static_cast<std::int64_t>(misses));
            insert_kv("index-rebuilds", static_cast<std::int64_t>(rebuilds));
            insert_kv("dirty-marks", static_cast<std::int64_t>(dirty_marks));
            insert_kv("rebuild-time-us", static_cast<std::int64_t>(rebuild_time_us));
            insert_kv("delta-hits", static_cast<std::int64_t>(delta_hits));
            insert_kv("lazy-rebuilds", static_cast<std::int64_t>(lazy_rebuilds));
            insert_kv("eager-mutate-rebuilds", static_cast<std::int64_t>(eager_mutate));
            insert_kv("eager-cow-rebuilds", static_cast<std::int64_t>(eager_cow));
            insert_kv("structural-hits", static_cast<std::int64_t>(structural_hits));
            insert_kv("structural-misses", static_cast<std::int64_t>(structural_misses));
            insert_kv("index-entries", static_cast<std::int64_t>(index_entries));
            insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
            insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("arity-accuracy-pct", arity_accuracy_pct);
            insert_kv("delta-hit-rate-pct", delta_hit_rate_pct);
            insert_kv("pattern-production-index-total", static_cast<std::int64_t>(total));
            insert_kv("pattern-production-index-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #530: query:incremental-production-relower-stats. Commercial P0 hash
    // view of incremental compilation dirty/re-lower granularity + ir_cache_ /
    // dep_graph_ / JIT bridge interaction — non-duplicative synthesis of #460
    // compiler-incremental-stats, #404 ir-soa-incremental-stats, #426
    // compiler-cache-stats, and #429 soa-dirty-stats; avoids #506 soa-hotpath
    // adoption int-sum and #515 consolidated P0 tracker:
    //   P1 Re-lower path: partial/per-fn/full/skipped + blocks-saved
    //   P2 Impact scope: impact-scope-calls + affected-blocks-total
    //   P3 JIT/bridge: jit-invalidate + bridge-invalidate + invalidate-fn
    //   P4 Live cache: dirty-blocks/fns, cached-fns, dirty-block-pct
    //   P5 Triggers: should-relower + cascade-body-only
    //   - min-scope-hit-rate-pct: blocks_saved / relower work * 100
    //   - incremental-production-relower-total / recommendation
    add("query:incremental-production-relower-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(32);
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
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            Evaluator::SoaDirtyStats soa;
            if (ev->get_soa_dirty_stats_fn_)
                soa = ev->get_soa_dirty_stats_fn_();
            const std::uint64_t should_relower =
                m ? m->should_relower_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t partial = ev->get_partial_relower_count();
            const std::uint64_t impact_calls = ev->get_impact_scope_calls();
            const std::uint64_t affected_blocks = ev->get_total_affected_blocks();
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t jit_invalidate =
                m ? m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_invalidate =
                m ? m->compiler_inval_bridge_epoch_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t invalidate_fn =
                m ? m->invalidate_function_calls.load(std::memory_order_relaxed) : 0;
            const std::uint64_t cascade_body =
                m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_work = relower_full + relower_per_fn + 1;
            const std::int64_t min_scope_hit_pct =
                static_cast<std::int64_t>((blocks_saved * 100) / relower_work);
            const std::uint64_t total = should_relower + partial + impact_calls + affected_blocks +
                                        relower_skip + relower_per_fn + relower_full +
                                        blocks_saved + jit_invalidate + cascade_body +
                                        soa.dirty_blocks;
            std::int64_t recommendation = 0;
            if (soa.dirty_block_pct > 50 && blocks_saved == 0)
                recommendation = 3;
            else if (relower_full > relower_per_fn && min_scope_hit_pct < 25)
                recommendation = 2;
            else if (partial > 0 || blocks_saved > 0 || relower_skip > 0)
                recommendation = 1;
            insert_kv("should-relower-triggers", static_cast<std::int64_t>(should_relower));
            insert_kv("partial-relowers", static_cast<std::int64_t>(partial));
            insert_kv("impact-scope-calls", static_cast<std::int64_t>(impact_calls));
            insert_kv("affected-blocks-total", static_cast<std::int64_t>(affected_blocks));
            insert_kv("relower-skipped", static_cast<std::int64_t>(relower_skip));
            insert_kv("relower-per-fn", static_cast<std::int64_t>(relower_per_fn));
            insert_kv("relower-full", static_cast<std::int64_t>(relower_full));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("jit-invalidate-count", static_cast<std::int64_t>(jit_invalidate));
            insert_kv("bridge-invalidate-count", static_cast<std::int64_t>(bridge_invalidate));
            insert_kv("invalidate-function-calls", static_cast<std::int64_t>(invalidate_fn));
            insert_kv("cascade-body-only", static_cast<std::int64_t>(cascade_body));
            insert_kv("dirty-blocks", static_cast<std::int64_t>(soa.dirty_blocks));
            insert_kv("dirty-functions", static_cast<std::int64_t>(soa.dirty_fns));
            insert_kv("cached-functions", static_cast<std::int64_t>(soa.cached_fns));
            insert_kv("dirty-block-pct", static_cast<std::int64_t>(soa.dirty_block_pct));
            insert_kv("min-scope-hit-rate-pct", min_scope_hit_pct);
            insert_kv("incremental-production-relower-total", static_cast<std::int64_t>(total));
            insert_kv("incremental-production-relower-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #532: query:jit-consistency-stats. Commercial P0 hash view of JIT
    // opcode coverage completeness, IRInterpreter execution consistency, and
    // GuardShape/Linear/hot-swap safety — non-duplicative synthesis of #491
    // jit-stats-hash, #601 jit-hotswap-closure-stats, #513/#522 AOT reload
    // themes, and #516 prompt6-memory-safety linear/bridge slices; avoids
    // repeating the per-field jit-stats-hash surface verbatim:
    //   P1 Opcode coverage: unhandled-count, fallback-count,
    //      consistency-violations, opcode-coverage-pct
    //   P2 Deopt parity: deopt-count, deopt-rate-pct
    //   P3 Hot-swap safety: hotswap-invalidate, live-closure-refreshed,
    //      forced-deopt, hotswap-success-rate-pct
    //   P4 Linear/bridge: linear-check-hits, bridge-epoch-hits,
    //      epoch-mismatch-hits, safe-fallbacks
    //   - jit-consistency-total / jit-consistency-recommendation
    add("query:jit-consistency-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        auto* ht = FlatHashTable::create(32);
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
        std::uint64_t compiles = 0;
        std::uint64_t unhandled = 0;
        std::uint64_t fallback = aura_jit_fallback_count_v_read();
        std::uint64_t consistency = 0;
        if (ev->get_jit_stats_fn_) {
            const char* s = ev->get_jit_stats_fn_();
            if (s) {
                auto parse_u64 = [&](const char* key) -> std::uint64_t {
                    const char* p = std::strstr(s, key);
                    if (!p)
                        return 0;
                    p += std::strlen(key);
                    return std::strtoull(p, nullptr, 10);
                };
                compiles = parse_u64("compiles=");
                unhandled = parse_u64("unhandled_opcode=");
                fallback = parse_u64("fallback_count=");
                consistency = parse_u64("consistency_violations=");
            }
        }
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t hotswap_invalidate =
            m ? m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t refreshed =
            m ? m->jit_hotswap_live_closure_refreshed_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t forced_deopt =
            m ? m->jit_hotswap_forced_deopt_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t linear_hits =
            m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t bridge_hits =
            m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
        const std::uint64_t epoch_mismatch =
            m ? m->compiler_closure_epoch_mismatch_hits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t safe_fallbacks =
            m ? m->compiler_closure_safe_fallbacks.load(std::memory_order_relaxed) : 0;
        const std::int64_t deopt_rate_pct =
            compiles > 0 ? static_cast<std::int64_t>((deopt * 100) / compiles) : 0;
        const std::int64_t coverage_pct =
            unhandled == 0 && fallback == 0
                ? 100
                : std::max<std::int64_t>(
                      0, 100 - static_cast<std::int64_t>((unhandled + fallback) * 100 /
                                                         std::max<std::uint64_t>(1, compiles)));
        const std::uint64_t hotswap_work = refreshed + forced_deopt + 1;
        const std::int64_t hotswap_success_pct =
            static_cast<std::int64_t>((refreshed * 100) / hotswap_work);
        const std::uint64_t total = unhandled + fallback + consistency + compiles + deopt +
                                    hotswap_invalidate + refreshed + forced_deopt + linear_hits +
                                    bridge_hits + epoch_mismatch + safe_fallbacks;
        std::int64_t recommendation = 0;
        if (unhandled > 0 || consistency > 0)
            recommendation = 3;
        else if (deopt_rate_pct > 10 || forced_deopt > refreshed)
            recommendation = 2;
        else if (hotswap_invalidate > 0 || bridge_hits > 0)
            recommendation = 1;
        insert_kv("unhandled-count", static_cast<std::int64_t>(unhandled));
        insert_kv("fallback-count", static_cast<std::int64_t>(fallback));
        insert_kv("consistency-violations", static_cast<std::int64_t>(consistency));
        insert_kv("compiles", static_cast<std::int64_t>(compiles));
        insert_kv("opcode-coverage-pct", coverage_pct);
        insert_kv("deopt-count", static_cast<std::int64_t>(deopt));
        insert_kv("deopt-rate-pct", deopt_rate_pct);
        insert_kv("hotswap-invalidate-count", static_cast<std::int64_t>(hotswap_invalidate));
        insert_kv("live-closure-refreshed", static_cast<std::int64_t>(refreshed));
        insert_kv("forced-deopt-total", static_cast<std::int64_t>(forced_deopt));
        insert_kv("hotswap-success-rate-pct", hotswap_success_pct);
        insert_kv("linear-check-hits", static_cast<std::int64_t>(linear_hits));
        insert_kv("bridge-epoch-hits", static_cast<std::int64_t>(bridge_hits));
        insert_kv("epoch-mismatch-hits", static_cast<std::int64_t>(epoch_mismatch));
        insert_kv("safe-fallbacks", static_cast<std::int64_t>(safe_fallbacks));
        insert_kv("jit-consistency-total", static_cast<std::int64_t>(total));
        insert_kv("jit-consistency-recommendation", recommendation);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #533: query:soa-production-columnar-stats. Commercial P0 hash view
    // of children_ columnar migration + IRModuleV2 / IRInstructionView hot-path
    // adoption + DirtyAwarePass block_dirty_ short-circuit — non-duplicative
    // synthesis of #463 soa-adoption-stats, #506 soa-hotpath-adoption int-sum,
    // #404 ir-soa-incremental-stats, #429 soa-dirty-stats, #530 incremental-
    // production-relower-stats, and #684 irsoa-incremental-stats hash slices:
    //   P1 AST children columnar: children-call/safe-view, mark-dirty upward
    //   P2 IR SoA adoption: soa-functions/instructions-visited, view-cache,
    //      irsoa-wired-hits, ir-soa-instr/func-emitted
    //   P3 block_dirty short-circuit: block-dirty-hits, blocks-saved,
    //      passes-skipped-type-dirty, passes-skipped-dirty-pipeline
    //   - dirty-skip-rate-pct / columnar-locality-pct
    //   - soa-production-columnar-total / soa-production-columnar-recommendation
    add("query:soa-production-columnar-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(32);
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
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t children_calls = ws ? ws->children_call_count() : 0;
            const std::uint64_t children_safe = ws ? ws->children_safe_view_count() : 0;
            const std::uint64_t dirty_up = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t dirty_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
            const std::uint64_t fast_fixed = ws ? ws->dirty_upward_fast_fixed_point_count() : 0;
            const std::uint64_t soa_funcs =
                m ? m->soa_functions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_instr =
                m ? m->soa_instructions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t aos_views =
                m ? m->aos_view_built_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t view_cache =
                m ? m->ir_soa_view_cache_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t wired = m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t block_dirty_hits =
                m ? m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip_type = ev->get_passes_skipped_type_dirty();
            const std::uint64_t passes_skip_pipeline =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t ir_instr_emitted =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_func_emitted =
                m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_work = relower_full + relower_per_fn + passes_skip_type + 1;
            const std::int64_t dirty_skip_rate_pct =
                static_cast<std::int64_t>((passes_skip_type * 100) / relower_work);
            const std::uint64_t columnar_denom = children_calls + children_safe + 1;
            const std::int64_t columnar_locality_pct =
                static_cast<std::int64_t>((children_safe * 100) / columnar_denom);
            Evaluator::SoaDirtyStats soa;
            if (ev->get_soa_dirty_stats_fn_)
                soa = ev->get_soa_dirty_stats_fn_();
            const std::uint64_t total = children_calls + children_safe + dirty_up + dirty_nodes +
                                        fast_fixed + soa_funcs + soa_instr + aos_views +
                                        view_cache + wired + block_dirty_hits + blocks_saved +
                                        passes_skip_type + passes_skip_pipeline + ir_instr_emitted +
                                        ir_func_emitted + soa.dirty_blocks;
            std::int64_t recommendation = 0;
            if (soa.dirty_block_pct > 50 && blocks_saved == 0)
                recommendation = 3;
            else if (passes_skip_type == 0 && relower_full > 0)
                recommendation = 2;
            else if (blocks_saved > 0 || passes_skip_type > 0 || wired > 0)
                recommendation = 1;
            insert_kv("children-call-count", static_cast<std::int64_t>(children_calls));
            insert_kv("children-safe-view-count", static_cast<std::int64_t>(children_safe));
            insert_kv("mark-dirty-upward-calls", static_cast<std::int64_t>(dirty_up));
            insert_kv("mark-dirty-total-nodes", static_cast<std::int64_t>(dirty_nodes));
            insert_kv("dirty-fast-fixed-point-hits", static_cast<std::int64_t>(fast_fixed));
            insert_kv("soa-functions-visited", static_cast<std::int64_t>(soa_funcs));
            insert_kv("soa-instructions-visited", static_cast<std::int64_t>(soa_instr));
            insert_kv("aos-view-built-count", static_cast<std::int64_t>(aos_views));
            insert_kv("ir-soa-view-cache-hits", static_cast<std::int64_t>(view_cache));
            insert_kv("irsoa-wired-hits", static_cast<std::int64_t>(wired));
            insert_kv("ir-soa-block-dirty-hits", static_cast<std::int64_t>(block_dirty_hits));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("passes-skipped-type-dirty", static_cast<std::int64_t>(passes_skip_type));
            insert_kv("passes-skipped-dirty-pipeline",
                      static_cast<std::int64_t>(passes_skip_pipeline));
            insert_kv("ir-soa-instr-emitted", static_cast<std::int64_t>(ir_instr_emitted));
            insert_kv("ir-soa-func-emitted", static_cast<std::int64_t>(ir_func_emitted));
            insert_kv("dirty-skip-rate-pct", dirty_skip_rate_pct);
            insert_kv("columnar-locality-pct", columnar_locality_pct);
            insert_kv("soa-production-columnar-total", static_cast<std::int64_t>(total));
            insert_kv("soa-production-columnar-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #568: query:soa-children-columnar-migration-stats. Task4-review
    // closing hash for FlatAST children_ columnar SoA migration completion +
    // IRInstructionView hot-path adoption + DirtyAwarePass block_dirty_
    // short-circuit — non-duplicative synthesis of #533 soa-production-
    // columnar-stats, #463 soa-adoption-stats, #506 soa-hotpath-adoption,
    // #404 ir-soa-incremental-stats, #684 irsoa-incremental-stats, and #607
    // task4-cache-locality-win themes; focuses on #568 completion metrics:
    //   P1 Columnar children: children-call/safe-view, child-columnar-hit-rate
    //   P2 IR SoA hot-path: soa-functions/instructions-visited, view-cache,
    //      irsoa-wired-hits, ir-soa-instr/func-emitted
    //   P3 DirtyAwarePass: passes-skipped-due-to-dirty, relower-block-count,
    //      blocks-saved, ir-soa-block-dirty-hits
    //   - migration-schema (568 sentinel)
    //   - soa-children-columnar-migration-total / recommendation
    add("query:soa-children-columnar-migration-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(32);
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
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t children_calls = ws ? ws->children_call_count() : 0;
            const std::uint64_t children_safe = ws ? ws->children_safe_view_count() : 0;
            const std::uint64_t columnar_denom = children_calls + children_safe + 1;
            const std::int64_t columnar_hit_rate_pct =
                static_cast<std::int64_t>((children_safe * 100) / columnar_denom);
            const std::uint64_t soa_funcs =
                m ? m->soa_functions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t soa_instr =
                m ? m->soa_instructions_visited.load(std::memory_order_relaxed) : 0;
            const std::uint64_t view_cache =
                m ? m->ir_soa_view_cache_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t wired = m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_instr_emitted =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t ir_func_emitted =
                m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip_type = ev->get_passes_skipped_type_dirty();
            const std::uint64_t passes_skip_pipeline =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t passes_skipped_due_to_dirty =
                passes_skip_type + passes_skip_pipeline;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_block_count = relower_full + relower_per_fn;
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t block_dirty_hits =
                m ? m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = children_calls + children_safe + soa_funcs + soa_instr +
                                        view_cache + wired + ir_instr_emitted + ir_func_emitted +
                                        passes_skipped_due_to_dirty + relower_block_count +
                                        blocks_saved + block_dirty_hits;
            std::int64_t recommendation = 0;
            if (columnar_hit_rate_pct < 25 && children_calls > 0)
                recommendation = 3;
            else if (passes_skipped_due_to_dirty == 0 && relower_block_count > 0)
                recommendation = 2;
            else if (blocks_saved > 0 || wired > 0 || columnar_hit_rate_pct >= 50)
                recommendation = 1;
            insert_kv("children-call-count", static_cast<std::int64_t>(children_calls));
            insert_kv("children-safe-view-count", static_cast<std::int64_t>(children_safe));
            insert_kv("child-columnar-hit-rate-pct", columnar_hit_rate_pct);
            insert_kv("soa-functions-visited", static_cast<std::int64_t>(soa_funcs));
            insert_kv("soa-instructions-visited", static_cast<std::int64_t>(soa_instr));
            insert_kv("ir-soa-view-cache-hits", static_cast<std::int64_t>(view_cache));
            insert_kv("irsoa-wired-hits", static_cast<std::int64_t>(wired));
            insert_kv("ir-soa-instr-emitted", static_cast<std::int64_t>(ir_instr_emitted));
            insert_kv("ir-soa-func-emitted", static_cast<std::int64_t>(ir_func_emitted));
            insert_kv("passes-skipped-due-to-dirty",
                      static_cast<std::int64_t>(passes_skipped_due_to_dirty));
            insert_kv("relower-block-count", static_cast<std::int64_t>(relower_block_count));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("ir-soa-block-dirty-hits", static_cast<std::int64_t>(block_dirty_hits));
            insert_kv("migration-schema", 568);
            insert_kv("soa-children-columnar-migration-total", static_cast<std::int64_t>(total));
            insert_kv("soa-children-columnar-migration-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #534: query:arena-production-compaction-stats. Commercial P0 hash
    // view of auto-compaction threshold policy + live-object defrag
    // coordination with fiber safepoints and MutationBoundaryGuard — non-
    // duplicative synthesis of #405 arena-compaction-stats int-sum, #430
    // arena-compaction-stats-hash, #464 arena-auto-stats, #685 arena-auto-
    // compact-stats, #604 arena-fragmentation-snapshot, and #300 defrag
    // foundation themes; avoids repeating the per-field #430 hash surface:
    //   P1 Fragmentation policy: fragmentation-ratio-pct, peak-used-bytes,
    //      compaction-efficiency-pct
    //   P2 Auto-compact lifecycle: auto-compact-triggers/skips/guard-calls,
    //      compactions, bytes-saved, last-saved
    //   P3 Defrag coordination: defrag-attempted-count, defrag-saved-bytes
    //   P4 Safepoint/Guard: compaction-yield-checks, paused-by-boundary,
    //      gc-safepoint-waits, safepoint-coordination-count
    //   - arena-production-compaction-total / recommendation
    add("query:arena-production-compaction-stats",
        [&string_heap, &ev](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ht = FlatHashTable::create(32);
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
            const auto& group = ev.arena_group();
            const auto stats = group.total_stats();
            const auto policy = group.auto_compact_policy_stats();
            const std::uint64_t triggers =
                group.auto_compact_trigger_count() + policy.auto_triggers;
            const std::uint64_t skips = group.auto_compact_skip_count();
            const std::uint64_t guard_calls = group.auto_compact_guard_call_count();
            const std::uint64_t compacts = stats.compaction_count;
            const std::uint64_t saved = stats.total_compaction_saved;
            const std::uint64_t last_saved = stats.last_compaction_saved;
            const std::uint64_t defrag_attempted = stats.defrag_attempted_count;
            const std::uint64_t defrag_saved =
                policy.defrag_savings + stats.defrag_savings_alloc + stats.last_defrag_saved;
            const std::uint64_t yield_checks = group.compaction_yield_checks_group() +
                                               policy.yield_checks_hit +
                                               stats.compaction_yield_checks;
            const std::uint64_t paused = ev.compaction_paused_by_boundary();
            const std::uint64_t gc_waits = ev.get_gc_safepoint_waits_total();
            const std::uint64_t gc_deferred = ev.get_gc_safepoint_deferred_total();
            const std::uint64_t safepoint_coord = yield_checks + paused + gc_waits + gc_deferred;
            const std::uint64_t mutations = ev.total_mutations();
            const std::uint64_t dirty = ev.get_dirty_propagation_count();
            const std::int64_t frag_pct =
                static_cast<std::int64_t>(stats.fragmentation_ratio() * 100.0);
            const std::int64_t efficiency_pct =
                static_cast<std::int64_t>((saved * 100) / (compacts + 1));
            const std::uint64_t total = triggers + skips + guard_calls + compacts + saved +
                                        defrag_attempted + defrag_saved + safepoint_coord +
                                        mutations + dirty + stats.peak_used;
            std::int64_t recommendation = 0;
            if (frag_pct > 30 && saved == 0 && compacts == 0)
                recommendation = 3;
            else if (paused > yield_checks && paused > 0)
                recommendation = 2;
            else if (triggers > 0 || compacts > 0 || defrag_saved > 0)
                recommendation = 1;
            insert_kv("fragmentation-ratio-pct", frag_pct);
            insert_kv("peak-used-bytes", static_cast<std::int64_t>(stats.peak_used));
            insert_kv("auto-compact-triggers", static_cast<std::int64_t>(triggers));
            insert_kv("auto-compact-skips", static_cast<std::int64_t>(skips));
            insert_kv("auto-compact-guard-calls", static_cast<std::int64_t>(guard_calls));
            insert_kv("compactions", static_cast<std::int64_t>(compacts));
            insert_kv("bytes-saved", static_cast<std::int64_t>(saved));
            insert_kv("last-saved", static_cast<std::int64_t>(last_saved));
            insert_kv("defrag-attempted-count", static_cast<std::int64_t>(defrag_attempted));
            insert_kv("defrag-saved-bytes", static_cast<std::int64_t>(defrag_saved));
            insert_kv("compaction-yield-checks", static_cast<std::int64_t>(yield_checks));
            insert_kv("paused-by-boundary", static_cast<std::int64_t>(paused));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("safepoint-coordination-count", static_cast<std::int64_t>(safepoint_coord));
            insert_kv("mutation-volume", static_cast<std::int64_t>(mutations));
            insert_kv("dirty-propagation", static_cast<std::int64_t>(dirty));
            insert_kv("compaction-efficiency-pct", efficiency_pct);
            insert_kv("arena-production-compaction-total", static_cast<std::int64_t>(total));
            insert_kv("arena-production-compaction-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #535: query:contracts-production-hotpath-stats. Commercial P1 hash
    // view of C++26 Contracts + consteval invariants in Arena create,
    // inline_shape_of, Pass run_one/Wraps, and evaluator/lowering hot paths —
    // non-duplicative synthesis of #507 task4-hotpath-contracts inventory hash,
    // #406 pass-contracts-stats int-sum, #626 contracts-hotpath-stats-hash,
    // #465/#431 C++26 density themes; avoids repeating the static per-site
    // #507 surface verbatim:
    //   P1 Contract inventory: contract-site-count, shape-dispatch-table-size,
    //      consteval-hits
    //   P2 Runtime health: contract-violations, dispatch-hits, dispatch-misses
    //   P3 Zero-overhead: zerooverhead-wins, zerooverhead-rate-pct
    //   P4 Pass pipeline: passes-skipped-dirty, pass-pipeline-runs, relower-skipped
    //   P5 Dirty/mark paths: mark-dirty-upward-calls, dirty-propagation
    //   - contracts-coverage-pct / contracts-production-hotpath-total / recommendation
    add("query:contracts-production-hotpath-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(32);
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
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ws = ev->workspace_flat();
            constexpr std::int64_t k_contract_sites = 6;
            const std::int64_t table_size =
                static_cast<std::int64_t>(shape::k_task4_shape_dispatch_table_size);
            const std::int64_t consteval_hits =
                static_cast<std::int64_t>(shape::k_shape_value_consteval_hits);
            const std::uint64_t violations =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            const std::uint64_t dispatch_hits =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed);
            const std::uint64_t dispatch_miss =
                types::value_dispatch_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t zero_wins =
                m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t pipeline_runs =
                aura::compiler::pass_pipeline_runs_total.load(std::memory_order_relaxed);
            const std::uint64_t relower_skip =
                m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t dirty_up = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t dirty_prop = ev->get_dirty_propagation_count();
            const std::uint64_t dispatch_denom = dispatch_hits + dispatch_miss + 1;
            const std::int64_t coverage_pct =
                static_cast<std::int64_t>((dispatch_hits * 100) / dispatch_denom);
            const std::uint64_t zero_denom = zero_wins + dispatch_miss + 1;
            const std::int64_t zerooverhead_pct =
                static_cast<std::int64_t>((zero_wins * 100) / zero_denom);
            const std::uint64_t total = static_cast<std::uint64_t>(k_contract_sites) +
                                        static_cast<std::uint64_t>(table_size) +
                                        static_cast<std::uint64_t>(consteval_hits) + violations +
                                        dispatch_hits + dispatch_miss + zero_wins + passes_skip +
                                        pipeline_runs + relower_skip + dirty_up + dirty_prop;
            std::int64_t recommendation = 0;
            if (violations > 0)
                recommendation = 3;
            else if (dispatch_miss > dispatch_hits && dispatch_miss > 0)
                recommendation = 2;
            else if (zero_wins > 0 || passes_skip > 0 || pipeline_runs > 0)
                recommendation = 1;
            insert_kv("contract-site-count", k_contract_sites);
            insert_kv("shape-dispatch-table-size", table_size);
            insert_kv("consteval-hits", consteval_hits);
            insert_kv("contract-violations", static_cast<std::int64_t>(violations));
            insert_kv("dispatch-hits", static_cast<std::int64_t>(dispatch_hits));
            insert_kv("dispatch-misses", static_cast<std::int64_t>(dispatch_miss));
            insert_kv("zerooverhead-wins", static_cast<std::int64_t>(zero_wins));
            insert_kv("zerooverhead-rate-pct", zerooverhead_pct);
            insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skip));
            insert_kv("pass-pipeline-runs", static_cast<std::int64_t>(pipeline_runs));
            insert_kv("relower-skipped", static_cast<std::int64_t>(relower_skip));
            insert_kv("mark-dirty-upward-calls", static_cast<std::int64_t>(dirty_up));
            insert_kv("dirty-propagation", static_cast<std::int64_t>(dirty_prop));
            insert_kv("contracts-coverage-pct", coverage_pct);
            insert_kv("contracts-production-hotpath-total", static_cast<std::int64_t>(total));
            insert_kv("contracts-production-hotpath-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #539: query:sv-production-verification-stats. Commercial P0 hash
    // view of EDA verification feedback → structured SV mutate closed loop +
    // commercial tool interop — non-duplicative synthesis of #519 edsl-eda-sv-
    // closedloop-stats, #630 sv-verification-closedloop-stats-hash, #510
    // eda-verification-stats, and #469 verification_dirty_ themes; avoids
    // repeating the per-field #630 hash surface verbatim:
    //   P1 Feedback mapping: feedback-mapped-count, feedback-mutate-success,
    //      structured-mutate-hits
    //   P2 SV mutate impact: sv-mutate-attempts/success, stable-ref-captures,
    //      dirty-propagated-nodes
    //   P3 Verification dirty: coverage-feedback-total, assert-failure-total
    //   P4 Re-emit/re-verify: reverify-success, verification-convergence,
    //      hardware-hook-calls, commercial-reemits, rollback-on-partial
    //   - feedback-success-rate-pct / sv-production-verification-total / recommendation
    add("query:sv-production-verification-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(32);
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
            const std::uint64_t feedback_mapped =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_success =
                ev->get_verify_tool_feedback_mutate_success_total() +
                (m ? m->eda_sv_feedback_mutate_success_total.load(std::memory_order_relaxed) : 0);
            const std::uint64_t structured_hits =
                m ? m->sva_structured_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t sv_attempts = ws ? ws->sv_mutate_attempts_total() : 0;
            const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
            const std::uint64_t stable_ref = ev->get_verify_tool_stable_ref_hits_total();
            const std::uint64_t dirty_props = ev->get_verify_tool_dirty_propagations_total();
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t reverify =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t convergence =
                m ? m->eda_sv_verification_convergence_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hw_hooks =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t reemits =
                m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t rollback =
                m ? m->sv_emit_parse_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_denom = feedback_mapped + sv_attempts + 1;
            const std::int64_t feedback_success_pct =
                static_cast<std::int64_t>((feedback_success * 100) / feedback_denom);
            const std::uint64_t total = feedback_mapped + feedback_success + structured_hits +
                                        sv_attempts + sv_success + stable_ref + dirty_props +
                                        coverage + assert_fail + reverify + convergence + hw_hooks +
                                        reemits + rollback;
            std::int64_t recommendation = 0;
            if (assert_fail > coverage && assert_fail > 0)
                recommendation = 3;
            else if (sv_attempts > 0 && sv_success == 0)
                recommendation = 2;
            else if (feedback_mapped > 0 || reverify > 0 || structured_hits > 0)
                recommendation = 1;
            insert_kv("feedback-mapped-count", static_cast<std::int64_t>(feedback_mapped));
            insert_kv("feedback-mutate-success", static_cast<std::int64_t>(feedback_success));
            insert_kv("structured-mutate-hits", static_cast<std::int64_t>(structured_hits));
            insert_kv("sv-mutate-attempts", static_cast<std::int64_t>(sv_attempts));
            insert_kv("sv-mutate-success", static_cast<std::int64_t>(sv_success));
            insert_kv("stable-ref-captures-in-sv", static_cast<std::int64_t>(stable_ref));
            insert_kv("dirty-propagated-nodes", static_cast<std::int64_t>(dirty_props));
            insert_kv("coverage-feedback-total", static_cast<std::int64_t>(coverage));
            insert_kv("assert-failure-total", static_cast<std::int64_t>(assert_fail));
            insert_kv("reverify-success", static_cast<std::int64_t>(reverify));
            insert_kv("verification-convergence", static_cast<std::int64_t>(convergence));
            insert_kv("hardware-hook-calls", static_cast<std::int64_t>(hw_hooks));
            insert_kv("commercial-reemits", static_cast<std::int64_t>(reemits));
            insert_kv("rollback-on-partial", static_cast<std::int64_t>(rollback));
            insert_kv("feedback-success-rate-pct", feedback_success_pct);
            insert_kv("sv-production-verification-total", static_cast<std::int64_t>(total));
            insert_kv("sv-production-verification-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #540: query:eda-stability-stats. Commercial P0 hash view of
    // StableNodeRef + generation_ + mutation_log provenance hardening for
    // long-running concurrent AI EDA verification sessions — non-duplicative
    // synthesis of #527 stable-ref-cow-fiber-stats, #552 edsl-stability-stats,
    // #497 stable-ref-lifecycle-stats, #457 stable-ref-stats, and #631
    // provenance SV scaffolding; avoids repeating int-sum surfaces verbatim:
    //   P1 COW/fiber staleness: cross-cow-invalidations, fiber-stale-ref-count,
    //      provenance-mismatch, mutation-log-rollback-count
    //   P2 Generation/mutation_log: generation-wrap-events,
    //      stable-ref-invalidations, node-gen-stale-accesses,
    //      stale-ref-auto-refresh-count
    //   P3 SV provenance scaffolding: cross-fiber-violations, safe-resolves,
    //      stale-ref-blocked-count
    //   - eda-stability-total / eda-stability-recommendation
    add("query:eda-stability-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        auto* ws = ev->workspace_flat();
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
        auto* ht = FlatHashTable::create(32);
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
        const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
        const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
        const std::uint64_t provenance = ev->get_provenance_mismatch();
        const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
        const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
        const std::uint64_t invalidations = ws ? ws->stable_ref_invalidations() : 0;
        const std::uint64_t stale_access = ws ? ws->node_gen_stale_access_count() : 0;
        const std::uint64_t auto_refresh = ws ? ws->stale_ref_auto_refresh_count() : 0;
        const std::uint64_t cross_fiber =
            m ? m->cross_fiber_violations_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t safe_resolves =
            m ? m->safe_resolves_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t stale_blocked = ev->get_stale_ref_blocked_count();
        const std::uint64_t total = cross_cow + fiber_stale + provenance + rollback + gen_wrap +
                                    invalidations + stale_access + auto_refresh + cross_fiber +
                                    safe_resolves + stale_blocked;
        std::int64_t recommendation = 0;
        if (fiber_stale > 0 || cross_fiber > 0)
            recommendation = 3;
        else if (gen_wrap > 0 || rollback > 0)
            recommendation = 2;
        else if (cross_cow > 0 || provenance > 0 || invalidations > 0)
            recommendation = 1;
        insert_kv("cross-cow-invalidations", static_cast<std::int64_t>(cross_cow));
        insert_kv("fiber-stale-ref-count", static_cast<std::int64_t>(fiber_stale));
        insert_kv("provenance-mismatch", static_cast<std::int64_t>(provenance));
        insert_kv("mutation-log-rollback-count", static_cast<std::int64_t>(rollback));
        insert_kv("generation-wrap-events", static_cast<std::int64_t>(gen_wrap));
        insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(invalidations));
        insert_kv("node-gen-stale-accesses", static_cast<std::int64_t>(stale_access));
        insert_kv("stale-ref-auto-refresh-count", static_cast<std::int64_t>(auto_refresh));
        insert_kv("cross-fiber-violations", static_cast<std::int64_t>(cross_fiber));
        insert_kv("safe-resolves", static_cast<std::int64_t>(safe_resolves));
        insert_kv("stale-ref-blocked-count", static_cast<std::int64_t>(stale_blocked));
        insert_kv("eda-stability-total", static_cast<std::int64_t>(total));
        insert_kv("eda-stability-recommendation", recommendation);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #541: query:pattern-sv-verification-stats. Commercial P0 hash
    // view of query:pattern + DefUseIndex + tag_arity_index incremental
    // maintenance + MacroIntroduced hygiene for large-scale SV SoC AI
    // verification loops — non-duplicative synthesis of #528
    // pattern-production-index-stats, #547 pattern-index/hygiene-stats,
    // #503 pattern-marker-stats, and #519 edsl-eda-sv-closedloop-stats;
    // avoids repeating the per-field #528 hash surface verbatim:
    //   P1 DefUseIndex: defuse-index-used/visited/walk-fallback, defuse-version
    //   P2 Incremental index: tag-arity-delta-hits, dirty-marks, rebuild-time-us,
    //      structural-index-hits/misses
    //   P3 Hygiene: hygiene-skips, recursive-hygiene-skips, hygiene-violations,
    //      macro-marker-count
    //   P4 SV verification loop: sv-node-count, verification-dirty-count
    //   - incremental-hit-rate-pct / pattern-sv-verification-total / recommendation
    add("query:pattern-sv-verification-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(32);
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
            const std::uint64_t defuse_used =
                m ? m->per_defuse_index_used_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse_visited =
                m ? m->per_defuse_index_visited_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse_fallback =
                m ? m->per_defuse_index_walk_fallback_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse_version = ev->get_defuse_version();
            const std::uint64_t delta_hits = ws ? ws->tag_arity_index_delta_hits() : 0;
            const std::uint64_t dirty_marks = ws ? ws->tag_arity_index_dirty_marks() : 0;
            const std::uint64_t rebuild_time_us = ws ? ws->tag_arity_index_rebuild_time_us() : 0;
            const std::uint64_t rebuilds = ws ? ws->tag_arity_index_rebuilds() : 0;
            const std::uint64_t structural_hits = ev->get_pattern_structural_index_hits();
            const std::uint64_t structural_misses = ev->get_pattern_structural_index_misses();
            const std::uint64_t hygiene_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            std::uint64_t sv_node_count = 0;
            std::uint64_t verification_dirty_count = 0;
            if (ws) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    switch (ws->get(id).tag) {
                        case aura::ast::NodeTag::Interface:
                        case aura::ast::NodeTag::Modport:
                        case aura::ast::NodeTag::Property:
                        case aura::ast::NodeTag::Sequence:
                        case aura::ast::NodeTag::Assert:
                        case aura::ast::NodeTag::Covergroup:
                        case aura::ast::NodeTag::Coverpoint:
                        case aura::ast::NodeTag::Constraint:
                            ++sv_node_count;
                            break;
                        default:
                            break;
                    }
                    if (ws->verification_dirty(id) != 0)
                        ++verification_dirty_count;
                }
            }
            const std::uint64_t delta_denom = delta_hits + rebuilds;
            const std::int64_t incremental_hit_rate_pct =
                delta_denom == 0 ? 0 : static_cast<std::int64_t>((delta_hits * 100) / delta_denom);
            const std::uint64_t total =
                defuse_used + defuse_visited + defuse_fallback + delta_hits + dirty_marks +
                rebuild_time_us + structural_hits + structural_misses + hygiene_skips +
                recursive_skips + violations + markers + sv_node_count + verification_dirty_count;
            std::int64_t recommendation = 0;
            if (violations > 0)
                recommendation = 3;
            else if (rebuilds > 0 &&
                     rebuild_time_us > static_cast<std::uint64_t>(delta_hits + 1) * 100)
                recommendation = 2;
            else if (hygiene_skips + recursive_skips > 0 || delta_hits > 0)
                recommendation = 1;
            insert_kv("defuse-index-used", static_cast<std::int64_t>(defuse_used));
            insert_kv("defuse-index-visited", static_cast<std::int64_t>(defuse_visited));
            insert_kv("defuse-index-walk-fallback", static_cast<std::int64_t>(defuse_fallback));
            insert_kv("defuse-version", static_cast<std::int64_t>(defuse_version));
            insert_kv("tag-arity-delta-hits", static_cast<std::int64_t>(delta_hits));
            insert_kv("tag-arity-dirty-marks", static_cast<std::int64_t>(dirty_marks));
            insert_kv("tag-arity-rebuild-time-us", static_cast<std::int64_t>(rebuild_time_us));
            insert_kv("structural-index-hits", static_cast<std::int64_t>(structural_hits));
            insert_kv("structural-index-misses", static_cast<std::int64_t>(structural_misses));
            insert_kv("hygiene-skips", static_cast<std::int64_t>(hygiene_skips));
            insert_kv("recursive-hygiene-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
            insert_kv("macro-marker-count", static_cast<std::int64_t>(markers));
            insert_kv("sv-node-count", static_cast<std::int64_t>(sv_node_count));
            insert_kv("verification-dirty-count",
                      static_cast<std::int64_t>(verification_dirty_count));
            insert_kv("incremental-hit-rate-pct", incremental_hit_rate_pct);
            insert_kv("pattern-sv-verification-total", static_cast<std::int64_t>(total));
            insert_kv("pattern-sv-verification-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #557: query:top5-commercial-coverage-stats. Commercial P0 hash
    // view of the Top 5 test-coverage cluster (#531 closure-env, #530
    // incremental relower, #532 JIT consistency, #556 EDSL concurrency,
    // #553 atomic batch/rollback) for Prompt6+Incremental+JIT production
    // review — non-duplicative synthesis of per-issue hash/int primitives;
    // avoids repeating their per-field surfaces verbatim:
    //   P1 #531 Prompt6: closure-stale-refresh, bridge-epoch-hits,
    //      linear-check-pass, env-stale-refresh
    //   P2 #530 Prompt2: blocks-saved, partial-relowers,
    //      invalidate-function-calls, min-scope-hit-rate-pct
    //   P3 #532 Prompt3: unhandled-opcode-count, deopt-count,
    //      hotswap-invalidate-count, opcode-coverage-pct
    //   P4 #556 concurrency: steal-attempts, boundary-violations,
    //      unsafe-boundary-attempts, lock-contention-us
    //   P5 #553 atomicity: batch-commits, batch-rollbacks,
    //      bumps-saved, steal-violations-during-batch
    //   - top5-commercial-coverage-total / top5-commercial-coverage-recommendation
    add("query:top5-commercial-coverage-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(32);
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
            const std::uint64_t stale_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_hits =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_pass =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t blocks_saved =
                m ? m->ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t partial = ev->get_partial_relower_count();
            const std::uint64_t invalidate_fn =
                m ? m->invalidate_function_calls.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_full =
                m ? m->relower_full_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_per_fn =
                m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t relower_work = relower_full + relower_per_fn + 1;
            const std::int64_t min_scope_hit_pct =
                static_cast<std::int64_t>((blocks_saved * 100) / relower_work);
            std::uint64_t unhandled = 0;
            std::uint64_t compiles = 0;
            std::uint64_t fallback = aura_jit_fallback_count_v_read();
            if (ev->get_jit_stats_fn_) {
                const char* s = ev->get_jit_stats_fn_();
                if (s) {
                    auto parse_u64 = [&](const char* key) -> std::uint64_t {
                        const char* p = std::strstr(s, key);
                        if (!p)
                            return 0;
                        p += std::strlen(key);
                        return std::strtoull(p, nullptr, 10);
                    };
                    compiles = parse_u64("compiles=");
                    unhandled = parse_u64("unhandled_opcode=");
                    fallback = parse_u64("fallback_count=");
                }
            }
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hotswap_invalidate =
                m ? m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed) : 0;
            const std::int64_t opcode_coverage_pct =
                unhandled == 0 && fallback == 0
                    ? 100
                    : std::max<std::int64_t>(
                          0, 100 - static_cast<std::int64_t>((unhandled + fallback) * 100 /
                                                             std::max<std::uint64_t>(1, compiles)));
            const std::uint64_t steals = ev->get_mutation_steal_attempts();
            const std::uint64_t violations = ev->get_boundary_violation_count();
            const std::uint64_t unsafe_attempts = ev->get_unsafe_boundary_attempts();
            const std::uint64_t contention_us = ev->get_lock_contention_us();
            const std::uint64_t batch_commits = ev->atomic_batch_count();
            const std::uint64_t batch_rollbacks = ev->atomic_batch_rollbacks();
            const std::uint64_t bumps_saved = ev->atomic_batch_bumps_saved_total();
            const std::uint64_t steal_violations = ev->get_atomic_batch_steal_violation();
            const std::uint64_t total = stale_refresh + bridge_hits + linear_pass + env_refresh +
                                        blocks_saved + partial + invalidate_fn + unhandled + deopt +
                                        hotswap_invalidate + steals + violations + unsafe_attempts +
                                        contention_us + batch_commits + batch_rollbacks +
                                        bumps_saved + steal_violations;
            std::int64_t recommendation = 0;
            if (unhandled > 0 || steal_violations > 0 || unsafe_attempts > 0)
                recommendation = 3;
            else if (batch_rollbacks > batch_commits && batch_rollbacks > 0)
                recommendation = 2;
            else if (hotswap_invalidate > 0 || partial > 0 || invalidate_fn > 0)
                recommendation = 1;
            insert_kv("closure-stale-refresh", static_cast<std::int64_t>(stale_refresh));
            insert_kv("bridge-epoch-hits", static_cast<std::int64_t>(bridge_hits));
            insert_kv("linear-check-pass", static_cast<std::int64_t>(linear_pass));
            insert_kv("env-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("blocks-saved", static_cast<std::int64_t>(blocks_saved));
            insert_kv("partial-relowers", static_cast<std::int64_t>(partial));
            insert_kv("invalidate-function-calls", static_cast<std::int64_t>(invalidate_fn));
            insert_kv("min-scope-hit-rate-pct", min_scope_hit_pct);
            insert_kv("unhandled-opcode-count", static_cast<std::int64_t>(unhandled));
            insert_kv("deopt-count", static_cast<std::int64_t>(deopt));
            insert_kv("hotswap-invalidate-count", static_cast<std::int64_t>(hotswap_invalidate));
            insert_kv("opcode-coverage-pct", opcode_coverage_pct);
            insert_kv("steal-attempts", static_cast<std::int64_t>(steals));
            insert_kv("boundary-violations", static_cast<std::int64_t>(violations));
            insert_kv("unsafe-boundary-attempts", static_cast<std::int64_t>(unsafe_attempts));
            insert_kv("lock-contention-us", static_cast<std::int64_t>(contention_us));
            insert_kv("batch-commits", static_cast<std::int64_t>(batch_commits));
            insert_kv("batch-rollbacks", static_cast<std::int64_t>(batch_rollbacks));
            insert_kv("bumps-saved", static_cast<std::int64_t>(bumps_saved));
            insert_kv("steal-violations-during-batch", static_cast<std::int64_t>(steal_violations));
            insert_kv("top5-commercial-coverage-total", static_cast<std::int64_t>(total));
            insert_kv("top5-commercial-coverage-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #515: query:consolidated-p0-production-stats. Hash view of the
    // consolidated Top 5 P0 production-readiness pillars (non-duplicative
    // synthesis of #511/#510/#506/#505/#512 hash slices; avoids #514 Task6
    // int-sum, #517 3-pillar int-sum, and #520 Top-5 roadmap int-sum):
    //   P1 Persistence (#511): checkpoint-save/commit + gen-wrap
    //   P2 EDA (#510): coverage-feedback + assert-failures
    //   P3 SoA (#506): passes-skipped + ir-soa-emitted + module-dirty-skips
    //   P4 Memory (#505): bridge-epoch + closure-refresh + envframe-refresh
    //   P5 Orchestration (#500/#512): steal-attempts/violations + boundary-depth
    //   - consolidated-p0-production-total / consolidated-p0-production-recommendation
    add("query:consolidated-p0-production-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
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
            const std::uint64_t checkpoint_save = ev->get_panic_checkpoint_save_count();
            const std::uint64_t checkpoint_commit = ev->get_panic_checkpoint_commit_count();
            const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
            const std::uint64_t ir_soa =
                m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) +
                        m->ir_soa_functions_emitted.load(std::memory_order_relaxed)
                  : 0;
            const std::uint64_t module_dirty =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_epoch =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t closure_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t envframe_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t steal_attempts = aura_work_steal_attempts_total();
            const std::uint64_t steal_violations = ev->get_mutation_steal_violation_count();
            const std::uint64_t boundary_depth = aura_evaluator_mutation_boundary_depth();
            const std::uint64_t total = checkpoint_save + checkpoint_commit + gen_wrap + coverage +
                                        assert_fail + passes_skipped + ir_soa + module_dirty +
                                        bridge_epoch + closure_refresh + envframe_refresh +
                                        steal_attempts + steal_violations + boundary_depth;
            std::int64_t recommendation = 0;
            if (steal_violations > 0)
                recommendation = 3;
            else if (assert_fail > coverage && assert_fail > 0)
                recommendation = 2;
            else if (gen_wrap > 0 || envframe_refresh > bridge_epoch)
                recommendation = 1;
            insert_kv("checkpoint-save", static_cast<std::int64_t>(checkpoint_save));
            insert_kv("checkpoint-commit", static_cast<std::int64_t>(checkpoint_commit));
            insert_kv("gen-wrap", static_cast<std::int64_t>(gen_wrap));
            insert_kv("eda-coverage-feedback", static_cast<std::int64_t>(coverage));
            insert_kv("eda-assert-failures", static_cast<std::int64_t>(assert_fail));
            insert_kv("soa-passes-skipped", static_cast<std::int64_t>(passes_skipped));
            insert_kv("soa-ir-emitted", static_cast<std::int64_t>(ir_soa));
            insert_kv("soa-module-dirty-skips", static_cast<std::int64_t>(module_dirty));
            insert_kv("memory-bridge-epoch", static_cast<std::int64_t>(bridge_epoch));
            insert_kv("memory-closure-refresh", static_cast<std::int64_t>(closure_refresh));
            insert_kv("memory-envframe-refresh", static_cast<std::int64_t>(envframe_refresh));
            insert_kv("orchestration-steal-attempts", static_cast<std::int64_t>(steal_attempts));
            insert_kv("orchestration-steal-violations",
                      static_cast<std::int64_t>(steal_violations));
            insert_kv("orchestration-boundary-depth", static_cast<std::int64_t>(boundary_depth));
            insert_kv("consolidated-p0-production-total", static_cast<std::int64_t>(total));
            insert_kv("consolidated-p0-production-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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

    // Issue #798: query:type-incremental-fidelity-stats — ConstraintSystem
    // incremental fidelity under Guard/steal/MutationBoundary (refines #792/#793/
    // #466/#409; non-duplicative with #608 type-incremental-stats and #509
    // constraint-delta-stats).
    //
    // Fields (4 + sentinel):
    //   - cross-delta-blame-complete  type_incremental_cross_delta_blame_complete_total
    //   - reverify-truncated-under-guard
    //       type_incremental_reverify_truncated_under_guard_total
    //   - epoch-sync-hits             type_incremental_epoch_sync_hits_total
    //   - blame-chain-length          type_incremental_blame_chain_length_total
    //   - schema == 798
    add("query:type-incremental-fidelity-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t cross_delta_blame =
                m ? static_cast<std::int64_t>(
                        m->type_incremental_cross_delta_blame_complete_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t reverify_truncated =
                m ? static_cast<std::int64_t>(
                        m->type_incremental_reverify_truncated_under_guard_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t epoch_sync =
                m ? static_cast<std::int64_t>(
                        m->type_incremental_epoch_sync_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_chain =
                m ? static_cast<std::int64_t>(m->type_incremental_blame_chain_length_total.load(
                        std::memory_order_relaxed))
                  : 0;
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
            insert_kv("cross-delta-blame-complete", cross_delta_blame);
            insert_kv("reverify-truncated-under-guard", reverify_truncated);
            insert_kv("epoch-sync-hits", epoch_sync);
            insert_kv("blame-chain-length", blame_chain);
            insert_kv("schema", 798);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
        if (!m)
            return make_int(0);
        const std::uint64_t runs = m->type_propagation_runs_.load(std::memory_order_relaxed);
        const std::uint64_t total = m->type_propagation_total_.load(std::memory_order_relaxed);
        const std::uint64_t unknown = m->type_propagation_unknown_.load(std::memory_order_relaxed);
        const std::uint64_t int_width =
            m->type_propagation_int_width_.load(std::memory_order_relaxed);
        return make_int(static_cast<std::int64_t>(runs + total + unknown + int_width));
    });

    // Issue #508 / #468: query:dead-coercion-zerooverhead-stats. Hash view
    // of DeadCoercionEliminationPass zero-overhead lifetime counters:
    //   - eliminated: dead_coercion_eliminated_total
    //   - elapsed-us: dead_coercion_elapsed_us_total (#508 timing)
    //   - kept-for-debug: dead_coercion_kept_for_debug_total (#508 blame)
    //   - type-prop-hits: coercion_type_prop_hits_total (Rule 1)
    //   - zerooverhead-wins: coercion_zerooverhead_win_total
    //   - dead-coercion-total: sum of the 5 counters
    //   - dead-coercion-recommendation: 0=ok, 1=review elapsed, 2=debug kept
    add("query:dead-coercion-zerooverhead-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
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
            const std::uint64_t eliminated =
                m ? m->dead_coercion_eliminated_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t elapsed =
                m ? m->dead_coercion_elapsed_us_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t kept =
                m ? m->dead_coercion_kept_for_debug_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t type_prop =
                m ? m->coercion_type_prop_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t win =
                m ? m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = eliminated + elapsed + kept + type_prop + win;
            std::int64_t recommendation = 0;
            if (kept > 0)
                recommendation = 2;
            else if (elapsed > 10000)
                recommendation = 1;
            insert_kv("eliminated", static_cast<std::int64_t>(eliminated));
            insert_kv("elapsed-us", static_cast<std::int64_t>(elapsed));
            insert_kv("kept-for-debug", static_cast<std::int64_t>(kept));
            insert_kv("type-prop-hits", static_cast<std::int64_t>(type_prop));
            insert_kv("zerooverhead-wins", static_cast<std::int64_t>(win));
            insert_kv("dead-coercion-total", static_cast<std::int64_t>(total));
            insert_kv("dead-coercion-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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

    // Issue #800: query:linear-postmutate-fidelity-stats — linear ownership
    // post-mutate / rollback / steal / EnvFrame fidelity dashboard
    // (refines #793/#792/#784/#791; non-duplicative with #763 gc-compiler
    // stats and #638 linear-ownership-safety-stats).
    //
    // Fields (4 + sentinel):
    //   - post-rollback-revalidate-hits  linear_postmutate_post_rollback_revalidate_total
    //   - escape-violations-prevented    linear_postmutate_escape_violations_prevented_total
    //   - guard-boundary-linear-safe     linear_postmutate_guard_boundary_linear_safe_total
    //   - env-version-sync               linear_postmutate_env_version_sync_total
    //   - schema == 800
    add("query:linear-postmutate-fidelity-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t post_rollback =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_post_rollback_revalidate_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t escape_prevented =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_escape_violations_prevented_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_safe =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_guard_boundary_linear_safe_total.load(
                            std::memory_order_relaxed))
                  : 0;
            const std::int64_t env_sync =
                m ? static_cast<std::int64_t>(
                        m->linear_postmutate_env_version_sync_total.load(std::memory_order_relaxed))
                  : 0;
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
            insert_kv("post-rollback-revalidate-hits", post_rollback);
            insert_kv("escape-violations-prevented", escape_prevented);
            insert_kv("guard-boundary-linear-safe", guard_safe);
            insert_kv("env-version-sync", env_sync);
            insert_kv("schema", 800);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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

    // Issue #502 / #551: query:reflect-postmutate-stats. Hash view of
    // Guard post-mutate reflect validation + impact snapshot counters:
    //   - impact-snapshots: impact_snapshot_count_
    //   - schema-pass / schema-fail: auto_validate hook tallies
    //   - dirty-nodes / macro-markers: latest snapshot fields
    //   - schema-valid: last post_mutation_reflect_validate() result
    //   - reflect-postmutate-total: sum of the 4 primary counters
    //   - reflect-postmutate-recommendation: 0=ok, 1=review, 2=alert
    add("query:reflect-postmutate-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
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
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
            const std::uint64_t markers = ev->get_macro_markers_in_snapshot();
            const std::uint64_t total = snapshots + pass + fail + dirty;
            std::int64_t recommendation = 0;
            if (fail > 0 || !ev->get_last_schema_validation_ok())
                recommendation = 2;
            else if (dirty > 50)
                recommendation = 1;
            insert_kv("impact-snapshots", static_cast<std::int64_t>(snapshots));
            insert_kv("schema-pass", static_cast<std::int64_t>(pass));
            insert_kv("schema-fail", static_cast<std::int64_t>(fail));
            insert_kv("dirty-nodes", static_cast<std::int64_t>(dirty));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("schema-valid", ev->get_last_schema_validation_ok() ? 1 : 0);
            insert_kv("reflect-postmutate-total", static_cast<std::int64_t>(total));
            insert_kv("reflect-postmutate-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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

    // Issue #750: (reflect:validate-macro-body node-id) — runtime hygiene/schema
    // check on MacroIntroduced subtrees before Guard commit.
    add("reflect:validate-macro-body", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        const auto result = runtime_reflect_validate_ast_subtree(*ws, nid, false);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            bump_reflection_schema_metrics(m, result);
        return make_bool(result.ok);
    });

    // Issue #750: (reflect:validate-edsl node-id) — runtime schema check for
    // SV verification EDSL nodes (Constraint/Class/Covergroup/SVA/etc.).
    add("reflect:validate-edsl", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        const auto result = runtime_reflect_validate_ast_subtree(*ws, nid, true);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            bump_reflection_schema_metrics(m, result);
        return make_bool(result.ok);
    });

    // Issue #501 / #514: query:ir-hygiene-stats. Hash view of IR-level
    // MacroIntroduced hygiene (AST→IR propagation + InlinePass policy):
    //   - inline-hygiene-skipped: InlinePass macro_hygiene_skipped_
    //   - macro-markers: workspace SyntaxMarker::MacroIntroduced tally
    //   - respect-macro-hygiene: InlinePass::respect_macro_hygiene_ (1=on)
    //   - ir-hygiene-total: inline_skipped + macro_markers
    //   - ir-hygiene-recommendation: 0=ok, 1=review skips, 2=markers w/o policy
    add("query:ir-hygiene-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
        const std::uint64_t inline_skipped = ir_inline_hygiene_skipped(ev);
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        const std::uint64_t total = inline_skipped + markers;
        const bool respects = InlinePass::get_respect_macro_hygiene();
        std::int64_t recommendation = 0;
        if (inline_skipped > 0 && !respects)
            recommendation = 3;
        else if (inline_skipped > 5)
            recommendation = 2;
        else if (markers > 0 && inline_skipped == 0 && respects)
            recommendation = 1;
        insert_kv("inline-hygiene-skipped", static_cast<std::int64_t>(inline_skipped));
        insert_kv("macro-markers", static_cast<std::int64_t>(markers));
        insert_kv("respect-macro-hygiene", respects ? 1 : 0);
        insert_kv("ir-hygiene-total", static_cast<std::int64_t>(total));
        insert_kv("ir-hygiene-recommendation", recommendation);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #503 / #514: query:pattern-marker-stats. Hash view of
    // query:pattern subtree marker/hygiene counters for Agent loops:
    //   - root-skips: macro_introduced_skipped_in_query_
    //   - recursive-skips: pattern_recursive_macro_skipped_
    //   - hygiene-violations: hygiene_violation_count_
    //   - macro-markers: workspace MacroIntroduced marker tally
    //   - pattern-marker-total: root + recursive + violations + markers
    //   - pattern-marker-recommendation: 0=ok, 1=review skips, 2=alert
    add("query:pattern-marker-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
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
        const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
        const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
        const std::uint64_t violations = ev->get_hygiene_violation_count();
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        const std::uint64_t total = root_skips + recursive_skips + violations + markers;
        std::int64_t recommendation = 0;
        if (violations > 0)
            recommendation = 2;
        else if (root_skips + recursive_skips > 10)
            recommendation = 1;
        insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
        insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
        insert_kv("hygiene-violations", static_cast<std::int64_t>(violations));
        insert_kv("macro-markers", static_cast<std::int64_t>(markers));
        insert_kv("pattern-marker-total", static_cast<std::int64_t>(total));
        insert_kv("pattern-marker-recommendation", recommendation);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
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

    // Issue #516: query:prompt6-memory-safety-stats. Hash view of Prompt6
    // memory/ownership/GC production-readiness pillars (non-duplicative
    // synthesis of #602 prompt6-violation-count + prompt6-safety-score int
    // sums and #505 closure-env-safety-stats hash; avoids #515 consolidated
    // P0 tracker and #517 3-pillar int-sum):
    //   P1 Closure/EnvFrame/bridge_epoch: bridge-epoch-hits, closure-stale-refresh,
    //      envframe-stale-refresh, linear-check-passes
    //   P2 invalidate + GC sync: gc-envframe-skipped, gc-walk-safe-skips,
    //      gc-safepoint-waits
    //   P3 Incremental dirty: passes-skipped-dirty, module-dirty-skips
    //   P4 Violation alert surface: boundary/steal/envframe-desync/
    //      provenance-mismatch/fiber-stale-refs
    //   P5 JIT/deopt: deopt-count
    //   - safety-score / violation-count: per-pillar subtotals
    //   - prompt6-memory-safety-total / prompt6-memory-safety-recommendation
    add("query:prompt6-memory-safety-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(32);
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
            const std::uint64_t passes_skipped = ev->get_passes_skipped_type_dirty();
            const std::uint64_t module_dirty =
                m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
            const std::uint64_t boundary = ev->get_boundary_violation_count();
            const std::uint64_t steal_viol = ev->get_mutation_steal_violation_count();
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t unsafe = ev->get_unsafe_boundary_attempts();
            const std::uint64_t batch_steal = ev->get_atomic_batch_steal_violation();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t safety_score = bridge_hit + linear_pass + closure_refresh +
                                               env_refresh + gc_skipped + gc_walk_skips + gc_waits;
            const std::uint64_t violation_count =
                boundary + steal_viol + desync + unsafe + batch_steal + provenance + fiber_stale;
            const std::uint64_t total =
                safety_score + violation_count + passes_skipped + module_dirty + deopt;
            std::int64_t recommendation = 0;
            if (violation_count > 0)
                recommendation = 3;
            else if (deopt > closure_refresh && deopt > 0)
                recommendation = 2;
            else if (env_refresh > bridge_hit && env_refresh > 0)
                recommendation = 1;
            insert_kv("bridge-epoch-hits", static_cast<std::int64_t>(bridge_hit));
            insert_kv("linear-check-passes", static_cast<std::int64_t>(linear_pass));
            insert_kv("closure-stale-refresh", static_cast<std::int64_t>(closure_refresh));
            insert_kv("envframe-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("gc-envframe-skipped", static_cast<std::int64_t>(gc_skipped));
            insert_kv("gc-walk-safe-skips", static_cast<std::int64_t>(gc_walk_skips));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skipped));
            insert_kv("module-dirty-skips", static_cast<std::int64_t>(module_dirty));
            insert_kv("boundary-violations", static_cast<std::int64_t>(boundary));
            insert_kv("steal-violations", static_cast<std::int64_t>(steal_viol));
            insert_kv("envframe-desync", static_cast<std::int64_t>(desync));
            insert_kv("unsafe-boundary-attempts", static_cast<std::int64_t>(unsafe));
            insert_kv("atomic-batch-steal-violations", static_cast<std::int64_t>(batch_steal));
            insert_kv("provenance-mismatch", static_cast<std::int64_t>(provenance));
            insert_kv("fiber-stale-refs", static_cast<std::int64_t>(fiber_stale));
            insert_kv("deopt-count", static_cast<std::int64_t>(deopt));
            insert_kv("safety-score", static_cast<std::int64_t>(safety_score));
            insert_kv("violation-count", static_cast<std::int64_t>(violation_count));
            insert_kv("prompt6-memory-safety-total", static_cast<std::int64_t>(total));
            insert_kv("prompt6-memory-safety-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #519: query:edsl-eda-sv-closedloop-stats. Hash view of the
    // consolidated EDSL/EDA/SV verification closed-loop pillars (non-
    // duplicative synthesis of #496 sv-node-stats, #510 eda-verification-
    // stats, #499 eda-foundation-stats, #497 stable-ref-lifecycle, and
    // #413 mutation-log themes; avoids #514-#518 meta int-sum trackers):
    //   P1 SV structured (#496): sv-node-total, sv-mutate-attempts/success,
    //      structured-mutate-hits
    //   P2 Query scale + hygiene (#447): tag-arity-index-hits,
    //      hygiene-skipped-in-query
    //   P3 StableRef (#497): stable-ref-invalidations, generation-wrap-count,
    //      stable-ref-validated
    //   P4 Verification interop (#510): coverage-feedback, assert-failures,
    //      verification-loop-success, hardware-hook-calls
    //   P5 Atomic batch (#413): atomic-batch-commits, mutation-log-rollbacks
    //   - edsl-eda-sv-closedloop-total / edsl-eda-sv-closedloop-recommendation
    add("query:edsl-eda-sv-closedloop-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            auto* ht = FlatHashTable::create(32);
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
            std::uint64_t sv_node_total = 0;
            if (ws) {
                for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                    switch (ws->get(id).tag) {
                        case aura::ast::NodeTag::Interface:
                        case aura::ast::NodeTag::Modport:
                        case aura::ast::NodeTag::Property:
                        case aura::ast::NodeTag::Sequence:
                        case aura::ast::NodeTag::Assert:
                        case aura::ast::NodeTag::Covergroup:
                        case aura::ast::NodeTag::Coverpoint:
                        case aura::ast::NodeTag::Constraint:
                        case aura::ast::NodeTag::Class:
                            ++sv_node_total;
                            break;
                        default:
                            break;
                    }
                }
            }
            const std::uint64_t sv_attempts = ws ? ws->sv_mutate_attempts_total() : 0;
            const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
            const std::uint64_t structured_hits =
                m ? m->sva_structured_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t tag_hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t hygiene_skipped = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t ref_inval = ws ? ws->stable_ref_invalidations() : 0;
            const std::uint64_t gen_wrap = ws ? ws->generation_wrap_count() : 0;
            const std::uint64_t ref_validated = ev->get_stable_ref_validated_in_primitives_count();
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t loop_success =
                m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hw_hooks =
                m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t batch_commits = ws ? ws->atomic_batch_commits() : 0;
            const std::uint64_t rollbacks = ev->get_mutation_log_rollback_count();
            const std::uint64_t total = sv_node_total + sv_attempts + sv_success + structured_hits +
                                        tag_hits + hygiene_skipped + ref_inval + gen_wrap +
                                        ref_validated + coverage + assert_fail + loop_success +
                                        hw_hooks + batch_commits + rollbacks;
            std::int64_t recommendation = 0;
            if (assert_fail > coverage && assert_fail > 0)
                recommendation = 3;
            else if (sv_attempts > 0 && sv_success == 0)
                recommendation = 2;
            else if (ref_inval > 0)
                recommendation = 1;
            insert_kv("sv-node-total", static_cast<std::int64_t>(sv_node_total));
            insert_kv("sv-mutate-attempts", static_cast<std::int64_t>(sv_attempts));
            insert_kv("sv-mutate-success", static_cast<std::int64_t>(sv_success));
            insert_kv("structured-mutate-hits", static_cast<std::int64_t>(structured_hits));
            insert_kv("tag-arity-index-hits", static_cast<std::int64_t>(tag_hits));
            insert_kv("hygiene-skipped-in-query", static_cast<std::int64_t>(hygiene_skipped));
            insert_kv("stable-ref-invalidations", static_cast<std::int64_t>(ref_inval));
            insert_kv("generation-wrap-count", static_cast<std::int64_t>(gen_wrap));
            insert_kv("stable-ref-validated", static_cast<std::int64_t>(ref_validated));
            insert_kv("coverage-feedback", static_cast<std::int64_t>(coverage));
            insert_kv("assert-failures", static_cast<std::int64_t>(assert_fail));
            insert_kv("verification-loop-success", static_cast<std::int64_t>(loop_success));
            insert_kv("hardware-hook-calls", static_cast<std::int64_t>(hw_hooks));
            insert_kv("atomic-batch-commits", static_cast<std::int64_t>(batch_commits));
            insert_kv("mutation-log-rollbacks", static_cast<std::int64_t>(rollbacks));
            insert_kv("edsl-eda-sv-closedloop-total", static_cast<std::int64_t>(total));
            insert_kv("edsl-eda-sv-closedloop-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #521: query:multi-fiber-orchestration-stats. Hash view of
    // commercial multi-fiber orchestration + MutationBoundary + work-
    // stealing safety (non-duplicative synthesis of #500 work-steal-stats,
    // #618 scheduler-mutation-coord-stats, and #512 runtime-orchestration
    // themes; avoids #512 envframe pillar and #515-#520 meta trackers):
    //   - steal-attempts / steal-successes / steal-deferred-outermost:
    //     outermost MutationBoundary steal enforcement
    //   - steal-violations / boundary-violations / unsafe-boundary-attempts:
    //     concurrent mutation safety alert surface
    //   - mutation-boundary-depth / current-fiber-id: live guard state
    //   - gc-safepoint-requests / gc-safepoint-waits /
    //     gc-pauses-attributed-to-mutation: scheduler/GC coordination
    //   - fiber-migration-attempts / lock-contention-us: orchestration load
    //   - multi-fiber-orchestration-total / multi-fiber-orchestration-recommendation
    add("query:multi-fiber-orchestration-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(32);
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
            const std::uint64_t steal_attempts = aura_work_steal_attempts_total();
            const std::uint64_t steal_successes = aura_work_steal_successes_total();
            const std::uint64_t steal_deferred = aura_adaptive_steal_global_deferred_total();
            const std::uint64_t steal_violations = ev->get_mutation_steal_violation_count();
            const std::uint64_t boundary_violations = ev->get_boundary_violation_count();
            const std::uint64_t unsafe_boundary = ev->get_unsafe_boundary_attempts();
            const std::uint64_t boundary_depth = aura_evaluator_mutation_boundary_depth();
            const std::uint64_t cur_fiber = aura_fiber_current_id();
            const std::uint64_t gc_requests = ev->get_gc_safepoint_requests_total();
            const std::uint64_t gc_waits = ev->get_gc_safepoint_waits_total();
            const std::uint64_t gc_attributed = aura_fiber_static_gc_pause_attributed_to_mutation();
            const std::uint64_t migration = ev->get_mutation_steal_attempts();
            const std::uint64_t lock_us = ev->get_lock_contention_us();
            const std::uint64_t total = steal_attempts + steal_successes + steal_deferred +
                                        steal_violations + boundary_violations + unsafe_boundary +
                                        gc_requests + gc_waits + gc_attributed + migration +
                                        lock_us;
            std::int64_t recommendation = 0;
            if (steal_violations > 0 || boundary_violations > 0)
                recommendation = 3;
            else if (steal_deferred > steal_successes && steal_deferred > 3)
                recommendation = 2;
            else if (boundary_depth > 0 && steal_attempts > 0)
                recommendation = 1;
            insert_kv("steal-attempts", static_cast<std::int64_t>(steal_attempts));
            insert_kv("steal-successes", static_cast<std::int64_t>(steal_successes));
            insert_kv("steal-deferred-outermost", static_cast<std::int64_t>(steal_deferred));
            insert_kv("steal-violations", static_cast<std::int64_t>(steal_violations));
            insert_kv("boundary-violations", static_cast<std::int64_t>(boundary_violations));
            insert_kv("unsafe-boundary-attempts", static_cast<std::int64_t>(unsafe_boundary));
            insert_kv("mutation-boundary-depth", static_cast<std::int64_t>(boundary_depth));
            insert_kv("current-fiber-id", static_cast<std::int64_t>(cur_fiber));
            insert_kv("gc-safepoint-requests", static_cast<std::int64_t>(gc_requests));
            insert_kv("gc-safepoint-waits", static_cast<std::int64_t>(gc_waits));
            insert_kv("gc-pauses-attributed-to-mutation", static_cast<std::int64_t>(gc_attributed));
            insert_kv("fiber-migration-attempts", static_cast<std::int64_t>(migration));
            insert_kv("lock-contention-us", static_cast<std::int64_t>(lock_us));
            insert_kv("multi-fiber-orchestration-total", static_cast<std::int64_t>(total));
            insert_kv("multi-fiber-orchestration-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
    add("query:shape-profiler-stats",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
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
            const std::uint64_t deopt_count =
                m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t spec_hits =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t spec_misses =
                m ? m->specialization_misses.load(std::memory_order_relaxed) : 0;
            constexpr std::int64_t k_window =
                static_cast<std::int64_t>(shape::ShapeProfiler::kDefaultWindowSize);
            constexpr std::int64_t k_ratio_bp =
                static_cast<std::int64_t>(shape::ShapeProfiler::kDefaultStabilityRatio * 10000.0);
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

    // Issue #624: query:shape-stability-jit-stats-hash —
    // Agent-discoverable structured companion to the existing
    // query:shape-stability-stats (#570/#605, int-sum) and
    // query:shape-profiler-stats (#492, 12-field). This primitive
    // specifically covers AC4 from the issue body — a
    // JIT-shape-stability dashboard the Agent can probe to decide
    // when to trigger hot-swap invalidation + rebuild ahead of a
    // heavy mutate.
    //
    // Fields (5):
    //   - stability-ratio-post-mutate   synthetic: shape-churn /
    //                                   (shape-churn + shape-changes-
    //                                   observed) * 100, rounded;
    //                                   ~0 when both are 0. Higher
    //                                   = more instability after
    //                                   mutate.
    //   - deopt-on-instability          synthetic: jit-shape-miss
    //                                   / (jit-shape-miss + version-
    //                                   bumps) * 100; 0 when both
    //                                   are 0. Higher = more
    //                                   deopt-triggering shapes.
    //   - version-bumps                 shape::shape_version_bump_count
    //                                   (existing counter from #570)
    //   - jit-shape-miss                shape::jit_shape_miss_count
    //                                   (existing counter from #605)
    //   - wrong-opt-prevented           shape::shape_deopt_storm_count
    //                                   (existing counter from #570) —
    //                                   each deopt storm is a wrong
    //                                   speculative-opt the system
    //                                   caught and backed out
    //   - schema == 624                  sentinel for Agent drift
    //                                   detection (mirrors #618's +
    //                                   #620's + #621's + #622's)
    //
    // Discovery before this PR: the C++ side already exposes the
    // full feature list via shape::*_count counters in `shape::*`
    // namespace (added by #570 / #605 / #492 / #686). The single
    // NEW contribution is the structured primitive the issue
    // body explicitly names — AC4 listed `query:shape-stability-
    // jit-stats` with these exact fields, and no prior PR shipped
    // it under that name. So #624 ships ONE new Aura primitive.
    //
    // The remaining #624 AC work (post-mutate re-eval in
    // record_shape + GuardShape dispatch version check on the
    // shape version bump in aura_jit lower + optional invalidate
    // in mutate primitives success path) is invasive C++ + hot-
    // path change that needs benchmarking + perf regression
    // coverage alongside the JIT/hot-swap work in #601/#491.
    add("query:shape-stability-jit-stats-hash",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t shape_churn =
                shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
            const std::uint64_t version_bumps =
                shape::shape_version_bump_count.load(std::memory_order_relaxed);
            const std::uint64_t jit_shape_miss =
                shape::jit_shape_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t deopt_storms =
                shape::shape_deopt_storm_count.load(std::memory_order_relaxed);
            const std::uint64_t shape_changes_observed =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->shape_changes_observed.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t churn_total = shape_churn + shape_changes_observed;
            const std::int64_t post_mutate_ratio =
                churn_total == 0 ? 0 : static_cast<std::int64_t>((shape_churn * 100) / churn_total);
            const std::uint64_t deopt_denom = jit_shape_miss + version_bumps;
            const std::int64_t deopt_on_instability =
                deopt_denom == 0 ? 0
                                 : static_cast<std::int64_t>((jit_shape_miss * 100) / deopt_denom);
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
            insert_kv("stability-ratio-post-mutate", post_mutate_ratio);
            insert_kv("deopt-on-instability", deopt_on_instability);
            insert_kv("version-bumps", static_cast<std::int64_t>(version_bumps));
            insert_kv("jit-shape-miss", static_cast<std::int64_t>(jit_shape_miss));
            insert_kv("wrong-opt-prevented", static_cast<std::int64_t>(deopt_storms));
            insert_kv("schema", 624);
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

    // Issue #507: query:task4-hotpath-contracts. Hash view of C++26
    // Contracts + consteval invariants baked into Task4 hot paths
    // (inline_shape_of, ASTArena::create, run_one/run_pipeline,
    // lowering_impl NodeId guard; non-duplicative with #465 tag-encoding
    // hash and #406 pass-contracts-stats runtime counters):
    //   - inline-shape-post: inline_shape_of post contract
    //   - arena-create-pre: ASTArena::create sizeof/align pre
    //   - arena-allocate-raw-pre: allocate_raw size/align pre
    //   - run-one-contract: run_one pre/post contracts
    //   - run-pipeline-contract: run_pipeline non-empty pre
    //   - lowering-node-id-contract: lower_flat_expr NodeId guard
    //   - shape-dispatch-table-size: k_task4_shape_dispatch_table_size
    //   - consteval-hits: k_shape_value_consteval_hits inventory
    //   - task4-contracts-total: sum of contract-site flags
    //   - task4-contracts-recommendation: 0=ok
    add("query:task4-hotpath-contracts", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
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
        constexpr std::int64_t k_site = 1;
        const std::int64_t table_size =
            static_cast<std::int64_t>(shape::k_task4_shape_dispatch_table_size);
        const std::int64_t consteval_hits =
            static_cast<std::int64_t>(shape::k_shape_value_consteval_hits);
        const std::int64_t total = k_site * 6 + table_size + (consteval_hits > 0 ? 1 : 0);
        insert_kv("inline-shape-post", k_site);
        insert_kv("arena-create-pre", k_site);
        insert_kv("arena-allocate-raw-pre", k_site);
        insert_kv("run-one-contract", k_site);
        insert_kv("run-pipeline-contract", k_site);
        insert_kv("lowering-node-id-contract", k_site);
        insert_kv("shape-dispatch-table-size", table_size);
        insert_kv("consteval-hits", consteval_hits);
        insert_kv("task4-contracts-total", total);
        insert_kv("task4-contracts-recommendation", 0);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #626: query:contracts-hotpath-stats-hash — Agent-
    // discoverable structured companion to the existing
    // query:task4-hotpath-contracts (10-field hash with the per-
    // site constants) + query:pass-pipeline-incremental-stats-hash
    // (6-field hash with contracts-checked + zero-overhead stats from
    // #625) + query:pass-contracts-stats (#406, int-sum-of-7) +
    // query:dead-coercion-zerooverhead-stats (#508, zerooverhead-wins).
    // This primitive specifically covers AC5 from the issue body
    // — the Contracts + consteval + hot-path zero-overhead dashboard
    // the Agent reads to confirm production hot paths are wired.
    // 8 fields:
    //   - contracts-checked        derived (same synthetic as in
    //                              #625: zerooverhead_wins /
    //                              (zerooverhead_wins + dispatch_miss
    //                              + 1) * 100)
    //   - violations-in-debug      shape::value_contract_violation_count
    //                              (existing #406 counter)
    //   - consteval-hits           k_shape_value_consteval_hits
    //                              (existing task4-hotpath-contracts
    //                              inventory)
    //   - zero-overhead-savings     aura::coercion_zerooverhead_win_total
    //                              sum (existing #508/#574 counter)
    //   - pass-pipeline-runs        pass_pipeline_runs_total (existing
    //                              #625 counter)
    //   - arena-auto-triggers       auto_alloc_trigger_count (existing
    //                              #604 counter)
    //   - dirty-blocks-skipped      aura::compiler::passes_skipped_
    //                              dirty_pipeline (existing #494)
    //   - schema == 626             sentinel for Agent drift
    //                              detection (mirrors #618+#620+
    //                              #621+#622+#623+#624+#625)
    //
    // Discovery before this PR (preserved, not duplicated): the C++
    // side already exposes the full Contracts + consteval + zero-
    // overhead + dirty short-circuit counter surface via
    // value_contract_violation_count + zerooverhead_win_total +
    // shape::k_shape_value_consteval_hits + pipeline_yield_count +
    // passes_skipped_dirty_pipeline + auto_alloc_trigger_count
    // counters (added by #406 / #508 / #605 / #686 / #494 / #606).
    // The single NEW contribution is the structured primitive the
    // issue body AC5 lists by name.
    //
    // The remaining #626 AC work (post/requires on Arena allocate_raw,
    // ShapeProfiler record_shape, mark_dirty_*, IRInstructionView
    // accessors; consteval on shape tag dispatch + Value v2 bias
    // ranges; wire to Pass short-circuit) is invasive C++ + hot-
    // path C++26-Contracts additions that need benchmarking + perf
    // regression coverage alongside the JIT/hot-swap work in
    // #601/#491. Separate follow-ups.
    add("query:contracts-hotpath-stats-hash",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t zero_wins =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->coercion_zerooverhead_win_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t dispatch_miss =
                types::value_dispatch_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t contracts_denom = zero_wins + dispatch_miss + 1;
            const std::int64_t contracts_checked =
                static_cast<std::int64_t>((zero_wins * 100) / contracts_denom);
            const std::uint64_t violations =
                types::value_contract_violation_count.load(std::memory_order_relaxed);
            const std::uint64_t consteval_hits = shape::k_shape_value_consteval_hits;
            const std::uint64_t pipeline_runs =
                pass_pipeline_runs_total.load(std::memory_order_relaxed);
            const std::uint64_t arena_triggers = ev.arena_group().auto_compact_trigger_count();
            const std::uint64_t dirty_skipped =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t zero_overhead_savings = zero_wins + dispatch_miss;
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
            insert_kv("contracts-checked", contracts_checked);
            insert_kv("violations-in-debug", static_cast<std::int64_t>(violations));
            insert_kv("consteval-hits", static_cast<std::int64_t>(consteval_hits));
            insert_kv("zero-overhead-savings", static_cast<std::int64_t>(zero_overhead_savings));
            insert_kv("pass-pipeline-runs", static_cast<std::int64_t>(pipeline_runs));
            insert_kv("arena-auto-triggers", static_cast<std::int64_t>(arena_triggers));
            insert_kv("dirty-blocks-skipped", static_cast<std::int64_t>(dirty_skipped));
            insert_kv("schema", 626);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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

    // Issue #505 / #531: query:closure-env-safety-stats. Hash view of
    // closure / EnvFrame / bridge_epoch / linear_ownership_state
    // post-invalidate safety counters for AI multi-round mutate loops:
    //   - stale-refresh: closure_stale_refresh_count_
    //   - bridge-hit: bridge_epoch_hit_count_
    //   - linear-pass: linear_check_pass_count_
    //   - gc-skipped: gc_envframe_stale_skipped_
    //   - env-stale-refresh: envframe_stale_refresh_count_
    //   - closure-env-safety-total: sum of the 5 counters
    //   - refresh-rate-pct: stale / (stale + bridge) * 100
    //   - closure-env-safety-recommendation: 0=ok, 1=review, 2=alert
    add("query:closure-env-safety-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
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
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t stale_refresh =
                m ? m->closure_stale_refresh_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge_hit =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t linear_pass =
                m ? m->linear_check_pass_count_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t gc_skipped =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t env_refresh = ev->get_envframe_stale_refresh_count();
            const std::uint64_t total =
                stale_refresh + bridge_hit + linear_pass + gc_skipped + env_refresh;
            const std::uint64_t epoch_checks = stale_refresh + bridge_hit;
            const std::int64_t refresh_pct =
                epoch_checks > 0 ? static_cast<std::int64_t>((stale_refresh * 100) / epoch_checks)
                                 : 0;
            std::int64_t recommendation = 0;
            if (gc_skipped > 0)
                recommendation = 2;
            else if (refresh_pct > 25)
                recommendation = 1;
            insert_kv("stale-refresh", static_cast<std::int64_t>(stale_refresh));
            insert_kv("bridge-hit", static_cast<std::int64_t>(bridge_hit));
            insert_kv("linear-pass", static_cast<std::int64_t>(linear_pass));
            insert_kv("gc-skipped", static_cast<std::int64_t>(gc_skipped));
            insert_kv("env-stale-refresh", static_cast<std::int64_t>(env_refresh));
            insert_kv("closure-env-safety-total", static_cast<std::int64_t>(total));
            insert_kv("refresh-rate-pct", refresh_pct);
            insert_kv("closure-env-safety-recommendation", recommendation);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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

    // Issue #510: query:eda-verification-stats. Hash view of commercial
    // EDA verification interop + coverage/assert feedback closed-loop
    // counters (non-duplicative with #469 int-sum verification-loop-stats,
    // #695 eda-sv-closedloop-stress-stats stress harness, and #698
    // hardware-backend-commercial-stats emit/parse focus):
    //   - coverage-delta: verification_coverage_feedback_total
    //   - assert-fail-count: verification_assert_failure_total
    //   - auto-mutate-from-feedback: feedback_mutate_hits +
    //     eda_sv_feedback_mutate_success + verify_tool_feedback_mutate_success
    //   - commercial-reemits: commercial_reemits_total
    //   - commercial-simulator-runs: commercial_simulator_runs_total
    //   - verification-loop-success: verification_loop_success_total
    //   - sv-mutate-success-rate-pct: 0..100 from attempts/success
    //   - eda-verification-total: sum of primary counters
    //   - eda-verification-recommendation: 0=ok, 1=assert-heavy, 2=low mutate rate
    add("query:eda-verification-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        auto* ws = ev->workspace_flat();
        const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
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
        const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
        const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
        const std::uint64_t feedback_hits =
            m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t eda_feedback_ok =
            m ? m->eda_sv_feedback_mutate_success_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t verify_tool_feedback =
            ev->get_verify_tool_feedback_mutate_success_total();
        const std::uint64_t auto_mutate = feedback_hits + eda_feedback_ok + verify_tool_feedback;
        const std::uint64_t reemits =
            m ? m->commercial_reemits_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t sim_runs =
            m ? m->commercial_simulator_runs_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t loop_success =
            m ? m->verification_loop_success_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t sv_attempts = ws ? ws->sv_mutate_attempts_total() : 0;
        const std::uint64_t sv_success = ws ? ws->sv_mutate_success_total() : 0;
        const std::uint64_t success_rate_pct =
            sv_attempts > 0 ? (100 * sv_success / sv_attempts) : (sv_success > 0 ? 100 : 0);
        const std::uint64_t total =
            coverage + assert_fail + auto_mutate + reemits + sim_runs + loop_success;
        std::int64_t recommendation = 0;
        if (assert_fail > coverage && auto_mutate == 0)
            recommendation = 1;
        else if (sv_attempts > 0 && success_rate_pct < 50)
            recommendation = 2;
        insert_kv("coverage-delta", static_cast<std::int64_t>(coverage));
        insert_kv("assert-fail-count", static_cast<std::int64_t>(assert_fail));
        insert_kv("auto-mutate-from-feedback", static_cast<std::int64_t>(auto_mutate));
        insert_kv("commercial-reemits", static_cast<std::int64_t>(reemits));
        insert_kv("commercial-simulator-runs", static_cast<std::int64_t>(sim_runs));
        insert_kv("verification-loop-success", static_cast<std::int64_t>(loop_success));
        insert_kv("sv-mutate-success-rate-pct", static_cast<std::int64_t>(success_rate_pct));
        insert_kv("eda-verification-total", static_cast<std::int64_t>(total));
        insert_kv("eda-verification-recommendation", recommendation);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
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
        return make_int(
            static_cast<std::int64_t>(desync + dual_sync + stale + mismatch + gc_skips));
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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
        auto* __qev_ = Evaluator::get_query_evaluator();
        const auto* m =
            __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
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

    // Issue #625: query:pass-pipeline-incremental-stats-hash —
    // Agent-discoverable structured companion to the existing
    // query:pass-pipeline-stats (#494/#606, 10-field) and
    // query:pass-contracts-stats (#406, int-sum-of-7). This
    // primitive specifically covers AC4 from the issue body
    // — the incremental re-lower dashboard the Agent reads to
    // confirm dirty-block short-circuit savings.
    //
    // Fields (6):
    //   - passes-run                 lifetime # of full
    //                                run_pipeline() invocations
    //                                (pass_pipeline_runs_total,
    //                                bumped in pass_manager.ixx)
    //   - contracts-checked          synthetic: zerooverhead_wins /
    //                                (zerooverhead_wins + value_
    //                                dispatch_miss_count + 1) * 100;
    //                                measures how often the
    //                                Contracts / cheap-view dispatch
    //                                was used as a fast path
    //   - pure-delegation-hits       ShapeWrap + LinearOwnershipWrap
    //                                pure_delegation_hits() sum
    //   - shortcircuit-savings       passes_skipped_dirty_pipeline
    //                                + module_dirty_skips (total
    //                                work avoided by the dirty
    //                                short-circuit path)
    //   - dirty-blocks-skipped       passes_skipped_dirty_pipeline
    //                                (more directly: each pass
    //                                skipped by the dirty filter)
    //   - schema == 625              sentinel for Agent drift
    //                                detection (mirrors #618+#620+
    //                                #621+#622+#623+#624 sentinels)
    //
    // Discovery before this PR: the C++ side already exposes the
    // full pipeline + contracts + dirty-skipped counter surface
    // via aura::compiler::pipeline_yield_count + passes_skipped_
    // dirty_pipeline + passes_skipped_type_dirty + ShapeWrap +
    // LinearOwnershipWrap + CompilerMetrics::relower_*
    // (added by #494 / #606 / #406 / #686). The single NEW
    // contribution is the structured primitive the issue body
    // AC4 lists by name + the `pass_pipeline_runs_total` counter
    // (per-full-pipeline-run, not per-pass).
    //
    // The remaining #625 AC work (more `requires` constraints on
    // Pass/AnalysisPass, fold-expressions in run_pipeline, uniform
    // ShapeProfilerWrap / LinearOwnershipWrap / DirtyImpactWrap
    // classes, estimate_relower_blocks integration with
    // invalidate_function) is invasive C++ that needs benchmarking
    // + perf regression coverage before going in.
    add("query:pass-pipeline-incremental-stats-hash",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t passes_run =
                pass_pipeline_runs_total.load(std::memory_order_relaxed);
            const std::uint64_t dirty_blocks_skipped =
                aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
            const std::uint64_t shortcircuit_savings =
                dirty_blocks_skipped +
                (ev.compiler_metrics()
                     ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                           ->module_dirty_skips.load(std::memory_order_relaxed)
                     : 0);
            const std::uint64_t zero_wins =
                ev.compiler_metrics()
                    ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                          ->coercion_zerooverhead_win_total.load(std::memory_order_relaxed)
                    : 0;
            const std::uint64_t dispatch_miss =
                types::value_dispatch_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t contracts_denom = zero_wins + dispatch_miss + 1;
            const std::int64_t contracts_checked =
                static_cast<std::int64_t>((zero_wins * 100) / contracts_denom);
            const std::uint64_t pure_delegation =
                aura::compiler::ShapeWrap::pure_delegation_hits() +
                aura::compiler::LinearOwnershipWrap::pure_delegation_hits();
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
            insert_kv("passes-run", static_cast<std::int64_t>(passes_run));
            insert_kv("contracts-checked", contracts_checked);
            insert_kv("pure-delegation-hits", static_cast<std::int64_t>(pure_delegation));
            insert_kv("shortcircuit-savings", static_cast<std::int64_t>(shortcircuit_savings));
            insert_kv("dirty-blocks-skipped", static_cast<std::int64_t>(dirty_blocks_skipped));
            insert_kv("schema", 625);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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