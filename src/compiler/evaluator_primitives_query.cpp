// evaluator_primitives_query.cpp — P0 step 8: standalone query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "compiler/evaluator_primitives_query_shared.hh"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/gc_coord_scope.h" // Issue #2131
#include "compiler/shape.h"
#include "compiler/shape_profiler.h"
#include "compiler/value_tags.h"
#include "core/gc_hooks.h"                    // #1593 safepoint wait linkage
#include "core/layout_stamp.hh"               // Issue #2432: kLayoutStampSchema
#include "core/lifetime_consistency_proof.hh" // Issue #2888: unified proof
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/multi_fiber_mailbox.h"             // #1597 orch readiness
#include "serve/parallel_orch.h"                   // #1597 orch readiness
#include "hash_meta.h"                             // FNV constants (#901)
#include "typed_mutation_audit.h"                  // Issue #1613 macro hygiene audit trail
#include "compiler/dce_elided_deopt_meta.h"        // Issue #2611: elided CastOp deopt meta
#include "compiler/castop_typed_meta.h"            // Issue #2624 Phase A: CastOp typed meta
#include "linear_occurrence_mutate_stats.h"        // Issue #2030 occurrence hit-rate ratios
#include "basis_points.h"                          // Issue #2030 ratio bp helpers
#include "core/provenance_tracker.hh"              // Issue #2030 linear-provenance consistency bp
#include "mutate_type_gate.hh"                     // Issue #2219 Soft/Hard post-mutate type gate
#include "compiler/type_system_health.hh"          // Issue #2350: query:type-system-health score
#include "compiler/type_linear_commit_health.hh"   // Issue #2613: query:type-linear-commit-health
#include "compiler/mutation_concurrency_health.hh" // Issue #2379: mutation-concurrency-health
#include "compiler/aot_hot_update_health.hh"       // Issue #2506: query:aot-hot-update-health
#include "compiler/hot_update_registry.hh"         // Issue #2506: reload recovery C snapshot
#include "compiler/compact_policy.hh"              // Issue #2500: query:compact-policy
#include "compiler/mutation_hold_budget.h"         // Issue #2500: hold estimate for split
#include "compiler/ownership_rebind.h"             // Issue #2695: query:ownership-rebind-stats
#include "compiler/lock_order_audit.h"             // Issue #2557: lock-order soft audit query
#include "core/densify_consistency_report.h"       // Issue #2379: densify fail / last-call axes

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.coercion_map; // Issue #2024: provenance completeness counters
import aura.compiler.ir;
import aura.compiler.macro_expansion; // Issue #2020: hygiene atomics for Agent diagnostics
import aura.compiler.pass_manager;
import aura.compiler.optimization_passes; // Issue #2282: dead_coercion_ir_elided_total + dirty_cone atomics
import aura.compiler.dirty_propagation; // Issue #2191: type cone mirror metrics
import aura.compiler.service;
import aura.compiler.type_checker; // Issue #2262: partial_cs_import_* module atomics
import aura.compiler.value;
import aura.compiler.ir_cache_pure; // Issue #2257: current_shape_stability_ratio
import aura.core.lifetime_pin;      // Issue #2350: linear pin miss rate for type-system-health
import aura.core.envframe_lifetime; // Issue #2500: active_guard_depth for compact policy
import aura.core.arena;             // Issue #2500: g_force_compact_blocked_* + arena frag

// Issue #1610: IR stamp + JIT hygiene counters (C linkage; avoid module cycles).
extern "C" std::uint64_t aura_hygiene_ir_macro_marker_total();
extern "C" std::uint64_t aura_hygiene_ir_provenance_stamped_total();
extern "C" std::uint64_t aura_hygiene_ir_ancestor_propagation_total();   // #2764
extern "C" std::uint64_t aura_multi_eval_macro_marker_preserved_total(); // #2764
extern "C" std::uint64_t aura_jit_macro_introduced_deopt();
extern "C" std::uint64_t aura_jit_macro_hygiene_consults();
// Issue #2022: native MacroIntroduced side-table observability after JIT/AOT.
extern "C" std::uint64_t aura_jit_native_marker_preserved_total();
extern "C" std::uint64_t aura_jit_live_macro_fn_count();
extern "C" std::uint64_t aura_jit_macro_provenance_recoverable_total();
extern "C" std::uint8_t aura_jit_fn_source_marker(std::int64_t func_id);
extern "C" std::uint32_t aura_jit_fn_provenance(std::int64_t func_id);
// Issue #2100: deopt round-trip preserved/lost (IR attrs → AST restamp).
extern "C" std::uint64_t aura_jit_macro_introduced_preserved_total();
extern "C" std::uint64_t aura_jit_macro_introduced_lost_total();
// Issue #2177: AOT marker propagation observability (refine #2100
// which was JIT-only). Defined in aura_jit_bridge.cpp.
extern "C" std::uint64_t aura_2177_aot_macro_marker_propagated_total(void);
extern "C" std::uint64_t aura_2177_aot_macro_marker_stripped_total(void);
// Issue #2018 / #2169: rest-param hygiene gensym counters (clone_macro_body).
extern "C" std::uint64_t aura_macro_rest_param_hygiene_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_rest_param_hygiene_incomplete_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_rest_gensym_serial_v_read() noexcept;
// Issue #2019: MacroIntroduced restamp-after-flat counter.
extern "C" std::uint64_t aura_macro_restamp_after_flat_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_expand_mutate_restamp_total_v_read() noexcept;
// Issue #2176: selective unstamp for MacroIntroduced subtrees (Agent
// experimental rollback path). Bumped per successful unstamp.
extern "C" std::uint64_t aura_unstamp_macro_introduced_total_v_read() noexcept;
// Issue #2237: rollback + strict-mode counters (macro_expansion.cpp).
extern "C" std::uint64_t aura_rollback_macro_introduced_total_v_read() noexcept;
extern "C" std::uint64_t aura_rollback_strict_audited_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_expand_sandbox_strict_v_read() noexcept;
extern "C" std::uint64_t aura_macro_schema_cache_dirty_stamped_total_v_read() noexcept;
// Issue #2239: rest-param nested qq + schema_cache rest stamping (macro_expansion.cpp).
extern "C" std::uint64_t aura_macro_rest_param_nested_qq_hits_total_v_read() noexcept;
extern "C" std::uint64_t aura_macro_schema_cache_rest_stamped_total_v_read() noexcept;
// Issue #2178 / #2240: cross-workspace hot-update reject (aura_jit_bridge.cpp).
// File-scope: block-scope extern "C" is not reliable under -fmodules-ts.
extern "C" std::uint64_t aura_cross_workspace_hot_update_rejected_total_v_read(void) noexcept;
extern "C" std::uint8_t aura_last_cross_workspace_reject_reason_v_read(void) noexcept;
extern "C" const char* aura_cross_workspace_reject_reason_string(std::uint8_t v) noexcept;
// Issue #2894: last remount fail reason (aura_jit_bridge.cpp).
extern "C" std::uint8_t aura_last_remount_fail_reason(void) noexcept;
// Issue #2021: depth + concurrent peak readers / metrics snapshot.
extern "C" std::uint64_t aura_macro_clone_concurrent_peak_v_read() noexcept;
extern "C" std::uint64_t aura_macro_clone_in_flight_v_read() noexcept;
extern "C" std::uint64_t aura_hygiene_tracer_depth_max_v_read() noexcept;
extern "C" std::uint64_t aura_macro_clone_concurrent_fiber_total_v_read() noexcept;
extern "C" void aura_macro_hygiene_snapshot_metrics(void* metrics_ptr) noexcept;

namespace aura::compiler::primitives_detail {

// Issue #2696: query:occurrence-goals-live — file-scope lifetime atomics
// (mirrors the #2693 / #2694 / #2695 pattern — light binaries get the
// file-level fallback path when no per-CompilerMetrics wired).
std::atomic<std::uint64_t> g_occurrence_goals_live_total{0};
std::atomic<std::uint64_t> g_occurrence_goals_live_truncated_total{0};
std::atomic<std::uint32_t> g_occurrence_goals_live_wired{1};

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using ModulePathResolver = std::function<std::string(const std::string&)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_keyword_idx;
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
using types::is_keyword;
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

// Issue #288 forward declaration for the best-effort schema
// shape check helper (defined at the bottom of this file).
bool validate_code_against_schema_simple(const std::string& code, const std::string& type_name,
                                         std::string& violation_reason,
                                         std::string& violation_field);

// Issue #514 / #501 / #1678: count MacroIntroduced nodes in the workspace
// marker column. Prefer max(live walk, snapshot) so a partial/stale walk
// never undercounts a prior COW snapshot. The old `count > 0 ? count : snapshot`
// returned the walk alone whenever both were non-zero (walk=1, snapshot=5 → 1).
// Snapshot + walk are read under the same WorkspaceSharedLock so a concurrent
// workspace_flat_ swap cannot pair an old snapshot with a new walk (#917).
std::uint64_t workspace_marker_macro_introduced(Evaluator* ev) {
    if (!ev)
        return 0;
    Evaluator::WorkspaceSharedLock lock(*ev);
    const std::uint64_t snapshot = ev->get_macro_markers_in_snapshot();
    std::uint64_t walk = 0;
    if (auto* ws = ev->workspace_flat()) {
        const auto& markers = ws->marker_column();
        for (auto m : markers) {
            if (m == aura::ast::SyntaxMarker::MacroIntroduced)
                ++walk;
        }
    }
    return walk > snapshot ? walk : snapshot;
}

std::uint64_t ir_inline_hygiene_skipped(Evaluator* ev) {
    if (!ev || !ev->get_macro_hygiene_skipped_fn_)
        return 0;
    return ev->get_macro_hygiene_skipped_fn_();
}

// ── Issue #1680: query:module-exports mtime cache + extractable parser ──
namespace module_export_cache {

    struct Entry {
        std::filesystem::file_time_type mtime{};
        std::vector<std::string> exports;
        bool valid = false; // true if file was readable and parsed
    };

    std::mutex g_mtx;
    std::unordered_map<std::string, Entry, aura::core::TransparentStringHash, std::equal_to<>>
        g_by_path;
    std::atomic<std::uint64_t> g_hit{0};
    std::atomic<std::uint64_t> g_miss{0};
    std::atomic<std::uint64_t> g_stat_fail{0};
    std::atomic<std::uint64_t> g_open_fail{0};

    // Extract (export SYM...) names from Aura source. Skips ; line comments,
    // #| |# block comments, and "…" strings (incl. simple backslash escapes).
    // Mirrors historical behavior: first top-level (export …) form only.
    [[nodiscard]] std::vector<std::string> parse_module_exports(std::string_view content) {
        std::vector<std::string> exports;
        const std::size_t n = content.size();
        std::size_t i = 0;
        auto skip_ws = [&] {
            while (i < n && (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' ||
                             content[i] == '\r'))
                ++i;
        };
        auto skip_line_comment = [&] {
            while (i < n && content[i] != '\n')
                ++i;
        };
        auto skip_block_comment = [&] {
            i += 2; // past #|
            while (i + 1 < n) {
                if (content[i] == '|' && content[i + 1] == '#') {
                    i += 2;
                    return;
                }
                ++i;
            }
            i = n;
        };
        auto skip_string = [&] {
            ++i; // opening "
            while (i < n) {
                if (content[i] == '\\' && i + 1 < n) {
                    i += 2;
                    continue;
                }
                if (content[i] == '"') {
                    ++i;
                    return;
                }
                ++i;
            }
        };
        auto is_sym_char = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '?' || c == '!' || c == '<' || c == '>' || c == '=' ||
                   c == '*' || c == '+' || c == '-' || c == '/' || c == '.' || c == '$';
        };

        while (i < n) {
            if (content[i] == ';') {
                skip_line_comment();
                continue;
            }
            if (content[i] == '#' && i + 1 < n && content[i + 1] == '|') {
                skip_block_comment();
                continue;
            }
            if (content[i] == '"') {
                skip_string();
                continue;
            }
            if (content[i] == '(' && i + 7 <= n && content.substr(i, 7) == "(export" &&
                (i + 7 == n || content[i + 7] == ' ' || content[i + 7] == '\t' ||
                 content[i + 7] == '\n' || content[i + 7] == '\r' || content[i + 7] == ')')) {
                // Boundary: start of file or whitespace/'(' before (export
                if (i > 0) {
                    const char prev = content[i - 1];
                    if (prev != '\n' && prev != ' ' && prev != '\t' && prev != '\r' &&
                        prev != '(') {
                        ++i;
                        continue;
                    }
                }
                i += 7;
                while (i < n && content[i] != ')') {
                    skip_ws();
                    if (i >= n || content[i] == ')')
                        break;
                    if (content[i] == ';') {
                        skip_line_comment();
                        continue;
                    }
                    if (content[i] == '#' && i + 1 < n && content[i + 1] == '|') {
                        skip_block_comment();
                        continue;
                    }
                    if (content[i] == '"') {
                        skip_string();
                        continue;
                    }
                    if (!is_sym_char(content[i])) {
                        ++i;
                        continue;
                    }
                    const std::size_t s = i;
                    while (i < n && is_sym_char(content[i]))
                        ++i;
                    if (i > s)
                        exports.emplace_back(content.substr(s, i - s));
                }
                break; // first (export …) only (historical contract)
            }
            ++i;
        }
        return exports;
    }

    // Returns (valid, exports). Copies exports under the lock to avoid dangling
    // map pointers across concurrent miss rehash.
    [[nodiscard]] std::pair<bool, std::vector<std::string>>
    lookup_or_load(const std::string& resolved) {
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(resolved, ec);
        if (ec) {
            g_stat_fail.fetch_add(1, std::memory_order_relaxed);
            g_miss.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(g_mtx);
            g_by_path[resolved] = Entry{};
            return {false, {}};
        }
        {
            std::lock_guard<std::mutex> lock(g_mtx);
            auto it = g_by_path.find(resolved);
            if (it != g_by_path.end() && it->second.valid && it->second.mtime == mtime) {
                g_hit.fetch_add(1, std::memory_order_relaxed);
                return {true, it->second.exports};
            }
        }
        // Miss: read + parse outside lock (I/O), then store.
        g_miss.fetch_add(1, std::memory_order_relaxed);
        std::ifstream f(resolved);
        Entry fresh;
        fresh.mtime = mtime;
        if (!f.is_open()) {
            g_open_fail.fetch_add(1, std::memory_order_relaxed);
            fresh.valid = false;
        } else {
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            fresh.exports = parse_module_exports(content);
            fresh.valid = true;
        }
        std::lock_guard<std::mutex> lock(g_mtx);
        // Another thread may have filled the cache; prefer freshest valid entry.
        auto& slot = g_by_path[resolved];
        if (!(slot.valid && slot.mtime == mtime))
            slot = std::move(fresh);
        if (!slot.valid)
            return {false, {}};
        return {true, slot.exports};
    }

    void reset_for_test() {
        std::lock_guard<std::mutex> lock(g_mtx);
        g_by_path.clear();
        g_hit.store(0, std::memory_order_relaxed);
        g_miss.store(0, std::memory_order_relaxed);
        g_stat_fail.store(0, std::memory_order_relaxed);
        g_open_fail.store(0, std::memory_order_relaxed);
    }

} // namespace module_export_cache

// Issue #750: ReflectRuntimeValidateResult in query_shared.hh (#2914)

bool is_edsl_verification_tag(aura::ast::NodeTag tag) noexcept {
    using aura::ast::NodeTag;
    return tag == NodeTag::Constraint || tag == NodeTag::Class || tag == NodeTag::Covergroup ||
           tag == NodeTag::Coverpoint || tag == NodeTag::Property || tag == NodeTag::Interface ||
           tag == NodeTag::Modport || tag == NodeTag::Sequence || tag == NodeTag::Assert;
}

ReflectRuntimeValidateResult runtime_reflect_validate_ast_subtree(aura::ast::FlatAST& flat,
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
    // Issue #1679: FlatAST is a DAG that recovery/EDSL self-mutate can cycle.
    // Dense seen[] prevents unbounded stack growth (infinite loop on cycles)
    // and avoids double-counting MacroIntroduced on diamond DAGs.
    std::vector<aura::ast::NodeId> stack;
    std::vector<std::uint8_t> seen(flat.size(), 0);
    stack.push_back(root);
    seen[static_cast<std::size_t>(root)] = 1;
    std::size_t visited = 1;
    // Hard ceiling: never process more nodes than the flat arena holds.
    const std::size_t kMaxVisit = flat.size();
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
            if (c == aura::ast::NULL_NODE)
                continue;
            if (c >= flat.size() || !flat.is_live_node(c)) {
                marker_ok = false;
                continue;
            }
            const auto ci = static_cast<std::size_t>(c);
            if (seen[ci])
                continue; // cycle edge or shared DAG child — do not re-push
            seen[ci] = 1;
            ++visited;
            if (visited > kMaxVisit) {
                // Defensive: impossible with seen[], but abort if arena lies.
                out.stale_prevented = true;
                out.hygiene_held = false;
                out.ok = false;
                return out;
            }
            stack.push_back(c);
        }
    }
    out.hygiene_held = marker_ok;
    out.ok = marker_ok;
    return out;
}

void bump_reflection_schema_metrics(CompilerMetrics* m,
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


// Issue #2914: peel forward decls
void register_query_obs_mid_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                       std::pmr::vector<std::string>& string_heap,
                                       void*& type_registry, ModulePathResolver resolve_module_path,
                                       Evaluator& ev);
void register_query_type_stats_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                          std::pmr::vector<std::string>& string_heap,
                                          void*& type_registry,
                                          ModulePathResolver resolve_module_path, Evaluator& ev);
void register_query_reflect_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                       std::pmr::vector<std::string>& string_heap,
                                       void*& type_registry, ModulePathResolver resolve_module_path,
                                       Evaluator& ev);
void register_query_lifecycle_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                         std::pmr::vector<std::string>& string_heap,
                                         void*& type_registry,
                                         ModulePathResolver resolve_module_path, Evaluator& ev);
void register_query_tail_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                                    std::pmr::vector<std::string>& string_heap,
                                    void*& type_registry, ModulePathResolver resolve_module_path,
                                    Evaluator& ev);

void register_query_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                               std::pmr::vector<std::string>& string_heap, void*& type_registry,
                               ModulePathResolver resolve_module_path, Evaluator& ev) {

    // Issue #1680: mtime-keyed cache; amortize re-read/re-parse of module files.
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
            auto [ok, exports] = module_export_cache::lookup_or_load(resolved);
            if (!ok)
                return make_void();
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

    // Facade-only (#1680): (stats:get "query:module-export-cache-stats")
    ObservabilityPrims::register_stats_impl(
        "query:module-export-cache-stats", [&string_heap](std::span<const EvalValue>) -> EvalValue {
            auto load = [](const std::atomic<std::uint64_t>& a) {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            std::size_t size = 0;
            {
                std::lock_guard<std::mutex> lock(module_export_cache::g_mtx);
                size = module_export_cache::g_by_path.size();
            }
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("schema", 1680);
            insert_kv("issue", 1680);
            insert_kv("hits", load(module_export_cache::g_hit));
            insert_kv("misses", load(module_export_cache::g_miss));
            insert_kv("stat-fail", load(module_export_cache::g_stat_fail));
            insert_kv("open-fail", load(module_export_cache::g_open_fail));
            insert_kv("entries", static_cast<std::int64_t>(size));
            const auto h = load(module_export_cache::g_hit);
            const auto m = load(module_export_cache::g_miss);
            const auto denom = h + m;
            insert_kv("hit-rate-bp", denom > 0 ? (h * 10000) / denom : 0);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    ObservabilityPrims::register_stats_impl(
        "query:jit-fallback-stats", [](std::span<const EvalValue> a) -> EvalValue {
            // Issue #461: read the global fallback counter. The
            // counter is bumped by `aura_jit_fallback_to_interpreter`
            // each time the JIT default case routes through the
            // fallback path. P0 ship: returns the counter as an
            // integer. Future ship: returns a list
            // (fallback-count deopt-count consistency-violations).
            (void)a;
            return make_int(static_cast<std::int64_t>(aura_jit_fallback_count_v_read()));
        });

    // Issue #1646: query:mutation-boundary-observability-stats
    // Hash of the 4 new MutationBoundaryGuard long-running observability
    // counters added in #1646 (paired legacy + per-CompilerMetrics
    // bumps via Evaluator::yield_hook_evaluator() null fallback):
    // success_total + macro_dirty_propagated_total +
    // epoch_bump_for_macro_total + hygiene_violation_total.
    // Distinct from the existing (query:mutation-boundary-stats) surface
    // (which exposes the legacy recovery_failure + rollback + yield_resume
    // counters from #1637 / #1908 / #1641 lineage) by being the
    // MutationBoundaryGuard long-running observability layer explicitly
    // requested in #1646 body (Guard dtor success path + macro-dirty
    // propagation + epoch-bump + hygiene-violation sites). Pairs with
    // the #1908 module-boundary pattern so calls from TUs without the
    // Evaluator module safely no-op.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-observability-stats",
        [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            std::int64_t success = 0, macro_dirty = 0, epoch_bump = 0, hygiene_violation = 0;
            if (ev) {
                if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                    m->mutation_boundary_observability_queries_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                success = static_cast<std::int64_t>(ev->mutation_boundary_success_total());
                macro_dirty =
                    static_cast<std::int64_t>(ev->mutation_boundary_macro_dirty_propagated_total());
                epoch_bump =
                    static_cast<std::int64_t>(ev->mutation_boundary_epoch_bump_for_macro_total());
                hygiene_violation =
                    static_cast<std::int64_t>(ev->mutation_boundary_hygiene_violation_total());
            }
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_int(success + macro_dirty + epoch_bump + hygiene_violation);
            (void)ht; // FlatHashTable fill follows the same pattern as query:ir-marker-stats.
            return make_int(success + macro_dirty + epoch_bump + hygiene_violation);
        });

    // Issue #455 / #1039 / #1644 / #1891: query:ir-marker-stats
    // Hash of SyntaxMarker counts read from the IR (per-instruction
    // source_marker across IRModule.functions[*].blocks[*].instructions[*])
    // — the authoritative IR-layer marker surface — augmented with the
    // two Issue #1644 IR-hygiene observability counters. #1891 prefers
    // CompilerService::last_ir_module() walk; falls back to AST marker
    // column when no IR has been compiled yet. Pairs marker stats with
    // cross-marker inliner-skip + lowering-propagation observability for
    // closed-loop MacroIntroduced hygiene in self-evolution.
    ObservabilityPrims::register_stats_impl(
        "query:ir-marker-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (ev) {
                if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
                    m->ir_marker_stats_queries_total.fetch_add(1, std::memory_order_relaxed);
            }
            std::int64_t ir_user = 0, ir_macro_intro = 0, ir_bool_lit = 0;
            std::int64_t ir_walked = 0;
            // AC3 / #1891: prefer last_ir_module instruction walk.
            if (ev && ev->compiler_service()) {
                auto* svc = static_cast<aura::compiler::CompilerService*>(ev->compiler_service());
                if (const auto& mod_opt = svc->last_ir_module(); mod_opt.has_value()) {
                    ir_walked = 1;
                    for (const auto& fn : mod_opt->functions) {
                        for (const auto& blk : fn.blocks) {
                            for (const auto& instr : blk.instructions) {
                                if (instr.source_marker == 1)
                                    ++ir_macro_intro;
                                else if (instr.source_marker == 2)
                                    ++ir_bool_lit;
                                else
                                    ++ir_user;
                            }
                        }
                    }
                }
            }
            // Fallback: AST marker column when IR not yet available.
            if (!ir_walked && ev && ev->workspace_flat()) {
                const auto& flat = *ev->workspace_flat();
                const auto n = flat.size();
                for (std::uint32_t i = 0; i < n; ++i) {
                    const auto mk = flat.marker(static_cast<aura::ast::NodeId>(i));
                    if (mk == aura::ast::SyntaxMarker::MacroIntroduced)
                        ++ir_macro_intro;
                    else if (mk == aura::ast::SyntaxMarker::BoolLiteral)
                        ++ir_bool_lit;
                    else
                        ++ir_user;
                }
            }
            // AC4 / #1891: CompilerMetrics when live; else shared C totals
            // (lowering) + InlinePass hook (skip) — module-boundary safe.
            std::int64_t lowering_marker_propagated = 0;
            std::int64_t ir_macro_introduced_inlined_skipped = 0;
            if (ev) {
                if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                    lowering_marker_propagated =
                        m->lowering_marker_propagated_total.load(std::memory_order_relaxed);
                    ir_macro_introduced_inlined_skipped =
                        m->ir_macro_introduced_inlined_skipped_total.load(
                            std::memory_order_relaxed);
                }
            }
            const auto c_stamped = static_cast<std::int64_t>(aura_hygiene_ir_macro_marker_total());
            if (c_stamped > lowering_marker_propagated)
                lowering_marker_propagated = c_stamped;
            const auto hook_skipped = static_cast<std::int64_t>(ir_inline_hygiene_skipped(ev));
            if (hook_skipped > ir_macro_introduced_inlined_skipped)
                ir_macro_introduced_inlined_skipped = hook_skipped;
            // Compat scalar: sum of IR marker buckets for pre-#1644 callers.
            const std::int64_t marker_total = ir_user + ir_macro_intro + ir_bool_lit;
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_int(marker_total);
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
                auto kidx = ev ? ev->push_string_heap(k_str) : 0;
                EvalValue key_ev = make_string(static_cast<std::uint64_t>(kidx));
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            if (!ev) {
                FlatHashTable::destroy(ht);
                return make_int(marker_total);
            }
            insert_kv("user", ir_user);
            insert_kv("macro-introduced", ir_macro_intro);
            insert_kv("bool-literal", ir_bool_lit);
            insert_kv("total", marker_total);
            insert_kv("lowering-marker-propagated", lowering_marker_propagated);
            insert_kv("ir-macro-introduced-inlined-skipped", ir_macro_introduced_inlined_skipped);
            insert_kv("ir-module-walked", ir_walked);
            insert_kv("schema", 1891);
            insert_kv("issue", 1891);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2096: query:macro-mutate-restamp-stats. Surfaces the
    // per-cloned-subtree MacroIntroduced restamp counter (subtree-local
    // coherence at expand exit + critical mutate entry). Paired with
    // (query:macro-hygiene-stats) key `macro-restamp-after-flat` which
    // surfaces the AST-wide sweep count.
    ObservabilityPrims::register_stats_impl(
        "query:macro-mutate-restamp-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            return make_int(
                static_cast<std::int64_t>(aura_macro_expand_mutate_restamp_total_v_read()));
        });

    // Issue #2176: query:macro-unstamp-stats. Surfaces the selective
    // unstamp counter for MacroIntroduced subtrees (Agent experimental
    // rollback path). Paired with (query:macro-hygiene-stats) for the
    // broader hygiene observability bundle.
    ObservabilityPrims::register_stats_impl(
        "query:macro-unstamp-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            return make_int(
                static_cast<std::int64_t>(aura_unstamp_macro_introduced_total_v_read()));
        });

    // Issue #2098 / #2239: query:macro-schema-cache-dirty-stamp-stats.
    // Surfaces the per-cloned-subtree schema-cache + dirty/provenance
    // stamp counter (clone_macro_body walk visibility for rest-param +
    // nested qq + schema_cache copy paths). #2239 expands the surface
    // from a single int to a hash so Agents can see the per-rest-param
    // + per-nested-qq breakdown under the same primitive name
    // (backward-compat: schema-cache-dirty-stamped-total key still
    // reads the existing #2098 counter). Pairs with
    // (query:macro-hygiene-stats) observability bundle so Agents /
    // dashboards see the stamping rate.
    ObservabilityPrims::register_stats_impl(
        "query:macro-schema-cache-dirty-stamp-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            // #2098: existing clone_macro_body per-node stamp counter.
            // Aliases (schema-cache-dirty-stamped / total) preserved so
            // existing read paths keep working.
            const std::int64_t stamped =
                static_cast<std::int64_t>(aura_macro_schema_cache_dirty_stamped_total_v_read());
            insert_kv("schema-cache-dirty-stamped-total", stamped);
            insert_kv("schema-cache-dirty-stamped", stamped);
            insert_kv("schema-cache-dirty-stamped-total-2098", stamped);
            // #2239: per-rest-param + nested-qq breakdown.
            insert_kv(
                "rest-param-nested-qq-hits-total",
                static_cast<std::int64_t>(aura_macro_rest_param_nested_qq_hits_total_v_read()));
            insert_kv(
                "rest-param-nested-qq-hits",
                static_cast<std::int64_t>(aura_macro_rest_param_nested_qq_hits_total_v_read()));
            insert_kv(
                "schema-cache-rest-stamped-total",
                static_cast<std::int64_t>(aura_macro_schema_cache_rest_stamped_total_v_read()));
            insert_kv(
                "schema-cache-rest-stamped",
                static_cast<std::int64_t>(aura_macro_schema_cache_rest_stamped_total_v_read()));
            insert_kv("schema", 2239);
            insert_kv("issue", 2239);
            insert_kv("active", 1);
            insert_kv("rest-param-qq-wired", 1);
            insert_kv("schema-cache-rest-stamp-wired", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #458: query:hygiene-stats. Returns an integer
    // equal to the total macro-introduced nodes skipped by
    // query:pattern so far (a single observable counter).
    // Future: returns a 3-tuple (violations skipped total-queries).
    // P0: returns the skipped count as an int.
    ObservabilityPrims::register_stats_impl(
        "query:hygiene-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            // Read via the thread-local yield-hook evaluator (same
            // pattern as the #453 hooks). Returns 0 when no
            // evaluator is active.
            auto* ev = Evaluator::yield_hook_evaluator();
            if (!ev)
                return make_int(0);
            return make_int(static_cast<std::int64_t>(ev->get_macro_introduced_skipped_in_query()));
        });

    // Issue #2099: query:hygiene-checkpoint-stats — observability
    // dashboard for (mutate:save-hygiene-checkpoint) /
    // (mutate:restore-hygiene-checkpoint) primitives. Returns a
    // hash with the 4 lifetime counters + pending slot count +
    // schema/issue markers. Agent uses this to monitor
    // what-if / self-evo rollback frequency + cross-fiber
    // rejection rate (the AC4 concurrent stress contract signal).
    ObservabilityPrims::register_stats_impl(
        "query:hygiene-checkpoint-stats",
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };

            insert_kv("save_total",
                      static_cast<std::int64_t>(ev->get_hygiene_checkpoint_save_total()));
            insert_kv("save-total",
                      static_cast<std::int64_t>(ev->get_hygiene_checkpoint_save_total()));
            insert_kv(
                "restore_success_total",
                static_cast<std::int64_t>(ev->get_hygiene_checkpoint_restore_success_total()));
            insert_kv(
                "restore-success-total",
                static_cast<std::int64_t>(ev->get_hygiene_checkpoint_restore_success_total()));
            insert_kv("restore_fail_total",
                      static_cast<std::int64_t>(ev->get_hygiene_checkpoint_restore_fail_total()));
            insert_kv("restore-fail-total",
                      static_cast<std::int64_t>(ev->get_hygiene_checkpoint_restore_fail_total()));
            insert_kv(
                "cross_fiber_reject_total",
                static_cast<std::int64_t>(ev->get_hygiene_checkpoint_cross_fiber_reject_total()));
            insert_kv(
                "cross-fiber-reject-total",
                static_cast<std::int64_t>(ev->get_hygiene_checkpoint_cross_fiber_reject_total()));
            insert_kv("pending_count",
                      static_cast<std::int64_t>(ev->hygiene_checkpoint_pending_count()));
            insert_kv("pending-count",
                      static_cast<std::int64_t>(ev->hygiene_checkpoint_pending_count()));
            insert_kv("schema", 2099);
            insert_kv("issue", 2099);
            insert_kv("active", 1);
            insert_kv("lineage-1893", 1893);
            insert_kv("nested-under-mutation-boundary", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #456 / #1036: query:dirty-subtree root-node-id
    // [reason-mask]. Walks the **subtree** rooted at root-node-id
    // (BFS over children) and returns the number of dirty nodes.
    // Optional 2nd arg: dirty-reason bitmask (default 0xFF = all).
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
        if (root == aura::ast::NULL_NODE || root >= ws->size())
            return make_int(0);
        // Issue #1036: BFS descendants (include root), not ancestor walk.
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
            m->dirty_subtree_bfs_walks_total.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t count = 0;
        std::vector<aura::ast::NodeId> stack;
        stack.push_back(root);
        while (!stack.empty()) {
            const auto cur = stack.back();
            stack.pop_back();
            if (cur == aura::ast::NULL_NODE || cur >= ws->size())
                continue;
            if ((ws->dirty(cur) & reason_mask) != 0)
                ++count;
            for (auto child : ws->children(cur))
                stack.push_back(static_cast<aura::ast::NodeId>(child));
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
    ObservabilityPrims::register_stats_impl(
        "query:mutation-impact-snapshot",
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
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-log", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:stability-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
            insert_kv(
                "stable-ref-validated",
                static_cast<std::int64_t>(ev->get_stable_ref_validated_in_primitives_count()));
            insert_kv("stale-ref-blocked",
                      static_cast<std::int64_t>(ev->get_stale_ref_blocked_count()));
            insert_kv("stale-ref-warned",
                      static_cast<std::int64_t>(ev->get_stale_ref_warned_count()));
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
    ObservabilityPrims::register_stats_impl(
        "query:epoch-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:epoch-delta-since-last-query", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-stats-hash",
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
            // Build a hash using the FNV-1a scheme (same as other
            // observability primitives). Capacity 16→64: #2170/#2250/
            // #2255/#2351 layout-stamp keys fit without silent drop.
            // Uses the `string_heap` reference passed into
            // register_query_primitives() (avoids the private
            // Evaluator::string_heap_ field).
            auto* ht = FlatHashTable::create(64);
            if (!ht)
                return make_int(rec_int);
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
            // Issue #2960: query:*-stable stamp counters (Agent export path).
            insert_kv("query-stable-ref-stamped-total",
                      static_cast<std::int64_t>(
                          aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
                              std::memory_order_relaxed)));
            insert_kv(
                "query-stable-ref-unstamped-prevented-total",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_unstamped_prevented_total_atomic()
                        .load(std::memory_order_relaxed)));
            insert_kv("schema-2960", 2960);
            insert_kv("issue-2960", 2960);
            // Issue #3000: restamp-lag export gate (additive; stamped /
            // unstamped_prevented non-regressing).
            insert_kv(
                "query-stable-ref-restamp-lag-prevented-total",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic()
                        .load(std::memory_order_relaxed)));
            insert_kv("query-stable-ref-restamp-lag-soft-observe-total",
                      static_cast<std::int64_t>(
                          aura::core::provenance::
                              g_query_stable_ref_restamp_lag_soft_observe_total_atomic()
                                  .load(std::memory_order_relaxed)));
            insert_kv(
                "query-stable-ref-restamp-lag-last-reason",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_restamp_lag_last_reason_atomic()
                        .load(std::memory_order_relaxed)));
            insert_kv("schema-3000", 3000);
            insert_kv("issue-3000", 3000);
            // Issue #3037: over-budget restamp torn export (additive on
            // stable-ref-stats-hash; #2960 stamped / #3000 lag non-regress).
            insert_kv(
                "query-stable-ref-restamp-torn-reject-total",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_restamp_torn_reject_total_atomic()
                        .load(std::memory_order_relaxed)));
            insert_kv("query-stable-ref-restamp-torn-soft-observe-total",
                      static_cast<std::int64_t>(
                          aura::core::provenance::
                              g_query_stable_ref_restamp_torn_soft_observe_total_atomic()
                                  .load(std::memory_order_relaxed)));
            insert_kv(
                "query-stable-ref-restamp-torn-wired",
                static_cast<std::int64_t>(
                    aura::core::provenance::g_query_stable_ref_restamp_torn_wired_atomic().load(
                        std::memory_order_relaxed)));
            insert_kv(
                "restamp-generation-torn",
                static_cast<std::int64_t>(
                    ev.workspace_flat() && ev.workspace_flat()->restamp_generation_torn() ? 1 : 0));
            insert_kv("schema-3037", 3037);
            insert_kv("issue-3037", 3037);
            // Issue #3041: restamp-budget QueryEpoch stale (poll after Guard).
            insert_kv("restamp-budget-query-epoch-stale-total",
                      static_cast<std::int64_t>(
                          aura::core::g_restamp_budget_query_epoch_stale_total().load(
                              std::memory_order_relaxed)));
            insert_kv("query-epoch-forced-stale",
                      aura::core::g_query_epoch_forced_stale().load(std::memory_order_relaxed) ? 1
                                                                                               : 0);
            insert_kv("schema-3041", 3041);
            insert_kv("issue-3041", 3041);
            // Issue #2170: LayoutStamp keys (schema bump — fold into the
            // existing stable-ref-stats-hash per #2170 AC contract,
            // "no new public prim if constrained"). The stamp captures
            // all 6 cross-subsystem epoch fields from a single helper
            // (current_layout_stamp()) so Agents / FFI / AOT emit can
            // read a coherent view instead of each picking a different
            // "current" value. bumpers / getters back these keys.
            const auto stamp = ev.current_layout_stamp();
            insert_kv("layout-stamp-arena-id", static_cast<std::int64_t>(stamp.arena_id));
            insert_kv("layout-stamp-arena-gen", static_cast<std::int64_t>(stamp.arena_gen));
            insert_kv("layout-stamp-flat-gen", static_cast<std::int64_t>(stamp.flat_gen));
            insert_kv("layout-stamp-mutation-epoch",
                      static_cast<std::int64_t>(stamp.mutation_epoch));
            insert_kv("layout-stamp-env-gen", static_cast<std::int64_t>(stamp.env_gen));
            insert_kv("layout-stamp-defuse-version",
                      static_cast<std::int64_t>(stamp.defuse_version));
            insert_kv("layout-stamp-publish-total",
                      static_cast<std::int64_t>(ev.get_layout_stamp_publish_total()));
            insert_kv("layout-stamp-last-arena-gen",
                      static_cast<std::int64_t>(ev.get_layout_stamp_last_arena_gen()));
            insert_kv("layout-stamp-last-flat-gen",
                      static_cast<std::int64_t>(ev.get_layout_stamp_last_flat_gen()));
            insert_kv("layout-stamp-schema",
                      static_cast<std::int64_t>(aura::core::kLayoutStampSchema));
            insert_kv("layout-stamp-issue",
                      static_cast<std::int64_t>(aura::core::kLayoutStampSchema));
            // Issue #2519: operator== is full 8-field (Agents / fiber freshness).
            insert_kv("layout-stamp-equality-8-field", 1);
            insert_kv("layout-stamp-equality-schema",
                      static_cast<std::int64_t>(aura::core::kLayoutStampEqualitySchema));
            insert_kv("schema-2519", 2519);
            insert_kv("issue-2519", 2519);
            insert_kv("layout-stamp-active", 1);
            // Issue #2250: LayoutStamp fence on Fiber resume/steal
            insert_kv("layout-stamp-resume-mismatch-total",
                      static_cast<std::int64_t>(ev.get_layout_stamp_resume_mismatch_total()));
            insert_kv("layout-stamp-resume-wired", 1);
            insert_kv("schema-2250", 2250);
            insert_kv("issue-2250", 2250);
            // Issue #2351: steal-complete LayoutStamp dual-check (before resume).
            insert_kv("layout-stamp-steal-mismatch-total",
                      static_cast<std::int64_t>(ev.get_layout_stamp_steal_mismatch_total()));
            insert_kv("layout_stamp_steal_mismatch_total",
                      static_cast<std::int64_t>(ev.get_layout_stamp_steal_mismatch_total()));
            insert_kv("layout-stamp-steal-missing-total",
                      static_cast<std::int64_t>(ev.get_layout_stamp_steal_missing_total()));
            insert_kv("layout_stamp_steal_missing_total",
                      static_cast<std::int64_t>(ev.get_layout_stamp_steal_missing_total()));
            insert_kv("layout-stamp-steal-wired", 1);
            insert_kv("schema-2351", 2351);
            insert_kv("issue-2351", 2351);
            // Issue #2510: transactional restamp on steal-complete success.
            insert_kv("steal-complete-restamp-total",
                      static_cast<std::int64_t>(ev.get_steal_complete_restamp_total()));
            insert_kv("steal-complete-layout-hard-fail-total",
                      static_cast<std::int64_t>(ev.get_steal_complete_layout_hard_fail_total()));
            insert_kv("steal-complete-restamp-wired", 1);
            insert_kv("schema-2510", 2510);
            insert_kv("issue-2510", 2510);
            // Issue #2255: ShapeProfiler monotonic generation (7th
            // LayoutStamp field) hard-fence counter.
            insert_kv("shape-version-fence-reject-total",
                      static_cast<std::int64_t>(ev.get_shape_version_fence_reject_total()));
            insert_kv("shape-version-fence-wired", 1);
            insert_kv("schema-2255", 2255);
            insert_kv("issue-2255", 2255);
            // Issue #2432: IR SoA generation fence (8th LayoutStamp field).
            insert_kv("layout-stamp-ir-soa-generation",
                      static_cast<std::int64_t>(stamp.ir_soa_generation));
            insert_kv("ir-generation-fence-hit-total",
                      static_cast<std::int64_t>(ev.get_ir_generation_fence_hit_total()));
            insert_kv("ir-generation-fence-wired", 1);
            insert_kv("schema-2432", 2432);
            insert_kv("issue-2432", 2432);
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

    // Issue #1470 / #1499 / #1593 / #1597 / #1599 / #1613:
    // query:ai-closedloop-readiness-stats — consolidated health for AI editing loops.
    //
    // #1470: wraps / invalidations / batch-commits / hygiene-skips /
    // dirty-prunes + recommendation [0..4].
    // #1499: production breakdown + health-score 0..100 + action.
    // #1593: SLO breach, health-trend, sibling linkage (#1591/#1592),
    // adaptive-safepoint recommendation + soft adapt signal.
    // #1597: parallel orchestration (join latency / mailbox backpressure /
    // parallel throughput / starvation mitigated) folded into health.
    // #1599: linear GC root audit + live-closure scans + mutation depth hist
    // linked into readiness.
    // #1613: macro hygiene submodule (macro-health-score + audit trail linkage).
    // Schema **1613** (Agents may still accept 1599|1597|1593|1499).
    //
    // Recommendation priority (most severe first, #1470 contract):
    //   0 = healthy  1 = wraps  2 = high invalidations  3 = hygiene
    //   4 = dirty-prunes
    // Action (#1499 orchestration hint, independent of recommendation):
    //   0 = ok  1 = investigate-refs  2 = throttle-mutate
    //   3 = raise-quota  4 = check-cascade
    ObservabilityPrims::register_stats_impl(
        "query:ai-closedloop-readiness-stats",
        [&pairs, &string_heap, &ev](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            (void)pairs;
            // Process-wide trend / SLO (Agent samples this primitive repeatedly).
            static std::atomic<std::int64_t> s_prev_health{100};
            static std::atomic<std::uint64_t> s_samples{0};
            static std::atomic<std::uint64_t> s_slo_breach_total{0};
            static std::atomic<std::uint64_t> s_adaptive_soft_triggers{0};

            std::uint64_t wraps = 0;
            std::uint64_t invalidations = 0;
            std::uint64_t batch_commits = 0;
            std::uint64_t dirty_prunes = 0;
            if (auto* ws = ev.workspace_flat()) {
                wraps = ws->generation_wrap_count();
                invalidations = ws->stable_ref_invalidations();
                batch_commits = ws->atomic_batch_commits();
                dirty_prunes = ws->mark_dirty_boundary_prune_count();
            }
            const std::uint64_t hygiene_skips = ir_inline_hygiene_skipped(&ev);
            std::int64_t rec_int = 0;
            if (wraps > 0)
                rec_int = 1;
            else if (invalidations >= 10)
                rec_int = 2;
            else if (hygiene_skips >= 100)
                rec_int = 3;
            else if (dirty_prunes >= 50)
                rec_int = 4;

            // Issue #1499: production counters from CompilerMetrics.
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const auto load =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                if (!m)
                    return 0;
                return (m->*field).load(std::memory_order_relaxed);
            };
            const std::uint64_t linear_enforce =
                load(&CompilerMetrics::linear_post_mutate_enforcements_total);
            const std::uint64_t linear_enforce_per =
                load(&CompilerMetrics::linear_post_mutate_enforcements);
            // #1599: linear GC root audit + live-closure scan linkage.
            const std::uint64_t linear_gc_audit =
                load(&CompilerMetrics::linear_gc_root_audit_checks_total);
            const std::uint64_t linear_live_scans =
                load(&CompilerMetrics::linear_live_closure_scans_total);
            std::uint64_t mut_depth_hist_sum = 0;
            if (m) {
                for (std::size_t i = 0; i < CompilerMetrics::kMutationStackDepthHistBuckets; ++i)
                    mut_depth_hist_sum +=
                        m->mutation_stack_depth_histogram[i].load(std::memory_order_relaxed);
            }
            const std::uint64_t cascade_max = load(&CompilerMetrics::invalidate_cascade_depth_max);
            const std::uint64_t cascade_total =
                load(&CompilerMetrics::invalidate_cascade_depth_total);
            const std::uint64_t protocol =
                load(&CompilerMetrics::unified_invalidation_protocol_total);
            const std::uint64_t steal_refresh =
                load(&CompilerMetrics::stable_ref_steal_auto_refresh_total);
            const std::uint64_t boundary_refresh =
                load(&CompilerMetrics::boundary_pinned_refresh_count);
            const std::uint64_t live_stale =
                load(&CompilerMetrics::compiler_live_closure_stale_prevented_total);
            const std::uint64_t rollbacks =
                load(&CompilerMetrics::mutation_boundary_yield_rollback_total);
            const std::uint64_t quota_rejects =
                load(&CompilerMetrics::resource_quota_rejects_total);
            const std::uint64_t relower_blocks =
                load(&CompilerMetrics::incremental_relower_blocks_total);
            const std::uint64_t full_relower = load(&CompilerMetrics::relower_full_called_count);
            const std::uint64_t per_fn_relower =
                load(&CompilerMetrics::relower_per_function_called_count);
            const std::uint64_t fiber_depth_max = ev.get_per_fiber_mutation_stack_depth_max();
            const std::uint64_t live_depth =
                static_cast<std::uint64_t>(Evaluator::mutation_boundary_depth());
            const std::uint64_t bridge_bumps = load(&CompilerMetrics::bridge_epoch_bumps_total);

            // #1593 sibling linkage (#1591 fairness / #1592 post-steal).
            const std::uint64_t safepoint_wait_us =
                aura::gc_hooks::safepoint_wait_while_mutation_held_us();
            const std::uint64_t safe_yield_skip = ev.get_safe_yield_skipped_held_total();
            const std::uint64_t post_steal_refresh = ev.get_post_steal_refresh_count();
            auto& steal_s = ::aura::serve::metrics::adaptive_steal_stats();
            const std::uint64_t steal_mitigated =
                steal_s.steal_inner_deferred_starvation_mitigated_count.load(
                    std::memory_order_relaxed);
            const std::uint64_t hold_total =
                load(&CompilerMetrics::mutation_boundary_hold_time_total_us);
            const std::uint64_t hold_samples =
                load(&CompilerMetrics::mutation_boundary_holds_total);
            const std::int64_t avg_hold_us =
                hold_samples > 0 ? static_cast<std::int64_t>(hold_total / hold_samples) : 0;

            // Cascade avg (bp for integer: total*100 / protocol when >0).
            std::int64_t cascade_avg_x100 = 0;
            if (protocol > 0)
                cascade_avg_x100 = static_cast<std::int64_t>((cascade_total * 100u) / protocol);

            // Relower partial ratio in basis points (partial / (partial+full)).
            std::int64_t relower_partial_bp = 10000;
            {
                const auto sum = per_fn_relower + full_relower;
                if (sum > 0)
                    relower_partial_bp = static_cast<std::int64_t>((per_fn_relower * 10000u) / sum);
            }

            // Health score 0..100 (higher is better). Start at 100 and
            // apply capped penalties so agents get a single ordinal signal.
            std::int64_t health = 100;
            auto penalize = [&](std::int64_t pts) {
                health -= pts;
                if (health < 0)
                    health = 0;
            };
            if (wraps > 0)
                penalize(20);
            if (invalidations >= 10)
                penalize(std::min<std::int64_t>(20, static_cast<std::int64_t>(invalidations / 5)));
            if (rollbacks >= 5)
                penalize(std::min<std::int64_t>(20, static_cast<std::int64_t>(rollbacks / 5)));
            if (cascade_max >= 8)
                penalize(std::min<std::int64_t>(15, static_cast<std::int64_t>(cascade_max)));
            if (quota_rejects >= 5)
                penalize(std::min<std::int64_t>(15, static_cast<std::int64_t>(quota_rejects / 5)));
            if (relower_partial_bp < 5000 && (full_relower + per_fn_relower) >= 10)
                penalize(10); // mostly full re-lower under load
            if (hygiene_skips >= 100)
                penalize(10);
            if (dirty_prunes >= 50)
                penalize(5);
            // #1613: macro hygiene submodule pressure on overall health.
            const std::uint64_t macro_viol =
                static_cast<std::uint64_t>(ev.get_hygiene_violation_count());
            const std::uint64_t naked_macro = load(&CompilerMetrics::naked_macro_mutate_attempt);
            const std::uint64_t macro_stale_refs = ev.get_macro_stale_ref_prevented();
            const std::uint64_t reflect_macro_rej =
                load(&CompilerMetrics::reflect_macro_hygiene_rejects_total);
            const std::uint64_t macro_query_skips = ev.get_macro_introduced_skipped_in_query() +
                                                    ev.get_pattern_recursive_macro_skipped();
            const std::uint64_t macro_audit_blocked =
                typed_audit::g_typed_mutation_audit_counters.macro_hygiene_blocked.load(
                    std::memory_order_relaxed);
            // Macro health subscore 0..100 (mirrored on macro-hygiene-stats).
            std::int64_t macro_health = 100;
            auto macro_pen = [&](std::int64_t pts) {
                macro_health -= pts;
                if (macro_health < 0)
                    macro_health = 0;
            };
            if (macro_viol > 0)
                macro_pen(std::min<std::int64_t>(40, static_cast<std::int64_t>(macro_viol) * 5));
            if (naked_macro > 0)
                macro_pen(std::min<std::int64_t>(25, static_cast<std::int64_t>(naked_macro) * 3));
            if (reflect_macro_rej > 0)
                macro_pen(
                    std::min<std::int64_t>(15, static_cast<std::int64_t>(reflect_macro_rej) * 2));
            if (macro_stale_refs > 20)
                macro_pen(
                    std::min<std::int64_t>(10, static_cast<std::int64_t>(macro_stale_refs) / 10));
            if (macro_health < 80)
                penalize(std::min<std::int64_t>(15, (80 - macro_health) / 2));
            // #1593: long holds / safepoint wait under mutation pressure.
            if (avg_hold_us >= 5000)
                penalize(std::min<std::int64_t>(10, avg_hold_us / 5000));
            if (safepoint_wait_us >= 100000)
                penalize(5);
            if (safe_yield_skip >= 50)
                penalize(5);

            // ── #1597: parallel orchestration health ─────────────────
            using aura::serve::Fiber;
            using aura::serve::mf_mailbox::g_mf_mailbox_stats;
            using aura::serve::parallel_orch::g_parallel_orch_stats;
            const std::uint64_t join_n = Fiber::join_total();
            const std::uint64_t join_wait_us = Fiber::join_wait_us_total();
            const std::uint64_t join_wait_max = Fiber::join_wait_us_max();
            const std::uint64_t join_timeouts = Fiber::join_timeout_total();
            const std::int64_t avg_join_us =
                join_n > 0 ? static_cast<std::int64_t>(join_wait_us / join_n) : 0;
            // Coarse histogram bucket counts (exported as sum + b0..b4).
            const std::uint64_t join_hist_sum = Fiber::join_latency_hist_sum();
            const std::uint64_t join_hist_b0 = Fiber::join_latency_hist(0);
            const std::uint64_t join_hist_b1 = Fiber::join_latency_hist(1);
            const std::uint64_t join_hist_b2 = Fiber::join_latency_hist(2);
            const std::uint64_t join_hist_b3 = Fiber::join_latency_hist(3);
            const std::uint64_t join_hist_b4 = Fiber::join_latency_hist(4);

            const std::uint64_t mb_pushes =
                g_mf_mailbox_stats.pushes.load(std::memory_order_relaxed);
            const std::uint64_t mb_bp =
                g_mf_mailbox_stats.backpressure_rejects.load(std::memory_order_relaxed);
            const std::uint64_t mb_recv_waits =
                g_mf_mailbox_stats.recv_waits.load(std::memory_order_relaxed);
            const std::uint64_t mb_recv_timeouts =
                g_mf_mailbox_stats.recv_timeouts.load(std::memory_order_relaxed);
            // Pseudo-p99 backpressure pressure (ms): 0 when no rejects; else
            // scales with reject rate (capped). Agents use as pressure gauge.
            std::int64_t mailbox_backpressure_p99 = 0;
            if (mb_bp > 0) {
                const auto denom = mb_pushes + mb_bp;
                const auto rate_bp =
                    denom > 0 ? static_cast<std::int64_t>((mb_bp * 10000u) / denom) : 10000;
                mailbox_backpressure_p99 =
                    std::min<std::int64_t>(10000, 50 + rate_bp + static_cast<std::int64_t>(mb_bp));
            }

            const std::uint64_t po_joined =
                g_parallel_orch_stats.tasks_joined.load(std::memory_order_relaxed);
            const std::uint64_t po_ok =
                g_parallel_orch_stats.tasks_ok.load(std::memory_order_relaxed);
            const std::uint64_t po_err =
                g_parallel_orch_stats.tasks_err.load(std::memory_order_relaxed);
            const std::uint64_t po_ff =
                g_parallel_orch_stats.fail_fast_aborts.load(std::memory_order_relaxed);
            const std::uint64_t po_to =
                g_parallel_orch_stats.timeouts.load(std::memory_order_relaxed);
            const std::uint64_t po_elapsed =
                g_parallel_orch_stats.parallel_elapsed_us.load(std::memory_order_relaxed);
            // Throughput (tasks/s ×1000 as milli-tps integer for EDSL int fields).
            std::int64_t parallel_throughput_mtps = 0;
            if (po_elapsed > 0 && po_ok > 0)
                parallel_throughput_mtps =
                    static_cast<std::int64_t>((po_ok * 1000000ull) / po_elapsed); // tasks/s

            // Orchestration subscore 0..100 (higher better).
            std::int64_t orch_health = 100;
            auto orch_pen = [&](std::int64_t pts) {
                orch_health -= pts;
                if (orch_health < 0)
                    orch_health = 0;
            };
            if (avg_join_us >= 1000)
                orch_pen(std::min<std::int64_t>(20, avg_join_us / 1000));
            if (join_timeouts >= 3)
                orch_pen(std::min<std::int64_t>(15, static_cast<std::int64_t>(join_timeouts) * 3));
            if (mb_bp >= 5)
                orch_pen(std::min<std::int64_t>(20, static_cast<std::int64_t>(mb_bp) / 2));
            if (po_joined > 0 && po_err * 5 >= po_joined)
                orch_pen(15); // ≥20% error rate
            if (po_ff >= 2)
                orch_pen(10);
            if (po_to >= 2)
                orch_pen(10);

            // Fold orch into main health (capped soft penalty).
            if (orch_health < 80)
                penalize(std::min<std::int64_t>(15, (80 - orch_health) / 2));
            if (avg_join_us >= 10000)
                penalize(5);
            if (mailbox_backpressure_p99 >= 500)
                penalize(5);

            // Starvation mitigated = steal fairness + join-path linear enforce
            // (join Ok after wait) + mailbox recv_waits that completed (proxy).
            const std::uint64_t orch_starvation_mitigated =
                steal_mitigated + Fiber::join_linear_enforcement_total();

            // Orchestration action (independent of #1470 recommendation).
            std::int64_t action = 0;
            if (quota_rejects >= 5)
                action = 3; // raise-quota
            else if (cascade_max >= 8)
                action = 4; // check-cascade
            else if (rollbacks >= 5 || invalidations >= 20 || orch_health < 50)
                action = 2; // throttle-mutate (incl. orch pressure)
            else if (wraps > 0 || invalidations >= 10 || boundary_refresh + steal_refresh >= 50 ||
                     mb_bp >= 10 || avg_join_us >= 5000)
                action = 1; // investigate-refs / orch latency

            // #1593 SLO: health < threshold OR severe action.
            constexpr std::int64_t kSloThreshold = 70;
            const bool slo_breach = (health < kSloThreshold) || (action >= 3);
            if (slo_breach)
                s_slo_breach_total.fetch_add(1, std::memory_order_relaxed);

            // Health trend vs previous sample (positive = improving).
            const std::int64_t prev = s_prev_health.exchange(health, std::memory_order_relaxed);
            const std::int64_t health_trend = health - prev;
            s_samples.fetch_add(1, std::memory_order_relaxed);

            // Adaptive safepoint soft linkage: on hard breach, nudge threshold
            // (orchestrators also read adaptive-safepoint-recommended).
            std::int64_t adaptive_recommended = 0;
            if (slo_breach || live_depth > 0 || avg_hold_us >= 2000 || safe_yield_skip >= 20 ||
                orch_health < 60)
                adaptive_recommended = 1;
            // #1597: recommend lowering parallel concurrency under orch pressure.
            std::int64_t adaptive_concurrency_recommended = 0;
            if (orch_health < 70 || mailbox_backpressure_p99 >= 200 || avg_join_us >= 5000 ||
                po_ff >= 1 || po_to >= 1)
                adaptive_concurrency_recommended = 1;
            if (slo_breach && health < 50) {
                s_adaptive_soft_triggers.fetch_add(1, std::memory_order_relaxed);
                // Soft adapt: one exponential backoff step (capped in helper).
                ev.bump_safepoint_adaptive_threshold();
            }

            auto* ht = FlatHashTable::create(128);
            if (!ht)
                return make_int(rec_int);
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
                        auto kidx = string_heap.size();
                        string_heap.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            // #1470 fields (stable contract)
            insert_kv("wraps", static_cast<std::int64_t>(wraps));
            insert_kv("invalidations", static_cast<std::int64_t>(invalidations));
            insert_kv("batch-commits", static_cast<std::int64_t>(batch_commits));
            insert_kv("hygiene-skips", static_cast<std::int64_t>(hygiene_skips));
            insert_kv("dirty-prunes", static_cast<std::int64_t>(dirty_prunes));
            insert_kv("recommendation", rec_int);
            // #1499 production health + breakdown
            insert_kv("health-score", health);
            insert_kv("action", action);
            insert_kv("linear-enforcements", static_cast<std::int64_t>(linear_enforce));
            insert_kv("linear-enforcements-per-frame",
                      static_cast<std::int64_t>(linear_enforce_per));
            insert_kv("cascade-depth-max", static_cast<std::int64_t>(cascade_max));
            insert_kv("cascade-depth-avg-x100", cascade_avg_x100);
            insert_kv("invalidation-protocol", static_cast<std::int64_t>(protocol));
            insert_kv("bridge-epoch-bumps", static_cast<std::int64_t>(bridge_bumps));
            insert_kv("steal-auto-refresh", static_cast<std::int64_t>(steal_refresh));
            insert_kv("boundary-pinned-refresh", static_cast<std::int64_t>(boundary_refresh));
            insert_kv("live-closure-stale-prevented", static_cast<std::int64_t>(live_stale));
            insert_kv("yield-rollbacks", static_cast<std::int64_t>(rollbacks));
            insert_kv("quota-rejects", static_cast<std::int64_t>(quota_rejects));
            insert_kv("relower-blocks", static_cast<std::int64_t>(relower_blocks));
            insert_kv("full-relower", static_cast<std::int64_t>(full_relower));
            insert_kv("partial-relower", static_cast<std::int64_t>(per_fn_relower));
            insert_kv("relower-partial-bp", relower_partial_bp);
            insert_kv("fiber-depth-max", static_cast<std::int64_t>(fiber_depth_max));
            insert_kv("live-mutation-depth", static_cast<std::int64_t>(live_depth));
            // #1593: SLO + trend + sibling linkage + adaptive
            insert_kv("slo-breach", slo_breach ? 1 : 0);
            insert_kv("slo-threshold", kSloThreshold);
            insert_kv("slo-breach-total", static_cast<std::int64_t>(
                                              s_slo_breach_total.load(std::memory_order_relaxed)));
            insert_kv("health-trend", health_trend);
            insert_kv("health-prev", prev);
            insert_kv("samples-total",
                      static_cast<std::int64_t>(s_samples.load(std::memory_order_relaxed)));
            insert_kv("avg-hold-time-us", avg_hold_us);
            insert_kv("safepoint-wait-while-mutation-held-us",
                      static_cast<std::int64_t>(safepoint_wait_us));
            insert_kv("safe-yield-skipped-held", static_cast<std::int64_t>(safe_yield_skip));
            insert_kv("post-steal-refresh-count", static_cast<std::int64_t>(post_steal_refresh));
            insert_kv("steal-inner-deferred-starvation-mitigated-count",
                      static_cast<std::int64_t>(steal_mitigated));
            insert_kv("adaptive-safepoint-recommended", adaptive_recommended);
            insert_kv("adaptive-soft-triggers",
                      static_cast<std::int64_t>(
                          s_adaptive_soft_triggers.load(std::memory_order_relaxed)));
            insert_kv("adaptive-safepoint-threshold",
                      static_cast<std::int64_t>(ev.get_safepoint_adaptive_threshold()));
            // #1597: parallel orchestration breakdown + SLO linkage
            insert_kv("orch-health-score", orch_health);
            insert_kv("avg-join-latency-us", avg_join_us);
            insert_kv("join-latency-max-us", static_cast<std::int64_t>(join_wait_max));
            insert_kv("join-total", static_cast<std::int64_t>(join_n));
            insert_kv("join-timeouts", static_cast<std::int64_t>(join_timeouts));
            insert_kv("join_latency_histogram", static_cast<std::int64_t>(join_hist_sum));
            insert_kv("join-latency-hist-b0", static_cast<std::int64_t>(join_hist_b0));
            insert_kv("join-latency-hist-b1", static_cast<std::int64_t>(join_hist_b1));
            insert_kv("join-latency-hist-b2", static_cast<std::int64_t>(join_hist_b2));
            insert_kv("join-latency-hist-b3", static_cast<std::int64_t>(join_hist_b3));
            insert_kv("join-latency-hist-b4", static_cast<std::int64_t>(join_hist_b4));
            insert_kv("mailbox_backpressure_p99", mailbox_backpressure_p99);
            insert_kv("mailbox-backpressure-rejects", static_cast<std::int64_t>(mb_bp));
            insert_kv("mailbox-pushes", static_cast<std::int64_t>(mb_pushes));
            insert_kv("mailbox-recv-waits", static_cast<std::int64_t>(mb_recv_waits));
            insert_kv("mailbox-recv-timeouts", static_cast<std::int64_t>(mb_recv_timeouts));
            insert_kv("parallel_task_throughput", parallel_throughput_mtps);
            insert_kv("parallel-tasks-ok", static_cast<std::int64_t>(po_ok));
            insert_kv("parallel-tasks-err", static_cast<std::int64_t>(po_err));
            insert_kv("parallel-tasks-joined", static_cast<std::int64_t>(po_joined));
            insert_kv("parallel-fail-fast", static_cast<std::int64_t>(po_ff));
            insert_kv("parallel-timeouts", static_cast<std::int64_t>(po_to));
            insert_kv("parallel-elapsed-us", static_cast<std::int64_t>(po_elapsed));
            insert_kv("orchestration_starvation_mitigated",
                      static_cast<std::int64_t>(orch_starvation_mitigated));
            insert_kv("adaptive-concurrency-recommended", adaptive_concurrency_recommended);
            // #1599: GC root audit + linear scan + mutation depth histogram
            insert_kv("linear-gc-root-audit-checks", static_cast<std::int64_t>(linear_gc_audit));
            insert_kv("linear_gc_root_audit_checks_total",
                      static_cast<std::int64_t>(linear_gc_audit));
            insert_kv("linear-live-closure-scans", static_cast<std::int64_t>(linear_live_scans));
            insert_kv("mutation_stack_depth_histogram",
                      static_cast<std::int64_t>(mut_depth_hist_sum));
            insert_kv("mutation-stack-depth-hist-sum",
                      static_cast<std::int64_t>(mut_depth_hist_sum));
            // #1613: macro hygiene submodule (ai-closedloop-macro-health)
            insert_kv("macro-health-score", macro_health);
            insert_kv("macro-hygiene-violations", static_cast<std::int64_t>(macro_viol));
            insert_kv("macro-naked-mutate-attempts", static_cast<std::int64_t>(naked_macro));
            insert_kv("macro-stale-ref-prevented", static_cast<std::int64_t>(macro_stale_refs));
            insert_kv("macro-query-skips", static_cast<std::int64_t>(macro_query_skips));
            insert_kv("macro-reflect-rejects", static_cast<std::int64_t>(reflect_macro_rej));
            insert_kv("macro-audit-blocked", static_cast<std::int64_t>(macro_audit_blocked));
            insert_kv("macro-audit-events",
                      static_cast<std::int64_t>(
                          typed_audit::g_typed_mutation_audit_counters.macro_hygiene_events.load(
                              std::memory_order_relaxed)));
            insert_kv("macro-hygiene-submodule-wired", 1);
            insert_kv("issue", 1613);
            insert_kv("schema", 1613); // lineage 1599|1597|1593|1499
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #738: query:stable-ref-boundary-stats-hash — cross-COW /
    // sub-workspace pinning + boundary validity observability for
    // concurrent AI orchestration. Complements #527
    // (stable-ref-cow-fiber-stats) and #457 (stable-ref-stats).
    // Issue #2189: Agent pin-lifecycle counters (schema-2189 fold-in).
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-boundary-stats-hash", [&ev, &string_heap](const auto&) -> EvalValue {
            // Capacity 32: #738 keys + #2189 pin-lifecycle fields.
            auto* ht = FlatHashTable::create(32);
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
            // Issue #2189: Agent pin table lifecycle (pin-stable-refs / with-pinned).
            insert_kv("pin-table-size", static_cast<std::int64_t>(pins_active));
            insert_kv("agent-pin-ops-total", static_cast<std::int64_t>(ev.agent_pin_ops_total()));
            insert_kv("agent-unpin-ops-total",
                      static_cast<std::int64_t>(ev.agent_unpin_ops_total()));
            insert_kv("agent-pin-restamp-total",
                      static_cast<std::int64_t>(ev.agent_pin_restamp_total()));
            insert_kv("agent-pin-invalidate-total",
                      static_cast<std::int64_t>(ev.agent_pin_invalidate_total()));
            insert_kv("schema-2189", 2189);
            insert_kv("issue-2189", 2189);
            insert_kv("agent-pin-lifecycle-wired", 1);
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
            // Issue #2056: mandate tenant + fiber stamp on provenance query capture.
            auto ref = ev.make_stamped_safe_ref(nid, /*workspace_id=*/0, cur_fiber);
            const bool is_live = ref.is_valid_in(*ws);
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_bool(false);
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
                      static_cast<std::int64_t>(
                          static_cast<std::uint16_t>(ref.last_validated_generation)));
            insert_kv("wrap-epoch", static_cast<std::int64_t>(ref.wrap_epoch));
            insert_kv("subtree-gen-at-capture",
                      static_cast<std::int64_t>(ref.subtree_gen_at_capture));
            insert_kv("is-live", is_live ? 1 : 0);
            // Issue #2056: tenant stamp on every Agent-facing capture.
            insert_kv("tenant-id", static_cast<std::int64_t>(ref.tenant_id));
            insert_kv("schema", 620);
            insert_kv("schema-2056", 2056);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #497: query:stable-ref-lifecycle-stats — long-session
    // generation/compaction/refresh observability for AI loops.
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-lifecycle-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-provenance-sv-stats-hash",
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
    ObservabilityPrims::register_stats_impl(
        "query:fiber-migration-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:gc-safepoint-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:verify-tool-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:orchestration-metrics", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            // The C-linkage shim returns the static
            // gc_pause_attributed_to_mutation_count_ from
            // the Fiber class. Per-Fiber yield counts are
            // aggregated via the active yield-hook
            // evaluator (the P0 reads them via a thread-
            // local; the follow-up uses GlobalMetrics).
            const std::uint64_t gc_pauses = aura_fiber_static_gc_pause_attributed_to_mutation();
            const std::uint64_t sum = gc_pauses;
            std::string result = "{\"gc_pauses_attributed_to_mutation\":";
            result += std::to_string(gc_pauses);
            result += "}";
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
    ObservabilityPrims::register_stats_impl(
        "query:scheduler-mutation-coord-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:query-stats", [](std::span<const EvalValue> a) -> EvalValue {
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

    // Issue #2914: peeled sub-registers
    register_query_obs_mid_primitives(add, pairs, string_heap, type_registry, resolve_module_path,
                                      ev);
    register_query_type_stats_primitives(add, pairs, string_heap, type_registry,
                                         resolve_module_path, ev);
    register_query_reflect_primitives(add, pairs, string_heap, type_registry, resolve_module_path,
                                      ev);
    register_query_lifecycle_primitives(add, pairs, string_heap, type_registry, resolve_module_path,
                                        ev);
    register_query_tail_primitives(add, pairs, string_heap, type_registry, resolve_module_path, ev);
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
bool validate_code_against_schema_simple(const std::string& code, const std::string& type_name,
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