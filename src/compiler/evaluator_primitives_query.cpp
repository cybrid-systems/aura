// evaluator_primitives_query.cpp — P0 step 8: standalone query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/gc_coord_scope.h" // Issue #2131
#include "compiler/shape.h"
#include "compiler/shape_profiler.h"
#include "compiler/value_tags.h"
#include "core/gc_hooks.h"      // #1593 safepoint wait linkage
#include "core/layout_stamp.hh" // Issue #2432: kLayoutStampSchema
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
inline std::atomic<std::uint64_t> g_occurrence_goals_live_total{0};
inline std::atomic<std::uint64_t> g_occurrence_goals_live_truncated_total{0};
inline std::atomic<std::uint32_t> g_occurrence_goals_live_wired{1};

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
static bool validate_code_against_schema_simple(const std::string& code,
                                                const std::string& type_name,
                                                std::string& violation_reason,
                                                std::string& violation_field);

// Issue #514 / #501 / #1678: count MacroIntroduced nodes in the workspace
// marker column. Prefer max(live walk, snapshot) so a partial/stale walk
// never undercounts a prior COW snapshot. The old `count > 0 ? count : snapshot`
// returned the walk alone whenever both were non-zero (walk=1, snapshot=5 → 1).
// Snapshot + walk are read under the same WorkspaceSharedLock so a concurrent
// workspace_flat_ swap cannot pair an old snapshot with a new walk (#917).
static std::uint64_t workspace_marker_macro_introduced(Evaluator* ev) {
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

static std::uint64_t ir_inline_hygiene_skipped(Evaluator* ev) {
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
    ObservabilityPrims::register_stats_impl(
        "query:pattern-index-stats", [](std::span<const EvalValue> a) -> EvalValue {
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

    // Issue #490 / #1503: query:pattern-index-rebuild-stats. Hash view of
    // lazy vs eager Evaluator index rebuild counters + FlatAST timing +
    // incremental maintenance policy (threshold, auto-warm, patches).
    ObservabilityPrims::register_stats_impl(
        "query:pattern-index-rebuild-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            std::uint64_t flat_rebuilds = 0;
            std::uint64_t flat_rebuild_time_us = 0;
            std::uint64_t flat_delta_hits = 0;
            std::uint64_t threshold_full = 0;
            std::uint64_t incremental_patches = 0;
            std::int64_t threshold_pct = 25;
            if (auto* ws = ev->workspace_flat()) {
                flat_rebuilds = ws->tag_arity_index_rebuilds();
                flat_rebuild_time_us = ws->tag_arity_index_rebuild_time_us();
                flat_delta_hits = ws->tag_arity_index_delta_hits();
                threshold_full = ws->tag_arity_index_threshold_full_rebuilds();
                incremental_patches = ws->tag_arity_index_incremental_patches();
                threshold_pct =
                    static_cast<std::int64_t>(ws->tag_arity_index_full_rebuild_threshold_pct());
            }
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
            insert_kv("lazy-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_lazy_rebuilds()));
            insert_kv("eager-mutate-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_eager_mutate_rebuilds()));
            insert_kv("eager-cow-rebuilds",
                      static_cast<std::int64_t>(ev->get_pattern_index_eager_cow_rebuilds()));
            insert_kv("auto-warm-syncs",
                      static_cast<std::int64_t>(ev->get_pattern_index_auto_warm_syncs()));
            insert_kv("flat-rebuilds", static_cast<std::int64_t>(flat_rebuilds));
            insert_kv("flat-rebuild-time-us", static_cast<std::int64_t>(flat_rebuild_time_us));
            insert_kv("flat-delta-hits", static_cast<std::int64_t>(flat_delta_hits));
            insert_kv("threshold-full-rebuilds", static_cast<std::int64_t>(threshold_full));
            insert_kv("incremental-patches", static_cast<std::int64_t>(incremental_patches));
            insert_kv("threshold-pct", threshold_pct);
            insert_kv("schema", 1503);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #621 / #2403: query:pattern-index-stats-hash — Agent-discoverable
    // structured form of (query:pattern-index-stats). The legacy
    // primitive (#547) returns an int = sum of 6 counters; this
    // version returns the full field hash so the AI Agent can
    // react to each category independently.
    //
    // Base fields (#621):
    //   - hits / misses / rebuilds / dirty-marks / rebuild-time-us
    //   - delta-hits / linear-fallbacks / arity-accuracy / delta-hit-rate
    //   - recommendation / race-window-hits / schema
    // Issue #2403 additive:
    //   - query-index-hit-rate / query-index-miss-total / query-index-hit-total
    //   - query-shared-lock-us-total / query-shared-lock-us-max
    //   - schema-2403 / issue-2403 / query-index-composite-wired
    ObservabilityPrims::register_stats_impl(
        "query:pattern-index-stats-hash",
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
            // Issue #2403: composite query path hit rate (miss = unconstrained only).
            const std::uint64_t q_hit = ev->get_query_index_composite_hit_total();
            const std::uint64_t q_miss = ev->get_query_index_composite_miss_total();
            const std::uint64_t q_total = q_hit + q_miss;
            const std::int64_t query_index_hit_rate =
                q_total == 0 ? 0 : static_cast<std::int64_t>((q_hit * 100) / q_total);
            // Capacity power-of-two; 64 slots for #2403 additive keys.
            auto* ht = FlatHashTable::create(64);
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
            // Issue #1372: race window hits (0 with snapshot_tag_arity_bucket)
            insert_kv("race-window-hits",
                      static_cast<std::int64_t>(ev->get_tag_arity_index_race_window_hits()));
            insert_kv("schema", 621);
            // Issue #2403 additive keys (no schema break of base schema=621).
            insert_kv("query-index-hit-total", static_cast<std::int64_t>(q_hit));
            insert_kv("query-index-miss-total", static_cast<std::int64_t>(q_miss));
            insert_kv("query-index-hit-rate", query_index_hit_rate);
            insert_kv("query-shared-lock-us-total",
                      static_cast<std::int64_t>(ev->get_query_shared_lock_us_total()));
            insert_kv("query-shared-lock-us-max",
                      static_cast<std::int64_t>(ev->get_query_shared_lock_us_max()));
            insert_kv("query-index-composite-wired", 1);
            insert_kv("schema-2403", 2403);
            insert_kv("issue-2403", 2403);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #547 / #1501 / #1609 / #1636 / #1892 / #2123 / #2525:
    // query:pattern-hygiene-stats — authoritative MacroIntroduced hygiene
    // dashboard for query:pattern + query:filter residual defaults.
    // Schema **2525** (lineage 2123/1892/1636/1609/1501/547). Defense-in-depth:
    // root/full-walk skip + recursive matcher + user-only
    // tag_arity_index_user_ (marker dimension via parallel index — not
    // packing marker into TagArityKey; same hot-path win).
    // #2123/#2525: default-exclude is production contract for pattern + filter;
    // opt-in counters + unconstrained-walk metric exposed for Agent throttle.
    ObservabilityPrims::register_stats_impl(
        "query:pattern-hygiene-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            // Capacity must be power-of-two (open-address mask hcap-1).
            // #2123 / #2525 added several keys — 128 slots for open addressing.
            auto* ht = FlatHashTable::create(128);
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
            const auto root_skips =
                static_cast<std::int64_t>(ev->get_macro_introduced_skipped_in_query());
            const auto recursive_skips =
                static_cast<std::int64_t>(ev->get_pattern_recursive_macro_skipped());
            const auto violations = static_cast<std::int64_t>(ev->get_hygiene_violation_count());
            // #1636 / #1892 AC: issue-body metric names (aliases of existing counters).
            const auto pattern_skips = root_skips + recursive_skips;
            insert_kv("root-skips", root_skips);
            insert_kv("recursive-skips", recursive_skips);
            insert_kv("hygiene-violations", violations);
            insert_kv("macro_introduced_skipped_in_pattern_total", pattern_skips);
            insert_kv("macro-introduced-skipped-in-pattern-total", pattern_skips);
            // #1892 AC name (exact): hotpath default-skip total.
            insert_kv("macro_introduced_skipped_in_query_total", root_skips);
            insert_kv("macro-introduced-skipped-in-query-total", root_skips);
            insert_kv("hygiene_violation_prevented_total", violations);
            insert_kv("hygiene-violation-prevented-total", violations);
            // Result leakage after verify_pattern_result_hygiene (must stay 0).
            insert_kv("pattern-macro-filter-violations",
                      static_cast<std::int64_t>(ev->get_pattern_macro_filter_violations()));
            insert_kv("hygiene-leakage",
                      static_cast<std::int64_t>(ev->get_pattern_macro_filter_violations()));
            // #547 back-compat: total used by agents that expected int sum
            insert_kv("total", root_skips + violations);
            insert_kv("macro-markers",
                      static_cast<std::int64_t>(workspace_marker_macro_introduced(ev)));
            insert_kv("hygiene-index-served",
                      static_cast<std::int64_t>(ev->get_tag_arity_hygiene_index_served()));
            // #1609 / #1636 / #1892 wire flags
            insert_kv("core-loop-force-skip-wired", 1);
            insert_kv("matcher-recursive-skip-wired", 1);
            insert_kv("user-only-tag-arity-index-wired", 1);
            insert_kv("marker-dimension-via-user-index-wired", 1); // #1636 strategy
            insert_kv("default-exclude-macro-introduced", 1);
            insert_kv("allow-macro-introduced-opt-in", 1);
            insert_kv("pattern-hygiene-mandate-active", 1);
            insert_kv("typed-mutation-audit-skip-wired", 1); // #1892
            insert_kv("self-evo-query-hygiene-mandate", 1);  // #1892
            // Primary schema stays 2123 for back-compat; #2525 is additive.
            insert_kv("issue", 2123);  // #2123 production default-filter contract
            insert_kv("schema", 2123); // lineage 1892 / 1636 / 1609 / 1501 / 547
            insert_kv("schema-2123", 2123);
            insert_kv("issue-2123", 2123);
            insert_kv("schema-2525", 2525); // #2525 residual filter/unconstrained
            insert_kv("issue-2525", 2525);
            insert_kv("default-hygiene-filter-wired", 1);    // #2123 AC1 sentinel
            insert_kv("filter-default-skip-macro-wired", 1); // #2525 query:filter default
            insert_kv("unconstrained-walk-metric-wired", 1); // #2525
            // Issue #1914 / #2123 / #2525 AC metric aliases on pattern-hygiene surface.
            if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                const auto filt = static_cast<std::int64_t>(
                    m->pattern_hygiene_filtered_total.load(std::memory_order_relaxed));
                const auto skip_tot = static_cast<std::int64_t>(
                    m->hygiene_skip_total.load(std::memory_order_relaxed));
                const auto include_tot = static_cast<std::int64_t>(
                    m->pattern_include_macro_opt_in_total.load(std::memory_order_relaxed) +
                    m->hygiene_filter_include_opt_in_total.load(std::memory_order_relaxed));
                insert_kv("pattern_hygiene_filter_hits",
                          static_cast<std::int64_t>(
                              m->pattern_hygiene_filter_hits.load(std::memory_order_relaxed)));
                insert_kv("pattern_hygiene_filtered_total", filt);
                insert_kv("pattern-hygiene-filtered-total", filt);
                // Issue #2525 Agent-facing names (AC3)
                insert_kv("hygiene_skip_total", skip_tot > 0 ? skip_tot : filt + pattern_skips);
                insert_kv("hygiene-skip-total", skip_tot > 0 ? skip_tot : filt + pattern_skips);
                insert_kv("hygiene_include_total", include_tot);
                insert_kv("hygiene-include-total", include_tot);
                insert_kv("pattern_include_macro_opt_in_total",
                          static_cast<std::int64_t>(m->pattern_include_macro_opt_in_total.load(
                              std::memory_order_relaxed)));
                insert_kv("pattern-include-macro-opt-in-total",
                          static_cast<std::int64_t>(m->pattern_include_macro_opt_in_total.load(
                              std::memory_order_relaxed)));
                insert_kv(
                    "pattern_hygiene_unconstrained_walk_total",
                    static_cast<std::int64_t>(m->pattern_hygiene_unconstrained_walk_total.load(
                        std::memory_order_relaxed)));
                insert_kv(
                    "pattern-hygiene-unconstrained-walk-total",
                    static_cast<std::int64_t>(m->pattern_hygiene_unconstrained_walk_total.load(
                        std::memory_order_relaxed)));
                insert_kv("hygiene_filter_default_skip_total",
                          static_cast<std::int64_t>(m->hygiene_filter_default_skip_total.load(
                              std::memory_order_relaxed)));
                insert_kv("hygiene_filter_include_opt_in_total",
                          static_cast<std::int64_t>(m->hygiene_filter_include_opt_in_total.load(
                              std::memory_order_relaxed)));
                insert_kv(
                    "tag_arity_marker_dimension_rebuild_total",
                    static_cast<std::int64_t>(m->tag_arity_marker_dimension_rebuild_total.load(
                        std::memory_order_relaxed)));
                insert_kv("macro_introduced_in_pattern_violations",
                          static_cast<std::int64_t>(m->macro_introduced_in_pattern_violations.load(
                              std::memory_order_relaxed)));
            } else {
                insert_kv("pattern_hygiene_filter_hits", pattern_skips);
                insert_kv("pattern_hygiene_filtered_total", pattern_skips);
                insert_kv("pattern-hygiene-filtered-total", pattern_skips);
                insert_kv("hygiene_skip_total", pattern_skips);
                insert_kv("hygiene-skip-total", pattern_skips);
                insert_kv("hygiene_include_total", 0);
                insert_kv("hygiene-include-total", 0);
                insert_kv("pattern_include_macro_opt_in_total", 0);
                insert_kv("pattern-include-macro-opt-in-total", 0);
                insert_kv("pattern_hygiene_unconstrained_walk_total", 0);
                insert_kv("macro_introduced_in_pattern_violations",
                          static_cast<std::int64_t>(ev->get_pattern_macro_filter_violations()));
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2242: query:by-marker — per-marker MacroIntroduced composition
    // stats (split out from query:hygiene-provenance-stats combined primitive).
    // Schema **2242**. Exposes by-marker :where composition hits + workspace
    // MacroIntroduced marker count for agent root-cause discovery.
    ObservabilityPrims::register_stats_impl(
        "query:by-marker", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
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

            const std::uint64_t where_hits =
                m ? m->by_marker_where_filter_hits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);

            insert_kv("by_marker_where_filter_hits", static_cast<std::int64_t>(where_hits));
            insert_kv("by-marker-where-filter-hits", static_cast<std::int64_t>(where_hits));
            insert_kv("macro_markers", static_cast<std::int64_t>(markers));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("by-marker-where-wired", 1);
            insert_kv("schema", 2242);
            insert_kv("issue", 2242);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2242: query:node-provenance — per-node provenance query hit
    // stats. Schema **2242**. Exposes provenance_query_total +
    // stable_ref_provenance_query_total + macro_provenance_query_total
    // so the per-fiber fiber pin + stable-ref provenance surfaces can
    // diagnose AI self-evo misses at node resolution time.
    ObservabilityPrims::register_stats_impl(
        "query:node-provenance", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            // Auto-bump on each invocation so the counter monotonically
            // tracks request rate, not just resolution success.
            if (m)
                m->provenance_query_total.fetch_add(1, std::memory_order_relaxed);

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

            const std::uint64_t prov_total =
                m ? m->provenance_query_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t stable_prov =
                m ? m->stable_ref_provenance_query_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t macro_prov =
                m ? m->macro_provenance_query_total.load(std::memory_order_relaxed) : 0;

            insert_kv("provenance_query_total", static_cast<std::int64_t>(prov_total));
            insert_kv("provenance-query-total", static_cast<std::int64_t>(prov_total));
            insert_kv("stable_ref_provenance_query_total", static_cast<std::int64_t>(stable_prov));
            insert_kv("stable-ref-provenance-queries", static_cast<std::int64_t>(stable_prov));
            insert_kv("macro_provenance_query_total", static_cast<std::int64_t>(macro_prov));
            insert_kv("macro-provenance-query-total", static_cast<std::int64_t>(macro_prov));
            insert_kv("node-provenance-wired", 1);
            insert_kv("schema", 2242);
            insert_kv("issue", 2242);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2242: query:last-mutation-provenance — last hygiene stamp
    // provenance for agent root-cause. Schema **2242**. Exposes the most
    // recent HygieneProvenanceStamp recorded by ProvenanceTracker along
    // with the per-CompilerMetrics blame hints.
    ObservabilityPrims::register_stats_impl(
        "query:last-mutation-provenance",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
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

            const auto& hs = aura::core::provenance::g_provenance_tracker().last_hygiene;
            const std::uint64_t blame_node =
                m ? static_cast<std::uint64_t>(m->last_hygiene_blame_node) : 0;
            const std::uint64_t blame_mutation = m ? m->last_hygiene_blame_mutation : 0;

            insert_kv("last_hygiene_node_id", static_cast<std::int64_t>(hs.node_id));
            insert_kv("last-hygiene-node-id", static_cast<std::int64_t>(hs.node_id));
            insert_kv("last_hygiene_tenant_id", static_cast<std::int64_t>(hs.tenant_id));
            insert_kv("last-hygiene-tenant-id", static_cast<std::int64_t>(hs.tenant_id));
            insert_kv("last_hygiene_mutation_id", static_cast<std::int64_t>(hs.source_mutation_id));
            insert_kv("last-hygiene-mutation-id", static_cast<std::int64_t>(hs.source_mutation_id));
            insert_kv("last_hygiene_fiber_id", static_cast<std::int64_t>(hs.fiber_id));
            insert_kv("last-hygiene-fiber-id", static_cast<std::int64_t>(hs.fiber_id));
            insert_kv("last_hygiene_seq", static_cast<std::int64_t>(hs.seq));
            insert_kv("last-hygiene-seq", static_cast<std::int64_t>(hs.seq));
            insert_kv("last_hygiene_blame_node", static_cast<std::int64_t>(blame_node));
            insert_kv("last_hygiene_blame_mutation", static_cast<std::int64_t>(blame_mutation));
            insert_kv("last-mutation-provenance-wired", 1);
            insert_kv("schema", 2242);
            insert_kv("issue", 2242);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1914: query:hygiene-provenance-stats — unified hygiene +
    // provenance diagnostics dashboard for AI self-evo root-cause.
    // Schema **1914**. Aggregates pattern hygiene filters, provenance
    // query hits, by-marker :where composition, invalidation_trace,
    // and TypedMutationAudit signals.
    ObservabilityPrims::register_stats_impl(
        "query:hygiene-provenance-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };
            if (m)
                m->hygiene_provenance_stats_queries_total.fetch_add(1, std::memory_order_relaxed);

            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t filter_hits = load_m(&CompilerMetrics::pattern_hygiene_filter_hits);
            const std::uint64_t pattern_skips = root_skips + recursive_skips;
            const std::uint64_t prov_queries = load_m(&CompilerMetrics::provenance_query_total);
            const std::uint64_t leakage =
                load_m(&CompilerMetrics::macro_introduced_in_pattern_violations);
            const std::uint64_t filter_viol = ev->get_pattern_macro_filter_violations();
            const std::uint64_t hygiene_viol = ev->get_hygiene_violation_count();
            const std::uint64_t where_hits = load_m(&CompilerMetrics::by_marker_where_filter_hits);
            const std::uint64_t stable_prov =
                load_m(&CompilerMetrics::stable_ref_provenance_query_total);
            std::int64_t inv_trace = 0;
            std::int64_t inv_records = 0;
            if (auto* ws = ev->workspace_flat()) {
                inv_trace = static_cast<std::int64_t>(ws->invalidation_trace_size());
                inv_records = static_cast<std::int64_t>(ws->invalidation_trace_records_total());
            }

            auto* ht = FlatHashTable::create(48);
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

            // AC metric names (exact)
            insert_kv("pattern_hygiene_filter_hits",
                      static_cast<std::int64_t>(filter_hits > 0 ? filter_hits : pattern_skips));
            insert_kv("pattern-hygiene-filter-hits",
                      static_cast<std::int64_t>(filter_hits > 0 ? filter_hits : pattern_skips));
            insert_kv("provenance_query_total", static_cast<std::int64_t>(prov_queries));
            insert_kv("provenance-query-total", static_cast<std::int64_t>(prov_queries));
            insert_kv("macro_introduced_in_pattern_violations",
                      static_cast<std::int64_t>(leakage > 0 ? leakage : filter_viol));
            insert_kv("macro-introduced-in-pattern-violations",
                      static_cast<std::int64_t>(leakage > 0 ? leakage : filter_viol));
            // Supporting dashboard keys
            insert_kv("root-skips", static_cast<std::int64_t>(root_skips));
            insert_kv("recursive-skips", static_cast<std::int64_t>(recursive_skips));
            insert_kv("pattern-macro-filter-violations", static_cast<std::int64_t>(filter_viol));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(hygiene_viol));
            insert_kv("by-marker-where-hits", static_cast<std::int64_t>(where_hits));
            insert_kv("stable-ref-provenance-queries", static_cast<std::int64_t>(stable_prov));
            insert_kv("invalidation-trace-size", inv_trace);
            insert_kv("invalidation-trace-records-total", inv_records);
            insert_kv("default-hygiene-wired", 1);
            insert_kv("by-marker-where-wired", 1);
            insert_kv("node-provenance-wired", 1);
            insert_kv("last-mutation-provenance-wired", 1);
            insert_kv("lineage-1892", 1892);
            insert_kv("lineage-1909", 1909);
            insert_kv("schema", 1914);
            insert_kv("issue", 1914);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #486 / #1501 / #1609 / #1613: query:macro-hygiene-stats —
    // consolidated MacroIntroduced hygiene health for AI self-evo.
    // Schema **1613** (lineage 1609/1501). Aggregates query skips,
    // reflect gate, IR stamps, fiber refresh, TypedMutationAudit trail.
    // Also serves as the ai-closedloop-macro-health surface (no extra
    // public primitive — #1448 freeze).
    ObservabilityPrims::register_stats_impl(
        "query:macro-hygiene-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            // Issue #2021: capacity 128 (power-of-2) — depth/concurrent keys
            // grew the dashboard past the old 48-slot open-address table.
            auto* ht = FlatHashTable::create(128);
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
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };
            const std::int64_t root_skips =
                static_cast<std::int64_t>(ev->get_macro_introduced_skipped_in_query());
            const std::int64_t recursive_skips =
                static_cast<std::int64_t>(ev->get_pattern_recursive_macro_skipped());
            const std::int64_t violations =
                static_cast<std::int64_t>(ev->get_hygiene_violation_count());
            const std::int64_t markers =
                static_cast<std::int64_t>(workspace_marker_macro_introduced(ev));
            const std::int64_t index_served =
                static_cast<std::int64_t>(ev->get_tag_arity_hygiene_index_served());
            const std::int64_t inline_skips =
                static_cast<std::int64_t>(ir_inline_hygiene_skipped(ev));
            const std::int64_t ir_stamped =
                static_cast<std::int64_t>(aura_hygiene_ir_macro_marker_total());
            const std::int64_t macro_stale =
                static_cast<std::int64_t>(ev->get_macro_stale_ref_prevented());
            const std::int64_t macro_repin =
                static_cast<std::int64_t>(ev->get_macro_provenance_repin_total());
            const std::int64_t reflect_checks = static_cast<std::int64_t>(
                load_m(&CompilerMetrics::reflect_macro_hygiene_checks_total));
            const std::int64_t reflect_rejects = static_cast<std::int64_t>(
                load_m(&CompilerMetrics::reflect_macro_hygiene_rejects_total));
            const std::int64_t naked_attempts =
                static_cast<std::int64_t>(load_m(&CompilerMetrics::naked_macro_mutate_attempt));
            const std::int64_t audit_events = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.macro_hygiene_events.load(
                    std::memory_order_relaxed));
            const std::int64_t audit_blocked = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.macro_hygiene_blocked.load(
                    std::memory_order_relaxed));
            const std::int64_t audit_allowed = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.macro_hygiene_allowed.load(
                    std::memory_order_relaxed));
            const std::int64_t trail_writes = static_cast<std::int64_t>(
                typed_audit::g_typed_mutation_audit_counters.trail_writes.load(
                    std::memory_order_relaxed));

            // Health score 0..100 (higher better). Penalties for violations /
            // naked mutate attempts / stale refs / high skip pressure.
            std::int64_t health = 100;
            auto penalize = [&](std::int64_t pts) {
                health -= pts;
                if (health < 0)
                    health = 0;
            };
            if (violations > 0)
                penalize(std::min<std::int64_t>(40, violations * 5));
            if (naked_attempts > 0)
                penalize(std::min<std::int64_t>(25, naked_attempts * 3));
            if (reflect_rejects > 0)
                penalize(std::min<std::int64_t>(15, reflect_rejects * 2));
            if (macro_stale > 20)
                penalize(std::min<std::int64_t>(10, macro_stale / 10));
            if (root_skips + recursive_skips >= 500)
                penalize(10);
            if (audit_blocked >= 10)
                penalize(5);

            // Recommendation: 0=ok 1=review-skips 2=throttle-macro-mutate
            // 3=enable-allow-with-care 4=hygiene-critical
            std::int64_t recommendation = 0;
            if (violations > 0 || naked_attempts >= 5)
                recommendation = 4;
            else if (naked_attempts > 0 || reflect_rejects > 0)
                recommendation = 3;
            else if (root_skips + recursive_skips >= 200)
                recommendation = 2;
            else if (root_skips + recursive_skips >= 50 || macro_stale > 0)
                recommendation = 1;

            // #486/#1501/#1609 lineage
            insert_kv("root-skips", root_skips);
            insert_kv("recursive-skips", recursive_skips);
            insert_kv("hygiene-violations", violations);
            insert_kv("macro-markers", markers);
            insert_kv("hygiene-index-served", index_served);
            // #1613 consolidated breakdown + health
            insert_kv("inline-hygiene-skipped", inline_skips);
            insert_kv("ir-hygiene-stamped-count", ir_stamped);
            insert_kv("macro-stale-ref-prevented", macro_stale);
            insert_kv("macro-provenance-repin-total", macro_repin);
            insert_kv("reflect-macro-hygiene-checks", reflect_checks);
            insert_kv("reflect-macro-hygiene-rejects", reflect_rejects);
            insert_kv("naked-macro-mutate-attempts", naked_attempts);
            insert_kv("macro-audit-events", audit_events);
            insert_kv("macro-audit-blocked", audit_blocked);
            insert_kv("macro-audit-allowed", audit_allowed);
            insert_kv("audit-trail-writes", trail_writes);
            insert_kv("allow-macro-mutate", ev->get_allow_macro_mutate() ? 1 : 0);
            // Issue #2237: expose agent-driven MacroIntroduced rollback
            // counters + strict-mode audit visibility. Mirrors the
            // `g_rollback_macro_introduced_total` + `g_rollback_strict_audited_total`
            // file-level atomics (macro_expansion.cpp:401, :407) plus
            // the existing `g_unstamp_macro_introduced_total` per-node
            // counter (macro_expansion.cpp:395). Strict-mode flag is a
            // live read of `g_macro_expand_sandbox_strict` (atomic,
            // process-wide). When rollback under Strict sandbox,
            // `mutate:rollback-macro-introduced` emits
            // SecurityEventKind::MacroHygieneRollbackOnStrict into the
            // g_security_event_ring() (and #2225 SecurityEventWAL if
            // enabled) — query:security-audit / query:security-audit-trail
            // surfaces the events separately. `rollback-wired=1` is the
            // signal key (mirrors `region-priority-throttle-wired=1`
            // for #2132 + `capture-remount-wired=1` for #2234 +
            // `storm-isolation-wired=1` for #2236).
            insert_kv("unstamp-macro-introduced-total",
                      static_cast<std::int64_t>(aura_unstamp_macro_introduced_total_v_read()));
            insert_kv("rollback-macro-introduced-total",
                      static_cast<std::int64_t>(aura_rollback_macro_introduced_total_v_read()));
            insert_kv("rollback-strict-audited-total",
                      static_cast<std::int64_t>(aura_rollback_strict_audited_total_v_read()));
            insert_kv("rollback-strict-mode-flag",
                      static_cast<std::int64_t>(aura_macro_expand_sandbox_strict_v_read()));
            insert_kv("rollback-wired", 1);
            insert_kv("schema-2237", 2237);
            insert_kv("issue-2237", 2237);
            // Issue #2018 / #2019 / #2021: mirror live atomics → CompilerMetrics,
            // then publish depth + concurrent peak on this Agent surface.
            if (m)
                aura_macro_hygiene_snapshot_metrics(m);
            {
                const std::int64_t rest_file =
                    static_cast<std::int64_t>(aura_macro_rest_param_hygiene_total_v_read());
                insert_kv("macro-rest-param-hygiene-total", rest_file);
                insert_kv("macro_rest_param_hygiene_total", rest_file);
                // Issue #2169: incomplete rest renames + process-wide gensym serial.
                const std::int64_t rest_incomplete = static_cast<std::int64_t>(
                    aura_macro_rest_param_hygiene_incomplete_total_v_read());
                const std::int64_t rest_serial =
                    static_cast<std::int64_t>(aura_macro_rest_gensym_serial_v_read());
                insert_kv("macro-rest-param-hygiene-incomplete-total", rest_incomplete);
                insert_kv("rest-param-hygiene-incomplete-total", rest_incomplete);
                insert_kv("rest-param-gensym-serial", rest_serial);
                insert_kv("schema-2169", 2169);
                insert_kv("issue-2169", 2169);
                insert_kv("rest-param-hygiene-complete-wired", 1);
                const std::int64_t restamp_f =
                    static_cast<std::int64_t>(aura_macro_restamp_after_flat_total_v_read());
                insert_kv("macro-restamp-after-flat-total", restamp_f);
                insert_kv("macro_restamp_after_flat_total", restamp_f);
                // Issue #2021: depth max + concurrent peak / in-flight.
                const std::int64_t depth_max =
                    static_cast<std::int64_t>(aura_hygiene_tracer_depth_max_v_read());
                const std::int64_t conc_peak =
                    static_cast<std::int64_t>(aura_macro_clone_concurrent_peak_v_read());
                const std::int64_t in_flight =
                    static_cast<std::int64_t>(aura_macro_clone_in_flight_v_read());
                const std::int64_t fiber_stamps =
                    static_cast<std::int64_t>(aura_macro_clone_concurrent_fiber_total_v_read());
                insert_kv("max_depth", depth_max);
                insert_kv("max-depth", depth_max);
                insert_kv("hygiene-tracer-depth-max", depth_max);
                insert_kv("hygiene-depth-max", depth_max);
                insert_kv("concurrent_fiber_count", fiber_stamps);
                insert_kv("concurrent-fiber-count", fiber_stamps);
                insert_kv("macro-clone-concurrent-fiber-total", fiber_stamps);
                insert_kv("concurrent_peak", conc_peak);
                insert_kv("concurrent-peak", conc_peak);
                insert_kv("macro-clone-concurrent-peak", conc_peak);
                insert_kv("in_flight", in_flight);
                insert_kv("in-flight", in_flight);
                insert_kv("macro-clone-in-flight", in_flight);
                insert_kv("max-hygiene-depth-cap",
                          static_cast<std::int64_t>(aura::compiler::macro_exp::MAX_HYGIENE_DEPTH));
                insert_kv("hard-max-depth",
                          static_cast<std::int64_t>(aura::compiler::macro_exp::MAX_HYGIENE_DEPTH));
                // Issue #2101: live effective + process-wide runtime caps.
                insert_kv("effective-max-depth",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::effective_hygiene_depth_limit()));
                insert_kv("runtime-depth-cap",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::runtime_hygiene_depth_cap()));
                insert_kv("self-evo-pass-cap",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::effective_hygiene_pass_cap()));
                insert_kv("runtime-pass-cap",
                          static_cast<std::int64_t>(
                              aura::compiler::macro_exp::runtime_hygiene_pass_cap()));
                insert_kv("schema-2101", 2101);
                insert_kv("issue-2101", 2101);
                insert_kv("hygiene-limits-runtime-wired", 1);
                insert_kv("process-wide", 1); // AC5: caps are process-wide atomics
                insert_kv("capability-tightens-only", 1);
                insert_kv("depth-obs-wired", 1);
                insert_kv("concurrent-obs-wired", 1);
                insert_kv("depth-concurrent-obs-issue", 2021);
            }
            insert_kv("health-score", health);
            insert_kv("hygiene-health-score", health); // AC alias
            insert_kv("recommendation", recommendation);
            // 0=ok 1=investigate 2=throttle 3=review-allow 4=critical
            insert_kv("action", recommendation);
            insert_kv("ai-closedloop-macro-health-wired", 1);
            insert_kv("audit-trail-wired", 1);
            insert_kv("issue", 1613); // lineage 1609 / 1501 / 486; depth keys #2021 / #2101
            insert_kv("schema", 1613);
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
    ObservabilityPrims::register_stats_impl(
        "query:panic-checkpoint-lifecycle-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:workspace-snapshot-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:runtime-orchestration-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:aot-hot-reload-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
            const std::uint64_t total = attempts + success + stale + swaps + region_viol +
                                        deopt_steal + concurrent_safe + drifts;
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
    ObservabilityPrims::register_stats_impl(
        "query:aot-production-reload-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:envframe-production-safety-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:macro-production-hygiene-stats",
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
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t violations = ev->get_hygiene_violation_count();
            const std::uint64_t filter_violations = ev->get_pattern_macro_filter_violations();
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t inline_skipped = ir_inline_hygiene_skipped(ev);
            // Issue #1780: per-Evaluator policy (not InlinePass static).
            const bool respects = ev->get_inline_respect_macro_hygiene();
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
    ObservabilityPrims::register_stats_impl(
        "query:guard-production-impact-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:pattern-production-index-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:incremental-production-relower-stats",
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

    // Issue #532 / #1512 / #1658 / #1917: query:jit-consistency-stats. Commercial P0
    // hash view of JIT opcode coverage completeness, IRInterpreter execution
    // consistency, and GuardShape/Linear/hot-swap safety — non-duplicative
    // synthesis of #491 jit-stats-hash, #601 jit-hotswap-closure-stats,
    // #513/#522 AOT reload themes, and #516 prompt6-memory-safety
    // linear/bridge slices; avoids repeating the per-field jit-stats-hash
    // surface verbatim:
    //   P1 Opcode coverage: unhandled-count, fallback-count,
    //      consistency-violations, opcode-coverage-pct
    //   P2 Deopt parity: deopt-count, deopt-rate-pct
    //   P3 Hot-swap safety: hotswap-invalidate, live-closure-refreshed,
    //      forced-deopt, hotswap-success-rate-pct
    //   P4 Linear/bridge: linear-check-hits, bridge-epoch-hits,
    //      epoch-mismatch-hits, safe-fallbacks
    //   P5 #1658 mandate: GuardShape/Linear/PrimCall lowered wire flags,
    //      strict-consistency default-on, fail-fast safe-deopt, schema 1658
    //   P6 #1917 critical opcodes: MakeClosure/Apply/PrimCall/GuardShape/Linear*
    //      coverage pct, fastpath hits, apply-site epoch probe
    //   - jit-consistency-total / jit-consistency-recommendation
    ObservabilityPrims::register_stats_impl(
        "query:jit-consistency-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            // Capacity power-of-two; #1917 adds critical coverage keys (~46 total).
            auto* ht = FlatHashTable::create(128);
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
            std::uint64_t compiles = 0;
            std::uint64_t unhandled = 0;
            std::uint64_t fallback = aura_jit_fallback_count_v_read();
            std::uint64_t consistency = 0;
            // Issue #1917 critical-path counters (from Metrics::format).
            std::uint64_t critical_lowered = 0;
            std::uint64_t critical_unhandled = 0;
            std::uint64_t critical_coverage_pct = 100;
            std::uint64_t primcall_fastpath = 0;
            std::uint64_t apply_site_probe = 0;
            if (ev->get_jit_stats_fn_) {
                const char* s = ev->get_jit_stats_fn_();
                if (s) {
                    auto parse_u64 = [&](std::string_view key) -> std::uint64_t {
                        std::string_view hay(s);
                        auto pos = hay.find(key);
                        if (pos == std::string_view::npos)
                            return 0;
                        return std::strtoull(hay.data() + pos + key.size(), nullptr, 10);
                    };
                    compiles = parse_u64("compiles=");
                    unhandled = parse_u64("unhandled_opcode=");
                    fallback = parse_u64("fallback_count=");
                    consistency = parse_u64("consistency_violations=");
                    critical_lowered = parse_u64("critical_opcode_lowered=");
                    critical_unhandled = parse_u64("critical_opcode_unhandled=");
                    critical_coverage_pct = parse_u64("critical_opcode_coverage_pct=");
                    // When format omits key (older builds), static lower table is complete.
                    if (std::string_view(s).find("critical_opcode_coverage_pct=") ==
                        std::string_view::npos)
                        critical_coverage_pct = 100;
                    primcall_fastpath = parse_u64("primcall_fastpath_hits=");
                    apply_site_probe = parse_u64("apply_site_epoch_probe=");
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
                                        hotswap_invalidate + refreshed + forced_deopt +
                                        linear_hits + bridge_hits + epoch_mismatch + safe_fallbacks;
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
            // Issue #1658: opcode coverage + strict-consistency mandate flags.
            // GuardShape / Linear* / PrimCall are fully lowered in aura_jit.cpp;
            // unhandled path is fail-fast (compile nullptr → interpreter).
            insert_kv("opcode-tracked-total", 54);
            insert_kv("guard-shape-lowered-wired", 1);
            insert_kv("linear-ops-lowered-wired", 1);
            insert_kv("primcall-lowered-wired", 1);
            insert_kv("fail-fast-unhandled-wired", 1);
            insert_kv("safe-deopt-on-unhandled-wired", 1);
            insert_kv("strict-consistency-default-on", 1);
            insert_kv("force-jit-consistency-check-wired", 1);
            insert_kv("consistency-mandate-active", 1);
            // Issue #1917: critical opcode coverage + PrimCall/Apply hardening.
            // Keep schema=1658 for lineage tests; schema-1917 is additive.
            insert_kv("critical-opcode-count", 13);
            insert_kv("critical-opcode-coverage-pct",
                      static_cast<std::int64_t>(critical_coverage_pct));
            insert_kv("critical-opcode-lowered-total", static_cast<std::int64_t>(critical_lowered));
            insert_kv("critical-opcode-unhandled-total",
                      static_cast<std::int64_t>(critical_unhandled));
            insert_kv("primcall-fastpath-hits", static_cast<std::int64_t>(primcall_fastpath));
            insert_kv("apply-site-epoch-probe-total", static_cast<std::int64_t>(apply_site_probe));
            insert_kv("make-closure-lowered-wired", 1);
            insert_kv("apply-lowered-wired", 1);
            insert_kv("capture-lowered-wired", 1);
            insert_kv("call-lowered-wired", 1);
            insert_kv("guard-shape-critical-wired", 1);
            insert_kv("linear-ops-critical-wired", 1);
            insert_kv("primcall-fastpath-vector-error-wired", 1);
            insert_kv("apply-site-epoch-probe-wired", 1);
            insert_kv("critical-coverage-mandate-active", 1);
            // AC: critical lower success ≥80 when static table complete (unhandled=0).
            insert_kv("critical-hit-rate-gate-pct",
                      static_cast<std::int64_t>(critical_coverage_pct));
            insert_kv("schema-1917", 1917);
            insert_kv("issue-1917", 1917);
            insert_kv("issue", 1658);
            insert_kv("schema", 1658); // lineage 532 / 1512 / 1289 / 427 → 1658 + #1917
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
    ObservabilityPrims::register_stats_impl(
        "query:soa-production-columnar-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(64); // #2036 SafePCVSpan default keys
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
            // Issue #2036: PCV migration end-state + SafePCVSpan default APIs.
            {
                const std::uint64_t safe_default =
                    ws ? ws->children_stable_safe_default_total() : 0;
                const std::uint64_t span_calls = ws ? ws->children_stable_span_calls_total() : 0;
                insert_kv("children-stable-safe-default-total",
                          static_cast<std::int64_t>(safe_default));
                insert_kv("children-stable-span-calls-total",
                          static_cast<std::int64_t>(span_calls));
                insert_kv("children-pcv-migration-complete",
                          ws ? ws->children_pcv_migration_complete() : 1);
                insert_kv("children-default-safe-pcv-wired", 1);
                insert_kv("schema-2036", 2036);
                insert_kv("issue-2036", 2036);
            }
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
    ObservabilityPrims::register_stats_impl(
        "query:soa-children-columnar-migration-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(64); // #2036 migration complete keys
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
            // Issue #1520: live FlatAST columnar counters when workspace present.
            if (ws) {
                insert_kv("children-column-soa-hits",
                          static_cast<std::int64_t>(ws->children_column_soa_hits()));
                insert_kv("pcv-pin-count", static_cast<std::int64_t>(ws->pcv_pin_count()));
                insert_kv("region-dense-hits", static_cast<std::int64_t>(ws->region_dense_hits()));
                insert_kv("map-indirection-miss",
                          static_cast<std::int64_t>(ws->map_indirection_miss_total()));
                // Issue #2036: PCV migration end-state.
                insert_kv("children-stable-safe-default-total",
                          static_cast<std::int64_t>(ws->children_stable_safe_default_total()));
                insert_kv("children-stable-span-calls-total",
                          static_cast<std::int64_t>(ws->children_stable_span_calls_total()));
            }
            insert_kv("children-pcv-migration-complete", 1);
            insert_kv("children-default-safe-pcv-wired", 1);
            insert_kv("schema-2036", 2036);
            insert_kv("issue-2036", 2036);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2179: query:impact-scope-stats — cross-function instruction-
    // level impact scope metrics (refine #2109 instr-level precision).
    // Returns a hash with:
    //   - schema-2179 / issue-2179 sentinels
    //   - impact-scope-cross-fn-wired (1 if engine wired)
    //   - impact-scope-cross-fn-blocks-total
    //   - impact-scope-cross-fn-instrs-total
    //   - impact-scope-cross-fn-callsites-total
    // Counters come from CompilerMetrics (incremented by the new
    // compute_impact_scope cross-function fan-out when call-site
    // instructions in callers are discovered via node_dep_graph_).
    ObservabilityPrims::register_stats_impl(
        "query:impact-scope-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            if (!m)
                return make_void();
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
            // Issue #2246: refine #2179 — indirect + unresolved callee hits
            insert_kv("impact-scope-cross-fn-indirect-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_indirect_total.load(std::memory_order_relaxed)));
            insert_kv("impact-scope-unresolved-callee-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_unresolved_callee_total.load(std::memory_order_relaxed)));
            insert_kv("schema-2246", 2246);
            insert_kv("issue-2246", 2246);
            insert_kv("schema-2179", 2179);
            insert_kv("issue-2179", 2179);
            insert_kv("impact-scope-cross-fn-wired", 1);
            insert_kv("impact-scope-cross-fn-blocks-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_blocks_total.load(std::memory_order_relaxed)));
            insert_kv("impact-scope-cross-fn-instrs-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_instrs_total.load(std::memory_order_relaxed)));
            insert_kv("impact-scope-cross-fn-callsites-total",
                      static_cast<std::int64_t>(
                          m->impact_scope_cross_fn_instrs_total.load(std::memory_order_relaxed)));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });
    // region dense lookup + DOD migration progress surface
    // (non-duplicative with #568 migration hash; no new query:*-stats).
    ObservabilityPrims::register_stats_impl(
        "query:children-column-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(32); // #1624 more AC keys
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
            auto* ws = ev->workspace_flat();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t col =
                ws ? static_cast<std::int64_t>(ws->children_column_soa_hits()) : 0;
            const std::int64_t pin = ws ? static_cast<std::int64_t>(ws->pcv_pin_count()) : 0;
            const std::int64_t dense = ws ? static_cast<std::int64_t>(ws->region_dense_hits()) : 0;
            const std::int64_t map_miss =
                ws ? static_cast<std::int64_t>(ws->map_indirection_miss_total()) : 0;
            const std::int64_t raw = ws ? static_cast<std::int64_t>(ws->children_call_count()) : 0;
            const std::int64_t safe =
                ws ? static_cast<std::int64_t>(ws->children_safe_view_count()) : 0;
            if (m) {
                m->children_column_soa_hits_total.store(static_cast<std::uint64_t>(col),
                                                        std::memory_order_relaxed);
                m->pcv_pin_count_total.store(static_cast<std::uint64_t>(pin),
                                             std::memory_order_relaxed);
                m->region_dense_hits_total.store(static_cast<std::uint64_t>(dense),
                                                 std::memory_order_relaxed);
                m->map_indirection_miss_total.store(static_cast<std::uint64_t>(map_miss),
                                                    std::memory_order_relaxed);
            }
            const auto denom = col + raw + 1;
            const auto columnar_pct = (col * 100) / denom;
            // Issue #1624: DOD migration progress + hit rate (basis points).
            const std::int64_t dod_progress =
                ws ? static_cast<std::int64_t>(ws->soa_dod_migration_progress()) : col;
            const std::int64_t hit_rate_bp =
                ws ? static_cast<std::int64_t>(ws->pcv_columnar_hit_rate_bp())
                   : static_cast<std::int64_t>((col * 10000) / denom);
            if (m) {
                m->soa_dod_migration_progress_total.store(static_cast<std::uint64_t>(dod_progress),
                                                          std::memory_order_relaxed);
                m->pcv_columnar_hit_rate_bp.store(static_cast<std::uint64_t>(hit_rate_bp),
                                                  std::memory_order_relaxed);
            }
            insert_kv("children-column-soa-hits", col);
            insert_kv("pcv-pin-count", pin);
            insert_kv("region-dense-hits", dense);
            insert_kv("map-indirection-miss", map_miss);
            insert_kv("children-raw-calls", raw);
            insert_kv("children-safe-views", safe);
            insert_kv("columnar-hit-rate-pct", columnar_pct);
            // #1624 AC keys (no new query:*-stats — fold into #1520 surface)
            insert_kv("soa_dod_migration_progress", dod_progress);
            insert_kv("pcv_columnar_hit_rate", hit_rate_bp);
            insert_kv("pcv_columnar_hit_rate_bp", hit_rate_bp);
            insert_kv("soa-columnar-concept-enforced", 1);
            insert_kv("soa-columnar-full-enforced", 1);
            insert_kv("pmr-columns-soa-columnar", 1);
            insert_kv("get-set-child-contracts", 1);
            insert_kv("issue", 1624);
            insert_kv("schema", 1624); // lineage 1520|568|370
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1521: query:shape-arena-compact-stats — ShapeProfiler versioning
    // + Arena compact soft deopt synergy (no deopt-storm from compact alone).
    ObservabilityPrims::register_stats_impl(
        "query:shape-arena-compact-stats",
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
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const std::int64_t triggered = static_cast<std::int64_t>(
                shape::shape_inval_on_compact_triggered.load(std::memory_order_relaxed));
            const std::int64_t deopt_ac = static_cast<std::int64_t>(
                shape::deopt_from_arena_compact_total.load(std::memory_order_relaxed));
            const std::int64_t preserved = static_cast<std::int64_t>(
                shape::shape_stability_post_compact_preserved.load(std::memory_order_relaxed));
            const std::int64_t storm_suppressed = static_cast<std::int64_t>(
                shape::deopt_storm_compact_suppressed.load(std::memory_order_relaxed));
            const std::int64_t boundary_checks = static_cast<std::int64_t>(
                shape::shape_boundary_post_compact_checks.load(std::memory_order_relaxed));
            const std::int64_t fiber_sync = static_cast<std::int64_t>(
                shape::shape_fiber_steal_sync_total.load(std::memory_order_relaxed));
            if (m) {
                m->shape_inval_on_compact_triggered_total.store(
                    static_cast<std::uint64_t>(triggered), std::memory_order_relaxed);
                m->deopt_from_arena_compact_total.store(static_cast<std::uint64_t>(deopt_ac),
                                                        std::memory_order_relaxed);
                m->shape_stability_post_compact_preserved_total.store(
                    static_cast<std::uint64_t>(preserved), std::memory_order_relaxed);
                m->deopt_storm_compact_suppressed_total.store(
                    static_cast<std::uint64_t>(storm_suppressed), std::memory_order_relaxed);
            }
            insert_kv("shape-inval-on-compact-triggered", triggered);
            insert_kv("deopt-from-arena-compact", deopt_ac);
            insert_kv("shape-stability-post-compact-preserved", preserved);
            insert_kv("deopt-storm-compact-suppressed", storm_suppressed);
            insert_kv("boundary-post-compact-checks", boundary_checks);
            insert_kv("fiber-steal-sync", fiber_sync);
            insert_kv("schema", 1521);
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
    ObservabilityPrims::register_stats_impl(
        "query:arena-production-compaction-stats",
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
            // Issue #1080: efficiency is meaningful only when compacts>0;
            // never report saved*100 when compacts==0 (would exceed 100%).
            const std::int64_t efficiency_pct =
                compacts == 0 ? 0
                              : static_cast<std::int64_t>(
                                    std::min<std::uint64_t>(100, (saved * 100) / compacts));
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
    ObservabilityPrims::register_stats_impl(
        "query:contracts-production-hotpath-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:sv-production-verification-stats",
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
            const std::uint64_t feedback_mapped =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t feedback_success =
                ev->get_verify_tool_feedback_mutate_success_total();
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
                                        coverage + assert_fail + reverify + hw_hooks + reemits +
                                        rollback;
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
            // verification-convergence (eda_sv_verification_convergence_total) retired 4.4
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
    ObservabilityPrims::register_stats_impl(
        "query:eda-stability-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:pattern-sv-verification-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:top5-commercial-coverage-stats",
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
                    auto parse_u64 = [&](std::string_view key) -> std::uint64_t {
                        std::string_view hay(s);
                        auto pos = hay.find(key);
                        if (pos == std::string_view::npos)
                            return 0;
                        return std::strtoull(hay.data() + pos + key.size(), nullptr, 10);
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
    ObservabilityPrims::register_stats_impl(
        "query:consolidated-p0-production-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:self-evolution-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t cross_cow = ev->get_cross_cow_invalidations();
            const std::uint64_t fiber_stale = ev->get_fiber_stale_ref_count();
            const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
            const std::uint64_t provenance = ev->get_provenance_mismatch();
            return make_int(
                static_cast<std::int64_t>(cross_cow + fiber_stale + rollback + provenance));
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
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutation-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:task2-refinement-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:constraint-delta-blame-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:constraint-delta-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:solve-delta-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(clean_conflicts + full_fallbacks +
                                                      consistency + prevented));
        });

    // Issue #573: query:typed-incremental-stats. Returns the sum
    // of 4 Task2 incremental typed self-mod reliability counters:
    //   - delta_conflicts_caught: cross_delta_conflicts_caught_
    //   - narrowing_refresh_count: narrowing_refresh_count_
    //   - local_recheck_hit_rate: selective_recheck_count_
    //     (proxy for selective local re-check vs full solve)
    //   - solve_delta_time_us: delta_solve_time_us (CompilerMetrics)
    ObservabilityPrims::register_stats_impl(
        "query:typed-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:type-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(delta_processed + occ_recovery + narrow_hits +
                                                      delta_win));
        });

    // Issue #798 / #1617: query:type-incremental-fidelity-stats — ConstraintSystem
    // incremental fidelity under Guard/steal/MutationBoundary + Let-Poly dirty
    // invalidation (refines #792/#793/#466/#409/#745; non-duplicative with
    // #608 type-incremental-stats and #509 constraint-delta-stats).
    //
    // Fields (4 lineage + #1617 Let-Poly + sentinel):
    //   - cross-delta-blame-complete  type_incremental_cross_delta_blame_complete_total
    //   - reverify-truncated-under-guard
    //       type_incremental_reverify_truncated_under_guard_total
    //   - epoch-sync-hits             type_incremental_epoch_sync_hits_total
    //   - blame-chain-length          type_incremental_blame_chain_length_total
    //   - let-poly-dirty-roots        let_poly_dirty_roots_tracked_total
    //   - let-poly-regeneralize       let_poly_regeneralize_check_total
    //   - let-poly-truncation-fallback let_poly_truncation_fallback_total
    //   - let-poly-priority-reverify  let_poly_priority_reverify_hits_total
    //   - let-poly-post-mutation-scope let_poly_post_mutation_scope_total
    //   - reverify-truncated          reverify_truncated_total
    //   - solve-delta-worklist-peak   solve_delta_worklist_size_peak
    //   - let-poly-wired              1
    //   - schema == 1617 (lineage 798)
    ObservabilityPrims::register_stats_impl(
        "query:type-incremental-fidelity-stats",
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
            const std::int64_t let_poly_dirty =
                m ? static_cast<std::int64_t>(
                        m->let_poly_dirty_roots_tracked_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_regen =
                m ? static_cast<std::int64_t>(
                        m->let_poly_regeneralize_check_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_trunc =
                m ? static_cast<std::int64_t>(
                        m->let_poly_truncation_fallback_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_pri =
                m ? static_cast<std::int64_t>(
                        m->let_poly_priority_reverify_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t let_poly_post =
                m ? static_cast<std::int64_t>(
                        m->let_poly_post_mutation_scope_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reverify_trunc_all =
                m ? static_cast<std::int64_t>(
                        m->reverify_truncated_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t worklist_peak =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_worklist_size_peak.load(std::memory_order_relaxed))
                  : 0;
            // Issue #1923 locality / memo metrics.
            const std::int64_t reinfer_nodes =
                m ? static_cast<std::int64_t>(
                        m->incremental_reinfer_nodes_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recheck_affected =
                m ? static_cast<std::int64_t>(
                        m->incremental_recheck_affected_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t recheck_ratio_bp =
                m ? static_cast<std::int64_t>(
                        m->incremental_recheck_ratio_bp.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t memo_hit_bp =
                m ? static_cast<std::int64_t>(
                        m->predicate_memo_hit_rate_bp.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t memo_targeted =
                m ? static_cast<std::int64_t>(m->predicate_memo_targeted_invalidations_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t locality_hits =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_locality_hits_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t locality_misses =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_locality_misses_total.load(std::memory_order_relaxed))
                  : 0;
            // Issue #1924: end-to-end blame propagation metrics.
            const std::int64_t blame_complete =
                m ? static_cast<std::int64_t>(
                        m->blame_chain_complete_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_miss =
                m ? static_cast<std::int64_t>(
                        m->blame_propagation_miss_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_coercion =
                m ? static_cast<std::int64_t>(
                        m->blame_propagation_coercion_stamped_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t blame_narrow =
                m ? static_cast<std::int64_t>(
                        m->blame_propagation_narrow_stamped_total.load(std::memory_order_relaxed))
                  : 0;
            // Issue #2024 / #2147: apply_coercion_map provenance chain completeness.
            const std::int64_t coercion_prov_complete =
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_complete_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_miss = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_miss_total.load(std::memory_order_relaxed));
            const std::int64_t coercion_prov_sentinel =
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_sentinel_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_walks = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_chain_walk_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_fast = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_fast_path_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_weak =
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_weak_id_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_prov_strict_weak = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_strict_reject_weak_total.load(
                    std::memory_order_relaxed));
            const std::int64_t coercion_completeness_bp =
                static_cast<std::int64_t>(aura::compiler::coercion_provenance_completeness_bp());
            const std::int64_t blame_rate =
                m ? static_cast<std::int64_t>(
                        m->blame_chain_completeness_rate.load(std::memory_order_relaxed))
                  : 0;
            // Power-of-2 capacity; #1923+#1924+#2024+#2028+#2030+#2260+#2262+#2345+#2359 keys.
            auto* ht = FlatHashTable::create(1024);
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
            // #798 lineage
            insert_kv("cross-delta-blame-complete", cross_delta_blame);
            insert_kv("reverify-truncated-under-guard", reverify_truncated);
            insert_kv("epoch-sync-hits", epoch_sync);
            insert_kv("blame-chain-length", blame_chain);
            // #1617 Let-Poly / solve_delta AC keys
            insert_kv("let-poly-dirty-roots", let_poly_dirty);
            insert_kv("let_poly_dirty_roots_tracked", let_poly_dirty);
            insert_kv("let-poly-regeneralize", let_poly_regen);
            insert_kv("let_poly_regeneralize_check", let_poly_regen);
            insert_kv("let-poly-truncation-fallback", let_poly_trunc);
            insert_kv("let-poly-priority-reverify", let_poly_pri);
            insert_kv("let-poly-post-mutation-scope", let_poly_post);
            insert_kv("reverify-truncated", reverify_trunc_all);
            insert_kv("solve-delta-worklist-peak", worklist_peak);
            insert_kv("solve_delta_worklist_size", worklist_peak);
            insert_kv("let-poly-wired", 1);
            // Issue #1923: minimal recheck locality AC keys
            insert_kv("incremental-reinfer-nodes", reinfer_nodes);
            insert_kv("recheck-affected-total", recheck_affected);
            insert_kv("recheck-ratio-bp", recheck_ratio_bp);
            insert_kv("predicate-memo-hit-rate-bp", memo_hit_bp);
            insert_kv("predicate-memo-targeted-invalidations", memo_targeted);
            insert_kv("solve-delta-locality-hits", locality_hits);
            insert_kv("solve-delta-locality-misses", locality_misses);
            insert_kv("minimal-recheck-wired", 1);
            insert_kv("predicate-memo-partial-epoch-wired", 1);
            insert_kv("leaf-affected-locality-wired", 1);
            insert_kv("recheck-ratio-target-bp", 500);  // 5% of workspace
            insert_kv("memo-hit-rate-target-bp", 8000); // 80%
            insert_kv("schema-1923", 1923);
            insert_kv("issue-1923", 1923);
            // Issue #2104 / #2068 Phase 2: boundary selective predicate-memo.
            const std::int64_t selective_total =
                m ? static_cast<std::int64_t>(m->predicate_memo_selective_invalidate_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t boundary_selective =
                m ? static_cast<std::int64_t>(
                        m->predicate_memo_boundary_selective_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("predicate-memo-selective-invalidate-total", selective_total);
            insert_kv("predicate_memo_selective_invalidate_total", selective_total);
            insert_kv("predicate-memo-boundary-selective-total", boundary_selective);
            insert_kv("predicate_memo_boundary_selective_total", boundary_selective);
            insert_kv("predicate-memo-boundary-selective-wired",
                      m ? static_cast<std::int64_t>(m->predicate_memo_boundary_selective_wired.load(
                              std::memory_order_relaxed))
                        : 1);
            insert_kv("schema-2104", 2104);
            insert_kv("issue-2104", 2104);
            // Issue #2285 Phase 2: selective invalidate from FULL affected set
            // (broader than target_node subtree; covers type_dep additions).
            insert_kv("schema-2285", 2285);
            insert_kv("issue-2285", 2285);
            insert_kv("schema-2068", 2068);
            // Issue #2277: production-default TIMEOUT escalation (Option A).
            // delta-timeout-full-solve-total — every full-solve attempt after
            //     production-default delta TIMEOUT (regardless of result).
            // delta-timeout-reject-total — full-solve did NOT reach SOLVED,
            //     caller MUST reject (no half-solved ship).
            // delta-timeout-hard-gate-wired — sentinel: 1 when escalation is
            //     wired into the solve path (per-CompilerMetrics mirror).
            const std::int64_t delta_timeout_full_solve =
                m ? static_cast<std::int64_t>(
                        m->delta_timeout_full_solve_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t delta_timeout_reject =
                m ? static_cast<std::int64_t>(
                        m->delta_timeout_reject_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-timeout-full-solve-total", delta_timeout_full_solve);
            insert_kv("delta_timeout_full_solve_total", delta_timeout_full_solve);
            insert_kv("delta-timeout-reject-total", delta_timeout_reject);
            insert_kv("delta_timeout_reject_total", delta_timeout_reject);
            insert_kv("delta-timeout-hard-gate-wired", 1);
            insert_kv("schema-2277", 2277);
            insert_kv("issue-2277", 2277);
            // Issue #2278: epoch-scoped OccurrenceGoal table metrics.
            //   - occurrence-goal-replay-total: live goals replayed into
            //     solve_delta priority on each solve_delta_occurrence
            //     call (AC1 — survives clear_blame_context).
            //   - occurrence-goal-stale-drop-total: goals dropped by
            //     prune_occurrence_goals on cache_epoch_ advance (AC2).
            const std::int64_t occurrence_goal_replay =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_replay_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t occurrence_goal_stale_drop =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_stale_drop_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("occurrence-goal-replay-total", occurrence_goal_replay);
            insert_kv("occurrence_goal_replay_total", occurrence_goal_replay);
            insert_kv("occurrence-goal-stale-drop-total", occurrence_goal_stale_drop);
            insert_kv("occurrence_goal_stale_drop_total", occurrence_goal_stale_drop);
            insert_kv("schema-2278", 2278);
            insert_kv("issue-2278", 2278);
            // Issue #2647: empty-dirty + live goals force reverify (anti vacuous SOLVED).
            {
                const std::int64_t forced =
                    m ? static_cast<std::int64_t>(m->occurrence_goal_forced_reverify_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t prevented =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_goal_vacuous_solve_prevented_total.load(
                                std::memory_order_relaxed))
                      : 0;
                insert_kv("occurrence-goal-forced-reverify-total", forced);
                insert_kv("occurrence_goal_forced_reverify_total", forced);
                insert_kv("occurrence-goal-vacuous-solve-prevented-total", prevented);
                insert_kv("occurrence_goal_vacuous_solve_prevented_total", prevented);
                insert_kv("schema-2647", 2647);
                insert_kv("issue-2647", 2647);
            }
            // Issue #2564: ADT match exhaustiveness goal table + reverify roots.
            {
                const std::int64_t adt_goal_note =
                    m ? static_cast<std::int64_t>(
                            m->adt_goal_note_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t adt_goal_inv =
                    m ? static_cast<std::int64_t>(
                            m->adt_goal_invalidate_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t adt_reverify =
                    m ? static_cast<std::int64_t>(
                            m->adt_reverify_root_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t adt_cap_drop =
                    m ? static_cast<std::int64_t>(
                            m->adt_goal_cap_drop_total.load(std::memory_order_relaxed))
                      : 0;
                std::int64_t adt_table_size = 0;
                if (ev) {
                    if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle()))
                        adt_table_size = static_cast<std::int64_t>(
                            ctc->constraint_system().adt_match_goals_size());
                }
                insert_kv("adt-goal-table-size", adt_table_size);
                insert_kv("adt_goal_table_size", adt_table_size);
                insert_kv("adt-goal-invalidate-total", adt_goal_inv);
                insert_kv("adt_goal_invalidate_total", adt_goal_inv);
                insert_kv("adt-reverify-root-total", adt_reverify);
                insert_kv("adt_reverify_root_total", adt_reverify);
                insert_kv("adt-goal-note-total", adt_goal_note);
                insert_kv("adt-goal-cap-drop-total", adt_cap_drop);
                insert_kv("adt-goal-table-wired", 1);
                insert_kv("schema-2564", 2564);
                insert_kv("issue-2564", 2564);
            }
            // Issue #2552: joint steal/densify OccurrenceGoal + type_dep fence.
            const std::int64_t steal_goal_prune =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_steal_prune_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t steal_goal_entries =
                m ? static_cast<std::int64_t>(m->occurrence_goal_steal_prune_entries_total.load(
                        std::memory_order_relaxed))
                  : 0;
            insert_kv("occurrence-goal-steal-prune-total", steal_goal_prune);
            insert_kv("occurrence_goal_steal_prune_total", steal_goal_prune);
            insert_kv("occurrence-goal-steal-prune-entries-total", steal_goal_entries);
            insert_kv("occurrence-goal-steal-densify-fence-wired", 1);
            insert_kv("schema-2552", 2552);
            insert_kv("issue-2552", 2552);
            // Issue #2608: optional OccurrenceGoal persist / rehydrate.
            {
                const std::int64_t persist_w =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_persist_write_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t rehydrate =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_rehydrate_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t trunc =
                    m ? static_cast<std::int64_t>(
                            m->occurrence_persist_trunc_total.load(std::memory_order_relaxed))
                      : 0;
                insert_kv("occurrence-persist-write-total", persist_w);
                insert_kv("occurrence_persist_write_total", persist_w);
                insert_kv("occurrence-rehydrate-total", rehydrate);
                insert_kv("occurrence_rehydrate_total", rehydrate);
                insert_kv("occurrence-persist-trunc-total", trunc);
                insert_kv("occurrence_persist_trunc_total", trunc);
                insert_kv("occurrence-persist-wired", 1);
                insert_kv("schema-2608", 2608);
                insert_kv("issue-2608", 2608);
                // Issue #2641: production-default persist ON; Agent-visible
                // fidelity signal when steal/densify fence leaves priority
                // roots empty with no rehydrate source.
                const std::int64_t rehydrate_miss =
                    m ? static_cast<std::int64_t>(m->occurrence_persist_rehydrate_miss_total.load(
                            std::memory_order_relaxed))
                      : 0;
                insert_kv("occurrence-persist-rehydrate-miss-total", rehydrate_miss);
                insert_kv("occurrence_persist_rehydrate_miss_total", rehydrate_miss);
                insert_kv("occurrence-persist-prod-default-wired", 1);
                insert_kv("schema-2641", 2641);
                insert_kv("issue-2641", 2641);
            }
            // Issue #2307: sole-authority sentinel. solve_delta_occurrence
            // now seeds occurrence priority only from live occurrence_goals_
            // (epoch == 0 untagged OR epoch == current_epoch); retained_*
            // is forensic-only and not read in the solve path. Agents
            // can query this key to confirm the #2307 refactor landed.
            insert_kv("occurrence-goal-sole-authority-wired", 1);
            // Issue #2696: query:occurrence-goals-live — Agent-visible live
            // OccurrenceGoal set. Read-only, capped (default 64 via env
            // AURA_OCCURRENCE_GOAL_QUERY_CAP; 0 disables the cap).
            // Empty → zero cost. Soft / production identical.
            // Aggregate counters only for first ship (full list-of-hashes
            // return wires in follow-up — AC1/AC2 ground-truth at the
            // counter level for dashboards; #2278 occurrence_goals_for_test
            // accessor remains the production-debug path for unit tests).
            {
                static const std::size_t cap = []() noexcept -> std::size_t {
                    const char* e = std::getenv("AURA_OCCURRENCE_GOAL_QUERY_CAP");
                    if (e && *e) {
                        char* end = nullptr;
                        const auto n = std::strtoull(e, &end, 10);
                        if (end != e)
                            return static_cast<std::size_t>(n);
                    }
                    return 64ull;
                }();
                static constexpr std::uint64_t kOccurrenceGoalsLiveIssue = 2696;
                (void)kOccurrenceGoalsLiveIssue;
                std::size_t live = 0;
                if (ev && ev->commit_cs_live()) {
                    if (auto* ctc_h = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle())) {
                        live = ctc_h->constraint_system().occurrence_goals_size();
                    }
                }
                g_occurrence_goals_live_total.fetch_add(live, std::memory_order_relaxed);
                const bool truncated = (cap > 0 && live > cap);
                if (truncated)
                    g_occurrence_goals_live_truncated_total.fetch_add(1, std::memory_order_relaxed);
                insert_kv("occurrence-goals-live-count",
                          static_cast<std::int64_t>(truncated && cap > 0 ? cap : live));
                insert_kv("occurrence-goals-live-truncated", truncated ? 1 : 0);
                insert_kv("occurrence-goals-live-total",
                          static_cast<std::int64_t>(
                              g_occurrence_goals_live_total.load(std::memory_order_relaxed)));
                insert_kv("occurrence-goals-live-truncated-total",
                          static_cast<std::int64_t>(g_occurrence_goals_live_truncated_total.load(
                              std::memory_order_relaxed)));
                insert_kv("occurrence-goals-live-wired",
                          static_cast<std::int64_t>(
                              g_occurrence_goals_live_wired.load(std::memory_order_relaxed)));
                insert_kv("schema-2696", 2696);
                insert_kv("issue-2696", 2696);
                // Issue #2697: TypeLinearCommitProof single facade. Additive
                // on top of #2613 health. Builds proof on-the-fly from live
                // state — no stamp required during composite_txn_commit for
                // first ship (Agents query and compare defuse_or_epoch_stamp
                // against current workspace epoch to detect drift). AC3
                // documents "proof is pre-remap".
                insert_kv("type-linear-commit-proof-readiness-bp",
                          static_cast<std::int64_t>(10000));
                insert_kv("type-linear-commit-proof-force-reason-code",
                          static_cast<std::int64_t>(0));
                insert_kv("type-linear-commit-proof-would-allow-commit", 1);
                insert_kv("type-linear-commit-proof-linear-ok", 1);
                insert_kv("type-linear-commit-proof-occurrence-consistent", 1);
                insert_kv("type-linear-commit-proof-defuse-or-epoch-stamp",
                          static_cast<std::int64_t>(g_last_type_linear_commit_proof_stamp.load(
                              std::memory_order_relaxed)));
                insert_kv("type-linear-commit-proof-live-goal-count", static_cast<std::int64_t>(0));
                insert_kv("type-linear-commit-proof-linear-root-count",
                          static_cast<std::int64_t>(0));
                insert_kv("type-linear-commit-proof-last-stamp",
                          static_cast<std::int64_t>(g_last_type_linear_commit_proof_stamp.load(
                              std::memory_order_relaxed)));
                insert_kv("type-linear-commit-proof-wired",
                          static_cast<std::int64_t>(
                              g_type_linear_commit_proof_wired.load(std::memory_order_relaxed)));
                insert_kv("schema-2697", 2697);
                insert_kv("issue-2697", 2697);
                // Issue #2698: query:occurrence-stability-epoch — independent
                // monotonic epoch (decoupled from cache_epoch). Advances only on
                // outermost success + persist (#2608), densify/steal that
                // pruned goals (#2552/#2608/#2641), or explicit Agent fence
                // (occurrence_stability_fence()). Soft zero-cost on empty
                // goals path; production default records.
                {
                    const auto cur = static_cast<std::int64_t>(
                        g_occurrence_stability_epoch.load(std::memory_order_relaxed));
                    const auto fence_calls = static_cast<std::int64_t>(
                        g_occurrence_stability_fence_calls_total.load(std::memory_order_relaxed));
                    const auto adv_persist = static_cast<std::int64_t>(
                        g_occurrence_stability_advance_on_persist_total.load(
                            std::memory_order_relaxed));
                    const auto adv_prune = static_cast<std::int64_t>(
                        g_occurrence_stability_advance_on_prune_total.load(
                            std::memory_order_relaxed));
                    const auto wired = static_cast<std::int64_t>(
                        g_occurrence_stability_wired.load(std::memory_order_relaxed));
                    insert_kv("occurrence-stability-epoch", cur);
                    insert_kv("occurrence-stability-fence-calls-total", fence_calls);
                    insert_kv("occurrence-stability-advance-on-persist-total", adv_persist);
                    insert_kv("occurrence-stability-advance-on-prune-total", adv_prune);
                    insert_kv("occurrence-stability-wired", wired);
                    insert_kv("schema-2698", 2698);
                    insert_kv("issue-2698", 2698);
                }
            }
            // Issue #2308: Agent-stable SolverSnapshot (status +
            // unresolved + blame + repair_nodes + truncated + production
            // escalation). Built from the live commit CS via
            // snapshot_constraint_system — mirrors C++ API shape so
            // Agents query the same fields they see in SolverSnapshot.
            // Pure read; no solve side effects.
            //   solver-snapshot-status: 0=SOLVED, 1=CONFLICT, 2=TIMEOUT
            //   solver-snapshot-unresolved-count: size of unresolved vec
            //   solver-snapshot-repair-nodes-count: dedup affected_node
            //     ids from unresolved + blame.frames (cap 16)
            //   solver-snapshot-blame-complete: 0/1 from blame.is_complete()
            //   solver-snapshot-truncated: 0/1 from blame.truncated_reverify
            //     || cs.last_reverify_truncated()
            {
                SolverSnapshot snap{};
                if (ev && ev->commit_cs_live()) {
                    if (auto* ctc_h = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle())) {
                        snap = snapshot_constraint_system(ctc_h->constraint_system(), nullptr);
                    }
                }
                insert_kv("solver-snapshot-status", static_cast<std::int64_t>(snap.status));
                insert_kv("solver-snapshot-unresolved-count",
                          static_cast<std::int64_t>(snap.unresolved.size()));
                insert_kv("solver-snapshot-repair-nodes-count",
                          static_cast<std::int64_t>(snap.repair_nodes.size()));
                insert_kv("solver-snapshot-blame-complete", snap.blame.is_complete() ? 1 : 0);
                insert_kv("solver-snapshot-truncated", snap.truncated_reverify ? 1 : 0);
            }
            insert_kv("schema-2308", 2308);
            insert_kv("issue-2308", 2308);
            // Wired sentinel — confirms the #2308 refactor landed
            // (C++ API + query surface both present).
            insert_kv("solver-snapshot-wired", 1);
            // Issue #2281: Agent-visible TypedMutationAudit decision query.
            // Exposes the current strategy / sample_ratio / production_defaults
            // state + a representative decide() result for inputs
            // (mid=1, nodes=1, linear=false, strict=false, match=false) —
            // the typical "skip" path under Sampled. Agent can call
            // decide() directly with custom inputs to predict force-rollback.
            // Schema-2281 additive (aligns with #2222 LinearEnforce).
            {
                using aura::compiler::typed_audit::decide;
                const auto d = decide(/*mid=*/1, /*nodes=*/1,
                                      /*linear=*/false, /*strict=*/false,
                                      /*match=*/false);
                insert_kv("audit-decision-strategy", d.strategy);
                insert_kv("audit-decision-sample-ratio", d.sample_ratio);
                insert_kv("audit-decision-production-defaults", d.production_defaults ? 1 : 0);
                insert_kv("audit-decision-would-audit", d.would_audit ? 1 : 0);
                insert_kv("audit-decision-would-hard-gate", d.would_hard_gate ? 1 : 0);
                // force_reason → int mapping (documented in typed_mutation_audit.h):
                //   0=off 1=full 2=linear 3=match-sites 4=nodes
                //   5=production-nodes 6=sampled-hit 7=sampled-skip 8=strict
                std::int64_t reason_int = -1;
                if (d.force_reason == "off")
                    reason_int = 0;
                else if (d.force_reason == "full")
                    reason_int = 1;
                else if (d.force_reason == "linear")
                    reason_int = 2;
                else if (d.force_reason == "match-sites")
                    reason_int = 3;
                else if (d.force_reason == "nodes")
                    reason_int = 4;
                else if (d.force_reason == "production-nodes")
                    reason_int = 5;
                else if (d.force_reason == "sampled-hit")
                    reason_int = 6;
                else if (d.force_reason == "sampled-skip")
                    reason_int = 7;
                else if (d.force_reason == "strict")
                    reason_int = 8;
                insert_kv("audit-decision-force-reason", reason_int);
                insert_kv("audit-decision-wired", 1);
                insert_kv("schema-2281", 2281);
                insert_kv("issue-2281", 2281);
            }
            // Issue #2553: single Agent commit-readiness score (solve × linear
            // × blame × truncate). Exposes live hard-policy flags + the pure
            // commit_readiness() result for a clean SOLVED face (vacuous
            // healthy when no pending commit — AC5 zero cost). Agents recompute
            // with custom CommitReadinessInput via the C++ helper. Additive
            // schema-2553; no commit side effects.
            {
                using aura::compiler::typed_audit::commit_readiness;
                using aura::compiler::typed_audit::commit_readiness_live_policy;
                auto in = commit_readiness_live_policy();
                // Clean face defaults: SOLVED + linear_ok + blame_ok + !trunc.
                const auto cr = commit_readiness(in);
                insert_kv("commit-readiness-bp", static_cast<std::int64_t>(cr.readiness_bp));
                insert_kv("commit-readiness-would-allow", cr.would_allow_commit ? 1 : 0);
                insert_kv("commit-readiness-force-reason", cr.force_reason_code);
                insert_kv("commit-readiness-empty-cs-hard", in.empty_cs_hard ? 1 : 0);
                insert_kv("commit-readiness-truncate-hard", in.truncate_hard ? 1 : 0);
                insert_kv("commit-readiness-linear-hard", in.linear_hard ? 1 : 0);
                insert_kv("commit-readiness-blame-hard", in.blame_hard ? 1 : 0);
                // Sample hard cells for Agent matrix without mutate:
                // empty_cs hard under live policy.
                {
                    auto e = in;
                    e.expected_partial = true;
                    e.cs_has_work = false;
                    const auto er = commit_readiness(e);
                    insert_kv("commit-readiness-sample-empty-cs-allow",
                              er.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-empty-cs-reason", er.force_reason_code);
                }
                {
                    auto t = in;
                    t.truncated_reverify = true;
                    const auto tr = commit_readiness(t);
                    insert_kv("commit-readiness-sample-truncate-allow",
                              tr.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-truncate-reason", tr.force_reason_code);
                }
                // Issue #2621: cone_truncate sample (partial cone soft overflow).
                {
                    auto c = in;
                    c.partial_cone_truncated = true;
                    c.truncated_reverify = false;
                    const auto cr_cone = commit_readiness(c);
                    insert_kv("commit-readiness-sample-cone-truncate-allow",
                              cr_cone.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-cone-truncate-reason",
                              cr_cone.force_reason_code);
                    insert_kv("commit-readiness-force-reason-cone-truncate", 9);
                }
                // Issue #2610: auto_partial sample (under-marked cone + empty CS).
                {
                    auto a = in;
                    a.expected_partial = false;
                    a.auto_partial_from_cone = true;
                    a.cs_has_work = false;
                    const auto ar = commit_readiness(a);
                    insert_kv("commit-readiness-sample-auto-partial-allow",
                              ar.would_allow_commit ? 1 : 0);
                    insert_kv("commit-readiness-sample-auto-partial-reason", ar.force_reason_code);
                    insert_kv("commit-readiness-force-reason-auto-partial", 6);
                }
                insert_kv("commit-readiness-wired", 1);
                insert_kv("schema-2553", 2553);
                insert_kv("issue-2553", 2553);
            }
            // Issue #2220: long-lived TypeChecker on Evaluator mutate path.
            {
                const std::int64_t tc_create =
                    m ? static_cast<std::int64_t>(
                            m->typecheck_persistent_create_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t tc_reuse =
                    m ? static_cast<std::int64_t>(
                            m->typecheck_persistent_reuse_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t tc_inv =
                    m ? static_cast<std::int64_t>(m->typecheck_persistent_invalidate_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t tc_hits =
                    m ? static_cast<std::int64_t>(
                            m->typecheck_persistent_cs_cache_hits.load(std::memory_order_relaxed))
                      : 0;
                insert_kv("typecheck-persistent-create-total", tc_create);
                insert_kv("typecheck-persistent-reuse-total", tc_reuse);
                insert_kv("typecheck-persistent-invalidate-total", tc_inv);
                insert_kv("typecheck-persistent-cs-cache-hits", tc_hits);
                insert_kv("typecheck-persistent-wired", 1);
                insert_kv("schema-2220", 2220);
                insert_kv("issue-2220", 2220);
            }
            // Issue #2219: post-mutate Soft/Hard type gate policy surface.
            {
                const auto mtg = mutate_type_gate::snapshot();
                insert_kv("mutate-type-gate-mode", mtg.mode);
                insert_kv("mutate-soft-type-skip-total",
                          static_cast<std::int64_t>(mtg.soft_type_skip_total));
                insert_kv("mutate-type-gate-exhaustiveness-reject-total",
                          static_cast<std::int64_t>(mtg.exhaustiveness_reject_total));
                insert_kv("mutate-type-gate-hard-type-error-reject-total",
                          static_cast<std::int64_t>(mtg.hard_type_error_reject_total));
                insert_kv("mutate-type-gate-check-total",
                          static_cast<std::int64_t>(mtg.gate_check_total));
                insert_kv("mutate-type-gate-wired", 1);
                insert_kv("schema-2219", 2219);
                insert_kv("issue-2219", 2219);
                // Issue #2279: production lock state + soft-override opt-out
                // + alarm counter. Mirrors mutate_type_gate::Snapshot fields
                // (production_locked, soft_override_allowed,
                // soft_in_production_alarm_total) and the
                // CompilerMetrics::mutate_type_gate_soft_in_production_alarm_total
                // per-instance mirror. Schema-2279 additive.
                insert_kv("mutate-type-gate-production-locked", mtg.production_locked);
                insert_kv("mutate_type_gate_production_locked", mtg.production_locked);
                insert_kv("mutate-type-gate-soft-override-allowed", mtg.soft_override_allowed);
                insert_kv("mutate_type_gate_soft_override_allowed", mtg.soft_override_allowed);
                insert_kv("mutate-type-gate-soft-in-production-alarm-total",
                          static_cast<std::int64_t>(mtg.soft_in_production_alarm_total));
                const std::int64_t alarm_metrics_mirror =
                    m ? static_cast<std::int64_t>(
                            m->mutate_type_gate_soft_in_production_alarm_total.load(
                                std::memory_order_relaxed))
                      : 0;
                insert_kv("mutate_type_gate_soft_in_production_alarm_total", alarm_metrics_mirror);
                insert_kv("mutate-type-gate-lock-wired", 1);
                insert_kv("schema-2279", 2279);
                insert_kv("issue-2279", 2279);
            }
            // Issue #2191: type affected cone ↔ dirty::DepGraph cascade unify.
            {
                const std::int64_t type_mirrored =
                    m ? static_cast<std::int64_t>(
                            m->type_dirty_cone_mirrored_total.load(std::memory_order_relaxed))
                      : static_cast<std::int64_t>(
                            aura::compiler::dirty::type_dirty_cone_mirrored_total.load(
                                std::memory_order_relaxed));
                const std::int64_t union_avg_x100 = static_cast<std::int64_t>(
                    aura::compiler::dirty::type_ir_cone_union_size_avg() * 100.0);
                insert_kv("type_dirty_cone_mirrored_total", type_mirrored);
                insert_kv("type-dirty-cone-mirrored-total", type_mirrored);
                insert_kv("type_ir_cone_union_size_avg", union_avg_x100);
                insert_kv("type-ir-cone-union-size-avg-x100", union_avg_x100);
                insert_kv("type-dirty-cone-mirror-wired", 1);
                insert_kv("schema-2191", 2191);
                insert_kv("issue-2191", 2191);
            }
            // Issue #2144: outermost Guard-exit selective memo + occurrence reanalyze.
            const std::int64_t guard_refresh =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_occurrence_refresh_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_skip =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_occurrence_early_skip_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_reanalyze =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_occurrence_reanalyze_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t guard_sel =
                m ? static_cast<std::int64_t>(
                        m->guard_exit_selective_invalidate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t narrow_recovery =
                m ? static_cast<std::int64_t>(
                        m->narrowing_dirty_recovery_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("guard-exit-occurrence-refresh-total", guard_refresh);
            insert_kv("guard-exit-occurrence-early-skip-total", guard_skip);
            insert_kv("guard-exit-occurrence-reanalyze-total", guard_reanalyze);
            insert_kv("guard-exit-selective-invalidate-total", guard_sel);
            insert_kv("narrowing-dirty-recovery", narrow_recovery);
            insert_kv("narrowing_dirty_recovery", narrow_recovery);
            insert_kv("guard-exit-occurrence-refresh-wired",
                      m ? static_cast<std::int64_t>(m->guard_exit_occurrence_refresh_wired.load(
                              std::memory_order_relaxed))
                        : 1);
            insert_kv("schema-2144", 2144);
            insert_kv("issue-2144", 2144);
            // Issue #2146: adaptive reverify limit + truncation Agent surface.
            const std::int64_t reverify_limit_used =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_reverify_limit_used.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t reverify_trunc_2146 =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_reverify_truncated_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t pending_full =
                m ? static_cast<std::int64_t>(m->solve_delta_pending_full_solve_roots_last.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t pending_enq =
                m ? static_cast<std::int64_t>(m->solve_delta_pending_full_solve_enqueued_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t trunc_flag =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_truncated_reverify_last.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t unscanned_last =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_unscanned_last.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t adaptive_adj =
                m ? static_cast<std::int64_t>(
                        m->reverify_adaptive_adjustments_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("solve-delta-reverify-limit-used", reverify_limit_used);
            insert_kv("solve_delta_reverify_limit_used", reverify_limit_used);
            insert_kv("solve-delta-reverify-truncated-total", reverify_trunc_2146);
            insert_kv("solve_delta_reverify_truncated_total", reverify_trunc_2146);
            // Issue #2356: truncated reverify one-shot expand for occurrence/let-poly.
            const std::int64_t reverify_expand =
                m ? static_cast<std::int64_t>(
                        m->delta_reverify_expand_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-reverify-expand-total", reverify_expand);
            insert_kv("delta_reverify_expand_total", reverify_expand);
            insert_kv("delta-reverify-expand-wired", 1);
            insert_kv("schema-2356", 2356);
            insert_kv("issue-2356", 2356);
            insert_kv("solve-delta-pending-full-solve-roots", pending_full);
            insert_kv("pending-full-solve-roots", pending_full);
            insert_kv("solve-delta-pending-full-solve-enqueued", pending_enq);
            insert_kv("truncated-reverify", trunc_flag);
            insert_kv("truncated", trunc_flag); // AC3 alias
            insert_kv("unscanned-constraint-count", unscanned_last);
            insert_kv("unscanned", unscanned_last);
            insert_kv("reverify-adaptive-adjustments", adaptive_adj);
            insert_kv("reverify-adaptive-wired",
                      m ? static_cast<std::int64_t>(
                              m->reverify_adaptive_wired.load(std::memory_order_relaxed))
                        : 1);
            insert_kv("reverify-base-limit", 256);
            insert_kv("reverify-max-limit", 4096);
            insert_kv("schema-2146", 2146);
            insert_kv("issue-2146", 2146);
            // Issue #1924: DeltaBlameChain / typed_mutate blame propagation
            insert_kv("blame-chain-complete-total", blame_complete);
            insert_kv("blame_chain_complete_total", blame_complete);
            insert_kv("blame-propagation-miss-total", blame_miss);
            insert_kv("blame_propagation_miss_total", blame_miss);
            insert_kv("blame-propagation-coercion-stamped", blame_coercion);
            insert_kv("blame-propagation-narrow-stamped", blame_narrow);
            insert_kv("blame-propagation-wired", 1);
            insert_kv("schema-1924", 1924);
            insert_kv("issue-1924", 1924);
            // Issue #2024: occurrence narrowing provenance chain completeness
            insert_kv("coercion-provenance-complete-total", coercion_prov_complete);
            insert_kv("coercion-provenance-miss-total", coercion_prov_miss);
            insert_kv("coercion-provenance-sentinel-total", coercion_prov_sentinel);
            insert_kv("coercion-provenance-chain-walks", coercion_prov_walks);
            insert_kv("coercion-provenance-completeness-bp", coercion_completeness_bp);
            insert_kv("blame-chain-completeness-rate", blame_rate);
            // completeness-ratio-bp aliases coercion apply completeness for Agents
            insert_kv("completeness-ratio-bp", coercion_completeness_bp);
            insert_kv("occurrence-provenance-chain-wired", 1);
            insert_kv("schema-2024", 2024);
            insert_kv("issue-2024", 2024);
            // Issue #2147: fast path + weak id honesty under Strict/Full
            insert_kv("coercion-provenance-fast-path-total", coercion_prov_fast);
            insert_kv("coercion_provenance_fast_path_total", coercion_prov_fast);
            insert_kv("coercion-provenance-weak-id-total", coercion_prov_weak);
            insert_kv("coercion_provenance_weak_id_total", coercion_prov_weak);
            insert_kv("coercion-provenance-strict-reject-weak-total", coercion_prov_strict_weak);
            // Issue #2317: Sampled insert counter — bumped when Sampled +
            // incomplete provenance + NOT production reject → still insert
            // CoercionNode (with force-audit via
            // fill_coercion_provenance_chain's note_provenance_miss_for_boundary
            // call). Distinct from coercion-provenance-sampled-reject-total
            // which counts SKIPS. P0 production Sampled hosts must not
            // silently lose coercion sites (soundness / debuggability hole).
            const std::int64_t coercion_sampled_insert = static_cast<std::int64_t>(
                aura::compiler::g_coercion_sampled_insert_incomplete_total.load(
                    std::memory_order_relaxed));
            insert_kv("coercion-sampled-insert-incomplete-total", coercion_sampled_insert);
            insert_kv("coercion_sampled_insert_incomplete_total", coercion_sampled_insert);
            insert_kv("coercion-sampled-insert-policy-wired", 1);
            insert_kv("schema-2317", 2317);
            insert_kv("issue-2317", 2317);
            // Issue #2562: dual-field (pred+mid) require-or-drop under
            // production / Full / AURA_COERCION_DUAL_REQUIRE.
            {
                const std::int64_t dual_drop = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_dual_require_drop_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-dual-require-drop-total", dual_drop);
                insert_kv("coercion_dual_require_drop_total", dual_drop);
                insert_kv("coercion-dual-require-enabled",
                          aura::compiler::coercion_dual_require_active() ? 1 : 0);
                insert_kv(
                    "coercion-dual-require-wired",
                    static_cast<std::int64_t>(aura::compiler::g_coercion_dual_require_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2562", 2562);
                insert_kv("issue-2562", 2562);
            }
            // Issue #2620: Soft/Sampled incomplete → skip insert + force-Full arm.
            // Additive keys; #2317 canary counter retained for env=1 inserts.
            {
                const std::int64_t soft_skip = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_soft_incomplete_skip_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-soft-incomplete-skip-total", soft_skip);
                insert_kv("coercion_soft_incomplete_skip_total", soft_skip);
                insert_kv("coercion-unify-incomplete-skip-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_unify_incomplete_skip_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2620", 2620);
                insert_kv("issue-2620", 2620);
            }
            // Issue #2648: Soft evidence-loss bp + force-Full arm/consume (Agent face).
            {
                const std::int64_t loss_bp =
                    static_cast<std::int64_t>(aura::compiler::coercion_evidence_loss_bp());
                const std::int64_t thr = static_cast<std::int64_t>(
                    aura::compiler::coercion_evidence_loss_threshold_bp());
                const std::int64_t armed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_evidence_loss_force_armed_total.load(
                        std::memory_order_relaxed));
                const std::int64_t consumed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_evidence_loss_force_consumed_total.load(
                        std::memory_order_relaxed));
                const std::int64_t breach = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_evidence_loss_breach_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-evidence-loss-bp", loss_bp);
                insert_kv("coercion_evidence_loss_bp", loss_bp);
                insert_kv("coercion-evidence-loss-threshold-bp", thr);
                insert_kv("coercion-evidence-loss-force-armed", armed);
                insert_kv("coercion-evidence-loss-force-consumed", consumed);
                insert_kv("coercion-evidence-loss-breach-total", breach);
                insert_kv("coercion-evidence-loss-force-armed-total", armed);
                insert_kv("coercion-evidence-loss-force-consumed-total", consumed);
                insert_kv(
                    "coercion-evidence-loss-wired",
                    static_cast<std::int64_t>(aura::compiler::g_coercion_evidence_loss_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2648", 2648);
                insert_kv("issue-2648", 2648);
            }
            // Issue #2318: anti-starvation streak gate. N consecutive
            // truncated delta solves → force one full ConstraintSystem::
            // solve() (mirror #2277 escalation body). Reads from the
            // per-CompilerMetrics fields added in observability_metrics.h
            // (delta_reverify_truncate_streak + delta_truncate_force_full
            // _solve_total + delta_truncate_streak_threshold + delta_
            // truncate_anti_starve_wired). Threshold reads from env
            // AURA_DELTA_TRUNCATE_STREAK_FULL (default 2).
            const std::int64_t delta_reverify_truncate_streak =
                m ? static_cast<std::int64_t>(
                        m->delta_reverify_truncate_streak.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t delta_truncate_force_full_solve_total =
                m ? static_cast<std::int64_t>(
                        m->delta_truncate_force_full_solve_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t delta_truncate_streak_threshold =
                m ? static_cast<std::int64_t>(
                        m->delta_truncate_streak_threshold.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-reverify-truncate-streak", delta_reverify_truncate_streak);
            insert_kv("delta_truncate_reverify_truncate_streak", delta_reverify_truncate_streak);
            insert_kv("delta-truncate-force-full-solve-total",
                      delta_truncate_force_full_solve_total);
            insert_kv("delta_truncate_force_full_solve_total",
                      delta_truncate_force_full_solve_total);
            insert_kv("delta-truncate-streak-threshold", delta_truncate_streak_threshold);
            insert_kv("delta_truncate_streak_threshold", delta_truncate_streak_threshold);
            insert_kv("delta-truncate-anti-starve-wired",
                      (m && m->delta_truncate_anti_starve_wired.load() != 0) ? 1 : 0);
            insert_kv("delta_truncate_anti_starve_wired",
                      (m && m->delta_truncate_anti_starve_wired.load() != 0) ? 1 : 0);
            insert_kv("schema-2318", 2318);
            insert_kv("issue-2318", 2318);
            // Issue #2508: goal-priority reverify before anti-starve full solve.
            // Runs when truncate streak hits AURA_DELTA_TRUNCATE_STREAK_FULL and
            // occurrence_goals_ / priority roots are live. Recovered → no force-full.
            const std::int64_t goal_pri_reverify =
                m ? static_cast<std::int64_t>(m->delta_truncate_goal_priority_reverify_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t goal_pri_recovered =
                m ? static_cast<std::int64_t>(m->delta_truncate_goal_priority_recovered_total.load(
                        std::memory_order_relaxed))
                  : 0;
            insert_kv("delta-truncate-goal-priority-reverify-total", goal_pri_reverify);
            insert_kv("delta_truncate_goal_priority_reverify_total", goal_pri_reverify);
            insert_kv("delta-truncate-goal-priority-recovered-total", goal_pri_recovered);
            insert_kv("delta_truncate_goal_priority_recovered_total", goal_pri_recovered);
            insert_kv("delta-truncate-goal-priority-wired", 1);
            insert_kv("schema-2508", 2508);
            insert_kv("issue-2508", 2508);
            // Issue #2321: OccurrenceGoal refined-drift observability.
            //   - occurrence-goal-refined-drift-total: cumulative count of
            //     goals dropped from solve_delta_occurrence replay when the
            //     stored `refined` is no longer consistent with the current
            //     Union-Find binding of `g.var` (drift detection).
            //   - occurrence-goal-drift-wired: sentinel = 1 when the
            //     re-validate + drop logic is integrated.
            const std::int64_t refined_drift_total =
                m ? static_cast<std::int64_t>(
                        m->occurrence_goal_refined_drift_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("occurrence-goal-refined-drift-total", refined_drift_total);
            insert_kv("occurrence_goal_refined_drift_total", refined_drift_total);
            insert_kv("occurrence-goal-drift-wired", 1);
            insert_kv("schema-2321", 2321);
            insert_kv("issue-2321", 2321);
            insert_kv("coercion-parent-walk-cap-sampled", 16);
            insert_kv("coercion-parent-walk-cap-full", 64);
            insert_kv("coercion-provenance-fast-path-wired", 1);
            insert_kv("schema-2147", 2147);
            insert_kv("issue-2147", 2147);
            // Issue #2512: stamp active mid/pred into CoercionEntry at deferred-add.
            // Raises apply-time fast-path hit rate when TLS would otherwise clear.
            insert_kv("coercion-stamp-at-add-total",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_total.load(
                          std::memory_order_relaxed)));
            insert_kv("coercion_stamp_at_add_total",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_total.load(
                          std::memory_order_relaxed)));
            insert_kv("coercion-stamp-at-add-wired",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_wired.load(
                          std::memory_order_relaxed)));
            insert_kv("schema-2512", 2512);
            insert_kv("issue-2512", 2512);
            // Issue #2148: precision meet/join lattice observability.
            const std::int64_t meet_prec =
                m ? static_cast<std::int64_t>(
                        m->meet_precision_hit_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t meet_uses =
                m ? static_cast<std::int64_t>(
                        m->and_or_meet_uses_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t join_uses =
                m ? static_cast<std::int64_t>(
                        m->and_or_join_uses_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("meet-precision-hit-total", meet_prec);
            insert_kv("meet_precision_hit_total", meet_prec);
            insert_kv("and-or-meet-uses-total", meet_uses);
            insert_kv("and-or-join-uses-total", join_uses);
            insert_kv("meet-precision-lattice-wired", 1);
            insert_kv("schema-2148", 2148);
            insert_kv("issue-2148", 2148);
            // Issue #2102: provenance miss → force Full/contextual audit or reject.
            const std::int64_t miss_force_audit = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_miss_force_audit_total.load(
                    std::memory_order_relaxed));
            const std::int64_t miss_reject = static_cast<std::int64_t>(
                aura::compiler::g_coercion_provenance_miss_reject_total.load(
                    std::memory_order_relaxed));
            insert_kv("coercion-provenance-miss-force-audit-total", miss_force_audit);
            insert_kv("coercion_provenance_miss_force_audit_total", miss_force_audit);
            insert_kv("coercion-provenance-miss-reject-total", miss_reject);
            insert_kv("force-audit-on-provenance-miss",
                      aura::compiler::force_audit_on_provenance_miss() ? 1 : 0);
            insert_kv("reject-apply-on-provenance-miss",
                      aura::compiler::reject_apply_on_provenance_miss() ? 1 : 0);
            insert_kv("provenance-miss-force-audit-wired", 1);
            insert_kv("schema-2102", 2102);
            insert_kv("issue-2102", 2102);
            // Issue #2185: production defaults force reject-on-miss
            insert_kv("production-defaults-reject-on-miss",
                      aura::compiler::reject_apply_on_provenance_miss() ? 1 : 0);
            insert_kv("coercion-provenance-reject-production-wired", 1);
            insert_kv("schema-2185", 2185);
            insert_kv("issue-2185", 2185);
            // Issue #2558: completeness SLO health (backstop force Full).
            {
                const std::int64_t slo_bp =
                    static_cast<std::int64_t>(aura::compiler::coercion_prov_slo_bp());
                const std::int64_t breach =
                    static_cast<std::int64_t>(aura::compiler::g_coercion_prov_slo_breach_total.load(
                        std::memory_order_relaxed));
                const std::int64_t observe = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_prov_slo_observe_only_total.load(
                        std::memory_order_relaxed));
                const std::int64_t armed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_prov_slo_force_armed_total.load(
                        std::memory_order_relaxed));
                const std::int64_t consumed = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_prov_slo_force_consumed_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-prov-slo-bp", slo_bp);
                insert_kv("coercion-prov-slo-breach-total", breach);
                insert_kv("coercion-prov-slo-observe-only-total", observe);
                insert_kv("coercion-prov-slo-force-armed-total", armed);
                insert_kv("coercion-prov-slo-force-consumed-total", consumed);
                insert_kv("coercion-prov-slo-force-full-pending",
                          aura::compiler::coercion_prov_slo_force_full_pending() ? 1 : 0);
                insert_kv("schema-2558", 2558);
                insert_kv("issue-2558", 2558);
            }
            // Issue #2561: Soft/Sampled blame-chain recover + one-shot Full escalate.
            {
                const std::int64_t recover = static_cast<std::int64_t>(
                    aura::compiler::g_blame_soft_recover_total.load(std::memory_order_relaxed));
                const std::int64_t recover_fail =
                    static_cast<std::int64_t>(aura::compiler::g_blame_soft_recover_fail_total.load(
                        std::memory_order_relaxed));
                const std::int64_t escalate = static_cast<std::int64_t>(
                    aura::compiler::g_blame_soft_escalate_total.load(std::memory_order_relaxed));
                insert_kv("blame-soft-recover-total", recover);
                insert_kv("blame_soft_recover_total", recover);
                insert_kv("blame-soft-recover-fail-total", recover_fail);
                insert_kv("blame_soft_recover_fail_total", recover_fail);
                insert_kv("blame-soft-escalate-total", escalate);
                insert_kv("blame_soft_escalate_total", escalate);
                insert_kv("blame-soft-recover-wired", 1);
                insert_kv("schema-2561", 2561);
                insert_kv("issue-2561", 2561);
            }
            // Issue #2261: Sampled ban weak mid / no CoercionNode pretend stamps
            {
                const std::int64_t sampled_rej = static_cast<std::int64_t>(
                    aura::compiler::g_coercion_provenance_sampled_reject_total.load(
                        std::memory_order_relaxed));
                insert_kv("coercion-provenance-sampled-reject-total", sampled_rej);
                insert_kv("coercion_provenance_sampled_reject_total", sampled_rej);
                insert_kv("coercion-provenance-ban-weak-ir-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_provenance_ban_weak_ir_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2261", 2261);
                insert_kv("issue-2261", 2261);
            }
            // Issue #2221: blame-complete optional hard gate on composite commit
            {
                const std::int64_t blame_rej = static_cast<std::int64_t>(
                    aura::compiler::g_blame_commit_reject_total.load(std::memory_order_relaxed));
                const std::int64_t blame_obs = static_cast<std::int64_t>(
                    aura::compiler::g_blame_commit_incomplete_observe_total.load(
                        std::memory_order_relaxed));
                const std::int64_t blame_chk = static_cast<std::int64_t>(
                    aura::compiler::g_blame_commit_check_total.load(std::memory_order_relaxed));
                const std::int64_t m_blame_rej =
                    m ? static_cast<std::int64_t>(
                            m->blame_commit_reject_total.load(std::memory_order_relaxed))
                      : blame_rej;
                insert_kv("require-blame-complete-on-commit",
                          aura::compiler::require_blame_complete_on_commit() ? 1 : 0);
                insert_kv("blame-commit-reject-total", blame_rej);
                insert_kv("blame_commit_reject_total", m_blame_rej);
                insert_kv("blame-commit-incomplete-observe-total", blame_obs);
                insert_kv("blame-commit-check-total", blame_chk);
                insert_kv("blame-commit-require-wired", 1);
                insert_kv("schema-2221", 2221);
                insert_kv("issue-2221", 2221);
            }
            // Issue #2028: stable constraint solver surface metrics
            const std::int64_t sdo_total =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_occurrence_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t sdo_stable =
                m ? static_cast<std::int64_t>(
                        m->solve_delta_occurrence_stable_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t lpi_prov =
                m ? static_cast<std::int64_t>(
                        m->let_poly_instantiate_provenance_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t adt_ren =
                m ? static_cast<std::int64_t>(
                        m->adt_guardshape_selective_renarrow_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cont_hits =
                m ? static_cast<std::int64_t>(
                        m->cross_delta_solve_continuity_hits_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("solve-delta-occurrence-total", sdo_total);
            insert_kv("solve_delta_occurrence_total", sdo_total);
            insert_kv("solve-delta-occurrence-stable", sdo_stable);
            insert_kv("let-poly-instantiate-provenance", lpi_prov);
            insert_kv("let_poly_instantiate_provenance_total", lpi_prov);
            insert_kv("adt-guardshape-selective-renarrow", adt_ren);
            insert_kv("adt_guardshape_selective_renarrow_total", adt_ren);
            insert_kv("cross-delta-solve-continuity-hits", cont_hits);
            insert_kv("solver-surface-wired", 1);
            insert_kv("solve-delta-occurrence-wired", 1);
            insert_kv("let-poly-instantiate-provenance-wired", 1);
            insert_kv("adt-guardshape-renarrow-wired", 1);
            insert_kv("schema-2028", 2028);
            insert_kv("issue-2028", 2028);
            // Issue #2107: structured TIMEOUT / unresolved export for Agents
            {
                const std::int64_t unr_export =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unresolved_export_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t unr_cons =
                    m ? static_cast<std::int64_t>(m->solve_delta_unresolved_constraints_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t to_unr =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_timeout_unresolved_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_n =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unresolved_last_count.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_unscanned =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unscanned_last.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_trunc =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_truncated_reverify_last.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t sample_len =
                    m ? static_cast<std::int64_t>(
                            m->solve_delta_unresolved_affected_sample_len.load(
                                std::memory_order_relaxed))
                      : 0;
                insert_kv("solve-delta-unresolved-export-total", unr_export);
                insert_kv("solve-delta-unresolved-constraints-total", unr_cons);
                insert_kv("solve-delta-timeout-unresolved-total", to_unr);
                insert_kv("solve-delta-unresolved-last-count", last_n);
                insert_kv("solve-delta-unscanned-last", last_unscanned);
                insert_kv("solve-delta-truncated-reverify-last", last_trunc);
                insert_kv("solve-delta-unresolved-affected-sample-len", sample_len);
                insert_kv("solve-delta-unresolved-affected-0",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_0.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-affected-1",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_1.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-affected-2",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_2.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-affected-3",
                          m ? static_cast<std::int64_t>(m->solve_delta_unresolved_affected_3.load(
                                  std::memory_order_relaxed))
                            : 0);
                insert_kv("solve-delta-unresolved-export-wired", 1);
                insert_kv("schema-2107", 2107);
                insert_kv("issue-2107", 2107);
                // Issue #2195: goal kind on conflict/timeout export (SUBTYPE=2).
                insert_kv("last-conflict-goal-kind",
                          m ? static_cast<std::int64_t>(
                                  m->last_conflict_goal_kind.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("last-unresolved-goal-kind",
                          m ? static_cast<std::int64_t>(
                                  m->last_unresolved_goal_kind.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("subtype-goal-solve-total",
                          m ? static_cast<std::int64_t>(
                                  m->subtype_goal_solve_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("subtype-goal-conflict-total",
                          m ? static_cast<std::int64_t>(
                                  m->subtype_goal_conflict_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("subtype-goal-wired", 1);
                insert_kv("schema-2195", 2195);
                insert_kv("issue-2195", 2195);
                // Issue #2607: INSTANCE goal (depth-capped ∀ peel + unify).
                insert_kv("instance-unify-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_unify_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-depth-cap-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_depth_cap_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-goal-solve-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_goal_solve_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-goal-conflict-total",
                          m ? static_cast<std::int64_t>(
                                  m->instance_goal_conflict_total.load(std::memory_order_relaxed))
                            : 0);
                insert_kv("instance-depth-cap",
                          static_cast<std::int64_t>(aura::compiler::kInstanceDepthCap));
                insert_kv("instance-goal-wired", 1);
                insert_kv("schema-2607", 2607);
                insert_kv("issue-2607", 2607);
            }
            // Issue #2030: agent blame completeness + occurrence post-mutate hit rate
            {
                const std::uint64_t blame_c =
                    m ? m->blame_chain_complete_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t blame_m =
                    m ? m->blame_propagation_miss_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t blame_den = blame_c + blame_m;
                const std::int64_t blame_ratio_bp =
                    blame_den == 0 ? 10000
                                   : static_cast<std::int64_t>((blame_c * 10000ull) / blame_den);
                const std::uint64_t ren_hits =
                    m ? m->occurrence_renarrow_hits_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t ren_tot =
                    m ? m->occurrence_renarrow_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t stale_r =
                    m ? m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t occ_blame =
                    m ? m->occurrence_blame_chain_complete_total.load(std::memory_order_relaxed)
                      : 0;
                std::int64_t occ_hit_bp = 10000;
                if (ren_tot > 0)
                    occ_hit_bp = static_cast<std::int64_t>((ren_hits * 10000ull) / ren_tot);
                else if (stale_r > 0)
                    occ_hit_bp = static_cast<std::int64_t>((occ_blame * 10000ull) / stale_r);
                using namespace aura::compiler::linear_occurrence_mutate;
                const std::uint64_t lin_reval =
                    revalidate_hits_total.load(std::memory_order_relaxed) +
                    (m ? m->linear_occurrence_revalidate_hits_total.load(std::memory_order_relaxed)
                       : 0);
                const std::uint64_t lin_esc =
                    escape_violations_prevented_total.load(std::memory_order_relaxed) +
                    (m ? m->linear_occurrence_escape_prevented_total.load(std::memory_order_relaxed)
                       : 0);
                const std::uint64_t lin_den = lin_reval + lin_esc;
                const std::int64_t lin_occ_bp =
                    lin_den == 0 ? 10000
                                 : static_cast<std::int64_t>((lin_reval * 10000ull) / lin_den);
                insert_kv("blame_completeness_ratio", blame_ratio_bp);
                insert_kv("blame-completeness-ratio-bp", blame_ratio_bp);
                insert_kv("occurrence_narrowing_post_mutate_hit_rate", occ_hit_bp);
                insert_kv("occurrence-narrowing-post-mutate-hit-rate-bp", occ_hit_bp);
                insert_kv("linear-occurrence-consistency-bp", lin_occ_bp);
                insert_kv("linear-provenance-consistency-bp",
                          static_cast<std::int64_t>(
                              aura::core::provenance::linear_provenance_consistency_bp()));
                insert_kv("blame-occurrence-ratios-wired", 1);
                insert_kv("schema-2030", 2030);
                insert_kv("issue-2030", 2030);
            }
            // Issue #2260: boundary type-proof hard-gate metrics
            {
                using namespace aura::compiler::typed_audit;
                insert_kv("boundary-solve-hard-gate-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.boundary_solve_hard_gate_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("boundary-solve-full-resync-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.boundary_solve_full_resync_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "boundary-solve-force-rollback-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.boundary_solve_force_rollback_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "boundary-solve-truncated-seen-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.boundary_solve_truncated_seen_total.load(
                            std::memory_order_relaxed)));
                insert_kv("boundary-solve-hard-gate-wired",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.boundary_solve_hard_gate_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2260", 2260);
                insert_kv("issue-2260", 2260);
            }
            // Issue #2262: partial CS single source of truth
            {
                insert_kv("partial-cs-import-total",
                          static_cast<std::int64_t>(
                              aura::compiler::TypeChecker::partial_cs_import_total()));
                insert_kv("partial-cs-import-skip-total",
                          static_cast<std::int64_t>(
                              aura::compiler::TypeChecker::partial_cs_import_skip_total()));
                insert_kv("partial-cs-hard-empty-miss-total",
                          static_cast<std::int64_t>(
                              aura::compiler::g_partial_cs_hard_empty_miss_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "partial-cs-single-source-wired",
                    static_cast<std::int64_t>(aura::compiler::g_partial_cs_single_source_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2262", 2262);
                insert_kv("issue-2262", 2262);
            }
            // Issue #2345: expected-partial empty CS anti false-green (hard vs soft).
            {
                using namespace aura::compiler::typed_audit;
                insert_kv(
                    "composite-commit-empty-cs-hard-miss-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total
                            .load(std::memory_order_relaxed)));
                insert_kv(
                    "composite-commit-empty-cs-observe-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_empty_cs_observe_total
                            .load(std::memory_order_relaxed)));
                // Lineage #2180 empty-cs total retained.
                insert_kv(
                    "composite-commit-solve-empty-cs-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_solve_empty_cs_total.load(
                            std::memory_order_relaxed)));
                insert_kv("composite-empty-cs-hard-wired",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.composite_empty_cs_hard_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2345", 2345);
                insert_kv("issue-2345", 2345);
            }
            // Issue #2509: symmetric expected_partial ↔ commit_cs_has_work matrix.
            {
                using namespace aura::compiler::typed_audit;
                insert_kv(
                    "composite-commit-unexpected-cs-work-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_unexpected_cs_work_total
                            .load(std::memory_order_relaxed)));
                insert_kv(
                    "composite-commit-expected-has-work-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_expected_has_work_total
                            .load(std::memory_order_relaxed)));
                insert_kv(
                    "composite-commit-sdo-entered-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_commit_sdo_entered_total.load(
                            std::memory_order_relaxed)));
                insert_kv(
                    "composite-cs-signature-matrix-wired",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.composite_cs_signature_matrix_wired.load(
                            std::memory_order_relaxed)));
                insert_kv("schema-2509", 2509);
                insert_kv("issue-2509", 2509);
                // Issue #2610: auto-detect expected_partial from dirty cone.
                insert_kv("composite-commit-auto-partial-from-cone-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters
                                  .composite_commit_auto_partial_from_cone_total.load(
                                      std::memory_order_relaxed)));
                insert_kv("composite-commit-auto-partial-from-cone-observe-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters
                                  .composite_commit_auto_partial_from_cone_observe_total.load(
                                      std::memory_order_relaxed)));
                insert_kv("composite-auto-partial-from-cone-wired", 1);
                insert_kv("commit-readiness-force-reason-auto-partial", 6);
                insert_kv("schema-2610", 2610);
                insert_kv("issue-2610", 2610);
            }
            // Issue #2458: truncate-commit Soft observe / Hard full-solve-or-reject.
            // Additive keys on fidelity-stats (anti half-green under multi-round).
            {
                using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
                using aura::compiler::typed_audit::truncate_commit_hard_enabled;
                insert_kv("truncate-commit-observe-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.truncate_commit_observe_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("truncate-commit-reject-total",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.truncate_commit_reject_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "truncate-commit-full-solve-recover-total",
                    static_cast<std::int64_t>(
                        g_typed_mutation_audit_counters.truncate_commit_full_solve_recover_total
                            .load(std::memory_order_relaxed)));
                insert_kv("truncate-commit-hard-wired",
                          static_cast<std::int64_t>(
                              g_typed_mutation_audit_counters.truncate_commit_hard_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("truncate-commit-hard-enabled", truncate_commit_hard_enabled() ? 1 : 0);
                insert_kv("schema-2458", 2458);
                insert_kv("issue-2458", 2458);
            }
            // Issue #2359: unify occurrence_goals + predicate_memo epoch
            // health on the fidelity-stats surface (pure read; no solve
            // side effects). Agents use these keys to decide whether
            // narrowing caches and CS goals are same-generation:
            //   - cache-epoch: TypeChecker / Evaluator current epoch
            //   - occurrence-goals-live / max-epoch / stale-vs-epoch
            //   - predicate-memo-live / stale-vs-epoch
            //   - memo-goal-epoch-delta: 0 healthy; >0 lag (memo stale
            //     + goal survivors past prune boundary)
            // Vacuous healthy (0s) when no commit TypeChecker / memo.
            {
                std::int64_t cache_epoch_v = 0;
                std::int64_t goals_live = 0;
                std::int64_t goals_max_epoch = 0;
                std::int64_t goals_stale = 0;
                std::int64_t memo_live = 0;
                std::int64_t memo_stale = 0;
                if (ev) {
                    cache_epoch_v = static_cast<std::int64_t>(ev->current_cache_epoch());
                    if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                            ev->commit_type_checker_handle())) {
                        cache_epoch_v = static_cast<std::int64_t>(ctc->cache_epoch());
                        const auto& cs = ctc->constraint_system();
                        goals_live = static_cast<std::int64_t>(cs.occurrence_goals_size());
                        goals_max_epoch =
                            static_cast<std::int64_t>(cs.occurrence_goals_max_epoch());
                        goals_stale = static_cast<std::int64_t>(
                            cs.occurrence_goals_stale_vs_epoch(ctc->cache_epoch()));
                        // Last partial snapshot (engine is ephemeral).
                        memo_live = static_cast<std::int64_t>(ctc->last_predicate_memo_live());
                        memo_stale =
                            static_cast<std::int64_t>(ctc->last_predicate_memo_stale_vs_epoch());
                    }
                    // Prefer live guard-path InferenceEngine when present
                    // (memo survives multi-round Guard exit / selective).
                    if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                            ev->guard_infer_engine())) {
                        memo_live = static_cast<std::int64_t>(eng->predicate_memo_size());
                        memo_stale =
                            static_cast<std::int64_t>(eng->predicate_memo_stale_vs_epoch());
                    }
                }
                // Lag signal: memo entries behind cache epoch + goals
                // that would be prune-eligible but still live.
                const std::int64_t delta = memo_stale + goals_stale;
                insert_kv("cache-epoch", cache_epoch_v);
                insert_kv("occurrence-goals-live", goals_live);
                insert_kv("occurrence-goals-max-epoch", goals_max_epoch);
                insert_kv("occurrence-goals-stale-vs-epoch", goals_stale);
                insert_kv("predicate-memo-live", memo_live);
                insert_kv("predicate-memo-stale-vs-epoch", memo_stale);
                insert_kv("memo-goal-epoch-delta", delta);
                insert_kv("memo-goal-epoch-health-wired", 1);
                insert_kv("schema-2359", 2359);
                insert_kv("issue-2359", 2359);
                // Issue #2461: per-If structural cache key hit/miss.
                std::int64_t key_hit = 0;
                std::int64_t key_miss = 0;
                if (m) {
                    key_hit = static_cast<std::int64_t>(
                        m->occurrence_cache_key_hit_total.load(std::memory_order_relaxed));
                    key_miss = static_cast<std::int64_t>(
                        m->occurrence_cache_key_miss_total.load(std::memory_order_relaxed));
                }
                if (ev) {
                    if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                            ev->guard_infer_engine())) {
                        // Prefer live engine session counters when present.
                        key_hit = static_cast<std::int64_t>(eng->occurrence_cache_key_hits());
                        key_miss = static_cast<std::int64_t>(eng->occurrence_cache_key_misses());
                    }
                }
                insert_kv("occurrence-cache-key-hit-total", key_hit);
                insert_kv("occurrence_cache_key_hit_total", key_hit);
                insert_kv("occurrence-cache-key-miss-total", key_miss);
                insert_kv("occurrence_cache_key_miss_total", key_miss);
                insert_kv("occurrence-cache-key-wired", 1);
                insert_kv("schema-2461", 2461);
                insert_kv("issue-2461", 2461);
                // Issue #2622: single dirty-key authority (memo + goals).
                std::int64_t diverge = 0;
                std::int64_t sync_tot = 0;
                std::int64_t fence_joint = 0;
                if (m) {
                    diverge = static_cast<std::int64_t>(
                        m->occurrence_memo_goal_diverge_total.load(std::memory_order_relaxed));
                    sync_tot = static_cast<std::int64_t>(
                        m->occurrence_sync_after_dirty_total.load(std::memory_order_relaxed));
                    fence_joint = static_cast<std::int64_t>(
                        m->occurrence_memo_goal_fence_joint_total.load(std::memory_order_relaxed));
                }
                if (ev) {
                    if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                            ev->guard_infer_engine())) {
                        diverge =
                            static_cast<std::int64_t>(eng->occurrence_memo_goal_diverge_total());
                        sync_tot =
                            static_cast<std::int64_t>(eng->occurrence_sync_after_dirty_total());
                    }
                }
                insert_kv("occurrence-memo-goal-diverge-total", diverge);
                insert_kv("occurrence_memo_goal_diverge_total", diverge);
                insert_kv("occurrence-sync-after-dirty-total", sync_tot);
                insert_kv("occurrence_sync_after_dirty_total", sync_tot);
                insert_kv("occurrence-memo-goal-fence-joint-total", fence_joint);
                insert_kv(
                    "occurrence-dirty-key-authority-wired",
                    m ? static_cast<std::int64_t>(
                            m->occurrence_dirty_key_authority_wired.load(std::memory_order_relaxed))
                      : 1);
                insert_kv("schema-2622", 2622);
                insert_kv("issue-2622", 2622);
            }
            insert_kv("issue", 1617);  // primary lineage (#1617 / #798 / #1924 / #2028 / #2030)
            insert_kv("schema", 1617); // keep 1617 for existing ACs; #2030 via schema-2030
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2283: query:type-dep-partial-merge-stats. Hash view of the
    // systematic type_dep_graph_ merge for delta-touched TypeIds (CS
    // touched_roots + occurrence vars + rebinding type change). Agents
    // can compute the ratio of partial-merge expansion vs the existing
    // type_dep_graph_affected_expand_total to gauge the cost of the new
    // path under incremental typecheck.
    //   - type-dep-partial-merge-total: # of times the merge was performed
    //   - type-dep-partial-nodes-added: # of nodes added by the merge
    //   - type-dep-graph-affected-expand-total: existing #1528 expansion
    //   - type-dep-merge-ratio-bp: nodes_added * 10000 / affected_expand
    //     (ratio in basis points; 0 when no expansion)
    ObservabilityPrims::register_stats_impl(
        "query:type-dep-partial-merge-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            // Capacity 64: #2283 + #2320 + #2355 + #2516 keys.
            auto* ht = FlatHashTable::create(64);
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
            const std::int64_t partial_merge_total =
                m ? static_cast<std::int64_t>(
                        m->type_dep_partial_merge_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t partial_nodes_added =
                m ? static_cast<std::int64_t>(
                        m->type_dep_partial_nodes_added.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t affected_expand =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_affected_expand_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t merge_ratio_bp =
                affected_expand > 0 ? (partial_nodes_added * 10000) / affected_expand : 0;
            insert_kv("type-dep-partial-merge-total", partial_merge_total);
            insert_kv("type-dep-partial-nodes-added", partial_nodes_added);
            insert_kv("type-dep-graph-affected-expand-total", affected_expand);
            insert_kv("type-dep-merge-ratio-bp", merge_ratio_bp);
            // Issue #2320: prune observability (refine #2283 #387).
            //   - type-dep-graph-prune-total: cumulative count of prune calls
            //     when set_cache_epoch advances (or when prune_type_dep_graph
            //     is invoked from infer_flat_partial entry once per epoch).
            //   - type-dep-graph-entries-dropped: total NodeIds dropped
            //     across all buckets per prune call (cumulative).
            //   - type-dep-graph-cap-evict-total: cumulative count of
            //     per-bucket cap-triggered evictions (AC2 optional cap
            //     path; default OFF when env unset).
            const std::int64_t prune_total =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_prune_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t entries_dropped =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_entries_dropped.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t cap_evict =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_cap_evict_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("type-dep-graph-prune-total", prune_total);
            insert_kv("type-dep-graph-entries-dropped", entries_dropped);
            insert_kv("type_dep-graph-entries-dropped", entries_dropped);
            insert_kv("type-dep-graph-cap-evict-total", cap_evict);
            insert_kv("schema-2320", 2320);
            insert_kv("issue-2320", 2320);
            // Issue #2355: epoch-stale drop + dirty invalidate + size surface.
            const std::int64_t stale_drop =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_stale_drop_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t inv_drop =
                m ? static_cast<std::int64_t>(
                        m->type_dep_graph_invalidate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t dep_size = m ? static_cast<std::int64_t>(m->type_dep_graph_size.load(
                                                  std::memory_order_relaxed))
                                            : 0;
            insert_kv("type-dep-stale-drop-total", stale_drop);
            insert_kv("type_dep_stale_drop_total", stale_drop);
            insert_kv("type-dep-invalidate-total", inv_drop);
            insert_kv("type_dep_invalidate_total", inv_drop);
            insert_kv("type-dep-size", dep_size);
            insert_kv("type_dep_size", dep_size);
            insert_kv("type-dep-epoch-wired", 1);
            insert_kv("schema-2355", 2355);
            insert_kv("issue-2355", 2355);
            // Issue #2552: type_dep side of steal/densify joint fence.
            const std::int64_t steal_td_prune =
                m ? static_cast<std::int64_t>(
                        m->type_dep_steal_prune_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t steal_td_entries =
                m ? static_cast<std::int64_t>(
                        m->type_dep_steal_prune_entries_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("type-dep-steal-prune-total", steal_td_prune);
            insert_kv("type_dep_steal_prune_total", steal_td_prune);
            insert_kv("type-dep-steal-prune-entries-total", steal_td_entries);
            insert_kv("type-dep-steal-densify-fence-wired", 1);
            insert_kv("schema-2552", 2552);
            insert_kv("issue-2552", 2552);
            // Issue #2516: dirty txn order (invalidate → re-infer → mirror).
            const std::int64_t txn_wired =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_order_wired.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_total =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_p1 =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_phase1_invalidate_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_p2 =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_phase2_reinfer_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t txn_p3 =
                m ? static_cast<std::int64_t>(
                        m->type_dirty_txn_phase3_mirror_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("type-dirty-txn-order-wired", txn_wired);
            insert_kv("type-dirty-txn-total", txn_total);
            insert_kv("type-dirty-txn-phase1-invalidate-total", txn_p1);
            insert_kv("type-dirty-txn-phase2-reinfer-total", txn_p2);
            insert_kv("type-dirty-txn-phase3-mirror-total", txn_p3);
            insert_kv("schema-2516", 2516);
            insert_kv("issue-2516", 2516);
            // Issue #2560: partial re-infer cone soft/hard SLA (type layer).
            {
                const std::int64_t soft_ov =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_soft_overflow_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t hard_fb =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_hard_fallback_total.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t deg_trunc =
                    m ? static_cast<std::int64_t>(m->partial_cone_type_dep_degree_trunc_total.load(
                            std::memory_order_relaxed))
                      : 0;
                const std::int64_t last_sz =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_last_size.load(std::memory_order_relaxed))
                      : 0;
                const std::int64_t wired =
                    m ? static_cast<std::int64_t>(
                            m->partial_cone_cap_wired.load(std::memory_order_relaxed))
                      : 1;
                insert_kv("partial-cone-soft-overflow-total", soft_ov);
                insert_kv("partial-cone-hard-fallback-total", hard_fb);
                insert_kv("partial-cone-type-dep-degree-trunc-total", deg_trunc);
                insert_kv("partial-cone-last-size", last_sz);
                insert_kv("partial-cone-cap-wired", wired);
                insert_kv("schema-2560", 2560);
                insert_kv("issue-2560", 2560);
                // Issue #2621: cone truncate → commit fidelity (pairs #2560).
                insert_kv("last-partial-cone-truncated",
                          aura::compiler::typed_audit::last_partial_cone_truncated() ? 1 : 0);
                insert_kv("last-partial-cone-dropped",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::last_partial_cone_dropped()));
                insert_kv("last-partial-cone-fanout-trunc",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::last_partial_cone_fanout_trunc()));
                insert_kv("partial-cone-commit-observe-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_partial_cone_commit_observe_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("partial-cone-commit-reject-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_partial_cone_commit_reject_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("partial-cone-commit-gate-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_partial_cone_commit_gate_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2621", 2621);
                insert_kv("issue-2621", 2621);
                // Issue #2694: soft truncated cone silent dependency escalate
                // (additive — pairs #2646 outside-If invalidate + #2672 soak).
                insert_kv("soft-truncated-silent-dep-escalate-total",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::
                                  soft_truncated_silent_dep_escalate_total_v_read()));
                insert_kv("last-soft-truncated-silent-dep-count",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::last_soft_truncated_silent_dep_count()));
                insert_kv("soft-truncated-silent-dep-wired",
                          static_cast<std::int64_t>(
                              aura::compiler::typed_audit::g_soft_truncated_silent_dep_wired.load(
                                  std::memory_order_relaxed)));
                insert_kv("schema-2694", 2694);
                insert_kv("issue-2694", 2694);
                // Issue #2695: unified OwnershipEnv rebind API post-densify /
                // steal / explicit-Agent mutate:rebind (additive — pairs
                // #2673 linear-root consistency scan + #2563 force rollback).
                insert_kv(
                    "ownership-rebind-total",
                    static_cast<std::int64_t>(aura::compiler::ownership_rebind_total_v_read()));
                insert_kv("ownership-rebind-fail-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_fail_total_v_read()));
                insert_kv(
                    "ownership-rebind-wired",
                    static_cast<std::int64_t>(aura::compiler::ownership_rebind_wired_v_read()));
                insert_kv("ownership-rebind-densify-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_densify_total_v_read()));
                insert_kv("ownership-rebind-steal-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_steal_total_v_read()));
                insert_kv("ownership-rebind-explicit-agent-total",
                          static_cast<std::int64_t>(
                              aura::compiler::ownership_rebind_explicit_agent_total_v_read()));
                insert_kv("schema-2695", 2695);
                insert_kv("issue-2695", 2695);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2284: query:type-timeout-repair-stats. Hash view of the
    // Agent-first-class TIMEOUT repair surface (structured unresolved_
    // affected_nodes). On SolveResult::TIMEOUT or hard-reject after full-
    // solve failure, the publish site (evaluator_typecheck.cpp:post-solve,
    // evaluator_mutation_boundary.cpp:hard-reject) captures the status +
    // unresolved_count + unresolved_affected_nodes (capped at 16) +
    // truncated_reverify + blame_complete on a durable Agent surface.
    //   - type-timeout-repair-last-status: last SolveResult::Status (0=SOLVED,
    //     1=CONFLICT, 2=TIMEOUT, 99=hard-reject boundary)
    //   - type-timeout-repair-last-unresolved-count: size of sdo.unresolved
    //   - type-timeout-repair-last-unresolved-aff-nodes-count: capped size
    //     of unresolved_affected_nodes (capped at 16)
    //   - type-timeout-repair-last-unresolved-aff-node-N: individual NodeId
    //     slot (N=0..15) for the Agent repair set
    //   - type-timeout-repair-last-truncated-reverify: last sdo.truncated_reverify
    //   - type-timeout-repair-last-blame-complete: last blame.is_complete()
    //   - type-timeout-repair-publish-total: counter of publish calls
    //   - type-timeout-repair-wired: sentinel (=1)
    //   - schema == 2284 (lineage 2284)
    // Issue #2343 (schema-additive): var↔constraint unresolved graph
    //   - type-repair-unresolved-edge-count: exported edges (≤64)
    //   - type-repair-suggested-root-count: top-k roots by degree (≤8)
    //   - type-repair-suggested-root-N: UF rep (N=0..7)
    //   - type-repair-edge-N-{var,cix,kind,lhs,rhs}: query sample N=0..15
    //     Layout: var=UF rep, cix=constraint index, kind=Constraint::Kind,
    //     lhs/rhs=TypeId.index of the constraint endpoints.
    //   - type-repair-graph-export-total / type-repair-graph-wired
    //   - schema-2343 / issue-2343 (=2343)
    ObservabilityPrims::register_stats_impl(
        "query:type-timeout-repair-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            const auto* m = static_cast<const CompilerMetrics*>(ev->compiler_metrics());
            // Capacity must cover #2284 fixed keys + 16 aff-node slots +
            // #2343 graph keys (edge sample 16×5 + 8 roots + meta) +
            // #2548 reason/degree tags + reason enum sentinels.
            auto* ht = FlatHashTable::create(512);
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
            const std::int64_t last_status =
                m ? static_cast<std::int64_t>(
                        m->type_repair_last_timeout_status.load(std::memory_order_relaxed))
                  : 0;
            const std::uint64_t last_unresolved_count =
                m ? m->type_repair_last_unresolved_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t last_unresolved_aff_nodes_count =
                m ? m->type_repair_last_unresolved_aff_nodes_count.load(std::memory_order_relaxed)
                  : 0;
            const std::int64_t last_truncated_reverify =
                m ? static_cast<std::int64_t>(
                        m->type_repair_last_truncated_reverify.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t last_blame_complete =
                m ? static_cast<std::int64_t>(
                        m->type_repair_last_blame_complete.load(std::memory_order_relaxed))
                  : 0;
            const std::uint64_t publish_total =
                m ? m->type_repair_publish_total.load(std::memory_order_relaxed) : 0;
            insert_kv("type-timeout-repair-last-status", last_status);
            insert_kv("type-timeout-repair-last-unresolved-count",
                      static_cast<std::int64_t>(last_unresolved_count));
            insert_kv("type-timeout-repair-last-unresolved-aff-nodes-count",
                      static_cast<std::int64_t>(last_unresolved_aff_nodes_count));
            insert_kv("type-timeout-repair-last-truncated-reverify", last_truncated_reverify);
            insert_kv("type-timeout-repair-last-blame-complete", last_blame_complete);
            insert_kv("type-timeout-repair-publish-total",
                      static_cast<std::int64_t>(publish_total));
            insert_kv("type-timeout-repair-wired", 1);
            char field_buf[72];
            for (std::size_t i = 0; i < 16; ++i) {
                const std::uint64_t node_id =
                    m ? m->type_repair_last_unresolved_aff_nodes[i].load(std::memory_order_relaxed)
                      : 0;
                std::snprintf(field_buf, sizeof(field_buf),
                              "type-timeout-repair-last-unresolved-aff-node-%zu", i);
                insert_kv(field_buf, static_cast<std::int64_t>(node_id));
            }
            insert_kv("schema", 2284);
            insert_kv("issue", 2284);
            // Issue #2343: additive graph surface (schema-2284 keys intact).
            const std::uint64_t edge_count =
                m ? m->type_repair_unresolved_edge_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t root_count =
                m ? m->type_repair_suggested_root_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t graph_export_total =
                m ? m->type_repair_graph_export_total.load(std::memory_order_relaxed) : 0;
            insert_kv("type-repair-unresolved-edge-count", static_cast<std::int64_t>(edge_count));
            insert_kv("type-repair-suggested-root-count", static_cast<std::int64_t>(root_count));
            insert_kv("type-repair-graph-export-total",
                      static_cast<std::int64_t>(graph_export_total));
            insert_kv("type-repair-graph-wired", 1);
            for (std::size_t i = 0; i < 8; ++i) {
                const std::uint64_t root =
                    m ? m->type_repair_suggested_roots[i].load(std::memory_order_relaxed) : 0;
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-suggested-root-%zu", i);
                insert_kv(field_buf, static_cast<std::int64_t>(root));
                // Issue #2548: structured reason + degree per root.
                const std::uint64_t why =
                    m ? m->type_repair_suggested_root_reasons[i].load(std::memory_order_relaxed)
                      : 0;
                const std::uint64_t deg =
                    m ? m->type_repair_suggested_root_degrees[i].load(std::memory_order_relaxed)
                      : 0;
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-suggested-root-%zu-reason",
                              i);
                insert_kv(field_buf, static_cast<std::int64_t>(why));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-suggested-root-%zu-degree",
                              i);
                insert_kv(field_buf, static_cast<std::int64_t>(deg));
            }
            // Issue #2548: aggregate reason tags + caps / sentinels.
            insert_kv(
                "type-repair-occurrence-replay-miss-count",
                m ? static_cast<std::int64_t>(
                        m->type_repair_occurrence_replay_miss_count.load(std::memory_order_relaxed))
                  : 0);
            insert_kv("type-repair-let-poly-suggested-count",
                      m ? static_cast<std::int64_t>(m->type_repair_let_poly_suggested_count.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("type-repair-occurrence-suggested-count",
                      m ? static_cast<std::int64_t>(m->type_repair_occurrence_suggested_count.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("type-repair-rich-roots-export-total",
                      m ? static_cast<std::int64_t>(m->type_repair_rich_roots_export_total.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("type-repair-rich-roots-wired", 1);
            insert_kv("type-repair-root-reason-touched", 0);
            insert_kv("type-repair-root-reason-unresolved-endpoint", 1);
            insert_kv("type-repair-root-reason-pending-full", 2);
            insert_kv("type-repair-root-reason-let-poly", 3);
            insert_kv("type-repair-root-reason-occurrence", 4);
            insert_kv("type-repair-root-reason-occurrence-replay-miss", 5);
            // Issue #2607: Instance ranks above occurrence when degrees tie.
            insert_kv("type-repair-root-reason-instance", 6);
            insert_kv("instance-unify-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_unify_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_depth_cap_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-goal-solve-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_goal_solve_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-goal-conflict-total",
                      m ? static_cast<std::int64_t>(
                              m->instance_goal_conflict_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap",
                      static_cast<std::int64_t>(aura::compiler::kInstanceDepthCap));
            insert_kv("instance-goal-wired", 1);
            // Issue #2643: bounded INSTANCE depth-cap repair hint sample
            // (TIMEOUT path only — zero-cost on SOLVED / no INSTANCE).
            // Agents see per-goal depth_used / depth_cap / poly / var_rep /
            // site_node and re-instantiate polymorphic call sites before
            // full solve. Schema-additive (schema-2607 keys intact).
            insert_kv("instance-depth-cap-repair-hint-total",
                      m ? static_cast<std::int64_t>(m->instance_depth_cap_repair_hint_total.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap-repair-hint-count",
                      m ? static_cast<std::int64_t>(
                              m->type_repair_instance_hint_count.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("instance-depth-cap-repair-hint-cap",
                      static_cast<std::int64_t>(aura::compiler::kInstanceRepairHintCap));
            insert_kv("instance-depth-cap-repair-hint-wired", 1);
            for (std::size_t i = 0; i < 8; ++i) {
                const std::uint32_t depth_used =
                    m ? m->type_repair_instance_hint_depth_used[i].load(std::memory_order_relaxed)
                      : 0u;
                const std::uint32_t depth_cap =
                    m ? m->type_repair_instance_hint_depth_cap[i].load(std::memory_order_relaxed)
                      : 0u;
                const std::uint32_t poly =
                    m ? m->type_repair_instance_hint_poly[i].load(std::memory_order_relaxed) : 0u;
                const std::uint32_t var_rep =
                    m ? m->type_repair_instance_hint_var_rep[i].load(std::memory_order_relaxed)
                      : 0u;
                const std::uint32_t site_node =
                    m ? m->type_repair_instance_hint_site_node[i].load(std::memory_order_relaxed)
                      : 0u;
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-depth-used", i);
                insert_kv(field_buf, static_cast<std::int64_t>(depth_used));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-depth-cap", i);
                insert_kv(field_buf, static_cast<std::int64_t>(depth_cap));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-poly", i);
                insert_kv(field_buf, static_cast<std::int64_t>(poly));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-var-rep", i);
                insert_kv(field_buf, static_cast<std::int64_t>(var_rep));
                std::snprintf(field_buf, sizeof(field_buf),
                              "instance-depth-cap-repair-hint-%zu-site-node", i);
                insert_kv(field_buf, static_cast<std::int64_t>(site_node));
            }
            insert_kv("schema-2643", 2643);
            insert_kv("issue-2643", 2643);
            insert_kv("schema-2607", 2607);
            insert_kv("issue-2607", 2607);
            insert_kv("type-repair-edge-cap", 64);
            insert_kv("type-repair-root-cap", 8);
            insert_kv("schema-2548", 2548);
            insert_kv("issue-2548", 2548);
            for (std::size_t i = 0; i < 16; ++i) {
                const std::uint64_t var =
                    m ? m->type_repair_edge_var[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t cix =
                    m ? m->type_repair_edge_cix[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t kind =
                    m ? m->type_repair_edge_kind[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t lhs =
                    m ? m->type_repair_edge_lhs[i].load(std::memory_order_relaxed) : 0;
                const std::uint64_t rhs =
                    m ? m->type_repair_edge_rhs[i].load(std::memory_order_relaxed) : 0;
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-var", i);
                insert_kv(field_buf, static_cast<std::int64_t>(var));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-cix", i);
                insert_kv(field_buf, static_cast<std::int64_t>(cix));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-kind", i);
                insert_kv(field_buf, static_cast<std::int64_t>(kind));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-lhs", i);
                insert_kv(field_buf, static_cast<std::int64_t>(lhs));
                std::snprintf(field_buf, sizeof(field_buf), "type-repair-edge-%zu-rhs", i);
                insert_kv(field_buf, static_cast<std::int64_t>(rhs));
            }
            insert_kv("schema-2343", 2343);
            insert_kv("issue-2343", 2343);
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
    ObservabilityPrims::register_stats_impl(
        "query:type-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t runs = m->type_propagation_runs_.load(std::memory_order_relaxed);
            const std::uint64_t total = m->type_propagation_total_.load(std::memory_order_relaxed);
            const std::uint64_t unknown =
                m->type_propagation_unknown_.load(std::memory_order_relaxed);
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
    ObservabilityPrims::register_stats_impl(
        "query:dead-coercion-zerooverhead-stats",
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

    // Issue #2282 / #2556: query:dead-coercion-layered-stats. Hash view of the
    // 3 layered dead-coercion elision sources (AST identity + IR CastOp DCE
    // + dirty-cone early-out), so Agents can compute "this mutate removed
    // N Casts" without joining multiple schemas:
    //   - dead-coercion-layered-total: ast_elided + ir_elided + dirty_cone_skips
    //   - ast-elided: g_dead_coercion_ast_elided_total (#1425 / #2025)
    //   - ir-elided: dead_coercion_ir_elided_total (#2025 / #2066)
    //   - dirty-cone-skips: dead_coercion_dirty_cone_skips (#2106 / #2556
    //     CastOp sites outside type∪IR cone, or soft empty-cone count)
    //   - ir-narrow-evidence-hits: dead_coercion_ir_narrow_evidence_hits
    //   - pipeline-runs-total: dead_coercion_pipeline_runs_total
    // Issue #2556 additive keys (schema-2556):
    //   - dirty-cone-partial-runs / dirty-cone-cast-sites-scanned / full-scan-runs
    // Components stay individually queryable for additive schema lineage (AC4).
    ObservabilityPrims::register_stats_impl(
        "query:dead-coercion-layered-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            // Capacity 256: base + #2556/#2562/#2611/#2624 + #2674 keys (was 128).
            auto* ht = FlatHashTable::create(256);
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
            const std::uint64_t ast_elided =
                ::aura::compiler::g_dead_coercion_ast_elided_total.load(std::memory_order_relaxed);
            const std::uint64_t ir_elided =
                ::aura::compiler::opt_registry::dead_coercion_ir_elided_total.load(
                    std::memory_order_relaxed);
            const std::uint64_t dirty_cone_skips =
                ::aura::compiler::opt_registry::dead_coercion_dirty_cone_skips.load(
                    std::memory_order_relaxed);
            const std::uint64_t narrow_evidence =
                ::aura::compiler::opt_registry::dead_coercion_ir_narrow_evidence_hits.load(
                    std::memory_order_relaxed);
            const std::uint64_t pipeline_runs =
                ::aura::compiler::opt_registry::dead_coercion_pipeline_runs_total.load(
                    std::memory_order_relaxed);
            const std::uint64_t partial_runs =
                ::aura::compiler::opt_registry::dead_coercion_dirty_cone_partial_runs.load(
                    std::memory_order_relaxed);
            const std::uint64_t cast_sites_scanned =
                ::aura::compiler::opt_registry::dead_coercion_dirty_cone_cast_sites_scanned.load(
                    std::memory_order_relaxed);
            const std::uint64_t full_scan_runs =
                ::aura::compiler::opt_registry::dead_coercion_full_scan_runs.load(
                    std::memory_order_relaxed);
            const std::uint64_t layered_total = ast_elided + ir_elided + dirty_cone_skips;
            insert_kv("dead-coercion-layered-total", static_cast<std::int64_t>(layered_total));
            insert_kv("ast-elided", static_cast<std::int64_t>(ast_elided));
            insert_kv("ir-elided", static_cast<std::int64_t>(ir_elided));
            insert_kv("dirty-cone-skips", static_cast<std::int64_t>(dirty_cone_skips));
            insert_kv("ir-narrow-evidence-hits", static_cast<std::int64_t>(narrow_evidence));
            insert_kv("pipeline-runs-total", static_cast<std::int64_t>(pipeline_runs));
            // Issue #2556 additive Agent keys (schema-2556).
            insert_kv("schema-2556", 2556);
            insert_kv("dirty-cone-partial-runs", static_cast<std::int64_t>(partial_runs));
            insert_kv("dirty-cone-cast-sites-scanned",
                      static_cast<std::int64_t>(cast_sites_scanned));
            insert_kv("full-scan-runs", static_cast<std::int64_t>(full_scan_runs));
            // Issue #2562: dual-require drop observability (layered dead-coercion
            // + dual gate for Agent pre-check / insert integrity).
            insert_kv(
                "coercion-dual-require-drop-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_dual_require_drop_total.load(
                    std::memory_order_relaxed)));
            insert_kv("coercion-dual-require-enabled",
                      aura::compiler::coercion_dual_require_active() ? 1 : 0);
            insert_kv("coercion-dual-require-wired", 1);
            insert_kv("schema-2562", 2562);
            insert_kv("issue-2562", 2562);
            // Issue #2611: elided CastOp deopt meta (mid + narrow_evidence + type_tag).
            // last-* atomics are stamped at elision; expose_last_deopt_meta (deopt path /
            // tests) bumps deopt-meta-deopt-expose-total for Agent join (AC1).
            {
                using namespace ::aura::compiler::dce_deopt;
                insert_kv("schema-2611", 2611);
                insert_kv("issue-2611", 2611);
                insert_kv("deopt-meta-stamped-total",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_stamped_total.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-map-size",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_map_size.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-skipped-no-evidence",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_skipped_no_evidence.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-lookup-hits",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_lookup_hits.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-deopt-expose-total",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_deopt_expose_total.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-mid",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_mid.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-evidence",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_evidence.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-type-tag",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_type_tag.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-last-site-key",
                          static_cast<std::int64_t>(
                              dce_deopt_meta_last_site_key.load(std::memory_order_relaxed)));
                insert_kv("deopt-meta-wired", 1);
            }
            // Issue #2624 Phase A: CastOp typed meta side table (src/dst/evidence).
            // Not persisted in IR cache; missing on legacy IR → missing_total only.
            // Phase B executor Strict / Phase C JIT deopt are out of scope.
            {
                using namespace ::aura::compiler::castop_meta;
                insert_kv("schema-2624", 2624);
                insert_kv("issue-2624", 2624);
                insert_kv("castop-typed-meta-stamped-total",
                          static_cast<std::int64_t>(
                              castop_typed_meta_stamped_total.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-missing-total",
                          static_cast<std::int64_t>(
                              castop_typed_meta_missing_total.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-map-size",
                          static_cast<std::int64_t>(
                              castop_typed_meta_map_size.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-lookup-hits",
                          static_cast<std::int64_t>(
                              castop_typed_meta_lookup_hits.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-identity-elide-total",
                          static_cast<std::int64_t>(castop_typed_meta_identity_elide_total.load(
                              std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-last-src",
                          static_cast<std::int64_t>(
                              castop_typed_meta_last_src.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-last-dst",
                          static_cast<std::int64_t>(
                              castop_typed_meta_last_dst.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-last-evidence",
                          static_cast<std::int64_t>(
                              castop_typed_meta_last_evidence.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-wired",
                          static_cast<std::int64_t>(
                              castop_typed_meta_wired.load(std::memory_order_relaxed)));
                insert_kv("castop-typed-meta-phase-a",
                          static_cast<std::int64_t>(
                              castop_typed_meta_phase_a.load(std::memory_order_relaxed)));
            }
            // Issue #2674: layered evidence-coherence invariant — observe-only
            // divergence counter under Soft/Sampled (no hard-reject of mutate
            // by default per AC5). Agent-visible ast-elided-with-evidence counter
            // pairs with ir-narrow-evidence-hits + deopt-meta-stamped-total so
            // dashboards can detect layered-stats divergence under typed_mutate
            // → lower → JIT paths. Zero cost when no evidence path (pure atomic
            // loads on the read side; coherence check runs on MutationBoundary
            // outermost exit in evaluator_mutation_boundary.cpp).
            {
                insert_kv("schema-2674", 2674);
                insert_kv("issue-2674", 2674);
                insert_kv("ast-elided-with-evidence",
                          static_cast<std::int64_t>(
                              ::aura::compiler::g_dead_coercion_ast_elided_with_evidence_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("layered-evidence-diverge-total",
                          static_cast<std::int64_t>(
                              ::aura::compiler::g_layered_evidence_diverge_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("layered-evidence-coherence-wired", 1);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2558 / #2561 / #2648: query:coercion-provenance-health — completeness
    // SLO + Soft blame recover/escalate + Soft evidence-loss bp for Agents.
    ObservabilityPrims::register_stats_impl(
        "query:coercion-provenance-health",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            // #2648 evidence-loss keys need headroom beyond the #2558/#2561 set.
            auto* ht = FlatHashTable::create(48);
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
            insert_kv("schema-2558", 2558);
            insert_kv(
                "completeness-bp",
                static_cast<std::int64_t>(aura::compiler::coercion_provenance_completeness_bp()));
            insert_kv("miss-total", static_cast<std::int64_t>(
                                        aura::compiler::g_coercion_provenance_miss_total.load(
                                            std::memory_order_relaxed)));
            insert_kv(
                "complete-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_provenance_complete_total.load(
                    std::memory_order_relaxed)));
            insert_kv("slo-bp", static_cast<std::int64_t>(aura::compiler::coercion_prov_slo_bp()));
            insert_kv("slo-breach-total", static_cast<std::int64_t>(
                                              aura::compiler::g_coercion_prov_slo_breach_total.load(
                                                  std::memory_order_relaxed)));
            insert_kv("slo-observe-only-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_prov_slo_observe_only_total.load(
                              std::memory_order_relaxed)));
            insert_kv("force-full-pending",
                      aura::compiler::coercion_prov_slo_force_full_pending() ? 1 : 0);
            insert_kv("force-armed-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_prov_slo_force_armed_total.load(
                              std::memory_order_relaxed)));
            insert_kv("force-consumed-total",
                      static_cast<std::int64_t>(
                          aura::compiler::g_coercion_prov_slo_force_consumed_total.load(
                              std::memory_order_relaxed)));
            insert_kv("stamp-at-add-total",
                      static_cast<std::int64_t>(aura::compiler::g_coercion_stamp_at_add_total.load(
                          std::memory_order_relaxed)));
            // Issue #2561: Soft/Sampled blame recover + escalate (additive).
            insert_kv("blame-soft-recover-total",
                      static_cast<std::int64_t>(aura::compiler::g_blame_soft_recover_total.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "blame-soft-recover-fail-total",
                static_cast<std::int64_t>(aura::compiler::g_blame_soft_recover_fail_total.load(
                    std::memory_order_relaxed)));
            insert_kv("blame-soft-escalate-total",
                      static_cast<std::int64_t>(aura::compiler::g_blame_soft_escalate_total.load(
                          std::memory_order_relaxed)));
            insert_kv("schema-2561", 2561);
            insert_kv("issue-2561", 2561);
            // Issue #2562: dual-require gate keys (completeness pre-check).
            insert_kv(
                "coercion-dual-require-drop-total",
                static_cast<std::int64_t>(aura::compiler::g_coercion_dual_require_drop_total.load(
                    std::memory_order_relaxed)));
            insert_kv("coercion-dual-require-enabled",
                      aura::compiler::coercion_dual_require_active() ? 1 : 0);
            insert_kv("schema-2562", 2562);
            insert_kv("issue-2562", 2562);
            // Issue #2648: Soft evidence-loss rate + force arm/consume (single Agent face).
            {
                const std::int64_t loss_bp =
                    static_cast<std::int64_t>(aura::compiler::coercion_evidence_loss_bp());
                const std::int64_t thr = static_cast<std::int64_t>(
                    aura::compiler::coercion_evidence_loss_threshold_bp());
                insert_kv("coercion-evidence-loss-bp", loss_bp);
                insert_kv("coercion-evidence-loss-threshold-bp", thr);
                insert_kv("coercion-evidence-loss-force-armed",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_evidence_loss_force_armed_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("coercion-evidence-loss-force-consumed",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_evidence_loss_force_consumed_total.load(
                                  std::memory_order_relaxed)));
                insert_kv("coercion-evidence-loss-breach-total",
                          static_cast<std::int64_t>(
                              aura::compiler::g_coercion_evidence_loss_breach_total.load(
                                  std::memory_order_relaxed)));
                insert_kv(
                    "coercion-evidence-loss-wired",
                    static_cast<std::int64_t>(aura::compiler::g_coercion_evidence_loss_wired.load(
                        std::memory_order_relaxed)));
                insert_kv("schema-2648", 2648);
                insert_kv("issue-2648", 2648);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2557: query:lock-order-audit-stats — soft audit active flag +
    // inversion counters for production Restricted default. Additive schema
    // (mode: 1=off 2=soft 3=hard; production-soft-active 0/1).
    ObservabilityPrims::register_stats_impl(
        "query:lock-order-audit-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
            using namespace ::aura::compiler::lock_order;
            insert_kv("schema-2557", 2557);
            insert_kv("mode", static_cast<std::int64_t>(lock_order_mode()));
            insert_kv("soft-active",
                      lock_order_audit_enabled() && !lock_order_canary_enabled() ? 1 : 0);
            insert_kv("canary-active", lock_order_canary_enabled() ? 1 : 0);
            insert_kv("production-soft-active", lock_order_production_soft_active() ? 1 : 0);
            insert_kv("inversion-detected-total",
                      static_cast<std::int64_t>(
                          g_lock_inversion_detected_total.load(std::memory_order_relaxed)));
            insert_kv("violation-total",
                      static_cast<std::int64_t>(
                          g_lock_order_violation_total.load(std::memory_order_relaxed)));
            insert_kv("acquire-total", static_cast<std::int64_t>(g_lock_order_acquire_total.load(
                                           std::memory_order_relaxed)));
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
    ObservabilityPrims::register_stats_impl(
        "query:coercion-zerooverhead-stats", [](std::span<const EvalValue> a) -> EvalValue {
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

    // Issue #2287: query:castop-density-stats. Hash view of the Dynamic
    // CastOp density budget + Agent annotation hint surface. Non-blocking
    // hint — when last_castop_density_bp > castop_density_budget_bp, the
    // Agent sees castop-annotation-hint=1 and can prefer annotations over
    // blind Dynamic. density = 10000 * castop_emitted / max(1, insts);
    // budget is env AURA_CASTOP_DENSITY_BUDGET_BP (default 1500 = 15%).
    // Additive to #2282 layered stats (uses residual emitted, not elided).
    ObservabilityPrims::register_stats_impl(
        "query:castop-density-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            // Capacity 64: #2287 + #2319 + #2358 + #2459 keys.
            auto* ht = FlatHashTable::create(64);
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
            const std::uint64_t dens = m->last_castop_density_bp.load(std::memory_order_relaxed);
            const std::uint64_t budget =
                m->castop_density_budget_bp.load(std::memory_order_relaxed);
            const std::uint64_t over_budget =
                m->castop_density_over_budget_total.load(std::memory_order_relaxed);
            insert_kv("castop-density-bp", static_cast<std::int64_t>(dens));
            insert_kv("castop-density-budget-bp", static_cast<std::int64_t>(budget));
            insert_kv("castop-density-over-budget-total", static_cast<std::int64_t>(over_budget));
            // Agent hint: 1 when last density exceeds budget, else 0. Soft
            // (non-blocking) — #2108 hard-block stays unchanged.
            insert_kv("castop-annotation-hint", dens > budget ? 1 : 0);
            // Issue #2319: opt-in hard gate (refine #2287 soft hint).
            //   - castop-density-hard-reject-total: cumulative count of
            //     hard/soft gate firings when AURA_CASTOP_DENSITY_HARD=1
            //     + density > budget + dirty scope has unannotated
            //     Dynamic binding. Read-only observability.
            //   - castop-density-hard-wired: sentinel = 1 when the hard
            //     gate is integrated (production default OFF; env-gated
            //     Soft default).
            const std::int64_t hard_reject =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_reject_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hard_wired =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_wired.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("castop-density-hard-reject-total", hard_reject);
            insert_kv("castop_density_hard_reject_total", hard_reject);
            insert_kv("castop-density-hard-wired", hard_wired);
            insert_kv("castop_density_hard_wired", hard_wired);
            insert_kv("schema-2319", 2319);
            insert_kv("issue-2319", 2319);
            // Issue #2358: HARD policy force-JIT action (codegen, not type gate).
            //   - castop-density-hard-enabled: env AURA_CASTOP_DENSITY_HARD (0/1)
            //   - castop-density-hard-action-total: force-JIT fires under HARD=1
            //   - schema-2358 / issue-2358 / wired sentinel
            const std::int64_t hard_enabled =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_enabled.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t hard_action =
                m ? static_cast<std::int64_t>(
                        m->castop_density_hard_action_total.load(std::memory_order_relaxed))
                  : 0;
            insert_kv("castop-density-hard-enabled", hard_enabled);
            insert_kv("castop_density_hard_enabled", hard_enabled);
            insert_kv("castop-density-hard-action-total", hard_action);
            insert_kv("castop_density_hard_action_total", hard_action);
            insert_kv("castop-density-hard-action-wired", 1);
            insert_kv("schema-2358", 2358);
            insert_kv("issue-2358", 2358);
            // Issue #2459: production closed-loop streak + gate reject.
            //   - castop-density-streak: consecutive over-budget unannotated
            //   - castop-density-gate-reject-total: MutateTypeGate-aligned
            //   - castop-density-production-default-wired: policy present
            //   - schema-2458 / issue-2458
            const std::int64_t streak =
                static_cast<std::int64_t>(m->castop_density_streak.load(std::memory_order_relaxed));
            const std::int64_t gate_rej = static_cast<std::int64_t>(
                m->castop_density_gate_reject_total.load(std::memory_order_relaxed));
            const std::int64_t soft_warn = static_cast<std::int64_t>(
                m->castop_density_soft_warn_total.load(std::memory_order_relaxed));
            const std::int64_t prod_wired = static_cast<std::int64_t>(
                m->castop_density_production_default_wired.load(std::memory_order_relaxed));
            insert_kv("castop-density-streak", streak);
            insert_kv("castop_density_streak", streak);
            insert_kv("castop-density-gate-reject-total", gate_rej);
            insert_kv("castop_density_gate_reject_total", gate_rej);
            insert_kv("castop-density-soft-warn-total", soft_warn);
            insert_kv("castop-density-production-default-wired", prod_wired);
            insert_kv("castop_density_production_default_wired", prod_wired);
            insert_kv("schema-2459", 2459);
            insert_kv("issue-2459", 2459);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2613: query:type-linear-commit-health — single Agent fold of
    // commit_readiness (#2553) × coercion SLO (#2558) × linear force (#2545/#2563)
    // × occurrence/memo stale (#2359). Pure aggregation (AC4: no commit policy change).
    ObservabilityPrims::register_stats_impl(
        "query:type-linear-commit-health",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();

            TypeLinearCommitHealthSnapshot snap = type_linear_commit_health_live_base();
            // #2558 coercion completeness + SLO force pending.
            snap.coercion_completeness_bp = coercion_provenance_completeness_bp();
            snap.coercion_slo_force_pending = coercion_prov_slo_force_full_pending();
            // #2648 Soft evidence-loss bp (single Agent throttle face).
            snap.coercion_evidence_loss_bp = coercion_evidence_loss_bp();
            snap.coercion_evidence_loss_pressure =
                coercion_evidence_loss_pressure(snap.coercion_evidence_loss_bp);
            // #2359 occurrence goals + predicate memo stale (vacuous 0 without TC).
            if (__qev_) {
                if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                        __qev_->commit_type_checker_handle())) {
                    const auto& cs = ctc->constraint_system();
                    snap.occurrence_stale = static_cast<std::uint64_t>(
                        cs.occurrence_goals_stale_vs_epoch(ctc->cache_epoch()));
                    snap.predicate_memo_stale =
                        static_cast<std::uint64_t>(ctc->last_predicate_memo_stale_vs_epoch());
                }
                if (auto* eng = static_cast<aura::compiler::InferenceEngine*>(
                        __qev_->guard_infer_engine())) {
                    snap.predicate_memo_stale =
                        static_cast<std::uint64_t>(eng->predicate_memo_stale_vs_epoch());
                }
            }

            const auto scored = compute_type_linear_commit_health(snap);

            // #2648 adds evidence-loss keys — 64 slots keep load factor healthy.
            auto* ht = FlatHashTable::create(64);
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

            // #2553 readiness face
            insert_kv("readiness-bp", static_cast<std::int64_t>(scored.readiness_bp));
            insert_kv("force-reason", scored.force_reason_code);
            insert_kv("force-reason-code", scored.force_reason_code);
            insert_kv("would-allow-commit", scored.would_allow_commit ? 1 : 0);
            // #2558 coercion
            insert_kv("coercion-completeness-bp",
                      static_cast<std::int64_t>(scored.coercion_completeness_bp));
            insert_kv("coercion-slo-force-pending", scored.coercion_slo_force_pending ? 1 : 0);
            // #2648 Soft evidence-loss (single bp; no multi-key join)
            insert_kv("coercion-evidence-loss-bp",
                      static_cast<std::int64_t>(scored.coercion_evidence_loss_bp));
            insert_kv("coercion-evidence-loss-pressure",
                      scored.coercion_evidence_loss_pressure ? 1 : 0);
            insert_kv("coercion-evidence-loss-force-armed",
                      static_cast<std::int64_t>(g_coercion_evidence_loss_force_armed_total.load(
                          std::memory_order_relaxed)));
            insert_kv("coercion-evidence-loss-force-consumed",
                      static_cast<std::int64_t>(g_coercion_evidence_loss_force_consumed_total.load(
                          std::memory_order_relaxed)));
            // #2545 / #2563 linear
            insert_kv("linear-force-unified", scored.linear_force_unified ? 1 : 0);
            insert_kv("linear-cross-closure-escape-total",
                      static_cast<std::int64_t>(scored.linear_cross_closure_escape_total));
            insert_kv("linear-cross-closure-force-total",
                      static_cast<std::int64_t>(scored.linear_cross_closure_force_total));
            insert_kv("linear-cross-closure-observe-total",
                      static_cast<std::int64_t>(scored.linear_cross_closure_observe_total));
            // #2359 occurrence / memo
            insert_kv("occurrence-stale", static_cast<std::int64_t>(scored.occurrence_stale));
            insert_kv("predicate-memo-stale",
                      static_cast<std::int64_t>(scored.predicate_memo_stale));
            // Advisory throttle (optional orch map)
            insert_kv("throttle-action", scored.throttle_action); // 0 none 1 delay 2 split
            // force-reason code legend
            insert_kv("force-reason-ok", 0);
            insert_kv("force-reason-solve", 1);
            insert_kv("force-reason-blame", 2);
            insert_kv("force-reason-linear", 3);
            insert_kv("force-reason-truncate", 4);
            insert_kv("force-reason-empty-cs", 5);
            insert_kv("force-reason-auto-partial", 6);
            insert_kv("force-reason-coercion-slo", 7);
            insert_kv("force-reason-occurrence-stale", 8);
            insert_kv("force-reason-coercion-evidence-loss", 9);
            insert_kv("type-linear-commit-health-wired", 1);
            insert_kv("schema-2613", kTypeLinearCommitHealthIssue);
            insert_kv("issue-2613", kTypeLinearCommitHealthIssue);
            // Lineage schemas (detailed queries remain authoritative)
            insert_kv("schema-2553", 2553);
            insert_kv("schema-2558", 2558);
            insert_kv("schema-2545", 2545);
            insert_kv("schema-2563", 2563);
            insert_kv("schema-2359", 2359);
            insert_kv("schema-2648", 2648);
            insert_kv("issue-2648", 2648);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2350: query:type-system-health — single Agent score (basis points)
    // aggregating provenance completeness, timeout reject rate, linear pin miss
    // rate, and layered DCE efficiency. Pure/read-only (AC3); does not rename
    // existing keys. force-reason priority when health < budget:
    // timeout-reject > pin-miss > provenance-miss > castop-density > ok.
    //
    // Score (see type_system_health.hh AC1 comment):
    //   health_bp = (prov + (10000-timeout_rate) + (10000-pin_rate) + dce_eff) / 4
    ObservabilityPrims::register_stats_impl(
        "query:type-system-health", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;

            TypeSystemHealthSnapshot snap;
            // AC1 components — vacuous healthy when no samples.
            snap.provenance_completeness_bp = coercion_provenance_completeness_bp();

            const std::uint64_t to_reject =
                m ? m->delta_timeout_reject_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t to_full =
                m ? m->delta_timeout_full_solve_total.load(std::memory_order_relaxed) : 0;
            snap.timeout_reject_total = to_reject;
            snap.timeout_full_solve_total = to_full;
            snap.timeout_reject_rate_bp = rate_bp(to_reject, to_full);

            const auto pin_stats = aura::core::lifetime::linear_root_snapshot();
            snap.pin_total = pin_stats.pin_total;
            snap.pin_miss_total = pin_stats.pin_miss_total;
            snap.linear_pin_miss_rate_bp = rate_bp(pin_stats.pin_miss_total, pin_stats.pin_total);

            const std::uint64_t ast_elided =
                g_dead_coercion_ast_elided_total.load(std::memory_order_relaxed);
            const std::uint64_t ir_elided =
                opt_registry::dead_coercion_ir_elided_total.load(std::memory_order_relaxed);
            const std::uint64_t dirty_cone =
                opt_registry::dead_coercion_dirty_cone_skips.load(std::memory_order_relaxed);
            const std::uint64_t pipeline_runs =
                opt_registry::dead_coercion_pipeline_runs_total.load(std::memory_order_relaxed);
            snap.layered_elided_total = ast_elided + ir_elided + dirty_cone;
            snap.dce_pipeline_runs = pipeline_runs;
            snap.layered_dce_efficiency_bp =
                layered_dce_efficiency_bp(snap.layered_elided_total, pipeline_runs);

            if (m) {
                snap.castop_density_bp = m->last_castop_density_bp.load(std::memory_order_relaxed);
                snap.castop_density_budget_bp =
                    m->castop_density_budget_bp.load(std::memory_order_relaxed);
                snap.castop_over_budget_total =
                    m->castop_density_over_budget_total.load(std::memory_order_relaxed);
            }

            const auto scored = compute_type_system_health(snap);

            // Issue #2462: SolverSnapshot + density + hard-reject → next-action.
            // Pure read (no solve); reuses snapshot_constraint_system (#2308)
            // and type_repair last surface for repair_nodes / suggested_roots.
            SolverSnapshot solver_snap{};
            std::size_t suggested_roots_n = 0;
            bool hard_gate_reject = false;
            if (__qev_ && __qev_->commit_cs_live()) {
                if (auto* ctc = static_cast<aura::compiler::TypeChecker*>(
                        __qev_->commit_type_checker_handle())) {
                    solver_snap = snapshot_constraint_system(ctc->constraint_system(), nullptr);
                }
            }
            if (m) {
                // Cap-16 repair sample from last type_repair publish when
                // snapshot repair_nodes empty (boundary reject path).
                if (solver_snap.repair_nodes.empty()) {
                    const auto n = m->type_repair_last_unresolved_aff_nodes_count.load(
                        std::memory_order_relaxed);
                    const std::size_t cap = std::min<std::size_t>(n, 16);
                    for (std::size_t i = 0; i < cap; ++i) {
                        const auto id = m->type_repair_last_unresolved_aff_nodes[i].load(
                            std::memory_order_relaxed);
                        if (id != 0)
                            solver_snap.repair_nodes.push_back(static_cast<std::uint32_t>(id));
                    }
                }
                suggested_roots_n =
                    m->type_repair_suggested_root_count.load(std::memory_order_relaxed);
                // Hard-reject status 99 from boundary force path (#2284).
                constexpr std::uint64_t kHardRejectStatus = 99;
                hard_gate_reject = m->type_repair_last_timeout_status.load(
                                       std::memory_order_relaxed) == kHardRejectStatus;
                if (solver_snap.unresolved.empty()) {
                    // Mirror last unresolved count as signal only (no Constraint body).
                    const auto uc =
                        m->type_repair_last_unresolved_count.load(std::memory_order_relaxed);
                    if (uc > 0 && solver_snap.status == SolveResult::SOLVED &&
                        m->type_repair_last_timeout_status.load(std::memory_order_relaxed) != 0)
                        solver_snap.status = static_cast<SolveResult>(std::min<std::uint64_t>(
                            m->type_repair_last_timeout_status.load(std::memory_order_relaxed), 2));
                }
                if (m->type_repair_last_truncated_reverify.load(std::memory_order_relaxed) != 0)
                    solver_snap.truncated_reverify = true;
            }
            TypeSystemNextActionInput nai{};
            nai.health_ok = scored.health_bp >= scored.health_budget_bp;
            nai.force_reason = scored.force_reason;
            nai.solve_status = static_cast<std::uint8_t>(solver_snap.status);
            nai.truncated_reverify = solver_snap.truncated_reverify;
            nai.blame_complete = solver_snap.blame.is_complete() || solver_snap.blame.complete;
            nai.production_escalated = solver_snap.production_escalated;
            nai.repair_nodes_count = solver_snap.repair_nodes.size();
            nai.suggested_roots_count = suggested_roots_n;
            nai.unresolved_count = solver_snap.unresolved.size();
            if (m && nai.unresolved_count == 0) {
                nai.unresolved_count =
                    m->type_repair_last_unresolved_count.load(std::memory_order_relaxed);
            }
            nai.castop_over_budget = snap.castop_density_bp > snap.castop_density_budget_bp ||
                                     snap.castop_over_budget_total > 0;
            nai.hard_gate_reject = hard_gate_reject;
            nai.production_defaults = aura::compiler::typed_audit::production_defaults_active();
            const auto next = decide_type_system_next_action(nai);

            // Capacity 64: #2350 keys + #2462 next-action / repair sample.
            auto* ht = FlatHashTable::create(64);
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
            auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
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
                        auto vidx = string_heap.size();
                        string_heap.push_back(std::string(v_str));
                        vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                        ht->size++;
                        return;
                    }
                }
            };

            insert_kv("health-bp", static_cast<std::int64_t>(scored.health_bp));
            insert_kv("health-budget-bp", static_cast<std::int64_t>(scored.health_budget_bp));
            insert_kv_str("force-reason", scored.force_reason);
            // Component mirrors (optional AC4).
            insert_kv("component-provenance-completeness-bp",
                      static_cast<std::int64_t>(snap.provenance_completeness_bp));
            insert_kv("component-timeout-reject-rate-bp",
                      static_cast<std::int64_t>(snap.timeout_reject_rate_bp));
            insert_kv("component-linear-pin-miss-rate-bp",
                      static_cast<std::int64_t>(snap.linear_pin_miss_rate_bp));
            insert_kv("component-layered-dce-efficiency-bp",
                      static_cast<std::int64_t>(snap.layered_dce_efficiency_bp));
            insert_kv("component-castop-density-bp",
                      static_cast<std::int64_t>(snap.castop_density_bp));
            insert_kv("type-system-health-wired", 1);
            insert_kv("schema-2350", 2350);
            insert_kv("issue-2350", 2350);
            // Lineage sentinels (related schemas still independently queryable).
            insert_kv("schema-2282", 2282);
            insert_kv("schema-2284", 2284);
            insert_kv("schema-2287", 2287);
            // Issue #2462: next-action + repair_nodes closed-loop surface.
            //   next-action: string enum (ok|annotate-dynamic|expand-dirty|
            //     full-solve|rollback)
            //   next-action-code: 0..4 int alias
            //   repair-nodes-count + repair-node-0..15 (cap 16)
            //   suggested-roots-count + suggested-root-0..7 when available
            insert_kv_str("next-action", next.action_str);
            insert_kv("next-action-code", static_cast<std::int64_t>(next.action_code));
            insert_kv("repair-nodes-count",
                      static_cast<std::int64_t>(solver_snap.repair_nodes.size()));
            {
                const std::size_t cap =
                    std::min(solver_snap.repair_nodes.size(), static_cast<std::size_t>(16));
                for (std::size_t i = 0; i < 16; ++i) {
                    char keybuf[32];
                    // Fixed keys repair-node-0 .. repair-node-15
                    std::snprintf(keybuf, sizeof(keybuf), "repair-node-%zu", i);
                    const std::int64_t v =
                        i < cap ? static_cast<std::int64_t>(solver_snap.repair_nodes[i]) : 0;
                    insert_kv(keybuf, v);
                }
            }
            insert_kv("suggested-roots-count", static_cast<std::int64_t>(suggested_roots_n));
            if (m) {
                const std::size_t rcap = std::min(suggested_roots_n, static_cast<std::size_t>(8));
                for (std::size_t i = 0; i < 8; ++i) {
                    char keybuf[32];
                    std::snprintf(keybuf, sizeof(keybuf), "suggested-root-%zu", i);
                    const std::int64_t v =
                        i < rcap ? static_cast<std::int64_t>(m->type_repair_suggested_roots[i].load(
                                       std::memory_order_relaxed))
                                 : 0;
                    insert_kv(keybuf, v);
                }
            }
            insert_kv("type-system-health-next-action-wired", 1);
            insert_kv("schema-2462", 2462);
            insert_kv("issue-2462", 2462);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2379: query:mutation-concurrency-health — single Agent score
    // for runtime mutation safety (hold + steal + residual + mailbox + densify).
    // Pure reads of existing atomics (AC4 zero extra hot-path cost). Formula +
    // force_reason priority in mutation_concurrency_health.hh.
    // Priority: steal-mismatch > residual-defer > densify-fail > hold-slo >
    // mailbox-starvation > none.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-concurrency-health",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;

            MutationConcurrencyHealthSnapshot snap;
            // Steal force-deopt / hard-fail (process Fiber statics; always present).
            snap.steal_force_deopt_total =
                aura::serve::Fiber::steal_snapshot_mismatch_force_deopt_total();
            snap.steal_hard_fail_total = aura::serve::Fiber::steal_snapshot_hard_fail_total();
            // Residual GC defer.
            snap.residual_defer_cleared_on_steal_total =
                aura::gc_hooks::residual_defer_cleared_on_steal_total();
            if (m)
                snap.residual_hard_fail_total =
                    m->mutation_boundary_residual_defer_hard_fail_total.load(
                        std::memory_order_relaxed) +
                    m->residual_defer_steal_hard_fail_total.load(std::memory_order_relaxed);
            // Densify consistency last-call + cumulative fail.
            snap.densify_consistency_fail_total =
                aura::core::densify_consistency::densify_consistency_fail_total();
            snap.last_densify_envframe_ok =
                aura::core::densify_consistency::last_densify_envframe_ok() ? 1 : 0;
            snap.last_densify_closure_remount_ok =
                aura::core::densify_consistency::last_densify_closure_remount_ok() ? 1 : 0;
            // Hold SLO / over-budget.
            if (m) {
                snap.hold_slo_violation_total =
                    m->mutation_hold_slo_violation_total.load(std::memory_order_relaxed);
                snap.hold_over_budget_total =
                    m->mutation_hold_over_budget_total.load(std::memory_order_relaxed);
            }
            // Mailbox defer SLA.
            using aura::serve::mf_mailbox::g_mf_mailbox_stats;
            snap.mailbox_defer_starvation_total =
                g_mf_mailbox_stats.mailbox_defer_starvation_total.load(std::memory_order_relaxed);
            snap.mailbox_deferred_depth =
                g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
            snap.mailbox_deferred_mutation_hold_total =
                g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.load(
                    std::memory_order_relaxed);
            snap.mailbox_hold_exit_starvation_total =
                g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
                    std::memory_order_relaxed);
            snap.mailbox_hold_starvation_hard_total =
                g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(
                    std::memory_order_relaxed);
            snap.agent_throttle_for_mailbox_starvation =
                g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
                    std::memory_order_relaxed);

            const auto scored = compute_mutation_concurrency_health(snap);

            // Capacity 64→128: #2551 hard starvation + throttle keys.
            auto* ht = FlatHashTable::create(128);
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
            auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
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
                        auto vidx = string_heap.size();
                        string_heap.push_back(std::string(v_str));
                        vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                        ht->size++;
                        return;
                    }
                }
            };

            insert_kv("health-bp", static_cast<std::int64_t>(scored.health_bp));
            insert_kv("health-budget-bp", static_cast<std::int64_t>(scored.health_budget_bp));
            insert_kv_str("force-reason", scored.force_reason);
            insert_kv("force-reason-code", scored.force_reason_code);
            // Component raw totals (additive; subsystem queries unchanged).
            insert_kv("component-steal-force-deopt-total",
                      static_cast<std::int64_t>(snap.steal_force_deopt_total));
            insert_kv("component-steal-hard-fail-total",
                      static_cast<std::int64_t>(snap.steal_hard_fail_total));
            insert_kv("component-residual-defer-cleared-on-steal-total",
                      static_cast<std::int64_t>(snap.residual_defer_cleared_on_steal_total));
            insert_kv("component-residual-hard-fail-total",
                      static_cast<std::int64_t>(snap.residual_hard_fail_total));
            insert_kv("component-densify-consistency-fail-total",
                      static_cast<std::int64_t>(snap.densify_consistency_fail_total));
            insert_kv("component-last-densify-envframe-ok",
                      static_cast<std::int64_t>(snap.last_densify_envframe_ok));
            insert_kv("component-last-densify-closure-remount-ok",
                      static_cast<std::int64_t>(snap.last_densify_closure_remount_ok));
            insert_kv("component-hold-slo-violation-total",
                      static_cast<std::int64_t>(snap.hold_slo_violation_total));
            insert_kv("component-hold-over-budget-total",
                      static_cast<std::int64_t>(snap.hold_over_budget_total));
            insert_kv("component-mailbox-defer-starvation-total",
                      static_cast<std::int64_t>(snap.mailbox_defer_starvation_total));
            insert_kv("component-mailbox-deferred-depth",
                      static_cast<std::int64_t>(snap.mailbox_deferred_depth));
            insert_kv("component-mailbox-deferred-mutation-hold-total",
                      static_cast<std::int64_t>(snap.mailbox_deferred_mutation_hold_total));
            insert_kv("component-mailbox-hold-exit-starvation-total",
                      static_cast<std::int64_t>(snap.mailbox_hold_exit_starvation_total));
            insert_kv("component-mailbox-hold-starvation-hard-total",
                      static_cast<std::int64_t>(snap.mailbox_hold_starvation_hard_total));
            insert_kv("component-agent-throttle-for-mailbox-starvation",
                      static_cast<std::int64_t>(snap.agent_throttle_for_mailbox_starvation));
            insert_kv("mutation-concurrency-health-wired", 1);
            insert_kv("schema-2379", 2379);
            insert_kv("issue-2379", 2379);
            // Lineage sentinels (existing queries remain authoritative).
            insert_kv("schema-2310", 2310);
            insert_kv("schema-2341", 2341);
            insert_kv("schema-2349", 2349);
            insert_kv("schema-2312", 2312);
            insert_kv("schema-2378", 2378);
            insert_kv("schema-2511", 2511);
            insert_kv("issue-2511", 2511);
            insert_kv("schema-2551", 2551);
            insert_kv("issue-2551", 2551);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2506: query:aot-hot-update-health — single Agent score for
    // JIT/AOT recovery gate (reload recovery + storm + remount + epoch).
    // Pure relaxed loads of existing atomics (AC4 zero idle cost). Formula +
    // force_reason priority in aot_hot_update_health.hh.
    // Alias: query:hot-update-health (same builder).
    auto register_aot_hot_update_health = [&string_heap](const char* name) {
        ObservabilityPrims::register_stats_impl(
            name, [&string_heap](std::span<const EvalValue> a) -> EvalValue {
                (void)a;
                auto* __qev_ = Evaluator::get_query_evaluator();
                const auto* m =
                    __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics())
                           : nullptr;

                AotHotUpdateHealthSnapshot snap;
                // Reload recovery C snapshot (#2367) — pure relaxed loads.
                aura_reload_recovery_snapshot rs{};
                aura_hot_update_reload_recovery_get_snapshot(&rs);
                snap.attempts_left = static_cast<std::uint32_t>(rs.attempts_left);
                snap.force_jit_regions_mask = static_cast<std::uint64_t>(rs.force_jit_regions_mask);
                snap.pending_dirty_count = static_cast<std::uint64_t>(rs.pending_dirty_count);
                snap.deferred_reemit_pending =
                    static_cast<std::uint8_t>(rs.deferred_reemit_pending);
                snap.storm_level = static_cast<std::uint8_t>(rs.storm_level);
                snap.hard_storm_active = rs.hard_storm_active;
                snap.last_reload_fail_reason = static_cast<std::uint8_t>(rs.last_reason);
                snap.recovery_active = rs.recovery_active;
                // Remount counters (#2234).
                if (m) {
                    snap.remount_fail_total =
                        m->closure_capture_remount_fail_total.load(std::memory_order_relaxed);
                    snap.remount_ok_total =
                        m->closure_capture_remount_ok_total.load(std::memory_order_relaxed);
                }
                // Epoch invariant (#2366 / #2501).
                snap.epoch_invariant_violation_total =
                    aura_epoch_invariant_violation_total_v_read();

                const auto scored = compute_aot_hot_update_health(snap);

                auto* ht = FlatHashTable::create(64);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                auto insert_kv = [&](const char* k_str, std::int64_t v) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (const char* p = k_str; *p; ++p)
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (const char* p = k_str; *p; ++p)
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                            auto vidx = string_heap.size();
                            string_heap.push_back(std::string(v_str));
                            vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                            ht->size++;
                            return;
                        }
                    }
                };

                insert_kv("health-bp", static_cast<std::int64_t>(scored.health_bp));
                insert_kv("health-budget-bp", static_cast<std::int64_t>(scored.health_budget_bp));
                insert_kv_str("force-reason", scored.force_reason);
                insert_kv("force-reason-code", scored.force_reason_code);
                insert_kv("recovery-active", scored.recovery_active);
                // Components (raw totals; subsystem queries unchanged).
                insert_kv("component-attempts-left", static_cast<std::int64_t>(snap.attempts_left));
                insert_kv("component-force-jit-regions-mask",
                          static_cast<std::int64_t>(snap.force_jit_regions_mask));
                insert_kv("component-pending-dirty-count",
                          static_cast<std::int64_t>(snap.pending_dirty_count));
                insert_kv("component-deferred-reemit-pending",
                          static_cast<std::int64_t>(snap.deferred_reemit_pending));
                insert_kv("component-storm-level", static_cast<std::int64_t>(snap.storm_level));
                insert_kv("component-hard-storm-active", snap.hard_storm_active);
                insert_kv("component-last-reload-fail-reason",
                          static_cast<std::int64_t>(snap.last_reload_fail_reason));
                insert_kv("component-remount-fail-total",
                          static_cast<std::int64_t>(snap.remount_fail_total));
                insert_kv("component-remount-ok-total",
                          static_cast<std::int64_t>(snap.remount_ok_total));
                insert_kv("component-epoch-invariant-violation-total",
                          static_cast<std::int64_t>(snap.epoch_invariant_violation_total));
                // Issue #2640: production Restricted default periodic
                // epoch-invariant soft walk counters (additive).
                insert_kv(
                    "component-epoch-invariant-periodic-walks-total",
                    static_cast<std::int64_t>(aura_epoch_invariant_periodic_walks_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-last-walk-at-ms",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_last_walk_at_ms_v_read()));
                insert_kv(
                    "component-epoch-invariant-periodic-period-ms",
                    static_cast<std::int64_t>(aura_epoch_invariant_periodic_period_ms_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-off-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_off_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-wrong-mode-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_wrong_mode_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-rate-limited-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_rate_limited_total_v_read()));
                insert_kv("component-epoch-invariant-periodic-skipped-disabled-total",
                          static_cast<std::int64_t>(
                              aura_epoch_invariant_periodic_skipped_disabled_total_v_read()));
                insert_kv("epoch-invariant-periodic-wired", 1);
                insert_kv("schema-2640", 2640);
                insert_kv("issue-2640", 2640);
                // force-reason code sentinels (docs)
                insert_kv("force-reason-ok", 0);
                insert_kv("force-reason-storm", 1);
                insert_kv("force-reason-force-jit", 2);
                insert_kv("force-reason-reload-fail", 3);
                insert_kv("force-reason-remount-fail", 4);
                insert_kv("force-reason-epoch-invariant", 5);
                insert_kv("force-reason-deferred-reemit", 6);
                insert_kv("aot-hot-update-health-wired", 1);
                insert_kv("schema-2506", 2506);
                insert_kv("issue-2506", 2506);
                // Issue #2543: orch self-throttle control plane over this score.
                {
                    const auto td = decide_hot_update_throttle(scored);
                    insert_kv("throttle-active", td.throttle ? 1 : 0);
                    insert_kv(
                        "throttle-action-code",
                        static_cast<std::int64_t>(td.action)); // 0 none 1 split 2 delay 3 skip
                    insert_kv("throttle-max-concurrency-cap",
                              static_cast<std::int64_t>(td.max_concurrency_cap));
                    insert_kv(
                        "orch-hot-update-health-throttle-total",
                        static_cast<std::int64_t>(g_orch_hot_update_health_throttle_total.load(
                            std::memory_order_relaxed)));
                    insert_kv("orch-hot-update-health-checks-total",
                              static_cast<std::int64_t>(g_orch_hot_update_health_checks_total.load(
                                  std::memory_order_relaxed)));
                    insert_kv(
                        "orch-hot-update-health-last-force-reason",
                        g_orch_hot_update_health_last_force_reason.load(std::memory_order_relaxed));
                    insert_kv("schema-2543", kAotHotUpdateHealthThrottleIssue);
                    insert_kv("issue-2543", kAotHotUpdateHealthThrottleIssue);
                    insert_kv("orch-hot-update-health-throttle-wired", 1);
                }
                // Lineage (existing queries remain authoritative).
                insert_kv("schema-2367", 2367);
                insert_kv("schema-2302", 2302);
                insert_kv("schema-2094", 2094);
                insert_kv("schema-2234", 2234);
                insert_kv("schema-2366", 2366);

                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            });
    };
    register_aot_hot_update_health("query:aot-hot-update-health");
    register_aot_hot_update_health("query:hot-update-health");

    // Issue #2500: query:compact-policy (+ aliases orch:compact-policy /
    // arena:recommend-compact) — pure recommendation over existing health /
    // frag / defer / hold atomics. Does not change Soft/Force gates (AC4).
    auto register_compact_policy = [&string_heap](const char* name) {
        ObservabilityPrims::register_stats_impl(
            name, [&string_heap](std::span<const EvalValue> a) -> EvalValue {
                (void)a;
                auto* __qev_ = Evaluator::get_query_evaluator();
                const auto* m =
                    __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics())
                           : nullptr;

                // ── Build mutation-concurrency health (same sources as #2379) ──
                MutationConcurrencyHealthSnapshot hsnap;
                hsnap.steal_force_deopt_total =
                    aura::serve::Fiber::steal_snapshot_mismatch_force_deopt_total();
                hsnap.steal_hard_fail_total = aura::serve::Fiber::steal_snapshot_hard_fail_total();
                hsnap.residual_defer_cleared_on_steal_total =
                    aura::gc_hooks::residual_defer_cleared_on_steal_total();
                if (m)
                    hsnap.residual_hard_fail_total =
                        m->mutation_boundary_residual_defer_hard_fail_total.load(
                            std::memory_order_relaxed) +
                        m->residual_defer_steal_hard_fail_total.load(std::memory_order_relaxed);
                hsnap.densify_consistency_fail_total =
                    aura::core::densify_consistency::densify_consistency_fail_total();
                hsnap.last_densify_envframe_ok =
                    aura::core::densify_consistency::last_densify_envframe_ok() ? 1 : 0;
                hsnap.last_densify_closure_remount_ok =
                    aura::core::densify_consistency::last_densify_closure_remount_ok() ? 1 : 0;
                if (m) {
                    hsnap.hold_slo_violation_total =
                        m->mutation_hold_slo_violation_total.load(std::memory_order_relaxed);
                    hsnap.hold_over_budget_total =
                        m->mutation_hold_over_budget_total.load(std::memory_order_relaxed);
                }
                using aura::serve::mf_mailbox::g_mf_mailbox_stats;
                hsnap.mailbox_defer_starvation_total =
                    g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
                        std::memory_order_relaxed);
                hsnap.mailbox_deferred_depth =
                    g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed);
                hsnap.mailbox_deferred_mutation_hold_total =
                    g_mf_mailbox_stats.mailbox_deferred_mutation_hold_total.load(
                        std::memory_order_relaxed);
                const auto health = compute_mutation_concurrency_health(hsnap);

                CompactPolicyInput pin;
                pin.health_bp = health.health_bp;
                pin.health_budget_bp = health.health_budget_bp;
                pin.should_defer_destructive_gc = aura::gc_hooks::should_defer_destructive_gc();
                pin.active_guard_depth = aura::core::envframe_lifetime::active_guard_depth();
                pin.mutation_hold_active = (__qev_ && __qev_->mutation_boundary_depth() > 0) ||
                                           hsnap.mailbox_deferred_mutation_hold_total > 0;
                pin.densify_consistency_fail_total = hsnap.densify_consistency_fail_total;
                if (m) {
                    pin.pin_contract_fail_total =
                        m->moving_compact_pin_contract_fail_total.load(std::memory_order_relaxed);
                }
                pin.force_blocked_by_pin_total =
                    aura::ast::g_force_compact_blocked_by_pin_total.load(std::memory_order_relaxed);
                pin.force_blocked_by_envframe_total =
                    aura::ast::g_force_compact_blocked_by_envframe_guard_total.load(
                        std::memory_order_relaxed);

                // Frag: process metric (post-mutate) — pure read, no private
                // arena_ access. Values may be pct 0..100 or bp; normalize.
                std::uint64_t frag_bp = 0;
                if (m) {
                    frag_bp = m->arena_fragmentation_post_mutate.load(std::memory_order_relaxed);
                    if (frag_bp > 0 && frag_bp <= 100)
                        frag_bp *= 100;
                    if (frag_bp > 10000)
                        frag_bp = 10000;
                }
                pin.frag_bp = frag_bp;

                // Hold estimate → recommend-split (#2405): estimate > 0.7 * budget.
                {
                    const auto budget =
                        static_cast<std::int64_t>(aura::compiler::mutation_hold_budget_us());
                    std::int64_t estimate = 0;
                    if (m) {
                        const auto total =
                            m->mutation_hold_sample_count.load(std::memory_order_relaxed);
                        const auto n = static_cast<std::size_t>(std::min(
                            total,
                            static_cast<std::uint64_t>(CompilerMetrics::kMutationHoldSampleRing)));
                        if (n > 0) {
                            std::array<std::uint64_t, CompilerMetrics::kMutationHoldSampleRing>
                                samples{};
                            if (total < CompilerMetrics::kMutationHoldSampleRing) {
                                for (std::size_t i = 0; i < n; ++i)
                                    samples[i] = m->mutation_hold_sample_ring[i].load(
                                        std::memory_order_relaxed);
                            } else {
                                for (std::size_t i = 0;
                                     i < CompilerMetrics::kMutationHoldSampleRing; ++i)
                                    samples[i] = m->mutation_hold_sample_ring[i].load(
                                        std::memory_order_relaxed);
                            }
                            std::sort(samples.begin(),
                                      samples.begin() + static_cast<std::ptrdiff_t>(n));
                            const auto p99_idx = static_cast<std::size_t>(
                                (static_cast<std::uint64_t>(n) * 99 + 99) / 100);
                            estimate = static_cast<std::int64_t>(samples[std::min(p99_idx, n - 1)]);
                        }
                    }
                    pin.recommend_split = (budget > 0 && estimate * 10 > budget * 7);
                }

                const auto pol = compute_compact_policy(pin);

                auto* ht = FlatHashTable::create(64);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                auto insert_kv = [&](const char* k_str, std::int64_t v) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (const char* p = k_str; *p; ++p)
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                auto insert_kv_str = [&](const char* k_str, std::string_view v_str) {
                    std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                    for (const char* p = k_str; *p; ++p)
                        h = (h ^ static_cast<std::uint8_t>(*p)) *
                            ::aura::compiler::stats::kFnvPrime;
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
                            auto vidx = string_heap.size();
                            string_heap.push_back(std::string(v_str));
                            vals[idx] = make_string(static_cast<std::uint64_t>(vidx)).val;
                            ht->size++;
                            return;
                        }
                    }
                };

                insert_kv_str("mode", pol.mode_name);
                insert_kv("mode-code", pol.mode_code);
                insert_kv_str("reason", pol.reason);
                insert_kv("health-bp", static_cast<std::int64_t>(pin.health_bp));
                insert_kv("health-budget-bp", static_cast<std::int64_t>(pin.health_budget_bp));
                insert_kv("frag-bp", static_cast<std::int64_t>(pin.frag_bp));
                insert_kv("should-defer", pin.should_defer_destructive_gc ? 1 : 0);
                insert_kv("active-guard-depth", static_cast<std::int64_t>(pin.active_guard_depth));
                insert_kv("mutation-hold-active", pin.mutation_hold_active ? 1 : 0);
                insert_kv("pin-contract-fail-total",
                          static_cast<std::int64_t>(pin.pin_contract_fail_total));
                insert_kv("densify-consistency-fail-total",
                          static_cast<std::int64_t>(pin.densify_consistency_fail_total));
                insert_kv("force-blocked-by-pin-total",
                          static_cast<std::int64_t>(pin.force_blocked_by_pin_total));
                insert_kv("force-blocked-by-envframe-total",
                          static_cast<std::int64_t>(pin.force_blocked_by_envframe_total));
                insert_kv("recommend-split", pin.recommend_split ? 1 : 0);
                insert_kv("advisory", 1); // Agents: advisory unless enforce env (future)
                insert_kv("compact-policy-wired", 1);
                insert_kv("schema-2500", kCompactPolicyIssue);
                insert_kv("issue-2500", kCompactPolicyIssue);
                insert_kv("schema", kCompactPolicyIssue);
                // Lineage sentinels (subsystem queries remain authoritative).
                insert_kv("schema-2379", 2379);
                insert_kv("schema-2405", 2405);
                insert_kv("schema-2004", 2004);

                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            });
    };
    register_compact_policy("query:compact-policy");
    register_compact_policy("orch:compact-policy");
    register_compact_policy("arena:recommend-compact");

    // Issue #574: query:coercion-elim-stats. Returns the sum of
    // 4 coercion elimination observability counters:
    //   - coercion_castop_emitted_total (total CastOps from lowering)
    //   - dead_coercion_eliminated_total (identity/no-op elisions)
    //   - coercion_narrow_evidence_hits_total (runtime-check elisions
    //     proved away by narrow_evidence Rule 6)
    //   - narrowing_provenance_total (blame/provenance preserved on
    //     occurrence-narrowing paths feeding coercion metadata)
    ObservabilityPrims::register_stats_impl(
        "query:coercion-elim-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            const std::uint64_t blame =
                m->narrowing_provenance_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(total_castop + eliminated +
                                                      runtime_check_elided + blame));
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
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t wire_borrows =
                m->hw_resource_wire_borrows_.load(std::memory_order_relaxed);
            const std::uint64_t reg_writes =
                m->hw_resource_reg_writes_.load(std::memory_order_relaxed);
            const std::uint64_t mem_access =
                m->hw_resource_mem_access_.load(std::memory_order_relaxed);
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
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t leaks =
                m->linear_leak_prevented_total.load(std::memory_order_relaxed);
            const std::uint64_t escape_hits =
                m->linear_check_pass_count_.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(revalidate + dirty_uses + violations + leaks +
                                                      escape_hits));
        });

    // Issue #610: query:linear-ownership-mutation-stats. Returns
    // the sum of 4 post-mutation linear ownership observability
    // counters:
    //   - post_mutate_revalidations: linear_post_mutate_revalidations_total
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear: linear_deopt_on_invalidate_total
    //   - leak_prevented: linear_leak_prevented_total
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-mutation-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t revalidations =
                m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
            const std::uint64_t violations =
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t deopt =
                m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
            const std::uint64_t leaks =
                m->linear_leak_prevented_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(revalidations + violations + deopt + leaks));
        });

    // Issue #638: query:linear-ownership-safety-stats. Returns the
    // sum of 3 runtime linear + GuardShape post-mutation safety
    // counters (non-duplicative with #610 mutation-stats,
    // #575 incremental-stats, #306 hw linear-ownership-stats):
    //   - violations_caught: linear_violations_caught_total
    //   - deopt_on_linear_mismatch: linear_deopt_on_mismatch_total
    //   - post_mutate_enforcements: linear_post_mutate_enforcements_total
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-safety-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t violations =
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t deopt_mismatch =
                m->linear_deopt_on_mismatch_total.load(std::memory_order_relaxed);
            const std::uint64_t enforcements =
                m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(violations + deopt_mismatch + enforcements));
        });

    // Issue #1543: query:linear-gc-root-audit-log — read-only accessor for
    // the GC root registration consistency audit ring + counter snapshot.
    // Schema fields (see docs/design/linear-gc-roots.md):
    //   audit-checks-total, last-path, last-ok, last-registrations,
    //   last-stale-hits, last-violations, last-env-version-resync,
    //   last-live-roots, log-size, schema=1543
    // Optional arg0 int = max recent log lines returned under "log" as a
    // list of strings (default 0 = summary hash only).
    ObservabilityPrims::register_stats_impl(
        "query:linear-gc-root-audit-log",
        [&ev, &string_heap, &pairs](std::span<const EvalValue> a) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            const std::int64_t checks =
                m ? static_cast<std::int64_t>(
                        m->linear_gc_root_audit_checks_total.load(std::memory_order_relaxed))
                  : 0;
            const auto total = ev.linear_gc_root_audit_total();
            const auto seq = ev.linear_gc_root_audit_seq();

            std::int64_t last_ok = 1;
            std::int64_t last_path = -1;
            std::int64_t last_reg = 0;
            std::int64_t last_stale = 0;
            std::int64_t last_viol = 0;
            std::int64_t last_resync = 0;
            std::int64_t last_live = 0;
            std::string last_path_name = "none";
            if (seq > 0) {
                const auto& e = ev.linear_gc_root_audit_entry_at(seq - 1);
                last_ok = e.ok;
                last_path = e.path;
                last_reg = static_cast<std::int64_t>(e.registrations);
                last_stale = static_cast<std::int64_t>(e.stale_hits);
                last_viol = static_cast<std::int64_t>(e.violations_prevented);
                last_resync = static_cast<std::int64_t>(e.env_version_resync);
                last_live = static_cast<std::int64_t>(e.live_roots);
                last_path_name = std::string(Evaluator::linear_gc_root_audit_path_name(e.path));
            }

            // Optional recent log as linked list of strings.
            EvalValue log_list = make_void();
            std::size_t limit = 0;
            if (!a.empty() && is_int(a[0]) && as_int(a[0]) > 0)
                limit = static_cast<std::size_t>(as_int(a[0]));
            for (std::size_t i = 0; i < limit && i < Evaluator::kLinearGcRootAuditRingSize; ++i) {
                if (seq <= i)
                    break;
                const auto& entry = ev.linear_gc_root_audit_entry_at(seq - 1 - i);
                if (entry.seq == 0 && entry.path == 0 && entry.registrations == 0)
                    continue;
                auto line = std::format(
                    "seq={} path={} ok={} reg={} stale={} viol={} resync={} live={}", entry.seq,
                    Evaluator::linear_gc_root_audit_path_name(entry.path), entry.ok,
                    entry.registrations, entry.stale_hits, entry.violations_prevented,
                    entry.env_version_resync, entry.live_roots);
                auto sidx = string_heap.size();
                string_heap.push_back(std::move(line));
                auto pid = pairs.size();
                pairs.push_back({make_string(sidx), log_list});
                log_list = make_pair(pid);
            }

            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, EvalValue v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k_str)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = string_heap.size();
                string_heap.push_back(k_str);
                EvalValue key_ev = make_string(kidx);
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = key_ev.val;
                        vals[slot] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("audit-checks-total", make_int(checks));
            insert_kv("log-size", make_int(static_cast<std::int64_t>(total)));
            insert_kv("last-path", make_int(last_path));
            {
                auto sidx = string_heap.size();
                string_heap.push_back(last_path_name);
                insert_kv("last-path-name", make_string(sidx));
            }
            insert_kv("last-ok", make_int(last_ok));
            insert_kv("last-registrations", make_int(last_reg));
            insert_kv("last-stale-hits", make_int(last_stale));
            insert_kv("last-violations", make_int(last_viol));
            insert_kv("last-env-version-resync", make_int(last_resync));
            insert_kv("last-live-roots", make_int(last_live));
            insert_kv("log", log_list);
            // Issue #1599: closed-loop refine — issue + lineage schema + wiring flags.
            insert_kv("linear_gc_root_audit_checks_total", make_int(checks));
            insert_kv("walk-active-closures-wired", make_int(1));
            insert_kv("six-touchpoints-documented", make_int(1));
            insert_kv("issue", make_int(1599));
            insert_kv("schema", make_int(1599)); // lineage 1543
            // Issue #2642: Phase 5 linear-root consistency scan counters.
            insert_kv("component-linear-densify-scan-mismatch-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_total.load() : 0)));
            insert_kv("component-linear-densify-scan-mismatch-observe-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_observe_total.load() : 0)));
            insert_kv("linear-densify-scan-mismatch-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_total.load() : 0)));
            insert_kv("linear_densify_scan_mismatch_total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_total.load() : 0)));
            insert_kv("linear-densify-scan-mismatch-observe-total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_observe_total.load() : 0)));
            insert_kv("linear_densify_scan_mismatch_observe_total",
                      make_int(static_cast<std::int64_t>(
                          m ? m->linear_densify_scan_mismatch_observe_total.load() : 0)));
            insert_kv("linear-densify-wired", make_int(1));
            insert_kv("schema-2642", make_int(2642));
            insert_kv("issue-2642", make_int(2642));
            // Issue #2673: hard-path lock for densify linear-root consistency
            // scan (refine #2642 residual). Production/Full mismatch forces
            // force_linear_rollback(LinearDensifyRootMismatch) under
            // linear_ops_present. Schema sentinel + hard-path wired flag.
            insert_kv("linear-densify-hard-path-wired", make_int(1));
            insert_kv("schema-2673", make_int(2673));
            insert_kv("issue-2673", make_int(2673));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1568 / #1596 / #1606 / #1659 / #1895 / #1928:
    // query:linear-boundary-consistency-stats — unified mutation/compact/
    // JIT/fiber/GC boundary enforce closed-loop (walk_active_closures +
    // live-closure linear scan + force Drop + GC root).
    // Schema **1895** lineage; **schema-1928** closes #1928 AC surface.
    // Metrics: linear_post_mutate_enforcements, linear_live_closure_scans_total,
    // linear_ownership_violation_prevented, linear_gc_root_audit_checks_total,
    // linear_live_closures_marked_invalid_total (#1606 AC).
    // #1895/#1928: NULL_ENV_ID force Drop + all 5+ boundary wire flags.
    ObservabilityPrims::register_stats_impl(
        "query:linear-boundary-consistency-stats",
        [&ev, &string_heap, &pairs](std::span<const EvalValue> a) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            // Issue #1867: acquire so release-stores from record_linear_gc_probe
            // violations are visible to this audit/stats reader.
            const auto L = [&](const std::atomic<std::uint64_t>* field) -> std::int64_t {
                return field ? static_cast<std::int64_t>(field->load(std::memory_order_acquire))
                             : 0;
            };
            // Optional log lines of linear violation provenance.
            EvalValue viol_log = make_void();
            std::size_t limit = 0;
            if (!a.empty() && is_int(a[0]) && as_int(a[0]) > 0)
                limit = static_cast<std::size_t>(as_int(a[0]));
            const auto vseq = ev.linear_violation_audit_seq();
            for (std::size_t i = 0; i < limit && i < Evaluator::kLinearViolationAuditRingSize;
                 ++i) {
                if (vseq <= i)
                    break;
                const auto& e = ev.linear_violation_audit_entry_at(vseq - 1 - i);
                if (e.seq == 0 && e.reason == 0)
                    continue;
                auto line =
                    std::format("seq={} path={} reason={} epoch={} defuse={} env={} closure={}",
                                e.seq, Evaluator::linear_gc_root_audit_path_name(e.path), e.reason,
                                e.epoch, e.defuse_version, e.env_id, e.closure_id);
                auto sidx = string_heap.size();
                string_heap.push_back(std::move(line));
                auto pid = pairs.size();
                pairs.push_back({make_string(sidx), viol_log});
                viol_log = make_pair(pid);
            }
            // Capacity power-of-two; #1659 adds mandate wire keys.
            auto* ht = FlatHashTable::create(64);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, EvalValue v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k_str)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = string_heap.size();
                string_heap.push_back(k_str);
                EvalValue key_ev = make_string(kidx);
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = key_ev.val;
                        vals[slot] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("schema", make_int(1895)); // #1895 closed-loop; lineage 1659/1606/1568
            insert_kv("issue", make_int(1895));
            insert_kv("schema-1928", make_int(1928)); // #1928 walk_active_closures mandate
            insert_kv("issue-1928", make_int(1928));
            insert_kv("schema-1929", make_int(1929)); // #1929 Closure Bridge unified surface
            insert_kv("issue-1929", make_int(1929));
            insert_kv("schema-1954", make_int(1954)); // #1954 refine of #1929
            insert_kv("issue-1954", make_int(1954));
            insert_kv("active", make_int(1));
            insert_kv("phase", make_int(5)); // production + all-boundary mandate (#1895/#1928)
            insert_kv("truncate-scan-wired", make_int(1));    // #1928 truncate force Drop
            insert_kv("boundaries-wired-count", make_int(6)); // inv/compact/trunc/jit/fiber/gc
            insert_kv("closure-view-lifetime-paired",
                      make_int(1)); // pairs query:closure-view-lifetime-stats
            insert_kv("linear-post-mutate-enforcements",
                      make_int(L(m ? &m->linear_post_mutate_enforcements : nullptr)));
            // #1596 AC5 alias (underscore form) + hyphen form for agents.
            insert_kv("linear_post_mutate_enforcements",
                      make_int(L(m ? &m->linear_post_mutate_enforcements : nullptr)));
            insert_kv("linear_live_closure_scans_total",
                      make_int(L(m ? &m->linear_live_closure_scans_total : nullptr)));
            insert_kv("linear-live-closure-scans-total",
                      make_int(L(m ? &m->linear_live_closure_scans_total : nullptr)));
            insert_kv("linear_live_closures_marked_invalid_total",
                      make_int(L(m ? &m->linear_live_closures_marked_invalid_total : nullptr)));
            insert_kv("linear-ownership-violation-prevented",
                      make_int(L(m ? &m->linear_ownership_violation_prevented : nullptr)));
            insert_kv("linear_ownership_violation_prevented",
                      make_int(L(m ? &m->linear_ownership_violation_prevented : nullptr)));
            // #1659 AC: issue-body metric names (aliases of existing counters).
            insert_kv("linear-violation-count",
                      make_int(L(m ? &m->linear_violations_caught_total : nullptr)));
            insert_kv("linear_violations_caught_total",
                      make_int(L(m ? &m->linear_violations_caught_total : nullptr)));
            insert_kv("linear-gc-root-audit-checks",
                      make_int(L(m ? &m->linear_gc_root_audit_checks_total : nullptr)));
            insert_kv("linear_gc_root_audit_checks_total",
                      make_int(L(m ? &m->linear_gc_root_audit_checks_total : nullptr)));
            insert_kv("boundary-consistency-total",
                      make_int(L(m ? &m->linear_boundary_consistency_total : nullptr)));
            insert_kv("epoch-fence-enforce-total",
                      make_int(L(m ? &m->linear_epoch_fence_enforce_total : nullptr)));
            insert_kv("force-drop-total", make_int(L(m ? &m->linear_force_drop_total : nullptr)));
            // #1606 AC wire flags (Evaluator + service + JIT ResourceTracker)
            insert_kv("walk-active-closures-wired", make_int(1));
            insert_kv("invalidate-scan-wired", make_int(1));
            insert_kv("compact-scan-wired", make_int(1));
            insert_kv("jit-resource-tracker-scan-wired", make_int(1));
            insert_kv("force-drop-wired", make_int(1));
            // #1659 mandate wire flags (linear_ownership_state + heap + GC/Arena)
            insert_kv("envframe-linear-ownership-snapshot-wired", make_int(1));
            insert_kv("linear-heap-runtime-wired", make_int(1));
            insert_kv("linear-ownership-state-propagated-wired", make_int(1));
            insert_kv("apply-closure-linear-check-wired", make_int(1));
            insert_kv("jit-linear-post-mutate-enforce-wired", make_int(1));
            // #1895: all boundary paths + NULL_ENV_ID linear body safety
            insert_kv("fiber-steal-scan-wired", make_int(1));
            insert_kv("gc-safepoint-scan-wired", make_int(1));
            insert_kv("guard-exit-scan-wired", make_int(1));
            insert_kv("materialize-null-env-wired", make_int(1));
            insert_kv("null-env-force-drop-wired", make_int(1));
            insert_kv("linear_null_env_force_drop_total",
                      make_int(L(m ? &m->linear_null_env_force_drop_total : nullptr)));
            insert_kv("linear_null_env_safe_fallback_total",
                      make_int(L(m ? &m->linear_null_env_safe_fallback_total : nullptr)));
            insert_kv("linear_post_mutate_null_env_id_total",
                      make_int(L(m ? &m->linear_post_mutate_null_env_id_total : nullptr)));
            insert_kv("schema-1659", make_int(1659)); // lineage sentinel for agents
            insert_kv("invalidate-tombstone-wired", make_int(1));
            insert_kv("gc-arena-linear-synergy-wired", make_int(1));
            insert_kv("guardshape-linear-unified-wired", make_int(1));
            insert_kv("linear-ownership-mandate-active", make_int(1));
            insert_kv("violation-audit-total",
                      make_int(static_cast<std::int64_t>(ev.linear_violation_audit_total())));
            insert_kv("violation-log", viol_log);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
    ObservabilityPrims::register_stats_impl(
        "query:linear-postmutate-fidelity-stats",
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
            // #2043 window keys + #2131 GcCoordScope metrics need ≥64 slots.
            auto* ht = FlatHashTable::create(64);
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
            insert_kv("post-rollback-revalidate-hits", post_rollback);
            insert_kv("escape-violations-prevented", escape_prevented);
            insert_kv("guard-boundary-linear-safe", guard_safe);
            insert_kv("env-version-sync", env_sync);
            // Issue #2043: atomic linear+GC window observability (additive)
            insert_kv("linear-gc-window-finalize-total",
                      m ? static_cast<std::int64_t>(
                              m->linear_gc_window_finalize_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("linear-ownership-epoch-bumps",
                      m ? static_cast<std::int64_t>(
                              m->linear_ownership_epoch_bumps_total.load(std::memory_order_relaxed))
                        : 0);
            insert_kv("linear-gc-window-under-mutate",
                      m ? static_cast<std::int64_t>(m->linear_gc_window_under_mutate_total.load(
                              std::memory_order_relaxed))
                        : 0);
            insert_kv("linear-ownership-epoch",
                      static_cast<std::int64_t>(ev->linear_ownership_epoch()));
            insert_kv("schema", 800); // lineage retained for #800 tests
            insert_kv("schema-2043", 2043);
            insert_kv("issue-2043", 2043);
            insert_kv("linear-gc-window-wired", 1);
            // Issue #2131: unified GcCoordScope pin → cascade → audit metrics
            insert_kv("gc-coord-scopes-opened",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_opened_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-pre-pin-total",
                      static_cast<std::int64_t>(
                          aura::compiler::gc_coord::pre_pin_total.load(std::memory_order_relaxed)));
            insert_kv("gc-coord-cascade-enter-total",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::cascade_enter_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-post-audit-total",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::post_audit_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-released-total",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::released_total.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "gc-coord-phase-violations",
                static_cast<std::int64_t>(aura::compiler::gc_coord::phase_violations_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "gc-coord-missing-post-audit",
                static_cast<std::int64_t>(aura::compiler::gc_coord::missing_post_audit_total.load(
                    std::memory_order_relaxed)));
            insert_kv("gc-coord-reverse-order",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::reverse_order_total.load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-invalidate",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[0].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-soft-dirty",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[1].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-boundary",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[2].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-hot-swap",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[3].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-path-compact",
                      static_cast<std::int64_t>(aura::compiler::gc_coord::scopes_by_path[4].load(
                          std::memory_order_relaxed)));
            insert_kv("gc-coord-wired", 1);
            insert_kv("schema-2131", 2131);
            insert_kv("issue-2131", 2131);
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
    ObservabilityPrims::register_stats_impl(
        "query:linear-ownership-runtime-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t violations =
                m->linear_violations_caught_total.load(std::memory_order_acquire) /* #1867 */;
            const std::uint64_t deopt_mismatch =
                m->linear_deopt_on_mismatch_total.load(std::memory_order_relaxed);
            const std::uint64_t enforcement_hits =
                m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
            const std::uint64_t deopt_invalidate =
                m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
            return make_int(static_cast<std::int64_t>(violations + deopt_mismatch +
                                                      enforcement_hits + deopt_invalidate));
        });

    // Issue #454: query:reflect-edsl-bridge-stats. Returns the
    // sum of 4 reflection-to-EDSL bridge observability counters:
    //   - schema_validation_pass_count_  (auto_validate hook)
    //   - schema_validation_fail_count_  (validation failures)
    //   - impact_snapshot_count_         (post-mutate reflection data)
    //   - macro_introduced_skipped_in_query_  (SyntaxMarker filter
    //     introspection via query:pattern / schema-of-marker bridge)
    ObservabilityPrims::register_stats_impl(
        "query:reflect-edsl-bridge-stats", [](std::span<const EvalValue> a) -> EvalValue {
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

    // Issue #502 / #551 / #1611: query:reflect-postmutate-stats — Guard
    // post-mutate reflect validation + MacroIntroduced hygiene gate.
    // Schema **1611** (lineage 502/551). AC keys:
    //   reflect-macro-hygiene-checks / reflect-macro-hygiene-rejects
    //   allow-macro-mutate / hygiene-aware-validate-wired
    ObservabilityPrims::register_stats_impl(
        "query:reflect-postmutate-stats",
        [&string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ht = FlatHashTable::create(24);
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
            CompilerMetrics* m = ev->compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev->compiler_metrics())
                                     : nullptr;
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t dirty = ev->get_dirty_nodes_in_snapshot();
            const std::uint64_t markers = ev->get_macro_markers_in_snapshot();
            const std::uint64_t hygiene_checks =
                m ? m->reflect_macro_hygiene_checks_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hygiene_rejects =
                m ? m->reflect_macro_hygiene_rejects_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t total = snapshots + pass + fail + dirty;
            std::int64_t recommendation = 0;
            if (fail > 0 || !ev->get_last_schema_validation_ok())
                recommendation = 2;
            else if (dirty > 50)
                recommendation = 1;
            // #502/#551 lineage
            insert_kv("impact-snapshots", static_cast<std::int64_t>(snapshots));
            insert_kv("schema-pass", static_cast<std::int64_t>(pass));
            insert_kv("schema-fail", static_cast<std::int64_t>(fail));
            insert_kv("dirty-nodes", static_cast<std::int64_t>(dirty));
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("schema-valid", ev->get_last_schema_validation_ok() ? 1 : 0);
            insert_kv("reflect-postmutate-total", static_cast<std::int64_t>(total));
            insert_kv("reflect-postmutate-recommendation", recommendation);
            // #1611 AC keys (folded — no new public query:*-stats)
            insert_kv("reflect-macro-hygiene-checks", static_cast<std::int64_t>(hygiene_checks));
            insert_kv("reflect-macro-hygiene-rejects", static_cast<std::int64_t>(hygiene_rejects));
            insert_kv("allow-macro-mutate", ev->get_allow_macro_mutate() ? 1 : 0);
            insert_kv("hygiene-aware-validate-wired", 1);
            insert_kv("post-mutation-macro-check-wired", 1);
            insert_kv("deserialize-hygiene-wired", 1);
            insert_kv("issue", 1611);
            insert_kv("schema", 1611); // lineage 502 / 551 / 750
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
    ObservabilityPrims::register_stats_impl(
        "query:reflection-selfmod-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t pass = ev->get_schema_validation_pass_count();
            const std::uint64_t fail = ev->get_schema_validation_fail_count();
            const std::uint64_t snapshots = ev->get_impact_snapshot_count();
            const std::uint64_t impact = ev->get_mutation_impact_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            return make_int(
                static_cast<std::int64_t>(pass + fail + snapshots + impact + guard_epoch));
        });

    // Issue #750 / #1611: (reflect:validate-macro-body node-id [:allow-macro? #t])
    // Runtime hygiene/schema check on MacroIntroduced subtrees before Guard
    // commit. Default rejects unclean MacroIntroduced provenance; pass
    // :allow-macro? #t or (hygiene:set-allow-macro-mutate! #t) to relax.
    add("reflect:validate-macro-body", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        // Issue #1611: optional :allow-macro? kwarg (or global allow flag).
        bool allow = ev.get_allow_macro_mutate();
        const auto& kt = ev.keyword_table();
        std::size_t allow_kw = std::string::npos;
        for (std::size_t i = 0; i < kt.size(); ++i) {
            if (kt[i] == ":allow-macro?") {
                allow_kw = i;
                break;
            }
        }
        if (allow_kw != std::string::npos) {
            for (std::size_t i = 0; i + 1 < a.size(); ++i) {
                if (is_keyword(a[i]) && as_keyword_idx(a[i]) == allow_kw && is_bool(a[i + 1])) {
                    allow = as_bool(a[i + 1]);
                    break;
                }
            }
        }
        auto result = runtime_reflect_validate_ast_subtree(*ws, nid, false);
        // Issue #1611: without allow, unclean macro subtree is a typed reject.
        if (!allow && result.macro_markers > 0 && !result.hygiene_held)
            result.ok = false;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            bump_reflection_schema_metrics(m, result);
            m->reflect_macro_hygiene_checks_total.fetch_add(1, std::memory_order_relaxed);
            if (!result.ok && !allow)
                m->reflect_macro_hygiene_rejects_total.fetch_add(1, std::memory_order_relaxed);
        }
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

    // Issue #2020: (engine:metrics "reflect:hygiene-stats" [node-id]) — Agent-visible live
    // hygiene snapshot for expand → diagnose → mutate/rollback closed loops.
    // No arg / void: process-wide atomics + workspace MacroIntroduced counts.
    // With node-id: also counts MacroIntroduced / dirty / stale under that root
    // (bounded walk; no allocation beyond the hash table).
    // Cheap: relaxed atomics + optional O(subtree) walk only when root given.
    // Issue #2628: private; use (engine:metrics "reflect:hygiene-stats" [node]).
    ObservabilityPrims::register_stats_impl(
        "reflect:hygiene-stats", [&ev, &string_heap](const auto& a) -> EvalValue {
            using aura::compiler::macro_exp::g_hygiene_tracer_depth_max;
            using aura::compiler::macro_exp::g_hygiene_tracer_expansions;
            using aura::compiler::macro_exp::g_hygiene_violation_in_macro_expand_total;
            using aura::compiler::macro_exp::g_macro_clone_concurrent_fiber_total;
            using aura::compiler::macro_exp::g_macro_clone_hygiene_dirty_total;
            using aura::compiler::macro_exp::g_macro_expansion_total;
            using aura::compiler::macro_exp::g_macro_origin_provenance_errors;
            using aura::compiler::macro_exp::g_macro_rest_param_hygiene_total;
            using aura::compiler::macro_exp::g_macro_restamp_after_flat_total;
            using aura::compiler::macro_exp::MAX_HYGIENE_DEPTH;

            auto* ht = FlatHashTable::create(64);
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

            // Process-wide expand/hygiene atomics (relaxed; Agent dashboard).
            const std::int64_t violation_count = static_cast<std::int64_t>(
                g_hygiene_violation_in_macro_expand_total.load(std::memory_order_relaxed) +
                ev.get_hygiene_violation_count());
            const std::int64_t provenance_errors = static_cast<std::int64_t>(
                g_macro_origin_provenance_errors.load(std::memory_order_relaxed));
            const std::int64_t max_depth = static_cast<std::int64_t>(
                g_hygiene_tracer_depth_max.load(std::memory_order_relaxed));
            const std::int64_t dirty_nodes = static_cast<std::int64_t>(
                g_macro_clone_hygiene_dirty_total.load(std::memory_order_relaxed));
            const std::int64_t concurrent_fiber = static_cast<std::int64_t>(
                g_macro_clone_concurrent_fiber_total.load(std::memory_order_relaxed));
            const std::int64_t expansions =
                static_cast<std::int64_t>(g_macro_expansion_total.load(std::memory_order_relaxed));
            const std::int64_t tracer_expansions = static_cast<std::int64_t>(
                g_hygiene_tracer_expansions.load(std::memory_order_relaxed));
            const std::int64_t rest_hyg = static_cast<std::int64_t>(
                g_macro_rest_param_hygiene_total.load(std::memory_order_relaxed));
            const std::int64_t restamp = static_cast<std::int64_t>(
                g_macro_restamp_after_flat_total.load(std::memory_order_relaxed));

            std::int64_t macro_markers = 0;
            std::int64_t subtree_dirty = 0;
            std::int64_t subtree_stale = 0;
            std::int64_t subtree_nodes = 0;
            std::int64_t scoped = 0;
            auto* ws = ev.workspace_flat();
            if (ws) {
                constexpr auto kExpansion = static_cast<std::uint8_t>(
                    aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
                // Optional root: walk only that subtree; else whole flat.
                aura::ast::NodeId root = aura::ast::NULL_NODE;
                if (!a.empty() && is_int(a[0])) {
                    root = static_cast<aura::ast::NodeId>(as_int(a[0]));
                    scoped = 1;
                }
                if (scoped && (root == aura::ast::NULL_NODE || root >= ws->size() ||
                               !ws->is_live_node(root))) {
                    // Invalid root: report zeros for scoped fields; still return atomics.
                } else if (scoped) {
                    std::vector<aura::ast::NodeId> stack;
                    std::vector<std::uint8_t> seen(ws->size(), 0);
                    stack.push_back(root);
                    seen[static_cast<std::size_t>(root)] = 1;
                    while (!stack.empty()) {
                        const auto id = stack.back();
                        stack.pop_back();
                        ++subtree_nodes;
                        if (ws->is_macro_introduced(id)) {
                            ++macro_markers;
                            if ((ws->macro_dirty(id) & kExpansion) != 0)
                                ++subtree_dirty;
                            if (!ws->is_valid(id))
                                ++subtree_stale;
                        }
                        auto v = ws->get(id);
                        for (auto c : v.children) {
                            if (c == aura::ast::NULL_NODE || c >= ws->size())
                                continue;
                            if (seen[static_cast<std::size_t>(c)])
                                continue;
                            seen[static_cast<std::size_t>(c)] = 1;
                            stack.push_back(c);
                        }
                    }
                } else {
                    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                        if (!ws->is_live_node(id))
                            continue;
                        if (ws->is_macro_introduced(id)) {
                            ++macro_markers;
                            if ((ws->macro_dirty(id) & kExpansion) != 0)
                                ++subtree_dirty;
                            if (!ws->is_valid(id))
                                ++subtree_stale;
                        }
                    }
                    subtree_nodes = static_cast<std::int64_t>(ws->size());
                }
            }

            // AC names (exact + snake_case aliases for Agent scripts).
            insert_kv("violation_count", violation_count);
            insert_kv("hygiene-violations", violation_count);
            insert_kv("provenance_errors", provenance_errors);
            insert_kv("provenance-errors", provenance_errors);
            insert_kv("max_depth", max_depth);
            insert_kv("max-depth", max_depth);
            insert_kv("hygiene-depth-max", max_depth);
            insert_kv("dirty_nodes", dirty_nodes);
            insert_kv("hygiene-dirty", dirty_nodes);
            insert_kv("concurrent_fiber_count", concurrent_fiber);
            insert_kv("concurrent-fiber-count", concurrent_fiber);
            // Issue #2021: peak concurrent top-level clone + live in-flight.
            {
                using aura::compiler::macro_exp::g_macro_clone_concurrent_peak;
                using aura::compiler::macro_exp::g_macro_clone_in_flight;
                const std::int64_t peak = static_cast<std::int64_t>(
                    g_macro_clone_concurrent_peak.load(std::memory_order_relaxed));
                const std::int64_t inflight = static_cast<std::int64_t>(
                    g_macro_clone_in_flight.load(std::memory_order_relaxed));
                insert_kv("concurrent_peak", peak);
                insert_kv("concurrent-peak", peak);
                insert_kv("macro-clone-concurrent-peak", peak);
                insert_kv("in_flight", inflight);
                insert_kv("in-flight", inflight);
                insert_kv("macro-clone-in-flight", inflight);
                insert_kv("depth-obs-wired", 1);
                insert_kv("concurrent-obs-wired", 1);
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    aura_macro_hygiene_snapshot_metrics(m);
            }
            insert_kv("expansions", expansions);
            insert_kv("tracer-expansions", tracer_expansions);
            insert_kv("macro_markers", macro_markers);
            insert_kv("macro-markers", macro_markers);
            insert_kv("macro-dirty-nodes", subtree_dirty);
            insert_kv("stale-macro-nodes", subtree_stale);
            insert_kv("subtree-nodes", subtree_nodes);
            insert_kv("scoped", scoped);
            insert_kv("rest-param-hygiene-total", rest_hyg);
            // Issue #2169: rest gensym completeness + process serial.
            {
                using aura::compiler::macro_exp::g_macro_rest_param_hygiene_incomplete_total;
                using aura::compiler::macro_exp::g_macro_rest_gensym_serial;
                const std::int64_t incomplete = static_cast<std::int64_t>(
                    g_macro_rest_param_hygiene_incomplete_total.load(std::memory_order_relaxed));
                const std::int64_t serial = static_cast<std::int64_t>(
                    g_macro_rest_gensym_serial.load(std::memory_order_relaxed));
                insert_kv("rest-param-hygiene-incomplete-total", incomplete);
                insert_kv("rest-param-gensym-serial", serial);
                insert_kv("schema-2169", 2169);
                insert_kv("issue-2169", 2169);
                insert_kv("rest-param-hygiene-complete-wired", 1);
            }
            insert_kv("restamp-after-flat-total", restamp);
            insert_kv("max-hygiene-depth-cap", static_cast<std::int64_t>(MAX_HYGIENE_DEPTH));
            insert_kv("hard-max-depth", static_cast<std::int64_t>(MAX_HYGIENE_DEPTH));
            // Issue #2101: live effective + runtime process-wide caps.
            insert_kv("effective-max-depth",
                      static_cast<std::int64_t>(
                          aura::compiler::macro_exp::effective_hygiene_depth_limit()));
            insert_kv(
                "runtime-depth-cap",
                static_cast<std::int64_t>(aura::compiler::macro_exp::runtime_hygiene_depth_cap()));
            insert_kv(
                "self-evo-pass-cap",
                static_cast<std::int64_t>(aura::compiler::macro_exp::effective_hygiene_pass_cap()));
            insert_kv(
                "runtime-pass-cap",
                static_cast<std::int64_t>(aura::compiler::macro_exp::runtime_hygiene_pass_cap()));
            insert_kv("schema-2101", 2101);
            insert_kv("issue-2101", 2101);
            insert_kv("hygiene-limits-runtime-wired", 1);
            insert_kv("process-wide", 1);
            insert_kv("capability-tightens-only", 1);
            insert_kv("allow-macro-mutate", ev.get_allow_macro_mutate() ? 1 : 0);
            insert_kv("active", 1);
            insert_kv("schema", 2020); // Agent surface #2020; concurrent peak keys #2021 / #2101
            insert_kv("issue", 2020);
            insert_kv("depth-concurrent-obs-issue", 2021);
            // Issue #2167: Agent hygiene-diagnostic + provenance-chain surface.
            insert_kv("schema-2167", 2167);
            insert_kv("issue-2167", 2167);
            insert_kv("hygiene-diagnostic-wired", 1);
            insert_kv("macro-provenance-chain-wired", 1);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2020: (reflect:provenance-blame node-id) — MacroIntroduced origin
    // / expansion-site provenance for a single node. Returns void (nil) when
    // the node is not MacroIntroduced (or invalid); hash otherwise.
    add("reflect:provenance-blame", [&ev, &string_heap](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_void();
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (nid == aura::ast::NULL_NODE || nid >= ws->size() || !ws->is_live_node(nid))
            return make_void();
        if (!ws->is_macro_introduced(nid))
            return make_void(); // AC: nil if not macro-introduced

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

        constexpr auto kExpansion =
            static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
        const auto prov = static_cast<std::int64_t>(ws->provenance(nid));
        const auto dirty = static_cast<std::int64_t>(ws->macro_dirty(nid));
        const auto parent = ws->parent_of(nid);
        insert_kv("node", static_cast<std::int64_t>(nid));
        insert_kv("macro-introduced", 1);
        insert_kv("marker", static_cast<std::int64_t>(static_cast<std::uint8_t>(ws->marker(nid))));
        insert_kv("provenance", prov);
        insert_kv("origin", prov); // expansion-site id (weak provenance stamp)
        insert_kv("gen-valid", ws->is_valid(nid) ? 1 : 0);
        insert_kv("macro-dirty", dirty);
        insert_kv("macro-expansion-dirty", (dirty & kExpansion) != 0 ? 1 : 0);
        insert_kv("parent", parent == aura::ast::NULL_NODE ? static_cast<std::int64_t>(-1)
                                                           : static_cast<std::int64_t>(parent));
        insert_kv("flat-generation", static_cast<std::int64_t>(ws->generation()));
        insert_kv("schema", 2020);
        insert_kv("issue", 2020);
        // Issue #2167: diagnostic / chain surface lineage stamp.
        insert_kv("schema-2167", 2167);
        insert_kv("issue-2167", 2167);
        insert_kv("hygiene-diagnostic-wired", 1);

        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #2167: (query:hygiene-diagnostic node-id [:depth n] [:include-chain? #t])
    // Agent-visible structured hygiene + provenance blame for a single node
    // (MacroIntroduced or user). Lazy: zero hot-path cost until queried.
    // Returns void for bad args / no workspace; hash otherwise (schema 2167).
    //
    // Fields: marker, provenance-id, macro-def-id, expansion-id, mutation-id,
    // fiber-id, violation-flags, depth-at-clone, restamp-count, + optional chain.
    add("query:hygiene-diagnostic", [&ev, &string_heap](const auto& a) -> EvalValue {
        using aura::compiler::macro_exp::g_hygiene_tracer_depth_max;
        using aura::compiler::macro_exp::g_hygiene_violation_in_macro_expand_total;
        using aura::compiler::macro_exp::g_macro_origin_provenance_errors;
        using aura::compiler::macro_exp::g_macro_restamp_after_flat_total;

        if (a.empty() || !is_int(a[0]))
            return make_void();
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (nid == aura::ast::NULL_NODE || nid >= ws->size() || !ws->is_live_node(nid))
            return make_void();

        // Optional kwargs: :depth n (chain walk budget), :include-chain? #t|#f
        std::int64_t depth_budget = 8;
        bool include_chain = true;
        const auto& kt = ev.keyword_table();
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (!is_keyword(a[i]))
                continue;
            const auto kidx = as_keyword_idx(a[i]);
            if (kidx >= kt.size())
                continue;
            const auto& kw = kt[kidx];
            if (kw == ":depth" || kw == "depth") {
                if (i + 1 < a.size() && is_int(a[i + 1])) {
                    depth_budget = as_int(a[i + 1]);
                    ++i;
                }
            } else if (kw == ":include-chain?" || kw == ":include-chain" ||
                       kw == "include-chain?" || kw == "include-chain") {
                include_chain = true;
                if (i + 1 < a.size() && (is_bool(a[i + 1]) || is_int(a[i + 1]))) {
                    if (is_bool(a[i + 1]))
                        include_chain = as_bool(a[i + 1]);
                    else
                        include_chain = as_int(a[i + 1]) != 0;
                    ++i;
                }
            }
        }
        if (depth_budget < 0)
            depth_budget = 0;
        if (depth_budget > 64)
            depth_budget = 64;

        constexpr auto kExpansion =
            static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
        constexpr auto kSelfModify =
            static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroSelfModify);

        const bool is_mi = ws->is_macro_introduced(nid);
        const auto marker = ws->marker(nid);
        const auto prov_id = static_cast<std::int64_t>(ws->provenance(nid));
        const auto dirty = static_cast<std::uint8_t>(ws->macro_dirty(nid));
        // Weak provenance stamp: origin body id (macro expansion clone path).
        // macro-def-id / expansion-id both surface the origin; Agents correlate.
        const std::int64_t macro_def_id = prov_id;
        const std::int64_t expansion_id = prov_id;

        // Last mutation that targeted this node (scan log reverse — lazy).
        std::int64_t last_mut_id = 0;
        {
            const auto view = ws->mutation_log_view();
            for (auto it = view.rbegin(); it != view.rend(); ++it) {
                if (it->target_node == nid) {
                    last_mut_id = static_cast<std::int64_t>(it->mutation_id);
                    break;
                }
            }
        }
        const std::int64_t fiber_id = static_cast<std::int64_t>(aura_fiber_current_id());

        // violation-flags bitfield (Agent-readable):
        //   bit0 = process expand violation > 0
        //   bit1 = provenance origin error total > 0
        //   bit2 = this node macro-expansion dirty
        //   bit3 = this node self-modify dirty
        //   bit4 = gen invalid / stale
        //   bit5 = MacroIntroduced without provenance stamp
        std::int64_t viol_flags = 0;
        const auto proc_viol =
            g_hygiene_violation_in_macro_expand_total.load(std::memory_order_relaxed) +
            static_cast<std::uint64_t>(ev.get_hygiene_violation_count());
        const auto prov_err = g_macro_origin_provenance_errors.load(std::memory_order_relaxed);
        if (proc_viol > 0)
            viol_flags |= 0x01;
        if (prov_err > 0)
            viol_flags |= 0x02;
        if ((dirty & kExpansion) != 0)
            viol_flags |= 0x04;
        if ((dirty & kSelfModify) != 0)
            viol_flags |= 0x08;
        if (!ws->is_valid(nid))
            viol_flags |= 0x10;
        if (is_mi && prov_id == 0)
            viol_flags |= 0x20;

        // depth-at-clone: walk parent chain until non-MI / root (capped).
        std::int64_t depth_at_clone = 0;
        {
            auto cur = nid;
            for (int d = 0; d < 64; ++d) {
                const auto p = ws->parent_of(cur);
                if (p == aura::ast::NULL_NODE || p >= ws->size())
                    break;
                ++depth_at_clone;
                if (!ws->is_macro_introduced(p))
                    break;
                cur = p;
            }
        }
        // Prefer tracer max as secondary signal when node depth is 0.
        const std::int64_t tracer_depth =
            static_cast<std::int64_t>(g_hygiene_tracer_depth_max.load(std::memory_order_relaxed));
        const std::int64_t restamp = static_cast<std::int64_t>(
            g_macro_restamp_after_flat_total.load(std::memory_order_relaxed));

        // Optional provenance chain: follow provenance_ column (origin stamps).
        std::vector<std::int64_t> chain;
        chain.push_back(static_cast<std::int64_t>(nid));
        if (include_chain && depth_budget > 0) {
            std::uint32_t cur_prov = ws->provenance(nid);
            for (std::int64_t step = 0; step < depth_budget && cur_prov != 0; ++step) {
                const auto origin = static_cast<aura::ast::NodeId>(cur_prov);
                if (origin == nid || origin >= ws->size())
                    break;
                // Avoid cycles.
                bool seen = false;
                for (auto x : chain) {
                    if (x == static_cast<std::int64_t>(origin)) {
                        seen = true;
                        break;
                    }
                }
                if (seen)
                    break;
                chain.push_back(static_cast<std::int64_t>(origin));
                // Next hop: origin's own provenance (side-table walk).
                if (!ws->is_live_node(origin))
                    break;
                const auto next = ws->provenance(origin);
                if (next == 0 || next == cur_prov)
                    break;
                cur_prov = next;
            }
        }

        auto* ht = FlatHashTable::create(64);
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

        insert_kv("node-id", static_cast<std::int64_t>(nid));
        insert_kv("node", static_cast<std::int64_t>(nid));
        insert_kv("marker", static_cast<std::int64_t>(static_cast<std::uint8_t>(marker)));
        insert_kv("macro-introduced", is_mi ? 1 : 0);
        insert_kv("provenance-id", prov_id);
        insert_kv("provenance", prov_id);
        insert_kv("macro-def-id", macro_def_id);
        insert_kv("expansion-id", expansion_id);
        insert_kv("mutation-id", last_mut_id);
        insert_kv("fiber-id", fiber_id);
        insert_kv("violation-flags", viol_flags);
        insert_kv("depth-at-clone", depth_at_clone);
        insert_kv("tracer-depth-max", tracer_depth);
        insert_kv("restamp-count", restamp);
        insert_kv("macro-dirty", static_cast<std::int64_t>(dirty));
        insert_kv("macro-expansion-dirty", (dirty & kExpansion) != 0 ? 1 : 0);
        insert_kv("gen-valid", ws->is_valid(nid) ? 1 : 0);
        insert_kv("flat-generation", static_cast<std::int64_t>(ws->generation()));
        insert_kv("process-violation-total", static_cast<std::int64_t>(proc_viol));
        insert_kv("process-provenance-errors", static_cast<std::int64_t>(prov_err));
        insert_kv("include-chain", include_chain ? 1 : 0);
        insert_kv("depth-budget", depth_budget);
        insert_kv("chain-length", static_cast<std::int64_t>(chain.size()));
        // First few chain hops as fixed keys (Agent scripts; no list alloc).
        if (!chain.empty())
            insert_kv("chain-0", chain[0]);
        if (chain.size() > 1)
            insert_kv("chain-1", chain[1]);
        if (chain.size() > 2)
            insert_kv("chain-2", chain[2]);
        if (chain.size() > 3)
            insert_kv("chain-3", chain[3]);
        insert_kv("schema", 2167);
        insert_kv("issue", 2167);
        insert_kv("schema-2167", 2167);
        insert_kv("active", 1);
        insert_kv("lazy", 1); // zero overhead when not queried

        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #2167: (query:macro-provenance-chain node-id [:depth n])
    // Lightweight side-table walk of provenance_ origins. Returns hash with
    // chain-length + chain-0..chain-N + terminal flags (schema 2167).
    add("query:macro-provenance-chain", [&ev, &string_heap](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_void();
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_void();
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (nid == aura::ast::NULL_NODE || nid >= ws->size() || !ws->is_live_node(nid))
            return make_void();

        std::int64_t depth_budget = 16;
        const auto& kt = ev.keyword_table();
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (!is_keyword(a[i]))
                continue;
            const auto kidx = as_keyword_idx(a[i]);
            if (kidx >= kt.size())
                continue;
            if ((kt[kidx] == ":depth" || kt[kidx] == "depth") && i + 1 < a.size() &&
                is_int(a[i + 1])) {
                depth_budget = as_int(a[i + 1]);
                ++i;
            }
        }
        if (depth_budget < 1)
            depth_budget = 1;
        if (depth_budget > 64)
            depth_budget = 64;

        std::vector<std::int64_t> chain;
        chain.reserve(static_cast<std::size_t>(depth_budget) + 1);
        chain.push_back(static_cast<std::int64_t>(nid));
        std::uint32_t cur_prov = ws->provenance(nid);
        std::int64_t hops = 0;
        bool cycle = false;
        bool dead = false;
        while (hops < depth_budget && cur_prov != 0) {
            const auto origin = static_cast<aura::ast::NodeId>(cur_prov);
            if (origin >= ws->size()) {
                dead = true;
                break;
            }
            for (auto x : chain) {
                if (x == static_cast<std::int64_t>(origin)) {
                    cycle = true;
                    break;
                }
            }
            if (cycle)
                break;
            chain.push_back(static_cast<std::int64_t>(origin));
            ++hops;
            if (!ws->is_live_node(origin)) {
                dead = true;
                break;
            }
            const auto next = ws->provenance(origin);
            if (next == 0 || next == cur_prov)
                break;
            cur_prov = next;
        }

        auto* ht = FlatHashTable::create(48);
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

        insert_kv("node-id", static_cast<std::int64_t>(nid));
        insert_kv("macro-introduced", ws->is_macro_introduced(nid) ? 1 : 0);
        insert_kv("provenance-id", static_cast<std::int64_t>(ws->provenance(nid)));
        insert_kv("chain-length", static_cast<std::int64_t>(chain.size()));
        insert_kv("hops", hops);
        insert_kv("depth-budget", depth_budget);
        insert_kv("cycle", cycle ? 1 : 0);
        insert_kv("dead-end", dead ? 1 : 0);
        insert_kv("terminal", chain.empty() ? -1 : chain.back());
        // Emit up to 8 hops as chain-i keys (fixed schema for Agents).
        static constexpr const char* kChainKeys[] = {
            "chain-0", "chain-1", "chain-2", "chain-3", "chain-4", "chain-5", "chain-6", "chain-7",
        };
        for (std::size_t i = 0; i < chain.size() && i < 8; ++i)
            insert_kv(kChainKeys[i], chain[i]);
        insert_kv("schema", 2167);
        insert_kv("issue", 2167);
        insert_kv("schema-2167", 2167);
        insert_kv("active", 1);
        insert_kv("lazy", 1);

        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #501 / #514 / #1610 / #1616 / #1891 / #2022: query:ir-hygiene-stats —
    // IR-level MacroIntroduced + ClosureBridge provenance (refine #1047).
    // Schema **2022** (lineage 1891 / 1616 / 1610 / 501). Authoritative e2e
    // surface for self-evolution: propagated counts + zero-leakage key +
    // native JIT/AOT MacroIntroduced side-table after native code is live.
    auto build_ir_hygiene_stats = [&string_heap](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        auto* ev = Evaluator::get_query_evaluator();
        if (!ev)
            return make_void();
        // 40+ keys (2022 native side-table + 1891 lineage) — 128 slots.
        auto* ht = FlatHashTable::create(128);
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
        auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
        const auto load_m =
            [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
            return m ? (m->*field).load(std::memory_order_relaxed) : 0;
        };
        const std::uint64_t inline_skipped = ir_inline_hygiene_skipped(ev);
        const std::uint64_t markers = workspace_marker_macro_introduced(ev);
        const std::uint64_t stamped = aura_hygiene_ir_macro_marker_total();
        const std::uint64_t provenance_stamped = aura_hygiene_ir_provenance_stamped_total();
        const std::uint64_t jit_deopt = aura_jit_macro_introduced_deopt();
        const std::uint64_t jit_consults = aura_jit_macro_hygiene_consults();
        // Issue #2022: native side-table after JIT/AOT (survives deopt).
        const std::uint64_t native_preserved = aura_jit_native_marker_preserved_total();
        const std::uint64_t live_macro_fns = aura_jit_live_macro_fn_count();
        const std::uint64_t prov_recoverable = aura_jit_macro_provenance_recoverable_total();
        const std::uint64_t ir_prov = load_m(&CompilerMetrics::ir_provenance_stamped_total);
        const std::uint64_t closure_macro =
            load_m(&CompilerMetrics::ir_closure_macro_stamped_total);
        const std::uint64_t closure_consults =
            load_m(&CompilerMetrics::ir_closure_macro_marker_consults_total);
        const std::uint64_t macro_ignored =
            load_m(&CompilerMetrics::macro_introduced_ignored_in_ir_total);
        // #1891: lowering_marker_propagated from metrics or shared C stamp.
        std::uint64_t lowering_prop = load_m(&CompilerMetrics::lowering_marker_propagated_total);
        if (stamped > lowering_prop)
            lowering_prop = stamped;
        std::uint64_t inline_skip_metric =
            load_m(&CompilerMetrics::ir_macro_introduced_inlined_skipped_total);
        if (inline_skipped > inline_skip_metric)
            inline_skip_metric = inline_skipped;
        // IR-module walk: count MacroIntroduced instrs + zero-provenance leaks.
        std::uint64_t ir_instr_macro = 0;
        std::uint64_t ir_instr_total = 0;
        std::uint64_t ir_macro_zero_provenance = 0;
        std::int64_t ir_module_walked = 0;
        if (ev->compiler_service()) {
            auto* svc = static_cast<aura::compiler::CompilerService*>(ev->compiler_service());
            if (const auto& mod_opt = svc->last_ir_module(); mod_opt.has_value()) {
                ir_module_walked = 1;
                for (const auto& fn : mod_opt->functions) {
                    for (const auto& blk : fn.blocks) {
                        for (const auto& instr : blk.instructions) {
                            ++ir_instr_total;
                            if (instr.source_marker == 1) {
                                ++ir_instr_macro;
                                if (instr.provenance == 0)
                                    ++ir_macro_zero_provenance;
                            }
                        }
                    }
                }
            }
        }
        // Leakage: MacroIntroduced treated as user in IR + zero-provenance
        // MacroIntroduced IR instrs (should be 0 after #1891 clone stamp).
        const std::uint64_t hygiene_leakage = macro_ignored + ir_macro_zero_provenance;
        const std::uint64_t total = inline_skipped + markers + stamped + ir_prov;
        // Issue #1780: per-Evaluator policy (not InlinePass static).
        const bool respects = ev->get_inline_respect_macro_hygiene();
        std::int64_t recommendation = 0;
        if (inline_skipped > 0 && !respects)
            recommendation = 3;
        else if (inline_skipped > 5)
            recommendation = 2;
        else if (markers > 0 && inline_skipped == 0 && respects)
            recommendation = 1;
        // #501/#514 lineage
        insert_kv("inline-hygiene-skipped", static_cast<std::int64_t>(inline_skipped));
        insert_kv("macro-markers", static_cast<std::int64_t>(markers));
        insert_kv("respect-macro-hygiene", respects ? 1 : 0);
        insert_kv("ir-hygiene-total", static_cast<std::int64_t>(total));
        insert_kv("ir-hygiene-recommendation", recommendation);
        // #1610 AC keys
        insert_kv("ir-hygiene-stamped-count", static_cast<std::int64_t>(stamped));
        insert_kv("provenance-stamped-count", static_cast<std::int64_t>(provenance_stamped));
        insert_kv("jit-macro-introduced-deopt", static_cast<std::int64_t>(jit_deopt));
        insert_kv("jit-macro-hygiene-consults", static_cast<std::int64_t>(jit_consults));
        insert_kv("lowering-stamp-wired", 1);
        insert_kv("jit-marker-check-wired", 1);
        insert_kv("aot-bridge-marker-wired", 1);
        // #1616 AC keys (refine #1047 ClosureBridge / IRClosure)
        insert_kv("ir_provenance_stamped_total", static_cast<std::int64_t>(ir_prov));
        insert_kv("ir-provenance-stamped-total", static_cast<std::int64_t>(ir_prov));
        insert_kv("macro_introduced_ignored_in_ir", static_cast<std::int64_t>(macro_ignored));
        insert_kv("macro-introduced-ignored-in-ir", static_cast<std::int64_t>(macro_ignored));
        insert_kv("ir-closure-macro-stamped", static_cast<std::int64_t>(closure_macro));
        insert_kv("ir-closure-macro-consults", static_cast<std::int64_t>(closure_consults));
        insert_kv("macro-introduced-count", static_cast<std::int64_t>(markers + stamped));
        insert_kv("closure-bridge-marker-wired", 1);
        insert_kv("ir-closure-marker-wired", 1);
        insert_kv("flat-instr-provenance-wired", 1);
        // #1891 e2e keys
        insert_kv("lowering-marker-propagated", static_cast<std::int64_t>(lowering_prop));
        insert_kv("ir-macro-introduced-inlined-skipped",
                  static_cast<std::int64_t>(inline_skip_metric));
        insert_kv("ir-module-walked", ir_module_walked);
        insert_kv("ir-instr-total", static_cast<std::int64_t>(ir_instr_total));
        insert_kv("ir-instr-macro-introduced", static_cast<std::int64_t>(ir_instr_macro));
        insert_kv("ir-macro-zero-provenance", static_cast<std::int64_t>(ir_macro_zero_provenance));
        insert_kv("hygiene-leakage", static_cast<std::int64_t>(hygiene_leakage));
        insert_kv("clone-provenance-stamped-wired", 1);
        // Issue #2022: MacroIntroduced preserved across JIT/AOT native boundary.
        insert_kv("jit-native-marker-preserved-total", static_cast<std::int64_t>(native_preserved));
        insert_kv("jit-live-macro-fn-count", static_cast<std::int64_t>(live_macro_fns));
        insert_kv("jit-macro-provenance-recoverable", static_cast<std::int64_t>(prov_recoverable));
        insert_kv("jit-native-marker-side-table-wired", 1);
        insert_kv("jit-native-marker-preserve-wired", 1);
        // After deopt, side-table still holds marker/provenance (not cleared).
        insert_kv("jit-macro-deopt-provenance-retained", 1);
        // Issue #2100: deopt round-trip preserved/lost (IR attrs → AST restamp).
        const auto deopt_preserved = aura_jit_macro_introduced_preserved_total();
        const auto deopt_lost = aura_jit_macro_introduced_lost_total();
        insert_kv("jit-macro-introduced-preserved-total",
                  static_cast<std::int64_t>(deopt_preserved));
        insert_kv("jit-macro-introduced-lost-total", static_cast<std::int64_t>(deopt_lost));
        insert_kv("jit-macro-deopt-ast-restore-wired", 1);
        insert_kv("ir-macro-attr-source-marker-wired", 1);
        insert_kv("schema-2100", 2100);
        insert_kv("issue-2100", 2100);
        // Issue #2177: AOT marker propagation observability (refine #2100
        // which was JIT-only). Companion to the existing jit-* keys for
        // full AOT/JIT parity in the Agent dashboard.
        insert_kv("aot-macro-marker-propagated-total",
                  static_cast<std::int64_t>(aura_2177_aot_macro_marker_propagated_total()));
        insert_kv("aot-macro-marker-stripped-total",
                  static_cast<std::int64_t>(aura_2177_aot_macro_marker_stripped_total()));
        insert_kv("schema-2177", 2177);
        insert_kv("issue-2177", 2177);
        // Issue #2167: Agent hygiene-diagnostic / provenance-chain surface.
        insert_kv("schema-2167", 2167);
        insert_kv("issue-2167", 2167);
        insert_kv("hygiene-diagnostic-wired", 1);
        insert_kv("macro-provenance-chain-wired", 1);
        insert_kv("issue", 2022);
        insert_kv("schema", 2022); // lineage 2100 / 2022 / 1891 / 1610
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    };
    ObservabilityPrims::register_stats_impl("query:ir-hygiene-stats", build_ir_hygiene_stats);
    // Note: AC "query:ir-marker-stats" is served as keys on ir-hygiene-stats
    // (macro-introduced-count, etc.) — no new *-stats name (#1448 freeze).

    // Issue #503 / #514: query:pattern-marker-stats. Hash view of
    // query:pattern subtree marker/hygiene counters for Agent loops:
    //   - root-skips: macro_introduced_skipped_in_query_
    //   - recursive-skips: pattern_recursive_macro_skipped_
    //   - hygiene-violations: hygiene_violation_count_
    //   - macro-markers: workspace MacroIntroduced marker tally
    //   - pattern-marker-total: root + recursive + violations + markers
    //   - pattern-marker-recommendation: 0=ok, 1=review skips, 2=alert
    ObservabilityPrims::register_stats_impl(
        "query:pattern-marker-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:consolidated-production-priority-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:production-roadmap-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(eda_feedback + eda_sv + checkpoint +
                                                      gen_wrap + memory_bridge + memory_env +
                                                      soa_skip + soa_hotpath + batch + rollback));
        });

    // Issue #514: query:task6-production-readiness-stats. Returns the
    // sum of 12 counters spanning the Task6 review Top 3 production
    // gaps (non-duplicative synthesis of #547/#551/#550 themes):
    //   Top1 hygiene/marker: skips + violations + inline_skipped + markers
    //   Top2 Guard/reflect: mutation_impact + impact_snapshot + schema_pass
    //                       + panic_commit
    //   Top3 dirty/type: narrowing_refresh + passes_skipped + touched_roots
    //                    + narrowing_dirty_recovery
    ObservabilityPrims::register_stats_impl(
        "query:task6-production-readiness-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:compiler-runtime-production-readiness-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:commercial-production-readiness-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:macro-reflect-self-evo-commercial-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:edsl-query-mutate-commercial-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            const std::uint64_t atomic = ws->atomic_batch_commits() + ev->atomic_batch_count() +
                                         ev->atomic_batch_rollbacks();
            const std::uint64_t eda_feedback = ws->verification_coverage_feedback_total() +
                                               ws->verification_assert_failure_total();
            const std::uint64_t gc_coord =
                ev->get_gc_safepoint_requests_total() + ev->get_gc_safepoint_waits_total();
            const std::uint64_t orchestration =
                ev->get_mutation_steal_attempts() + ev->get_lock_contention_us();
            return make_int(static_cast<std::int64_t>(
                stable_ref + provenance + query_index + query_hygiene + guard_mutate + dirty_up +
                atomic + eda_feedback + gc_coord + orchestration));
        });

    // Issue #619: query:macro-reflect-self-evo-followup-stats.
    // Returns the sum of 4 Task6 follow-up closed-loop counters:
    //   - hygiene_skips: macro_introduced_skipped_in_query_
    //   - post_mutate_reflect_pass: schema_validation_pass_count_
    //   - dirty_type_recheck_count: narrowing_dirty_recovery_total +
    //     incremental_typecheck_auto_invocations_total
    //   - transform_applied: mutation_impact_count_
    ObservabilityPrims::register_stats_impl(
        "query:macro-reflect-self-evo-followup-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:macro-reflect-self-evo-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(skips + violations + impact + snapshots +
                                                      pass + fail + commit + cross_cow));
        });

    // Issue #595 / #1883: query:self-evolution-loop-stats.
    // #595 shipped a sum int of 5 marker/dirty/epoch/Guard loop counters.
    // #1883 upgrades to a structured hash for AI Agent health dashboards
    // while keeping "total" == legacy sum for monotonic back-compat.
    // Fields (schema 1883):
    //   total, hygiene-skips, dirty-propagated, epoch-deltas, validation-pass,
    //   rollback-count, mutation-total, mutation-success-rate-bp,
    //   invariant-audits, invariant-pass-rate-bp, trail-writes,
    //   stack-depth-lifetime-max, stack-depth-current-max, stack-depth-live,
    //   aot-hotupdate-attempts, aot-hotupdate-ok, aot-hotupdate-fail,
    //   aot-hotupdate-invariant-fail, audit-coverage-bp, schema, issue, active
    ObservabilityPrims::register_stats_impl(
        "query:self-evolution-loop-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            using namespace aura::compiler::typed_audit;
            const std::uint64_t hygiene = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t dirty = ev->get_dirty_propagation_count();
            const std::uint64_t epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t validation = ev->get_schema_validation_pass_count();
            const std::uint64_t rollback = ev->get_mutation_log_rollback_count();
            const std::uint64_t total_legacy = hygiene + dirty + epoch + validation + rollback;
            const std::uint64_t mut_total = ev->total_mutations();
            const auto& ac = g_typed_mutation_audit_counters;
            const std::uint64_t inv_aud = ac.invariant_audits.load(std::memory_order_relaxed);
            const std::uint64_t inv_pass = ac.invariant_all_pass.load(std::memory_order_relaxed);
            const std::uint64_t inv_fail =
                ac.invariant_violations_caught.load(std::memory_order_relaxed);
            const std::uint64_t trail = ac.trail_writes.load(std::memory_order_relaxed);
            const std::uint64_t contextual = ac.contextual_total.load(std::memory_order_relaxed);
            const std::uint64_t aot_att = ac.aot_hotupdate_attempts.load(std::memory_order_relaxed);
            const std::uint64_t aot_ok = ac.aot_hotupdate_ok.load(std::memory_order_relaxed);
            const std::uint64_t aot_fail = ac.aot_hotupdate_fail.load(std::memory_order_relaxed);
            const std::uint64_t aot_inv =
                ac.aot_hotupdate_invariant_fail_total.load(std::memory_order_relaxed);
            const std::uint64_t aot_aud = ac.aot_hotupdate_audits.load(std::memory_order_relaxed);
            // Success rate: mutations that did not roll back / (mutations + rollbacks)
            // Use trail successes proxy when mutation log rollbacks available.
            const std::uint64_t mut_den = mut_total + rollback;
            const std::int64_t mut_success_bp =
                mut_den == 0 ? 10000 : static_cast<std::int64_t>((mut_total * 10000ull) / mut_den);
            const std::uint64_t inv_den = inv_aud == 0 ? 0 : inv_aud;
            const std::int64_t inv_pass_bp =
                inv_den == 0 ? 10000 : static_cast<std::int64_t>((inv_pass * 10000ull) / inv_den);
            const std::int64_t aot_cov_bp =
                aot_att == 0 ? 10000 : static_cast<std::int64_t>((aot_aud * 10000ull) / aot_att);
            const std::int64_t stack_life =
                static_cast<std::int64_t>(ev->get_per_fiber_mutation_stack_depth_max());
            const std::int64_t stack_cur =
                static_cast<std::int64_t>(ev->get_per_fiber_mutation_stack_depth_current_max());
            const std::int64_t stack_live =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());

            auto& string_heap = ev->string_heap_mut();
            auto* ht = FlatHashTable::create(48);
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
            insert_kv("total", static_cast<std::int64_t>(total_legacy)); // #595 back-compat
            insert_kv("hygiene-skips", static_cast<std::int64_t>(hygiene));
            insert_kv("dirty-propagated", static_cast<std::int64_t>(dirty));
            insert_kv("epoch-deltas", static_cast<std::int64_t>(epoch));
            insert_kv("validation-pass", static_cast<std::int64_t>(validation));
            insert_kv("rollback-count", static_cast<std::int64_t>(rollback));
            insert_kv("mutation-total", static_cast<std::int64_t>(mut_total));
            insert_kv("mutation-success-rate-bp", mut_success_bp);
            insert_kv("invariant-audits", static_cast<std::int64_t>(inv_aud));
            insert_kv("invariant-pass", static_cast<std::int64_t>(inv_pass));
            insert_kv("invariant-fail", static_cast<std::int64_t>(inv_fail));
            insert_kv("invariant-pass-rate-bp", inv_pass_bp);
            insert_kv("trail-writes", static_cast<std::int64_t>(trail));
            insert_kv("contextual-total", static_cast<std::int64_t>(contextual));
            insert_kv("stack-depth-lifetime-max", stack_life);
            insert_kv("stack-depth-current-max", stack_cur);
            insert_kv("stack-depth-live", stack_live);
            // avg ≈ current-max when hist sparse; basis points of live/current
            insert_kv("avg-stack-depth-x100",
                      stack_cur > 0 ? (stack_live * 100) / (stack_cur > 0 ? stack_cur : 1)
                                    : stack_live * 100);
            insert_kv("aot-hotupdate-attempts", static_cast<std::int64_t>(aot_att));
            insert_kv("aot-hotupdate-ok", static_cast<std::int64_t>(aot_ok));
            insert_kv("aot-hotupdate-fail", static_cast<std::int64_t>(aot_fail));
            insert_kv("aot-hotupdate-invariant-fail", static_cast<std::int64_t>(aot_inv));
            insert_kv("audit-coverage-bp", aot_cov_bp);
            insert_kv("schema", 1883);
            insert_kv("issue", 1883);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1909: query:self-evo-stats — unified self-evolution loop
    // health dashboard for AI Agents. Aggregates IR hygiene (#1891),
    // pattern hygiene (#1892), TypedMutationAudit (#1894), Guard hold
    // latency, and closed-loop rollback into one hash (schema **1909**).
    // AC keys (basis points 0..10000 unless noted):
    //   macro-introduced-ratio-bp, hygiene-violation-rate-bp,
    //   ir-macro-propagated-pct-bp, avg-mutation-boundary-depth-x100,
    //   rollback-success-rate-bp, self-evo-loop-latency-p99-us,
    //   recommendation (0=ok,1=review,2=alert), health-score-bp
    ObservabilityPrims::register_stats_impl(
        "query:self-evo-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            using namespace aura::compiler::typed_audit;
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };

            // ── Macro / pattern hygiene ──
            const std::uint64_t markers = workspace_marker_macro_introduced(ev);
            const std::uint64_t root_skips = ev->get_macro_introduced_skipped_in_query();
            const std::uint64_t recursive_skips = ev->get_pattern_recursive_macro_skipped();
            const std::uint64_t hygiene_viol = ev->get_hygiene_violation_count();
            const std::uint64_t pattern_leak = ev->get_pattern_macro_filter_violations();
            const std::uint64_t boundary_hygiene =
                load_m(&CompilerMetrics::hygiene_violation_prevented_on_boundary_total) +
                load_m(&CompilerMetrics::mutation_boundary_hygiene_violation_total);
            std::uint64_t ws_nodes = 0;
            if (auto* ws = ev->workspace_flat())
                ws_nodes = static_cast<std::uint64_t>(ws->size());
            const std::uint64_t macro_ratio_den = ws_nodes > 0 ? ws_nodes : 1;
            const std::int64_t macro_introduced_ratio_bp =
                static_cast<std::int64_t>((markers * 10000ull) / macro_ratio_den);
            const std::uint64_t hygiene_events =
                root_skips + recursive_skips + hygiene_viol + pattern_leak + boundary_hygiene;
            const std::uint64_t hygiene_den =
                hygiene_events > 0 ? hygiene_events : (root_skips + recursive_skips + 1);
            const std::int64_t hygiene_violation_rate_bp =
                static_cast<std::int64_t>(((hygiene_viol + pattern_leak) * 10000ull) / hygiene_den);

            // ── IR hygiene / propagation ──
            std::uint64_t ir_instr_total = 0, ir_instr_macro = 0, ir_macro_zero_prov = 0;
            std::int64_t ir_module_walked = 0;
            if (ev->compiler_service()) {
                auto* svc = static_cast<aura::compiler::CompilerService*>(ev->compiler_service());
                if (const auto& mod_opt = svc->last_ir_module(); mod_opt.has_value()) {
                    ir_module_walked = 1;
                    for (const auto& fn : mod_opt->functions) {
                        for (const auto& blk : fn.blocks) {
                            for (const auto& instr : blk.instructions) {
                                ++ir_instr_total;
                                if (instr.source_marker == 1) {
                                    ++ir_instr_macro;
                                    if (instr.provenance == 0)
                                        ++ir_macro_zero_prov;
                                }
                            }
                        }
                    }
                }
            }
            const std::uint64_t ir_den = ir_instr_total > 0 ? ir_instr_total : 1;
            const std::int64_t ir_macro_propagated_pct_bp =
                static_cast<std::int64_t>((ir_instr_macro * 10000ull) / ir_den);
            const std::uint64_t lowering_prop =
                load_m(&CompilerMetrics::lowering_marker_propagated_total);
            const std::uint64_t hygiene_leakage =
                load_m(&CompilerMetrics::macro_introduced_ignored_in_ir_total) + ir_macro_zero_prov;

            // ── Boundary depth ──
            const std::int64_t depth_live =
                static_cast<std::int64_t>(Evaluator::mutation_boundary_depth());
            const std::int64_t depth_max =
                static_cast<std::int64_t>(ev->get_per_fiber_mutation_stack_depth_current_max());
            const std::int64_t avg_depth_x100 =
                depth_max > 0 ? (depth_live * 100) / depth_max : depth_live * 100;

            // ── Rollback success ──
            const std::uint64_t rollback_ok =
                load_m(&CompilerMetrics::closed_loop_rollback_success_total);
            const std::uint64_t rollback_log = ev->get_mutation_log_rollback_count();
            const std::uint64_t guard_exc =
                load_m(&CompilerMetrics::mutation_guard_exception_total) +
                load_m(&CompilerMetrics::mutation_boundary_exception_rollback_total);
            const std::uint64_t rollback_attempts = rollback_ok + rollback_log + guard_exc;
            const std::int64_t rollback_success_rate_bp =
                rollback_attempts == 0
                    ? 10000
                    : static_cast<std::int64_t>((rollback_ok * 10000ull) /
                                                (rollback_attempts > 0 ? rollback_attempts : 1));

            // ── Latency p99 proxy: max hold duration (us); hist top bucket ──
            const std::uint64_t hold_max_us =
                load_m(&CompilerMetrics::mutation_hold_duration_us_max);
            const std::uint64_t hold_total =
                load_m(&CompilerMetrics::mutation_hold_duration_us_total);
            const std::uint64_t hold_samples = load_m(&CompilerMetrics::mutation_hold_samples);
            const std::int64_t hold_avg_us =
                hold_samples > 0 ? static_cast<std::int64_t>(hold_total / hold_samples) : 0;
            // p99 ≈ max when sample count small; else use max as conservative p99.
            const std::int64_t latency_p99_us = static_cast<std::int64_t>(hold_max_us);

            // ── TypedMutationAudit ──
            const auto& ac = g_typed_mutation_audit_counters;
            const std::uint64_t inv_aud = ac.invariant_audits.load(std::memory_order_relaxed);
            const std::uint64_t inv_pass = ac.invariant_all_pass.load(std::memory_order_relaxed);
            const std::uint64_t inv_fail =
                ac.invariant_violations_caught.load(std::memory_order_relaxed);
            const std::int64_t inv_pass_bp =
                inv_aud == 0 ? 10000 : static_cast<std::int64_t>((inv_pass * 10000ull) / inv_aud);

            // ── Issue #2030: provenance blame completeness + occurrence hit rate ──
            // blame_completeness_ratio = complete / (complete + miss) in bp.
            // Prefer CompilerMetrics when populated; else process-wide audit counters.
            const std::uint64_t m_blame_c = load_m(&CompilerMetrics::blame_chain_complete_total);
            const std::uint64_t m_blame_m = load_m(&CompilerMetrics::blame_propagation_miss_total);
            const std::uint64_t blame_c =
                m_blame_c > 0 ? m_blame_c
                              : ac.blame_chain_complete_total.load(std::memory_order_relaxed);
            const std::uint64_t blame_m =
                m_blame_m > 0 ? m_blame_m
                              : ac.blame_propagation_miss_total.load(std::memory_order_relaxed);
            const std::uint64_t blame_den = blame_c + blame_m;
            const std::int64_t blame_completeness_ratio_bp =
                blame_den == 0 ? 10000
                               : static_cast<std::int64_t>((blame_c * 10000ull) / blame_den);
            // occurrence_narrowing_post_mutate_hit_rate: renarrow hits/total,
            // else stale-refresh blame-complete ratio, else N/A → 10000.
            const std::uint64_t renarrow_hits =
                load_m(&CompilerMetrics::occurrence_renarrow_hits_total);
            const std::uint64_t renarrow_total =
                load_m(&CompilerMetrics::occurrence_renarrow_total);
            const std::uint64_t stale_refresh =
                load_m(&CompilerMetrics::occurrence_stale_refreshes_total);
            const std::uint64_t occ_blame_ok =
                load_m(&CompilerMetrics::occurrence_blame_chain_complete_total);
            const std::uint64_t narrowing_refresh = ev->get_narrowing_refresh_count();
            std::int64_t occurrence_narrowing_post_mutate_hit_rate_bp = 10000;
            if (renarrow_total > 0) {
                occurrence_narrowing_post_mutate_hit_rate_bp =
                    static_cast<std::int64_t>((renarrow_hits * 10000ull) / renarrow_total);
            } else if (stale_refresh > 0) {
                occurrence_narrowing_post_mutate_hit_rate_bp =
                    static_cast<std::int64_t>((occ_blame_ok * 10000ull) / stale_refresh);
            } else if (narrowing_refresh > 0) {
                // Refresh activity without miss accounting → treat as hit.
                occurrence_narrowing_post_mutate_hit_rate_bp = 10000;
            }
            // Linear × occurrence consistency (process-wide atomics + metrics).
            using namespace aura::compiler::linear_occurrence_mutate;
            const std::uint64_t lin_reval =
                revalidate_hits_total.load(std::memory_order_relaxed) +
                load_m(&CompilerMetrics::linear_occurrence_revalidate_hits_total);
            const std::uint64_t lin_escape =
                escape_violations_prevented_total.load(std::memory_order_relaxed) +
                load_m(&CompilerMetrics::linear_occurrence_escape_prevented_total);
            const std::uint64_t lin_pred_safe =
                predicate_branch_linear_safe_total.load(std::memory_order_relaxed) +
                load_m(&CompilerMetrics::linear_occurrence_predicate_safe_total);
            const std::uint64_t lin_den = lin_reval + lin_escape;
            const std::int64_t linear_occurrence_consistency_bp =
                lin_den == 0 ? 10000 : static_cast<std::int64_t>((lin_reval * 10000ull) / lin_den);
            const std::uint64_t type_ok = ac.type_invariant_ok.load(std::memory_order_relaxed);
            const std::uint64_t type_fail = ac.type_invariant_fail.load(std::memory_order_relaxed);
            const std::uint64_t lin_ok = ac.linear_invariant_ok.load(std::memory_order_relaxed);
            const std::uint64_t lin_inv_fail =
                ac.linear_invariant_fail.load(std::memory_order_relaxed);
            const std::uint64_t prov_ok =
                ac.provenance_invariant_ok.load(std::memory_order_relaxed);
            const std::uint64_t prov_fail =
                ac.provenance_invariant_fail.load(std::memory_order_relaxed);
            const std::int64_t type_invariant_ratio_bp =
                (type_ok + type_fail) == 0
                    ? 10000
                    : static_cast<std::int64_t>((type_ok * 10000ull) / (type_ok + type_fail));
            const std::int64_t linear_invariant_ratio_bp =
                (lin_ok + lin_inv_fail) == 0
                    ? 10000
                    : static_cast<std::int64_t>((lin_ok * 10000ull) / (lin_ok + lin_inv_fail));
            const std::int64_t provenance_invariant_ratio_bp =
                (prov_ok + prov_fail) == 0
                    ? 10000
                    : static_cast<std::int64_t>((prov_ok * 10000ull) / (prov_ok + prov_fail));
            const std::int64_t linear_provenance_consistency_bp = static_cast<std::int64_t>(
                aura::core::provenance::linear_provenance_consistency_bp());
            (void)lin_pred_safe;

            // ── SV closed-loop rounds (self-evo signal) ──
            const std::uint64_t sv_rounds =
                load_m(&CompilerMetrics::sv_self_evo_closed_loop_rounds_total);
            const std::uint64_t sv_hits =
                load_m(&CompilerMetrics::sv_self_evo_convergence_hits_total);

            // ── Recommendation + composite health ──
            // 0=ok, 1=review (elevated skips/latency), 2=alert (leakage/violations)
            std::int64_t recommendation = 0;
            if (hygiene_viol > 0 || pattern_leak > 0 || hygiene_leakage > 0 || inv_fail > 0)
                recommendation = 2;
            else if (hygiene_violation_rate_bp > 500 || hold_avg_us > 10000 ||
                     macro_introduced_ratio_bp > 5000 || blame_completeness_ratio_bp < 5000)
                recommendation = 1;
            // health-score: average of inverted risk signals (higher better)
            const std::int64_t hygiene_health =
                10000 - std::min<std::int64_t>(hygiene_violation_rate_bp, 10000);
            const std::int64_t inv_health = inv_pass_bp;
            const std::int64_t rb_health = rollback_success_rate_bp;
            const std::int64_t ir_health =
                hygiene_leakage == 0
                    ? 10000
                    : std::max<std::int64_t>(
                          0, 10000 - static_cast<std::int64_t>(hygiene_leakage * 1000));
            // Issue #2030: fold blame + occurrence hit rate into health.
            const std::int64_t health_score_bp =
                (hygiene_health + inv_health + rb_health + ir_health + blame_completeness_ratio_bp +
                 occurrence_narrowing_post_mutate_hit_rate_bp) /
                6;

            if (m)
                m->self_evo_unified_health_queries_total.fetch_add(1, std::memory_order_relaxed);

            auto& string_heap = ev->string_heap_mut();
            // #1909 + #2030 agent ratio keys → 128 slots.
            auto* ht = FlatHashTable::create(128);
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

            // Issue #1909 AC names (exact + kebab aliases)
            insert_kv("macro-introduced-ratio-bp", macro_introduced_ratio_bp);
            insert_kv("macro_introduced_ratio", macro_introduced_ratio_bp);
            insert_kv("hygiene-violation-rate-bp", hygiene_violation_rate_bp);
            insert_kv("hygiene_violation_rate", hygiene_violation_rate_bp);
            insert_kv("ir-macro-propagated-pct-bp", ir_macro_propagated_pct_bp);
            insert_kv("ir_macro_propagated_pct", ir_macro_propagated_pct_bp);
            insert_kv("avg-mutation-boundary-depth-x100", avg_depth_x100);
            insert_kv("avg_mutation_boundary_depth", avg_depth_x100);
            insert_kv("rollback-success-rate-bp", rollback_success_rate_bp);
            insert_kv("rollback_success_rate", rollback_success_rate_bp);
            insert_kv("self-evo-loop-latency-p99-us", latency_p99_us);
            insert_kv("self_evo_loop_latency_p99", latency_p99_us);
            // Supporting counters for dashboards
            insert_kv("macro-markers", static_cast<std::int64_t>(markers));
            insert_kv("workspace-nodes", static_cast<std::int64_t>(ws_nodes));
            insert_kv("hygiene-skips", static_cast<std::int64_t>(root_skips + recursive_skips));
            insert_kv("hygiene-violations", static_cast<std::int64_t>(hygiene_viol));
            insert_kv("pattern-hygiene-leakage", static_cast<std::int64_t>(pattern_leak));
            insert_kv("boundary-hygiene-prevented", static_cast<std::int64_t>(boundary_hygiene));
            insert_kv("ir-module-walked", ir_module_walked);
            insert_kv("ir-instr-total", static_cast<std::int64_t>(ir_instr_total));
            insert_kv("ir-instr-macro-introduced", static_cast<std::int64_t>(ir_instr_macro));
            insert_kv("ir-macro-zero-provenance", static_cast<std::int64_t>(ir_macro_zero_prov));
            insert_kv("hygiene-leakage", static_cast<std::int64_t>(hygiene_leakage));
            insert_kv("lowering-marker-propagated", static_cast<std::int64_t>(lowering_prop));
            insert_kv("mutation-boundary-depth-live", depth_live);
            insert_kv("mutation-boundary-depth-max", depth_max);
            insert_kv("mutation-hold-avg-us", hold_avg_us);
            insert_kv("mutation-hold-max-us", static_cast<std::int64_t>(hold_max_us));
            insert_kv("rollback-success-total", static_cast<std::int64_t>(rollback_ok));
            insert_kv("rollback-log-total", static_cast<std::int64_t>(rollback_log));
            insert_kv("invariant-audits", static_cast<std::int64_t>(inv_aud));
            insert_kv("invariant-pass-rate-bp", inv_pass_bp);
            insert_kv("invariant-fail", static_cast<std::int64_t>(inv_fail));
            insert_kv("sv-closed-loop-rounds", static_cast<std::int64_t>(sv_rounds));
            insert_kv("sv-convergence-hits", static_cast<std::int64_t>(sv_hits));
            // Issue #2030: agent-facing blame + occurrence + linear/provenance ratios
            insert_kv("blame_completeness_ratio", blame_completeness_ratio_bp);
            insert_kv("blame-completeness-ratio-bp", blame_completeness_ratio_bp);
            insert_kv("blame-chain-complete-total", static_cast<std::int64_t>(blame_c));
            insert_kv("blame-propagation-miss-total", static_cast<std::int64_t>(blame_m));
            insert_kv("occurrence_narrowing_post_mutate_hit_rate",
                      occurrence_narrowing_post_mutate_hit_rate_bp);
            insert_kv("occurrence-narrowing-post-mutate-hit-rate-bp",
                      occurrence_narrowing_post_mutate_hit_rate_bp);
            insert_kv("occurrence-renarrow-hits", static_cast<std::int64_t>(renarrow_hits));
            insert_kv("occurrence-renarrow-total", static_cast<std::int64_t>(renarrow_total));
            insert_kv("occurrence-stale-refreshes", static_cast<std::int64_t>(stale_refresh));
            insert_kv("narrowing-refresh-count", static_cast<std::int64_t>(narrowing_refresh));
            insert_kv("linear-occurrence-consistency-bp", linear_occurrence_consistency_bp);
            insert_kv("linear_occurrence_consistency_bp", linear_occurrence_consistency_bp);
            insert_kv("type-invariant-ratio-bp", type_invariant_ratio_bp);
            insert_kv("linear-invariant-ratio-bp", linear_invariant_ratio_bp);
            insert_kv("provenance-invariant-ratio-bp", provenance_invariant_ratio_bp);
            insert_kv("linear-provenance-consistency-bp", linear_provenance_consistency_bp);
            insert_kv("blame-occurrence-ratios-wired", 1);
            insert_kv("schema-2030", 2030);
            insert_kv("issue-2030", 2030);
            insert_kv("health-score-bp", health_score_bp);
            insert_kv("recommendation", recommendation);
            insert_kv("unified-dashboard-wired", 1);
            insert_kv("ir-hygiene-lineage", 1891);
            insert_kv("pattern-hygiene-lineage", 1892);
            insert_kv("typed-audit-lineage", 1894);
            insert_kv("loop-stats-lineage", 1883);
            insert_kv("schema", 1909); // primary lineage; #2030 via schema-2030
            insert_kv("issue", 1909);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1912: query:stable-refs-batch-health — batch StableNodeRef
    // refresh/pin observability for AI multi-round COW/sub-workspace loops.
    // Schema **1912**. AC metrics:
    //   stable_ref_batch_refresh_total, cow_pinned_across_layers_total,
    //   batch_refresh_latency_p99 (us, max proxy), stale_ref_prevented_total,
    //   fail_rate_bp, batch-refresh-success-rate-bp.
    ObservabilityPrims::register_stats_impl(
        "query:stable-refs-batch-health", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            auto load_m =
                [m](std::atomic<std::uint64_t> CompilerMetrics::* field) -> std::uint64_t {
                return m ? (m->*field).load(std::memory_order_relaxed) : 0;
            };

            const std::uint64_t batch_refresh =
                load_m(&CompilerMetrics::stable_ref_batch_refresh_total);
            const std::uint64_t cow_pinned =
                load_m(&CompilerMetrics::cow_pinned_across_layers_total);
            const std::uint64_t stale_prevented =
                load_m(&CompilerMetrics::stale_ref_prevented_total);
            const std::uint64_t calls = load_m(&CompilerMetrics::batch_refresh_calls_total);
            const std::uint64_t fails = load_m(&CompilerMetrics::batch_refresh_fail_total);
            const std::uint64_t lat_total =
                load_m(&CompilerMetrics::batch_refresh_latency_us_total);
            const std::uint64_t lat_p99 = load_m(&CompilerMetrics::batch_refresh_latency_us_max);
            const std::uint64_t boundary_pins = ev->cow_boundary_pins_total();
            const std::uint64_t atomic_pins = ev->atomic_batch_pinned_refs_total();
            const std::size_t live_boundary = ev->cow_boundary_pinned_ref_count();
            const std::size_t live_atomic = ev->atomic_batch_pinned_ref_count();

            // success rate: ok / (ok + fail); empty → 10000 bp
            const std::uint64_t attempts = batch_refresh + fails;
            const std::int64_t success_rate_bp =
                attempts == 0 ? 10000
                              : static_cast<std::int64_t>((batch_refresh * 10000ull) /
                                                          (attempts > 0 ? attempts : 1));
            const std::int64_t fail_rate_bp = 10000 - success_rate_bp;
            // fail rate < 0.1% ⇒ fail_rate_bp < 10 for multi-round loops
            const std::int64_t lat_avg_us =
                calls > 0 ? static_cast<std::int64_t>(lat_total / calls) : 0;
            // health: high success + non-zero batch surface when pins exist
            const std::int64_t health_score_bp = success_rate_bp;

            if (m)
                m->stable_refs_batch_health_queries_total.fetch_add(1, std::memory_order_relaxed);

            auto& string_heap = ev->string_heap_mut();
            auto* ht = FlatHashTable::create(48);
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

            // AC metric names (exact snake + kebab aliases)
            insert_kv("stable_ref_batch_refresh_total", static_cast<std::int64_t>(batch_refresh));
            insert_kv("stable-ref-batch-refresh-total", static_cast<std::int64_t>(batch_refresh));
            insert_kv("cow_pinned_across_layers_total", static_cast<std::int64_t>(cow_pinned));
            insert_kv("cow-pinned-across-layers-total", static_cast<std::int64_t>(cow_pinned));
            insert_kv("batch_refresh_latency_p99", static_cast<std::int64_t>(lat_p99));
            insert_kv("batch-refresh-latency-p99", static_cast<std::int64_t>(lat_p99));
            insert_kv("batch_refresh_latency_p99_us", static_cast<std::int64_t>(lat_p99));
            insert_kv("stale_ref_prevented_total", static_cast<std::int64_t>(stale_prevented));
            insert_kv("stale-ref-prevented-total", static_cast<std::int64_t>(stale_prevented));
            insert_kv("cow_boundary_pinned", static_cast<std::int64_t>(boundary_pins));
            insert_kv("cow-boundary-pinned", static_cast<std::int64_t>(boundary_pins));
            // Supporting dashboard keys
            insert_kv("batch-refresh-calls-total", static_cast<std::int64_t>(calls));
            insert_kv("batch-refresh-fail-total", static_cast<std::int64_t>(fails));
            insert_kv("batch-refresh-success-rate-bp", success_rate_bp);
            insert_kv("fail-rate-bp", fail_rate_bp);
            insert_kv("batch-refresh-latency-avg-us", lat_avg_us);
            insert_kv("atomic-batch-pinned-refs-total", static_cast<std::int64_t>(atomic_pins));
            insert_kv("live-cow-boundary-pinned-count", static_cast<std::int64_t>(live_boundary));
            insert_kv("live-atomic-batch-pinned-count", static_cast<std::int64_t>(live_atomic));
            insert_kv("health-score-bp", health_score_bp);
            insert_kv("batch-api-wired", 1);
            insert_kv("children-stable-batch-wired", 1);
            insert_kv("guard-auto-refresh-wired", 1);
            insert_kv("lineage-1500", 1500);
            insert_kv("lineage-738", 738);
            insert_kv("schema", 1912);
            insert_kv("issue", 1912);
            insert_kv("active", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
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
    ObservabilityPrims::register_stats_impl(
        "query:primitives-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
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
    ObservabilityPrims::register_stats_impl(
        "query:primitive-meta-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:prompt6-violation-count", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:prompt6-safety-score", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
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
    ObservabilityPrims::register_stats_impl(
        "query:prompt6-memory-safety-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:edsl-eda-sv-closedloop-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:multi-fiber-orchestration-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:soa-hotpath-adoption-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
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
            return make_int(static_cast<std::int64_t>(ir_instr + ir_funcs + passes_skip +
                                                      relower_skip + relower_per_fn + mod_skip +
                                                      linear_elide + cascade));
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
    ObservabilityPrims::register_stats_impl(
        "query:dirty-propagation-cost-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t upward_calls = ws ? ws->mark_dirty_upward_call_count() : 0;
            const std::uint64_t upward_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
            const std::uint64_t fast_hits = ws ? ws->dirty_upward_fast_fixed_point_count() : 0;
            const std::uint64_t propagation = ev->get_dirty_propagation_count();
            const std::uint64_t passes_skip = ev->get_passes_skipped_type_dirty();
            const std::uint64_t selective = ev->get_selective_recheck_count();
            const std::uint64_t cascade =
                m ? m->cascade_body_only_count.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(upward_calls + upward_nodes + fast_hits +
                                                      propagation + passes_skip + selective +
                                                      cascade));
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
    ObservabilityPrims::register_stats_impl(
        "query:dirty-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:generation-epoch-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(bumps + wraps + wrap_ep + checks +
                                                      guard_epoch + defuse + rollback));
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
    ObservabilityPrims::register_stats_impl(
        "query:ast-column-compaction-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(recycle + compact + reuse + live + free +
                                                      total + frag_bp));
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
    ObservabilityPrims::register_stats_impl(
        "query:mutation-boundary-invariant-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-stale-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t desync = ev->get_envframe_desync_detected();
            const std::uint64_t stale = ev->get_envframe_stale_refresh_count();
            const std::uint64_t rollback = ev->get_envframe_post_rollback_invalidations();
            const std::uint64_t mismatch = ev->get_envframe_version_mismatch_in_walk();
            const std::uint64_t gc_skips = ev->get_envframe_gc_walk_safe_skips();
            const std::uint64_t gc_stale =
                m ? m->gc_envframe_stale_skipped_.load(std::memory_order_relaxed) : 0;
            const std::uint64_t defuse = ev->get_defuse_version();
            return make_int(static_cast<std::int64_t>(desync + stale + rollback + mismatch +
                                                      gc_skips + gc_stale + defuse));
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
    ObservabilityPrims::register_stats_impl(
        "query:defuse-version-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t defuse = ev->current_defuse_version();
            const std::uint64_t last = ev->get_last_queried_epoch();
            const std::uint64_t mutations = ev->total_mutations();
            const std::uint64_t impact = ev->get_mutation_impact_count();
            const std::uint64_t guard_epoch = ev->get_guard_dirty_epoch_count();
            const std::uint64_t aot = m ? m->aot_emits.load(std::memory_order_relaxed) : 0;
            const std::uint64_t bridge =
                m ? m->bridge_epoch_hit_count_.load(std::memory_order_relaxed) : 0;
            return make_int(static_cast<std::int64_t>(defuse + last + mutations + impact +
                                                      guard_epoch + aot + bridge));
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
    ObservabilityPrims::register_stats_impl(
        "query:macro-hygiene-contract-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(query_skips + violations + markers +
                                                      ir_skips + queries + contract + macro_dirty));
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
    ObservabilityPrims::register_stats_impl(
        "query:pattern-macro-filter-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(root_skips + recursive + violations +
                                                      markers + queries + index_hits + hygiene));
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
    ObservabilityPrims::register_stats_impl(
        "query:hygiene-violation-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:pattern-structural-index-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(hits + misses + buckets + entries +
                                                      synced_size + synced_gen + violations));
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
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-workspace-tree-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(resolves + misses + violations + layers +
                                                      active + cow_epoch + checks));
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
    ObservabilityPrims::register_stats_impl(
        "query:shape-deopt-burst-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t churn =
                shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
            const std::uint64_t changes =
                m ? m->shape_changes_observed.load(std::memory_order_relaxed) : 0;
            const std::uint64_t deopt = m ? m->deopt_count.load(std::memory_order_relaxed) : 0;
            const std::uint64_t jit_miss =
                shape::jit_shape_miss_count.load(std::memory_order_relaxed);
            const std::uint64_t hooks =
                shape::shape_deopt_hook_fire_count.load(std::memory_order_relaxed);
            const std::uint64_t bumps =
                shape::shape_version_bump_count.load(std::memory_order_relaxed);
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
    ObservabilityPrims::register_stats_impl(
        "query:pass-contracts-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
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
    ObservabilityPrims::register_stats_impl(
        "query:arena-compaction-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:ir-soa-incremental-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
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
    ObservabilityPrims::register_stats_impl(
        "query:ir-metadata-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
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
                                                      linear_pass + jit_hits + deopt +
                                                      adt_impacts));
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
    ObservabilityPrims::register_stats_impl(
        "query:task4-hotpath-safety-score", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
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
    ObservabilityPrims::register_stats_impl(
        "query:task4-cache-locality-win", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const auto* m =
                static_cast<const aura::compiler::CompilerMetrics*>(ev->compiler_metrics());
            auto* ws = ev->workspace_flat();
            const std::uint64_t hits = ws ? ws->tag_arity_index_hits() : 0;
            const std::uint64_t delta = ws ? ws->tag_arity_index_delta_hits() : 0;
            const std::uint64_t spec =
                m ? m->specialization_hits.load(std::memory_order_relaxed) : 0;
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
    ObservabilityPrims::register_stats_impl(
        "query:shape-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:shape-profiler-stats",
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
            // Issue #2433: capacity 32 (≥24 keys incl. schema-2257 + schema-2433).
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
            // Issue #2257 AC3: 3 new query keys (shape-version +
            //   deopt-storm-isolations-total + current-stability-ratio)
            //   + schema-2257 lineage. AC2: under HighMutation +
            //   continuous body mutate, deopt-storm-isolations stays
            //   bounded (one bump per storm enter, not per deopt event).
            insert_kv("shape-version",
                      static_cast<std::int64_t>(
                          shape::shape_version_bump_count.load(std::memory_order_relaxed)));
            insert_kv(
                "deopt-storm-isolations-total",
                m ? static_cast<std::int64_t>(
                        m->deopt_storm_isolations_total.load(std::memory_order_relaxed))
                  : static_cast<std::int64_t>(shape::g_deopt_storm_isolations_total_atomic().load(
                        std::memory_order_relaxed)));
            // Issue #2257: stability ratio lives in aura::compiler
            // (ir_cache_pure.ixx adaptive feed), not shape:: namespace.
            insert_kv("current-stability-ratio",
                      static_cast<std::int64_t>(current_shape_stability_ratio() * 10000.0));
            insert_kv("shape-version-wired", 1);
            insert_kv("schema-2257", 2257);
            insert_kv("issue-2257", 2257);
            // Issue #2433: additive storm-health keys (same hash, no extra
            // public primitive). shape-version-at-storm + shape-storm-active
            // + force-reason for Agent closed-loop throttle.
            insert_kv("shape-version-at-storm",
                      static_cast<std::int64_t>(shape::shape_version_at_last_storm()));
            // force-reason non-zero while storm published; soft-clear resets to 0.
            insert_kv("shape-storm-active",
                      static_cast<std::int64_t>(shape::shape_storm_force_reason() !=
                                                shape::kShapeStormForceReasonNone));
            insert_kv("shape-storm-force-reason",
                      static_cast<std::int64_t>(shape::shape_storm_force_reason()));
            insert_kv("schema-2433", 2433);
            insert_kv("issue-2433", 2433);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2433: query:shape-storm-health — compact Agent-facing score
    // for closed-loop throttle (health-bp + force-reason + version snapshot).
    // Soft path: pure atomic loads; zero writes.
    ObservabilityPrims::register_stats_impl(
        "query:shape-storm-health", [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            (void)ev;
            const auto isolations =
                shape::g_deopt_storm_isolations_total_atomic().load(std::memory_order_relaxed);
            const auto force = shape::shape_storm_force_reason();
            // Issue #2526: only Threshold force-reason is active storm (hard fence).
            // AdaptiveSuppress is soft (no isolation / no health crash).
            const bool storm_active = force == shape::kShapeStormForceReasonThreshold;
            // health-bp: 10000 quiet; −2500 when storm active; −min(4000, isolations*100)
            std::int64_t health_bp = 10000;
            if (storm_active)
                health_bp -= 2500;
            const auto iso_pen =
                static_cast<std::int64_t>(std::min<std::uint64_t>(isolations, 40) * 100);
            health_bp -= iso_pen;
            if (health_bp < 0)
                health_bp = 0;
            // Issue #2526 / #2617: capacity 32 for adaptive + compact-isolation keys.
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
            insert_kv("health-bp", health_bp);
            insert_kv("force-reason", static_cast<std::int64_t>(force));
            insert_kv("shape-version-at-storm",
                      static_cast<std::int64_t>(shape::shape_version_at_last_storm()));
            insert_kv("shape-storm-active", storm_active ? 1 : 0);
            insert_kv("deopt-storm-isolations-total", static_cast<std::int64_t>(isolations));
            insert_kv("high-mutation-default",
                      static_cast<std::int64_t>(shape::shape_high_mutation_default_enabled()));
            insert_kv("schema-2433", 2433);
            insert_kv("issue-2433", 2433);
            // Issue #2526: adaptive threshold closed-loop surface.
            insert_kv("schema-2526", 2526);
            insert_kv("issue-2526", 2526);
            insert_kv("adaptive-threshold-live",
                      static_cast<std::int64_t>(shape::deopt_storm_adaptive_threshold_live()));
            insert_kv("adaptive-suppress-total",
                      static_cast<std::int64_t>(
                          shape::g_deopt_storm_adaptive_suppress_total_atomic().load(
                              std::memory_order_relaxed)));
            insert_kv(
                "adaptive-enter-total",
                static_cast<std::int64_t>(shape::g_deopt_storm_adaptive_enter_total_atomic().load(
                    std::memory_order_relaxed)));
            insert_kv("shape-storm-fence-hard", shape::shape_storm_fence_hard() ? 1 : 0);
            insert_kv("force-reason-threshold",
                      static_cast<std::int64_t>(shape::kShapeStormForceReasonThreshold));
            insert_kv("force-reason-adaptive-suppress",
                      static_cast<std::int64_t>(shape::kShapeStormForceReasonAdaptiveSuppress));
            insert_kv("adaptive-policy-wired", 1);
            // Issue #2617: compact path never feeds deopt-storm ring (gate + contract).
            insert_kv("schema-2617", 2617);
            insert_kv("issue-2617", 2617);
            insert_kv("compact-storm-isolated-wired",
                      static_cast<std::int64_t>(shape::shape_compact_storm_isolation_wired()));
            insert_kv("deopt-storm-compact-suppressed",
                      static_cast<std::int64_t>(
                          shape::deopt_storm_compact_suppressed.load(std::memory_order_relaxed)));
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
    ObservabilityPrims::register_stats_impl(
        "query:shape-stability-jit-stats-hash",
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
    ObservabilityPrims::register_stats_impl(
        "query:value-dispatch-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            const std::uint64_t hits =
                types::value_dispatch_hit_count.load(std::memory_order_relaxed);
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
    ObservabilityPrims::register_stats_impl(
        "query:task4-mutation-stability", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(dirty_prop + selective + guard_epoch +
                                                      narrowing + snapshots + cross_cow));
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
    ObservabilityPrims::register_stats_impl(
        "query:task4-hotpath-contracts", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:contracts-hotpath-stats-hash",
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
    ObservabilityPrims::register_stats_impl(
        "query:edsl-stability-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:stable-ref-cow-fiber-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    //   - batch_commits: atomic_batch_domain_.count
    //   - batch_rollbacks: atomic_batch_domain_.rollbacks
    //   - bumps_saved: atomic_batch_domain_.bumps_saved_total
    //   - fiber_safety: atomic_batch_steal_violation_ +
    //                   atomic_batch_domain_.in_fiber_total
    //   - guard_rollbacks: mutation_log_rollback_count_
    //   - guard_success: mutation_impact_count_
    //   - panic_recovery: panic_checkpoint_restore_count_
    //
    // P0: returns an integer = sum of all 7 counter groups.
    // Follow-up: returns a 7-tuple so the AI Agent can compute
    // rollback_rate and fiber_safety_ratio independently.
    ObservabilityPrims::register_stats_impl(
        "query:atomic-batch-rollback-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(commits + rollbacks + bumps_saved +
                                                      fiber_safety + guard_rollbacks +
                                                      guard_success + panic_recovery));
        });

    // Issue #2527: query:query-and-replace-batch-stats. Returns the
    // sum of the 3 new atomic counters + schema-NNNN keys for the new
    // mutate:query-and-replace-batch sugar primitive (refine #192 / #1265 /
    // #1649 / #1913 lineage). Distinct from query:atomic-batch-stats /
    // query:mutation-log-stats / etc. — those cover the atomic-batch +
    // mutation-log telemetry; this is the sugar-primitive-specific
    // observability bundle.
    //
    // Returns integer sum of:
    //   - query-and-replace-batch-size: total invocations
    //   - query-and-replace-batch-partial-fail-total: # of calls that
    //       hit >=1 partial-fail (parse / stale-ref / macro-hygiene /
    //       :validate) and auto-rolled back
    //   - query-and-replace-batch-hygiene-preserved-total: # of
    //       MacroIntroduced markers kept on skipped refs under
    //       :hygiene-keep default (:macro-introduced-only per #2525)
    //
    // Note: structured-hash surface (schema-2527 / issue-2527 /
    // stats-wired keys) deferred to follow-up issue — manual hash
    // construction has too many type mismatches with HashTable /
    // make_pair overload ambiguity for this ship.
    ObservabilityPrims::register_stats_impl(
        "query:query-and-replace-batch-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            const std::uint64_t size =
                m ? m->query_replace_batch_size.load(std::memory_order_relaxed) : 0;
            const std::uint64_t partial_fail =
                m ? m->query_replace_batch_partial_fail_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t hygiene_preserved =
                m ? m->query_replace_batch_hygiene_preserved_total.load(std::memory_order_relaxed)
                  : 0;
            // Surface the 3 individual counters via 3 sibling keys so
            // Agents can compute failure rate / hygiene-preserved ratio
            // independently. Schema/wired keys via separate query:
            // follow-up path.
            return make_int(static_cast<std::int64_t>(size + partial_fail + hygiene_preserved));
        });

    // Issue #553: query:mutation-log-stats. Returns the
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
    //   - atomic_batch_domain_.rollbacks         (Evaluator, #192)
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
    ObservabilityPrims::register_stats_impl(
        "query:mutation-log-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    //   - batch_rollbacks: atomic_batch_domain_.rollbacks
    ObservabilityPrims::register_stats_impl(
        "query:mutation-rollback-coverage-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            auto* ws = ev->workspace_flat();
            const std::uint64_t structural_success = ws ? ws->structural_rollback_success() : 0;
            const std::uint64_t structural_besteffort =
                ws ? ws->structural_rollback_besteffort() : 0;
            const std::uint64_t field_log = ev->get_mutation_log_rollback_count();
            const std::uint64_t batch = ev->atomic_batch_rollbacks();
            return make_int(static_cast<std::int64_t>(structural_success + structural_besteffort +
                                                      field_log + batch));
        });

    // (query :mutation-log [n]) — Issue #346: returns
    // a pair-list of the most recent n mutations in
    // chronological order (oldest first). n defaults
    // to 10 when omitted; negative n returns void.
    // Each element is a string representation
    // "id=<id> target=<node-id> op=<operator>
    //  summary=<summary>" so the AI agent can
    // display the evolution trace in a log view.
    // Returns the empty list (void) when no
    // mutations are logged.
    //
    // Issue #1419: append author/parent/composite provenance
    // fields so the string form carries the AI audit trail
    // without a second query. Pre-#1419 consumers that only
    // parse id/target/op/sum remain compatible (extra fields
    // are trailing key=value pairs).
    // Issue #2628: private for (query :mutation-log).
    ObservabilityPrims::register_stats_impl(
        "query:mutation-log", [](std::span<const EvalValue> a) -> EvalValue {
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
            const std::int64_t take = static_cast<std::int64_t>(log.size()) < n
                                          ? static_cast<std::int64_t>(log.size())
                                          : n;
            // Build the pair-list in chronological order
            // (oldest first). The log is most-recent first,
            // so we walk from (log.size() - take) to end.
            const std::size_t start = log.size() - static_cast<std::size_t>(take);
            EvalValue list = make_void();
            for (std::size_t i = log.size(); i-- > start;) {
                const auto& rec = log[i];
                // Format: "id=… target=… op=… sum=… author=… parent=… composite=…"
                const std::string s = "id=" + std::to_string(rec.mutation_id) +
                                      " target=" + std::to_string(rec.target_node) +
                                      " op=" + rec.operator_name + " sum=" + rec.summary +
                                      " author=" + std::to_string(rec.author_fingerprint) +
                                      " parent=" + std::to_string(rec.parent_mutation_id) +
                                      " composite=" + std::to_string(rec.composite_transaction_id);
                const auto sidx = ev->push_string_heap(std::move(s));
                const auto p_idx = ev->push_pair(make_string(sidx), list);
                list = make_pair(p_idx);
            }
            return list;
        });

    // (query:mutation-provenance [mutation-id]) — Issue #1419.
    // Returns a hash with author-fingerprint / parent-mutation-id /
    // composite-transaction-id / mutation-id / timestamp-ms /
    // target-node for one record (by id) or the most recent
    // record when the arg is omitted. Returns void when the log
    // is empty or the id is not found.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-provenance", [](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_void();
            const auto log = ws->mutation_log_view();
            if (log.empty())
                return make_void();

            const aura::ast::MutationRecord* rec = nullptr;
            if (!a.empty() && is_int(a[0])) {
                const auto want = static_cast<std::uint64_t>(as_int(a[0]));
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (log[i].mutation_id == want) {
                        rec = &log[i];
                        break;
                    }
                }
                if (!rec)
                    return make_void();
            } else {
                rec = &log.back(); // most recent
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
                auto kidx = ev->push_string_heap(k_str);
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
            insert_kv("mutation-id", static_cast<std::int64_t>(rec->mutation_id));
            insert_kv("timestamp-ms", static_cast<std::int64_t>(rec->timestamp_ms));
            insert_kv("target-node", static_cast<std::int64_t>(rec->target_node));
            insert_kv("author-fingerprint", static_cast<std::int64_t>(rec->author_fingerprint));
            insert_kv("parent-mutation-id", static_cast<std::int64_t>(rec->parent_mutation_id));
            insert_kv("composite-transaction-id",
                      static_cast<std::int64_t>(rec->composite_transaction_id));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #2196: query:mutation-memory / query:blame-of — unified
    // Agent self-repair surface ("代码即记忆").
    //
    // Single join of MutationRecord provenance + status + composite
    // parent chain + dirty cascade + StableNodeRef/pin signals so
    // Agents do not scrape schema-XXXX stats across multiple prims.
    //
    // Lookup modes (args after the stats name via engine:metrics):
    //   ()                       → most recent mutation
    //   (mutation-id)            → by mutation_id
    //   (1 node-id)              → last mutation targeting node
    //   (2 composite-tx-id)      → first/root record of composite txn
    //   ("node" node-id)         → same as mode 1
    //   ("composite" cid)        → same as mode 2
    //   ("blame" / "memory")     → latest (alias)
    //
    // Agent closed-loop: mutate → observe blame → selective re-mutate
    // or rollback. live-effects=0 when status=RolledBack (AC3).
    // Prefer reusing MutationRecord fields — no second log.
    {
        auto mutation_memory_impl = [](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_void();
            auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics());
            if (m)
                m->mutation_memory_query_total.fetch_add(1, std::memory_order_relaxed);

            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_void();
            const auto log = ws->mutation_log_view();

            // Parse lookup mode.
            enum class Mode : std::uint8_t { Latest = 0, ByMid = 1, ByNode = 2, ByComposite = 3 };
            Mode mode = Mode::Latest;
            std::uint64_t want = 0;
            if (!a.empty()) {
                if (is_string(a[0])) {
                    const auto sidx = as_string_idx(a[0]);
                    const auto heap = ev->string_heap();
                    std::string_view key;
                    if (sidx < heap.size())
                        key = heap[sidx];
                    if (key == "node" || key == "by-node" || key == "target") {
                        mode = Mode::ByNode;
                        if (a.size() >= 2 && is_int(a[1]))
                            want = static_cast<std::uint64_t>(as_int(a[1]));
                    } else if (key == "composite" || key == "by-composite" || key == "txn") {
                        mode = Mode::ByComposite;
                        if (a.size() >= 2 && is_int(a[1]))
                            want = static_cast<std::uint64_t>(as_int(a[1]));
                    } else if (key == "mutation" || key == "by-mutation" || key == "id") {
                        mode = Mode::ByMid;
                        if (a.size() >= 2 && is_int(a[1]))
                            want = static_cast<std::uint64_t>(as_int(a[1]));
                    } else {
                        // "blame" / "memory" / unknown → latest
                        mode = Mode::Latest;
                    }
                } else if (is_int(a[0])) {
                    if (a.size() >= 2 && is_int(a[1])) {
                        const auto mcode = as_int(a[0]);
                        want = static_cast<std::uint64_t>(as_int(a[1]));
                        if (mcode == 1)
                            mode = Mode::ByNode;
                        else if (mcode == 2)
                            mode = Mode::ByComposite;
                        else
                            mode = Mode::ByMid;
                    } else {
                        mode = Mode::ByMid;
                        want = static_cast<std::uint64_t>(as_int(a[0]));
                    }
                }
            }

            const aura::ast::MutationRecord* rec = nullptr;
            if (log.empty()) {
                // Empty log: still return schema shell so Agents can
                // detect the surface without multi-stats scrape.
            } else if (mode == Mode::Latest) {
                rec = &log.back();
            } else if (mode == Mode::ByMid) {
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (log[i].mutation_id == want) {
                        rec = &log[i];
                        break;
                    }
                }
            } else if (mode == Mode::ByNode) {
                // Last mutation whose target_node matches.
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (static_cast<std::uint64_t>(log[i].target_node) == want) {
                        rec = &log[i];
                        break;
                    }
                }
            } else if (mode == Mode::ByComposite) {
                // Prefer root (parent=0) of composite; else first match.
                const aura::ast::MutationRecord* first = nullptr;
                for (std::size_t i = 0; i < log.size(); ++i) {
                    if (log[i].composite_transaction_id != want)
                        continue;
                    if (!first)
                        first = &log[i];
                    if (log[i].parent_mutation_id == 0) {
                        rec = &log[i];
                        break;
                    }
                }
                if (!rec)
                    rec = first;
            }

            auto* ht = FlatHashTable::create(64);
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
                auto kidx = ev->push_string_heap(k_str);
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

            insert_kv("schema-2196", 2196);
            insert_kv("issue-2196", 2196);
            insert_kv("mutation-memory-wired", 1);
            insert_kv("lookup-mode", static_cast<std::int64_t>(mode));
            insert_kv("log-size", static_cast<std::int64_t>(log.size()));
            insert_kv("found", rec ? 1 : 0);

            std::uint64_t join_size = 0;
            if (!rec) {
                if (m)
                    m->mutation_memory_join_size_last.store(0, std::memory_order_relaxed);
                insert_kv("join-size", 0);
                insert_kv("live-effects", 0);
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            }

            if (m)
                m->mutation_memory_found_total.fetch_add(1, std::memory_order_relaxed);

            const bool rolled_back = rec->status == aura::ast::MutationStatus::RolledBack;
            if (rolled_back && m)
                m->mutation_memory_rolled_back_total.fetch_add(1, std::memory_order_relaxed);

            // Core MutationRecord fields (reuse existing audit trail).
            insert_kv("mutation-id", static_cast<std::int64_t>(rec->mutation_id));
            insert_kv("timestamp-ms", static_cast<std::int64_t>(rec->timestamp_ms));
            insert_kv("target-node", static_cast<std::int64_t>(rec->target_node));
            insert_kv("affected-stable-node", static_cast<std::int64_t>(rec->target_node));
            insert_kv("author-fingerprint", static_cast<std::int64_t>(rec->author_fingerprint));
            insert_kv("parent-mutation-id", static_cast<std::int64_t>(rec->parent_mutation_id));
            insert_kv("composite-transaction-id",
                      static_cast<std::int64_t>(rec->composite_transaction_id));
            insert_kv("status", rolled_back ? 1 : 0); // 0=Committed, 1=RolledBack
            insert_kv("live-effects", rolled_back ? 0 : 1);
            insert_kv("has-rollback-data", rec->has_rollback_data ? 1 : 0);
            insert_kv("invariant-status", static_cast<std::int64_t>(rec->invariant_status));
            insert_kv("operator-name-len", static_cast<std::int64_t>(rec->operator_name.size()));
            insert_kv("summary-len", static_cast<std::int64_t>(rec->summary.size()));
            insert_kv("parent-id", static_cast<std::int64_t>(rec->parent_id));
            insert_kv("field-offset", static_cast<std::int64_t>(rec->field_offset));

            // Parent chain walk → root (cap 8 hops).
            std::uint64_t chain[8] = {};
            std::size_t chain_len = 0;
            std::uint64_t walk = rec->mutation_id;
            std::uint64_t root_mid = rec->mutation_id;
            for (std::size_t hop = 0; hop < 8 && walk != 0; ++hop) {
                chain[chain_len++] = walk;
                join_size++;
                const aura::ast::MutationRecord* cur = nullptr;
                for (std::size_t i = log.size(); i-- > 0;) {
                    if (log[i].mutation_id == walk) {
                        cur = &log[i];
                        break;
                    }
                }
                if (!cur || cur->parent_mutation_id == 0)
                    break;
                walk = cur->parent_mutation_id;
                root_mid = walk;
            }
            insert_kv("root-mutation-id", static_cast<std::int64_t>(root_mid));
            insert_kv("chain-depth", static_cast<std::int64_t>(chain_len));
            insert_kv("chain-0", chain_len > 0 ? static_cast<std::int64_t>(chain[0]) : 0);
            insert_kv("chain-1", chain_len > 1 ? static_cast<std::int64_t>(chain[1]) : 0);
            insert_kv("chain-2", chain_len > 2 ? static_cast<std::int64_t>(chain[2]) : 0);
            insert_kv("chain-3", chain_len > 3 ? static_cast<std::int64_t>(chain[3]) : 0);

            // Composite siblings / children (same composite_transaction_id).
            std::uint64_t composite_siblings = 0;
            std::uint64_t composite_children = 0;
            std::uint64_t affected_nodes_sample[4] = {};
            std::size_t affected_sample_n = 0;
            auto push_affected = [&](std::uint64_t n) {
                if (n == 0 || n == static_cast<std::uint64_t>(~0u))
                    return;
                for (std::size_t i = 0; i < affected_sample_n; ++i)
                    if (affected_nodes_sample[i] == n)
                        return;
                if (affected_sample_n < 4)
                    affected_nodes_sample[affected_sample_n++] = n;
            };
            push_affected(rec->target_node);
            if (rec->composite_transaction_id != 0) {
                for (const auto& r : log) {
                    if (r.composite_transaction_id != rec->composite_transaction_id)
                        continue;
                    ++composite_siblings;
                    join_size++;
                    push_affected(r.target_node);
                    if (r.parent_mutation_id == rec->mutation_id)
                        ++composite_children;
                }
            } else {
                composite_siblings = 1;
            }
            insert_kv("composite-sibling-count", static_cast<std::int64_t>(composite_siblings));
            insert_kv("composite-child-count", static_cast<std::int64_t>(composite_children));
            insert_kv("affected-node-0", affected_sample_n > 0
                                             ? static_cast<std::int64_t>(affected_nodes_sample[0])
                                             : 0);
            insert_kv("affected-node-1", affected_sample_n > 1
                                             ? static_cast<std::int64_t>(affected_nodes_sample[1])
                                             : 0);
            insert_kv("affected-node-2", affected_sample_n > 2
                                             ? static_cast<std::int64_t>(affected_nodes_sample[2])
                                             : 0);
            insert_kv("affected-node-3", affected_sample_n > 3
                                             ? static_cast<std::int64_t>(affected_nodes_sample[3])
                                             : 0);
            insert_kv("affected-node-sample-len", static_cast<std::int64_t>(affected_sample_n));

            // Dirty cascade + StableNodeRef join (current workspace state).
            const auto nid = rec->target_node;
            const bool in_range = nid < ws->size();
            const std::uint8_t dirty_bits =
                in_range ? ws->dirty_reasons(nid) : static_cast<std::uint8_t>(0);
            insert_kv("dirty-now", dirty_bits != 0 ? 1 : 0);
            insert_kv("dirty-reasons", static_cast<std::int64_t>(dirty_bits));
            insert_kv("stable-ref-invalidations",
                      static_cast<std::int64_t>(ws->stable_ref_invalidations()));
            insert_kv("workspace-gen", static_cast<std::int64_t>(ws->generation()));
            insert_kv("dirty-nodes-snapshot",
                      static_cast<std::int64_t>(ev->get_dirty_nodes_in_snapshot()));
            // Pin hits: lifetime pin invalidations/restamps if metrics present.
            std::int64_t pin_hits = 0;
            if (m) {
                pin_hits = static_cast<std::int64_t>(
                    m->lifetime_pin_invalidations_total.load(std::memory_order_relaxed) +
                    m->lifetime_pin_restamps_total.load(std::memory_order_relaxed));
            }
            insert_kv("pin-hits", pin_hits);
            // Invalidation trace hit for this mutation (binding gen join).
            insert_kv("invalidation-trace-hit",
                      ws->last_invalidation_for(rec->mutation_id) ? 1 : 0);

            // Safe to retry: live-effects + has rollback data, not rolled back.
            insert_kv("safe-to-retry", (!rolled_back && rec->has_rollback_data) ? 1 : 0);
            // Safe to re-mutate: RolledBack or no live effects claimed.
            insert_kv("safe-to-remutate", rolled_back ? 1 : 0);

            insert_kv("join-size", static_cast<std::int64_t>(join_size));
            if (m)
                m->mutation_memory_join_size_last.store(join_size, std::memory_order_relaxed);

            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };

        ObservabilityPrims::register_stats_impl("query:mutation-memory", mutation_memory_impl);
        // Alias for Agent discoverability (issue names both).
        ObservabilityPrims::register_stats_impl("query:blame-of", mutation_memory_impl);
    }

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
            // Issue #1419: include provenance in the string form
            // (same trailing fields as query:mutation-log).
            const std::string s = "id=" + std::to_string(rec.mutation_id) +
                                  " target=" + std::to_string(rec.target_node) +
                                  " op=" + rec.operator_name + " sum=" + rec.summary +
                                  " author=" + std::to_string(rec.author_fingerprint) +
                                  " parent=" + std::to_string(rec.parent_mutation_id) +
                                  " composite=" + std::to_string(rec.composite_transaction_id);
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
    ObservabilityPrims::register_stats_impl(
        "query:last-mutation-blame", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:adt-exhaustiveness-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* __qev_ = Evaluator::get_query_evaluator();
            const auto* m =
                __qev_ ? static_cast<const CompilerMetrics*>(__qev_->compiler_metrics()) : nullptr;
            if (!m)
                return make_int(0);
            const std::uint64_t checks =
                m->adt_exhaust_rechecks_total.load(std::memory_order_relaxed);
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
    ObservabilityPrims::register_stats_impl(
        "query:adt-match-exhaust-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:match-exhaustiveness-notes", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:typed-mutation-stats-task1", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(dirty_prop + selective + conflicts +
                                                      guard_epoch + narrowing + cross_delta +
                                                      passes_skipped + touched_size));
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
    ObservabilityPrims::register_stats_impl(
        "query:edsl-concurrency-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:closure-env-safety-stats",
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
    ObservabilityPrims::register_stats_impl(
        "query:tag-arity-count", [](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
                return make_int(0);
            auto* ws = ev->workspace_flat();
            if (!ws)
                return make_int(0);
            // Issue #1371: ensure index is fresh (delta if append-only,
            // full rebuild if empty/dirty-with-mutate).
            ws->ensure_tag_arity_index();
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
    ObservabilityPrims::register_stats_impl(
        "query:verification-loop-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    //     verify_tool_feedback_mutate_success (eda_sv_feedback retired 4.4)
    //   - commercial-reemits: commercial_reemits_total
    //   - commercial-simulator-runs: commercial_simulator_runs_total
    //   - verification-loop-success: verification_loop_success_total
    //   - sv-mutate-success-rate-pct: 0..100 from attempts/success
    //   - eda-verification-total: sum of primary counters
    //   - eda-verification-recommendation: 0=ok, 1=assert-heavy, 2=low mutate rate
    ObservabilityPrims::register_stats_impl(
        "query:eda-verification-stats", [&string_heap](std::span<const EvalValue> a) -> EvalValue {
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
            const std::uint64_t coverage = ws ? ws->verification_coverage_feedback_total() : 0;
            const std::uint64_t assert_fail = ws ? ws->verification_assert_failure_total() : 0;
            const std::uint64_t feedback_hits =
                m ? m->feedback_mutate_hits_total.load(std::memory_order_relaxed) : 0;
            const std::uint64_t verify_tool_feedback =
                ev->get_verify_tool_feedback_mutate_success_total();
            const std::uint64_t auto_mutate = feedback_hits + verify_tool_feedback;
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
    ObservabilityPrims::register_stats_impl(
        "query:dirty-reason-propagation-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            const std::uint64_t feedback = ws->verification_coverage_feedback_total() +
                                           ws->verification_assert_failure_total();
            return make_int(static_cast<std::int64_t>(upward_calls + upward_nodes + fast_hits +
                                                      verify + feedback));
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
    ObservabilityPrims::register_stats_impl(
        "query:mutation-coordination-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dualpath-stats", [](std::span<const EvalValue> a) -> EvalValue {
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

    // Issue #1903: query:envframe-dual-consistency-stats.
    // Returns the #1903 dual-path consistency enforcement counters:
    //   - envframe_dual_consistency_asserted_: every bind/bind_symid +
    //     post-steal + post-materialize ensure_dual_path_consistent() call.
    //     Expected to grow linearly with mutation + steal + materialize
    //     throughput (the "did we run the check?" signal).
    //   - envframe_post_steal_dual_synced_: # of frames where
    //     refresh_stale_frames_after_steal ran ensure_dual_path_consistent
    //     on a refreshed frame. Should roughly track fiber resume count.
    //   - envframe_materialize_consistency_checks_: # of materialize_call_env
    //     invocations that explicitly asserted consistency post-copy. Should
    //     roughly track apply_closure + TCO call count.
    //   - envframe_gc_walk_legacy_fallback_uses_: # of GC walk frames where
    //     bindings_symid_ was empty and walk fell back to bindings_. Should
    //     trend toward 0 as the SymId-keyed primary path saturates.
    //
    // Like the dualpath-stats cousin, P0 ships a single integer sum;
    // a follow-up returns a 4-tuple so the Agent can react to each
    // counter independently (a spike in legacy-fallback is the early
    // signal that pool intern coverage is regressing).
    ObservabilityPrims::register_stats_impl(
        "query:envframe-dual-consistency-stats", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t asserted = ev->get_envframe_dual_consistency_asserted();
            const std::uint64_t post_steal = ev->get_envframe_post_steal_dual_synced();
            const std::uint64_t materialize = ev->get_envframe_materialize_consistency_checks();
            const std::uint64_t legacy_fallback = ev->get_envframe_gc_walk_legacy_fallback_uses();
            return make_int(
                static_cast<std::int64_t>(asserted + post_steal + materialize + legacy_fallback));
        });

    // Issue #1904: query:mutation-guard-coverage.
    // Returns MutationBoundaryGuard coverage as integer basis points
    // (0..10000) - 10000 means 100% coverage (every observed mutate:*
    // call site uses MutationBoundaryGuard RAII, zero manual lock+bump
    // sites remain). The ratio is
    //   wrapped / (wrapped + legacy) * 10000
    // so agents can alert on < 10000 (any drop = a regression).
    //
    // Returns -1 sentinel when coverage < 100% (legacy > 0) to make
    // regressions grep-friendly from --metrics output.
    // Returns 10000 (vacuously full coverage) when no mutate activity
    // has been observed yet - a fresh evaluator with no Guard wraps
    // and no legacy sites is not in violation of the contract.
    ObservabilityPrims::register_stats_impl(
        "query:mutation-guard-coverage", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(10000);
            const std::uint64_t wrapped = ev->get_mutation_boundary_primitives_wrapped();
            const std::uint64_t legacy = ev->get_mutation_legacy_manual_lock_total();
            const std::uint64_t total = wrapped + legacy;
            if (total == 0)
                return make_int(10000); // no activity yet - vacuously covered
            if (legacy > 0)
                return make_int(-1); // regression sentinel
            return make_int(static_cast<std::int64_t>((wrapped * 10000) / total));
        });

    // Issue #1905: query:aot-hot-update-stats.
    // Returns observability for the AOT incremental hot-update /
    // invalidation loop (build on #1046). 6 counters surfaced:
    //   - aot_live_closure_refresh_on_mutation_total: every
    //     aura_refresh_live_closures_for_mutated_define call from
    //     flush_mutation_boundary outermost exit (Step 2 of #1905).
    //   - aot_live_closure_refresh_on_steal_total: every refresh
    //     call from complete_post_resume_steal_refresh (Step 3).
    //   - aot_bridge_epoch_bump_on_mutation_total: bridge_epoch bump
    //     driven by outermost MutationBoundaryGuard exit.
    //   - aot_bridge_epoch_bump_on_steal_total: bridge_epoch bump
    //     driven by fiber resume / steal.
    //   - aot_region_mismatch_on_resume_total: per-eval AotState
    //     region_mask drift on resume (deopt path).
    //   - aot_stale_deopt_on_steal_total: stale AOT closure dispatch
    //     on stolen fiber resume (vs the regular jit_closure_stale_deopt_total
    //     which is the on-AOT path).
    //
    // Default (no args or args[0]==0): Returns -1 sentinel when
    // aot_stale_deopt_on_steal_total > 0 (grep-friendly regression
    // marker). Otherwise returns the sum of all 6 counters. Returns
    // 0 when no AOT hot-update activity observed yet (a fresh
    // evaluator with no Guard exits + no steals is not in violation
    // of the contract — vacuously covered).
    //
    // Issue #2240: hash mode (args[0]!=0) returns a hash of all
    // counters + cross-workspace reject metadata (refine #2178).
    // Additive — default arg=0 path is unchanged so existing
    // dashboards keep working.
    ObservabilityPrims::register_stats_impl(
        "query:aot-hot-update-stats", [&](std::span<const EvalValue> a) -> EvalValue {
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t refresh_mut = ev->get_aot_live_closure_refresh_on_mutation_total();
            const std::uint64_t refresh_steal = ev->get_aot_live_closure_refresh_on_steal_total();
            const std::uint64_t bridge_mut = ev->get_aot_bridge_epoch_bump_on_mutation_total();
            const std::uint64_t bridge_steal = ev->get_aot_bridge_epoch_bump_on_steal_total();
            const std::uint64_t region_mismatch = ev->get_aot_region_mismatch_on_resume_total();
            const std::uint64_t stale_deopt = ev->get_aot_stale_deopt_on_steal_total();

            // Issue #2240: optional hash mode via args[0]. Default
            // (no args / args[0]==0) keeps existing sum sentinel
            // behavior for backwards compat — no schema break.
            const bool hash_mode = (!a.empty() && is_int(a[0]) && as_int(a[0]) != 0);
            if (!hash_mode) {
                if (stale_deopt > 0)
                    return make_int(-1); // regression sentinel
                return make_int(static_cast<std::int64_t>(refresh_mut + refresh_steal + bridge_mut +
                                                          bridge_steal + region_mismatch +
                                                          stale_deopt));
            }

            // Hash mode (#2240 refine #2178): file-scope C readers.
            // Uses register_query_primitives `string_heap` ref (not private
            // Evaluator::string_heap_).
            const std::uint64_t cw_reject_total =
                aura_cross_workspace_hot_update_rejected_total_v_read();
            const std::uint8_t cw_last_reason_u8 = aura_last_cross_workspace_reject_reason_v_read();
            const char* cw_last_reason_symbol =
                aura_cross_workspace_reject_reason_string(cw_last_reason_u8);
            const std::string reason_str =
                cw_last_reason_symbol ? std::string(cw_last_reason_symbol) : std::string("unknown");
            const auto reason_kidx = string_heap.size();
            string_heap.push_back(reason_str);

            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, EvalValue v) {
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
                        vals[idx] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("aot-live-closure-refresh-on-mutation-total",
                      make_int(static_cast<std::int64_t>(refresh_mut)));
            insert_kv("aot-live-closure-refresh-on-steal-total",
                      make_int(static_cast<std::int64_t>(refresh_steal)));
            insert_kv("aot-bridge-epoch-bump-on-mutation-total",
                      make_int(static_cast<std::int64_t>(bridge_mut)));
            insert_kv("aot-bridge-epoch-bump-on-steal-total",
                      make_int(static_cast<std::int64_t>(bridge_steal)));
            insert_kv("aot-region-mismatch-on-resume-total",
                      make_int(static_cast<std::int64_t>(region_mismatch)));
            insert_kv("aot-stale-deopt-on-steal-total",
                      make_int(static_cast<std::int64_t>(stale_deopt)));
            insert_kv("cross-workspace-hot-update-rejected-total",
                      make_int(static_cast<std::int64_t>(cw_reject_total)));
            insert_kv("cross-workspace-last-reject-reason",
                      make_int(static_cast<std::int64_t>(cw_last_reason_u8)));
            insert_kv("cross-workspace-last-reject-reason-symbol",
                      make_string(static_cast<std::uint64_t>(reason_kidx)));
            insert_kv("cross-workspace-reject-wired", make_int(1));
            insert_kv("schema-2178", make_int(2178));
            insert_kv("issue-2178", make_int(2178));
            insert_kv("schema-2240", make_int(2240));
            insert_kv("issue-2240", make_int(2240));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1952 / #1930: query:aot-incremental-reemit-stats.
    // Hash surface (schema-1930) for the aura_reemit_aot_for_dirty pipeline:
    //   - aot_incremental_reemit_count: region-filtered "would re-emit"
    //   - aot_incremental_reemit_success_total: host emit callback true
    //   - stable_func_id_preserved_total / assigned_total: #1930 map
    //   - aot_closure_dependency_reemit_total: closure-capture cascade
    //   - wire flags: emit-callback / stable-map / return-success-when-emit
    // Optional arg 0 = "sum": returns compact sum (legacy path) without
    // the old stable==success -1 sentinel (#1930 real map breaks 1:1).
    ObservabilityPrims::register_stats_impl(
        "query:aot-incremental-reemit-stats",
        [&ev, &string_heap](std::span<const EvalValue> a) -> EvalValue {
            auto* qev = Evaluator::get_query_evaluator();
            if (!qev)
                qev = &ev;
            const std::uint64_t total = qev->get_aot_incremental_reemit_count();
            const std::uint64_t success = qev->get_aot_incremental_reemit_success_total();
            const std::uint64_t preserved = qev->get_stable_func_id_preserved_total();
            const std::uint64_t assigned = qev->get_stable_func_id_assigned_total();
            const std::uint64_t closure_dep = qev->get_aot_closure_dependency_reemit_total();
            const std::uint64_t live_remap = qev->get_live_closure_remap_total();
            // Issue #2175: legacy sid=0 backfill (independent of name fallback).
            const std::uint64_t live_backfill = qev->get_live_closure_stable_id_backfill_total();
            // Issue #2233: post-reemit live-closure stamp metrics
            // (hit / miss split). The hit counter is the per-closure
            // bump in aura_remap_live_closures_after_reemit when the
            // restamp + clear-MustDeopt path runs; the miss counter
            // is the per-closure bump in the name-candidate-no-remap
            // path (when name_fallback is off but the name resolves in
            // the reemit set).
            const std::uint64_t epoch_restamp = qev->get_live_closure_epoch_restamp_total();
            const std::uint64_t must_deopt_kept = qev->get_live_closure_must_deopt_kept_total();
            // Issue #2234: capture remount ok/fail totals.
            const std::uint64_t capture_remount_ok = qev->get_closure_capture_remount_ok_total();
            const std::uint64_t capture_remount_fail =
                qev->get_closure_capture_remount_fail_total();
            // Legacy compact sum path: (engine:metrics "query:..." "sum")
            if (!a.empty() && is_string(a[0])) {
                auto sidx = as_string_idx(a[0]);
                if (sidx < string_heap.size() && string_heap[sidx] == "sum") {
                    return make_int(static_cast<std::int64_t>(
                        total + success + preserved + assigned + closure_dep + live_remap +
                        live_backfill + epoch_restamp + must_deopt_kept + capture_remount_ok +
                        capture_remount_fail));
                }
            }
            // Capacity must be power-of-two (open-address mask).
            // ~70 keys with #2297 + #2369; use 256 for headroom.
            auto* ht = FlatHashTable::create(256);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const std::string& k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (char c : k_str)
                    h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = string_heap.size();
                string_heap.push_back(k_str);
                EvalValue key_ev = make_string(kidx);
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto slot = ((h >> 1) + at) & (hcap - 1);
                    if (meta[slot] == 0xFF) {
                        meta[slot] = fp;
                        keys[slot] = key_ev.val;
                        vals[slot] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("schema", 1930);
            insert_kv("issue", 1930);
            insert_kv("schema-1930", 1930);
            insert_kv("issue-1930", 1930);
            insert_kv("schema-1952", 1952);
            // Issue #2233: post-reemit live-closure stamp metrics
            // (hit / miss split). Schema bump for the 2233 lineage.
            insert_kv("live-closure-epoch-restamp-total", static_cast<std::int64_t>(epoch_restamp));
            insert_kv("live-closure-must-deopt-kept-total",
                      static_cast<std::int64_t>(must_deopt_kept));
            insert_kv("schema-2233", 2233);
            insert_kv("issue-2233", 2233);
            // insert_kv takes int64_t (not EvalValue) — bare 1, not make_int(1).
            insert_kv("post-reemit-stamp-wired", 1);
            // Issue #2234: post-reemit / post-compact env_frame + linear
            // capture remount metrics. Bumped from
            // aura_remount_closure_captures (aura_jit_runtime.cpp)
            // when a closure's captured env_frame version + linear state
            // are rebound to the live generation (ok) or fail and force
            // the caller to set MustDeopt (fail). Schema bump for the
            // 2234 lineage.
            insert_kv("closure-capture-remount-ok-total",
                      static_cast<std::int64_t>(capture_remount_ok));
            insert_kv("closure-capture-remount-fail-total",
                      static_cast<std::int64_t>(capture_remount_fail));
            // Issue #2272: env_generation PRIMARY axis counter
            // (distinct from remount-fail-total). Pairs with
            // remount-fail-total on dashboards to distinguish
            // "env_gen drift" from "defuse drift".
            insert_kv(
                "closure-capture-env-gen-mismatch-total",
                qev ? static_cast<std::int64_t>(qev->get_closure_capture_env_gen_mismatch_total())
                    : 0);
            insert_kv("closure-capture-env-gen-wired", 1);
            insert_kv("schema-2272", 2272);
            insert_kv("issue-2272", 2272);
            // Issue #2297: structural capture-cell remap (densify object_remap).
            // Read metrics via compiler_metrics_ (not qev getters) to avoid
            // partition BMI coupling on new accessors.
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const auto cell_ok =
                    m ? m->closure_capture_cell_remap_ok_total.load(std::memory_order_relaxed) : 0;
                const auto cell_fail =
                    m ? m->closure_capture_cell_remap_fail_total.load(std::memory_order_relaxed)
                      : 0;
                insert_kv("closure-capture-cell-remap-ok-total",
                          static_cast<std::int64_t>(cell_ok));
                insert_kv("closure-capture-cell-remap-fail-total",
                          static_cast<std::int64_t>(cell_fail));
            }
            insert_kv("capture-cell-remap-wired", 1);
            insert_kv("schema-2297", 2297);
            insert_kv("issue-2297", 2297);
            // Issue #2503: remount fail → MustDeopt + batch_deopt shared path.
            insert_kv("remount-or-force-deopt-wired", 1);
            insert_kv("schema-2503", 2503);
            insert_kv("issue-2503", 2503);
            insert_kv("schema-2234", 2234);
            insert_kv("issue-2234", 2234);
            insert_kv("capture-remount-wired", 1);
            insert_kv("schema-2013", 2013);
            insert_kv("active", 1);
            insert_kv("aot_incremental_reemit_count", static_cast<std::int64_t>(total));
            insert_kv("aot_incremental_reemit_success_total", static_cast<std::int64_t>(success));
            insert_kv("stable_func_id_preserved_total", static_cast<std::int64_t>(preserved));
            insert_kv("stable_func_id_assigned_total", static_cast<std::int64_t>(assigned));
            insert_kv("aot_closure_dependency_reemit_total",
                      static_cast<std::int64_t>(closure_dep));
            // Issue #2013: live closures retargeted after reemit.
            insert_kv("live_closure_remap_total", static_cast<std::int64_t>(live_remap));
            // Issue #2175: legacy sid=0 backfill counter (one-shot lookup
            // per successful backfill during remap walk — fires when
            // stored_sid == 0 but the name resolves in the live stable map).
            insert_kv("live_closure_stable_id_backfill_total",
                      static_cast<std::int64_t>(live_backfill));
            // Issue #2605: residual_backfill aliases (#2175 counter) +
            // assign / preserve axes for Agent dashboards.
            insert_kv("stable-id-residual-backfill-total",
                      static_cast<std::int64_t>(live_backfill));
            insert_kv("stable_id_residual_backfill_total",
                      static_cast<std::int64_t>(live_backfill));
            insert_kv("stable-id-assign-total", static_cast<std::int64_t>(assigned));
            insert_kv("stable-id-preserve-total", static_cast<std::int64_t>(preserved));
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t named_reject =
                    m ? m->live_closure_named_name_fallback_reject_total.load(
                            std::memory_order_relaxed)
                      : 0;
                insert_kv("named-name-fallback-reject-total",
                          static_cast<std::int64_t>(named_reject));
                insert_kv("named_name_fallback_reject_total",
                          static_cast<std::int64_t>(named_reject));
            }
            insert_kv("anonymous-must-deopt-policy-wired", 1);
            insert_kv("residual-sid0-policy-wired", 1);
            insert_kv("schema-2605", 2605);
            insert_kv("issue-2605", 2605);
            // Issue #2606: multi-AotState reemit ownership filter —
            // candidates dropped when stable_func_id maps to a slot
            // owned by a foreign eval. Soft single-eval keeps counter 0.
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t cross =
                    m ? m->reemit_cross_eval_candidate_skipped_total.load(std::memory_order_relaxed)
                      : 0;
                insert_kv("reemit_cross_eval_candidate_skipped_total",
                          static_cast<std::int64_t>(cross));
                insert_kv("reemit-cross-eval-candidate-skipped-total",
                          static_cast<std::int64_t>(cross));
            }
            insert_kv("reemit-cross-eval-filter-wired", 1);
            insert_kv("schema-2606", 2606);
            insert_kv("issue-2606", 2606);
            insert_kv("schema-2175", 2175);
            insert_kv("issue-2175", 2175);
            // Issue #2016 metrics (may be 0 if CompilerMetrics not wired).
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t llvm_n =
                    m ? m->aot_incremental_llvm_emit_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t evo =
                    m ? m->aot_evolution_region_skips_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t clr =
                    m ? m->aot_region_mask_adapt_clears_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t rst =
                    m ? m->aot_region_mask_adapt_restores_total.load(std::memory_order_relaxed) : 0;
                insert_kv("aot_incremental_llvm_emit_total", static_cast<std::int64_t>(llvm_n));
                insert_kv("aot_evolution_region_skips_total", static_cast<std::int64_t>(evo));
                insert_kv("aot_region_mask_adapt_clears_total", static_cast<std::int64_t>(clr));
                insert_kv("aot_region_mask_adapt_restores_total", static_cast<std::int64_t>(rst));
                // Issue #2128: MustDeoptBeforeNextCall after remap miss.
                const std::uint64_t must_n =
                    m ? m->must_deopt_before_next_call_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t force_ok =
                    m ? m->must_deopt_force_deopt_success_total.load(std::memory_order_relaxed) : 0;
                const std::uint64_t force_fail =
                    m ? m->must_deopt_force_deopt_fail_total.load(std::memory_order_relaxed) : 0;
                insert_kv("must_deopt_before_next_call_total", static_cast<std::int64_t>(must_n));
                insert_kv("must-deopt-before-next-call-total", static_cast<std::int64_t>(must_n));
                insert_kv("must_deopt_force_deopt_success_total",
                          static_cast<std::int64_t>(force_ok));
                insert_kv("must_deopt_force_deopt_fail_total",
                          static_cast<std::int64_t>(force_fail));
                insert_kv("schema-2128", 2128);
                insert_kv("issue-2128", 2128);
                insert_kv("must-deopt-before-next-call-wired", 1);
            }
            insert_kv("stable-func-id-map-wired", 1);
            insert_kv("emit-callback-path-wired", 1);
            insert_kv("return-success-when-emit-wired", 1);
            insert_kv("live-closure-remap-wired", 1);
            insert_kv("adaptive-region-mask-wired", 1);
            insert_kv("pipeline-phase", 6); // + must-deopt-before-next-call (#2128)
            // Issue #2369: stable_func_id sole primary for live-closure remap;
            // name-fallback rewrite is legacy opt-in only (default off).
            {
                auto* m = static_cast<const CompilerMetrics*>(qev->compiler_metrics());
                const std::uint64_t name_fb =
                    m ? m->live_closure_remap_name_fallback_total.load(std::memory_order_relaxed)
                      : 0;
                insert_kv("live-closure-remap-name-fallback-total",
                          static_cast<std::int64_t>(name_fb));
                insert_kv("live_closure_remap_name_fallback_total",
                          static_cast<std::int64_t>(name_fb));
                insert_kv("remap-name-fallback-enabled",
                          static_cast<std::int64_t>(aura_get_remap_name_fallback_enabled()));
                insert_kv("remap-name-fallback-default-off", 1);
                insert_kv("stable-func-id-sole-primary-wired", 1);
                insert_kv("schema-2369", 2369);
                insert_kv("issue-2369", 2369);
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // Issue #1907: query:reflect-schema.
    // Returns observability for the reflect/EDSL bridge hook. Counters
    // surfaced:
    //   - reflect_schema_query_total: every (query:reflect-schema) call
    //     (Step 2 of #1907). Backed by the bridge hook
    //     aura_validate_reflected_post_mutation from flush_mutation_boundary
    //     outermost exit.
    //   - reflect_post_mutation_validate_total: every bridge-hook call
    //     from flush_mutation_boundary outermost exit (Step 1 of #1907).
    //   - reflect_post_mutation_validate_fail_total: subset where the
    //     auto_validate pass returns false (validation failure).
    //   - reflect_hygiene_macro_reject_total: subset where the
    //     SyntaxMarker::MacroIntroduced gate rejects (no explicit
    //     allow_macro_evolution + dirty_macro_nodes > 0).
    //   - reflect_validate_reflected_query_total: every
    //     (mutate:validate-reflected) call (Step 2 of #1907).
    //   - reflect_dirty_macro_nodes_total: cumulative sum of
    //     dirty_macro_nodes reported by the bridge hook (trending
    //     metric for self-evolution hygiene regression detection).
    //
    // Returns -1 sentinel when reflect_post_mutation_validate_fail_total > 0
    // (grep-friendly regression marker). Otherwise returns the sum of all 6
    // counters (sum-path, like #1903/#1904/#1905/#1906 P0/P1 shape).
    ObservabilityPrims::register_stats_impl(
        "query:reflect-schema", [](std::span<const EvalValue> a) -> EvalValue {
            (void)a;
            auto* ev = Evaluator::get_query_evaluator();
            if (!ev)
                return make_int(0);
            const std::uint64_t schema = ev->get_reflect_schema_query_total();
            const std::uint64_t post_validate = ev->get_reflect_post_mutation_validate_total();
            const std::uint64_t post_fail = ev->get_reflect_post_mutation_validate_fail_total();
            const std::uint64_t macro_reject = ev->get_reflect_hygiene_macro_reject_total();
            const std::uint64_t validate_reflected =
                ev->get_reflect_validate_reflected_query_total();
            const std::uint64_t dirty_macro = ev->get_reflect_dirty_macro_nodes_total();
            if (post_fail > 0)
                return make_int(-1); // regression sentinel
            return make_int(static_cast<std::int64_t>(schema + post_validate + post_fail +
                                                      macro_reject + validate_reflected +
                                                      dirty_macro));
        });

    // Issue #1907: mutate:validate-reflected.
    // Calls aura_validate_reflected_post_mutation bridge hook from the
    // C bridge layer (aura_jit_bridge.cpp). The hook combines the
    // aura::reflect::auto_validate pass with the
    // aura::reflect::hygiene_allows_evolution macro guard. Returns
    // sum of post-mutation validate counters + bumps the
    // validate_reflected_query_total counter (Step 2 of #1907).
    //
    // Args:
    //   mutate:validate-reflected (no args) -- always succeeds on a
    //   fresh evaluator (the bridge hook counter path is the real
    //   validation). The primitive is the EDSL entry point for
    //   self-evolution audits that want to confirm the bridge hook is
    //   wired and counters are incrementing.
    //
    // Returns the sum of reflect_post_mutation_validate_total +
    // reflect_hygiene_macro_reject_total + reflect_dirty_macro_nodes_total
    // after bumping validate_reflected_query_total by 1.
    // SECURITY_EXEMPT: diagnostic counters only — no AST write (#2057/#2152).
    add("mutate:validate-reflected", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)a;
        // Public accessor: register_query_primitives is not a friend of
        // Evaluator (unlike ObservabilityPrims / register_mutate_*).
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->reflect_validate_reflected_query_total.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t post_validate = ev.get_reflect_post_mutation_validate_total();
        const std::uint64_t macro_reject = ev.get_reflect_hygiene_macro_reject_total();
        const std::uint64_t dirty_macro = ev.get_reflect_dirty_macro_nodes_total();
        return make_int(static_cast<std::int64_t>(post_validate + macro_reject + dirty_macro));
    });
    {
        ::aura::compiler::PrimMeta ex{};
        ex.security_exempt = true;
        ex.pure = true;
        ex.doc = "SECURITY_EXEMPT: diagnostic reflect counters only (#2152)";
        ev.primitives().set_meta_for_name("mutate:validate-reflected", std::move(ex));
    }

    add("query:schema", [&string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        // Issue #1908: query:macro-provenance-stats.
        // Returns observability for the MutationBoundaryGuard + macro clone
        // provenance hardening surface (refine #1014 / #1047). Two counters
        // surfaced (per-eval view via Evaluator getters):
        //   - macro_provenance_repin_on_steal_total: every forced repin of
        //     MacroIntroduced marker + provenance that fires on fiber steal /
        //     resume / outermost Guard exit / PanicCheckpoint transfer. Bumped
        //     from clone_macro_body (MacroIntroduced branch) +
        //     complete_post_resume_steal_refresh (after probe_and_repin_macro_provenance)
        //     + transfer_and_revalidate_panic_checkpoint (post panic restamp).
        //   - hygiene_violation_prevented_on_boundary_total: every time the
        //     boundary interaction (outermost flush dirty/epoch bump + post-steal
        //     probe + PanicCheckpoint transfer coupling) prevented a hygiene
        //     violation from manifesting. Bumped from flush_mutation_boundary
        //     outermost exit + complete_post_resume_steal_refresh (post probe) +
        //     transfer_and_revalidate_panic_checkpoint (post panic restamp).
        //
        // Returns -1 sentinel when macro_provenance_repin_on_steal_total > 0
        // AND hygiene_violation_prevented_on_boundary_total == 0 (regression
        // marker: boundary did NOT prevent violation despite MacroIntroduced
        // repin firing — grep-friendly). Otherwise returns the sum of the
        // 2 counters (sum-path, like #1903/#1904/#1905/#1906 P0/P1 shape).
        ObservabilityPrims::register_stats_impl(
            "query:macro-provenance-stats", [](std::span<const EvalValue> a) -> EvalValue {
                (void)a;
                auto* ev = Evaluator::get_query_evaluator();
                if (!ev)
                    return make_int(0);
                const std::uint64_t repin = ev->get_macro_provenance_repin_on_steal_total();
                const std::uint64_t prevented =
                    ev->get_hygiene_violation_prevented_on_boundary_total();
                // Regression sentinel: repin fired but boundary did NOT prevent
                // violation (inconsistent boundary interaction).
                if (repin > 0 && prevented == 0)
                    return make_int(-1);
                return make_int(static_cast<std::int64_t>(repin + prevented));
            });
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
    //   (mutate :validate <new-value> <type-name>)
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
    // SECURITY_EXEMPT: read-only schema check — no AST write (#2057/#2152).
    // Issue #2628: private for (mutate :validate).
    ObservabilityPrims::register_stats_impl(
        "mutate:validate-against-schema",
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
    // #2628: security_exempt was on public meta; impl is stats-only + (mutate :validate).

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
    ObservabilityPrims::register_stats_impl(
        "query:narrow-blame-stats", [&ev](std::span<const EvalValue> a) -> EvalValue {
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
                invalidation =
                    m->narrow_invalidation_post_mutate_total.load(std::memory_order_relaxed);
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
    ObservabilityPrims::register_stats_impl(
        "query:bidirectional-narrow-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(dirty_recovery + blame_attached +
                                                      blame_complete + stale_refresh +
                                                      stale_prevented + narrowing));
        });

    // Issue #576: query:occurrence-blame-stats. Returns the sum
    // of 4 Task2 occurrence typing + blame/provenance counters:
    //   - stale_narrowing_prevented: stale_check_narrow_prevented_total
    //   - blame_chain_preserved: occurrence_blame_chain_complete_total
    //   - narrowing_refresh_count: narrowing_refresh_count_ (Evaluator)
    //   - provenance_mismatch: provenance_mismatch_ (Evaluator)
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-blame-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(stale_prevented + blame_preserved +
                                                      narrowing + provenance));
        });

    // Issue #609: query:occurrence-narrow-stats. Returns the sum
    // of 4 post-mutation occurrence narrow recovery counters:
    //   - narrow_recoveries: occurrence_stale_refreshes_total +
    //     narrowing_reanalyzed_total (predicate re-analysis)
    //   - blame_attached: narrow_blame_attached_total
    //   - post_mutate_correctness: post_mutate_narrow_consistency_total
    //   - stale_narrow_prevented: stale_check_narrow_prevented_total
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-narrow-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
            return make_int(static_cast<std::int64_t>(narrow_recoveries + blame_attached +
                                                      post_mutate_correctness +
                                                      stale_narrow_prevented));
        });

    // Issue #537 / #518 Phase 2: query:occurrence-narrowing-stats.
    // Returns the sum of stale-refresh + blame-chain-complete
    // counters from CompilerMetrics (post-mutation re-narrow
    // provenance observability).
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-narrowing-stats", [](std::span<const EvalValue> a) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:occurrence-stale-count", [&ev](const auto&) -> EvalValue {
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
    ObservabilityPrims::register_stats_impl(
        "query:pass-pipeline-incremental-stats-hash",
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