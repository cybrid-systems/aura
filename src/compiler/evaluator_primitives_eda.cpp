// evaluator_primitives_eda.cpp — Issue #499: Foundational EDA primitives
// module (parse/query/mutate/waveform/hardware feedback) for Agent-driven
// verification + hardware co-design workflows.
//
// Issue #1968: eda:* is a commercial EDA vertical (DOMAIN_STATUS deferred).
// Registration gated by AURA_ENABLE_EDA (CMake option, default ON).
// Slim/core builds: -DAURA_ENABLE_EDA=OFF → register_eda_primitives no-op.
// See docs/eda.md + scripts/check_primitive_surface.py COMMERCIAL_DOMAIN_BUDGETS.

module;

#include <sys/stat.h>

#include <cctype>
#include <fstream>
#include <string>
#include <string_view>

#include "observability_metrics.h"
#include "runtime_shared.h"
#include "security_capabilities.h"
#include "hash_meta.h" // FNV constants (#901)

// Default ON when the TU is compiled outside the CMake graph (tools/IDE).
#ifndef AURA_ENABLE_EDA
#define AURA_ENABLE_EDA 1
#endif

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

namespace aura::compiler::primitives_detail::eda_detail {

using EvalValue = types::EvalValue;
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

inline std::optional<aura::ast::NodeTag> tag_from_name(std::string_view name) {
    const auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    auto eq_ci = [&](std::string_view lit) {
        if (name.size() != lit.size())
            return false;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (lower(name[i]) != lower(lit[i]))
                return false;
        }
        return true;
    };
    if (eq_ci("interface"))
        return aura::ast::NodeTag::Interface;
    if (eq_ci("modport"))
        return aura::ast::NodeTag::Modport;
    if (eq_ci("property"))
        return aura::ast::NodeTag::Property;
    if (eq_ci("sequence"))
        return aura::ast::NodeTag::Sequence;
    if (eq_ci("assert"))
        return aura::ast::NodeTag::Assert;
    if (eq_ci("covergroup"))
        return aura::ast::NodeTag::Covergroup;
    if (eq_ci("coverpoint"))
        return aura::ast::NodeTag::Coverpoint;
    if (eq_ci("constraint"))
        return aura::ast::NodeTag::Constraint;
    if (eq_ci("class"))
        return aura::ast::NodeTag::Class;
    return std::nullopt;
}

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

inline std::vector<std::string_view> split_colon(std::string_view line) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        auto pos = line.find(':', start);
        if (pos == std::string_view::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

inline std::vector<aura::ast::SymId> split_ports(aura::ast::StringPool& pool,
                                                 std::string_view ports_csv) {
    std::vector<aura::ast::SymId> out;
    std::size_t start = 0;
    while (start <= ports_csv.size()) {
        auto pos = ports_csv.find(',', start);
        const auto slice = trim(ports_csv.substr(
            start, pos == std::string_view::npos ? std::string_view::npos : pos - start));
        if (!slice.empty())
            out.push_back(pool.intern(std::string(slice)));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return out;
}

void maybe_hardware_feedback(Evaluator& ev, aura::ast::NodeId node) {
    auto* ws = ev.workspace_flat();
    if (!ws || node >= ws->size())
        return;
    if (!aura::compiler::hardware::should_invoke_sv_closedloop_hook(*ws, node))
        return;
    const auto sv_reasons = aura::compiler::hardware::sv_structural_dirty_reasons(*ws, node);
    const auto ppa_reasons = ws->ppa_dirty_reasons(node);
    aura::compiler::hardware::on_structural_mutation(
        node, static_cast<std::uint8_t>(aura::ast::FlatAST::kGeneralDirty | sv_reasons),
        ppa_reasons);
    if (auto* pool = ev.workspace_pool()) {
        const auto reemit = aura::compiler::sv_ir::reemit_sv_node(*ws, *pool, node);
        const auto validation = aura::compiler::sv_ir::validate_sv_emit(reemit.sv_text);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->commercial_reemits_total.fetch_add(1, std::memory_order_relaxed);
            m->sv_verification_dirty_reemit_total.fetch_add(1, std::memory_order_relaxed);
            if (validation.ok)
                m->sv_emit_parse_success_total.fetch_add(1, std::memory_order_relaxed);
            else
                m->sv_emit_parse_fail_total.fetch_add(1, std::memory_order_relaxed);
            m->hardware_backend_hook_calls_total.fetch_add(1, std::memory_order_relaxed);
        }
        ev.record_sv_commercial_emit_fidelity(validation.ok, true,
                                              !reemit.commercial_do_stub.empty());
    }
}

} // namespace aura::compiler::primitives_detail::eda_detail

namespace aura::compiler::primitives_detail {

void register_eda_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev) {
#if !AURA_ENABLE_EDA
    // Issue #1968: commercial EDA vertical disabled for this build.
    (void)add;
    (void)ev;
    return;
#else
    using namespace eda_detail;

    add("eda:parse-netlist", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto* ws = ev.workspace_flat();
        auto* pool = ev.workspace_pool();
        if (!ws || !pool)
            return make_int(0);
        const auto& text = ev.string_heap_[idx];
        std::uint64_t parsed = 0;
        aura::ast::NodeId last_iface = aura::ast::NULL_NODE;
        std::size_t line_start = 0;
        while (line_start <= text.size()) {
            auto line_end = text.find('\n', line_start);
            const auto line = trim(std::string_view{
                text.data() + line_start,
                (line_end == std::string::npos ? text.size() : line_end) - line_start});
            if (!line.empty()) {
                auto parts = split_colon(line);
                if (!parts.empty()) {
                    const auto kind = trim(parts[0]);
                    if (kind == "interface" && parts.size() >= 2) {
                        const auto name = trim(parts[1]);
                        last_iface = ws->add_interface(pool->intern(std::string(name)), {});
                        ++parsed;
                    } else if (kind == "modport" && parts.size() >= 3 &&
                               last_iface != aura::ast::NULL_NODE) {
                        const auto name = trim(parts[1]);
                        const auto ports = split_ports(*pool, trim(parts[2]));
                        const auto mp = ws->add_modport(pool->intern(std::string(name)), ports);
                        ws->insert_child(last_iface, ws->get(last_iface).children.size(), mp);
                        ++parsed;
                        // Issue #661: SV InterfaceIR + ModportIR structure
                        // observability. Bump modport-views by 1 and
                        // ports-count by the size of the modport's
                        // port list. Direction-change wiring is the
                        // follow-up (issue body Action #3).
                        ev.bump_sv_interface_modport_views();
                        ev.bump_sv_interface_ports(ports.size());
                    } else if (kind == "constraint" && parts.size() >= 3) {
                        const auto name = trim(parts[1]);
                        const auto expr = trim(parts[2]);
                        (void)ws->add_constraint(
                            pool->intern(std::string(name)),
                            std::span<const aura::ast::SymId>{pool->intern(std::string(expr))});
                        ++parsed;
                    } else if (kind == "property" && parts.size() >= 3) {
                        const auto name = trim(parts[1]);
                        const auto expr = trim(parts[2]);
                        (void)ws->add_property(pool->intern(std::string(name)),
                                               pool->intern(std::string(expr)));
                        ++parsed;
                    } else if (kind == "coverpoint" && parts.size() >= 3) {
                        const auto var = trim(parts[1]);
                        const auto bins = split_ports(*pool, trim(parts[2]));
                        (void)ws->add_coverpoint(pool->intern(std::string(var)), bins);
                        ++parsed;
                    }
                }
            }
            if (line_end == std::string::npos)
                break;
            line_start = line_end + 1;
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->eda_foundation_parse_total.fetch_add(parsed, std::memory_order_relaxed);
            if (parsed > 0)
                m->eda_infra_parse_success_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_int(static_cast<std::int64_t>(parsed));
    });

    add("eda:query-nodes", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        const auto tag_opt = tag_from_name(ev.string_heap_[idx]);
        if (!tag_opt)
            return make_int(0);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        std::int64_t count = 0;
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (ws->get(id).tag == *tag_opt)
                ++count;
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->eda_foundation_query_total.fetch_add(1, std::memory_order_relaxed);
        return make_int(count);
    });

    add("eda:mutate-add-instance", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        Evaluator::MutationBoundaryGuard guard(ev, &ok);
        if (a.size() < 3 || !is_int(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        auto* pool = ev.workspace_pool();
        if (!ws || !pool)
            return make_bool(false);
        const auto parent = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (!ws->is_live_node(parent))
            return make_bool(false);
        if (ws->get(parent).tag != aura::ast::NodeTag::Interface)
            return make_bool(false);
        auto name_idx = as_string_idx(a[1]);
        auto ports_idx = as_string_idx(a[2]);
        if (name_idx >= ev.string_heap_.size() || ports_idx >= ev.string_heap_.size())
            return make_bool(false);
        const auto ports = split_ports(*pool, ev.string_heap_[ports_idx]);
        ws->restamp_subtree_generation(parent);
        aura::ast::FlatAST::StableNodeRef pref = ws->make_ref(parent);
        if (!pref.is_valid_in(*ws))
            return make_bool(false);
        ev.bump_stable_ref_validated_in_primitives_count();
        const auto inst = ws->add_modport(pool->intern(ev.string_heap_[name_idx]), ports);
        ws->insert_child(parent, ws->get(parent).children.size(), inst);
        ws->bump_sv_mutate_attempt();
        ws->add_mutation(parent, "eda-mutate-add-instance", "interface", "interface+modport",
                         "added modport instance via #499");
        ws->apply_verification_dirty_bits(parent, aura::ast::FlatAST::kCoverageFeedbackDirty);
        ws->apply_verify_dirty_bits(parent, aura::ast::FlatAST::kSvaDirty);
        ws->mark_dirty_upward(parent, aura::ast::FlatAST::kGeneralDirty,
                              ws->ppa_dirty_reasons(parent));
        maybe_hardware_feedback(ev, parent);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->eda_foundation_mutate_total.fetch_add(1, std::memory_order_relaxed);
            m->sva_structured_mutate_hits_total.fetch_add(1, std::memory_order_relaxed);
            m->sv_verification_structure_mutate_hits_total.fetch_add(1, std::memory_order_relaxed);
            m->eda_infra_structured_mutate_total.fetch_add(1, std::memory_order_relaxed);
        }
        ws->bump_sv_mutate_success();
        return make_bool(true);
    });

    add("eda:waveform-snapshot", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_int(0);
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (!ws->is_live_node(nid))
            return make_int(0);
        ws->restamp_subtree_generation(nid);
        aura::ast::FlatAST::StableNodeRef pref = ws->make_ref(nid);
        if (!pref.is_valid_in(*ws))
            return make_int(0);
        ev.bump_stable_ref_validated_in_primitives_count();
        std::int64_t emit_len = 0;
        if (auto* pool = ev.workspace_pool()) {
            const auto reemit = aura::compiler::sv_ir::reemit_sv_node(*ws, *pool, nid);
            emit_len = static_cast<std::int64_t>(reemit.sv_text.size());
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->eda_foundation_waveform_total.fetch_add(1, std::memory_order_relaxed);
        return make_int(emit_len);
    });

    add("eda:run-hardware-feedback", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool ok = true;
        Evaluator::MutationBoundaryGuard guard(ev, &ok);
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto* ws = ev.workspace_flat();
        if (!ws)
            return make_bool(false);
        const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
        if (!ws->is_live_node(nid))
            return make_bool(false);
        ws->restamp_subtree_generation(nid);
        aura::ast::FlatAST::StableNodeRef pref = ws->make_ref(nid);
        if (!pref.is_valid_in(*ws))
            return make_bool(false);
        ev.bump_stable_ref_validated_in_primitives_count();
        ws->mark_dirty_upward(nid, aura::ast::FlatAST::kGeneralDirty, ws->ppa_dirty_reasons(nid));
        maybe_hardware_feedback(ev, nid);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->eda_foundation_feedback_total.fetch_add(1, std::memory_order_relaxed);
            m->feedback_mutate_hits_total.fetch_add(1, std::memory_order_relaxed);
            m->eda_infra_feedback_ingest_total.fetch_add(1, std::memory_order_relaxed);
        }
        return make_bool(true);
    });

    // Issue #616: EDA hardware-co-design primitives — file I/O layer
    // (load-sv, parse-verification-result). These sit on top of the
    // existing (eda:parse-netlist) parser + (json-parse) primitive
    // and provide the Agent loop with a clean file-boundary surface
    // for SV / verification-result files, eliminating the need for
    // callers to manage intermediate string_heap slots.
    //
    // Both primitives return a small hash describing the outcome
    // rather than a bare int/void, so the Agent can branch on
    // status-ok / success without a second query call.

    // build_hash: factor of the kv-builder pattern duplicated across
    // evaluator_primitives_observability/agent/compile. For now this
    // is local to eda.cpp; a shared helper is a separate follow-up
    // (touching every file that owns a copy of the same 35 lines).
    auto build_hash_eda = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
        // Issue #1229 Phase 1: track FlatHashTable creates / estimated alloc bytes.
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->eda_hash_table_creates_total.fetch_add(1, std::memory_order_relaxed);
            m->eda_alloc_bytes_total.fetch_add(static_cast<std::uint64_t>(16 * 24 + kv.size() * 32),
                                               std::memory_order_relaxed);
        }
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        for (auto& [k, v] : kv) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back(k);
            EvalValue key_ev = make_string(kidx);
            bool inserted = false;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key_ev.val;
                    vals[idx] = v.val;
                    ht->size++;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                FlatHashTable::destroy(ht);
                return make_void();
            }
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    };

    // (eda:load-sv path) — Read an SV/SVA file from disk and feed it
    // through the same line-based parser that (eda:parse-netlist)
    // uses, returning a hash describing the outcome.
    //
    // Returned hash:
    //   - path:    the input path string (echoed for chaining)
    //   - node-count: number of nodes added (interface/modport/
    //                 constraint/property/coverpoint)
    //   - status-ok: 1 if the file was read AND parsed; 0 otherwise
    //   - reason:    empty string on success; human-readable failure
    //                reason otherwise (missing file / non-regular /
    //                no-workspace)
    //
    // Bumps eda_load_sv_total on success; eda_load_sv_failure_total
    // on any failure path. Together these two counters let
    // (query:eda-hw-stats) surface a success-rate signal for the
    // Agent loop without needing a separate ratio counter.
    add("eda:load-sv", [&ev, build_hash_eda](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0])) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_load_sv_failure_total.fetch_add(1, std::memory_order_relaxed);
            return build_hash_eda({{"path", make_string(0)},
                                   {"node-count", make_int(0)},
                                   {"status-ok", make_int(0)},
                                   {"reason", make_string(0)}});
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_load_sv_failure_total.fetch_add(1, std::memory_order_relaxed);
            return build_hash_eda({{"path", make_string(0)},
                                   {"node-count", make_int(0)},
                                   {"status-ok", make_int(0)},
                                   {"reason", make_string(0)}});
        }
        const auto& path = ev.string_heap_[idx];
        // Echo the input path back so the Agent can correlate.
        auto path_ev = make_string(idx);
        // Stat the file (skip directories). Use the same pattern as
        // (read-file) in evaluator_primitives_file.cpp:60+.
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_load_sv_failure_total.fetch_add(1, std::memory_order_relaxed);
            auto reason_idx = ev.string_heap_.size();
            ev.string_heap_.emplace_back("missing-or-not-regular-file");
            return build_hash_eda({{"path", path_ev},
                                   {"node-count", make_int(0)},
                                   {"status-ok", make_int(0)},
                                   {"reason", make_string(reason_idx)}});
        }
        std::ifstream f(path);
        if (!f) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_load_sv_failure_total.fetch_add(1, std::memory_order_relaxed);
            auto reason_idx = ev.string_heap_.size();
            ev.string_heap_.emplace_back("open-failed");
            return build_hash_eda({{"path", path_ev},
                                   {"node-count", make_int(0)},
                                   {"status-ok", make_int(0)},
                                   {"reason", make_string(reason_idx)}});
        }
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // Parse using the same line-based pattern as (eda:parse-netlist).
        // For now this is a copy of the parser; the cleaner refactor
        // (extract parse_netlist_string helper, share with both call
        // sites) is a separate follow-up.
        std::uint64_t parsed = 0;
        auto* ws = ev.workspace_flat();
        auto* pool = ev.workspace_pool();
        aura::ast::NodeId last_iface = aura::ast::NULL_NODE;
        std::size_t line_start = 0;
        while (line_start <= text.size()) {
            auto line_end = text.find('\n', line_start);
            const auto line = trim(std::string_view{
                text.data() + line_start,
                (line_end == std::string::npos ? text.size() : line_end) - line_start});
            if (!line.empty()) {
                auto parts = split_colon(line);
                if (!parts.empty()) {
                    const auto kind = trim(parts[0]);
                    // Always count parseable directives (so node-count
                    // is meaningful even when no workspace is loaded);
                    // only mutate the workspace when one is available.
                    const bool recognised = (kind == "interface" && parts.size() >= 2) ||
                                            (kind == "modport" && parts.size() >= 3) ||
                                            (kind == "constraint" && parts.size() >= 3) ||
                                            (kind == "property" && parts.size() >= 3) ||
                                            (kind == "coverpoint" && parts.size() >= 3);
                    if (recognised) {
                        ++parsed;
                        if (ws && pool) {
                            if (kind == "interface" && parts.size() >= 2) {
                                const auto name = trim(parts[1]);
                                last_iface = ws->add_interface(pool->intern(std::string(name)), {});
                            } else if (kind == "modport" && parts.size() >= 3 &&
                                       last_iface != aura::ast::NULL_NODE) {
                                const auto name = trim(parts[1]);
                                const auto ports = split_ports(*pool, trim(parts[2]));
                                const auto mp =
                                    ws->add_modport(pool->intern(std::string(name)), ports);
                                ws->insert_child(last_iface, ws->get(last_iface).children.size(),
                                                 mp);
                                // Issue #661: SV InterfaceIR + ModportIR
                                // structure observability. (Second call
                                // site — mirrors the first site at line ~165.)
                                // Direction-change wiring is the follow-up
                                // (issue body Action #3).
                                ev.bump_sv_interface_modport_views();
                                ev.bump_sv_interface_ports(ports.size());
                            } else if (kind == "constraint" && parts.size() >= 3) {
                                const auto name = trim(parts[1]);
                                const auto expr = trim(parts[2]);
                                (void)ws->add_constraint(pool->intern(std::string(name)),
                                                         std::span<const aura::ast::SymId>{
                                                             pool->intern(std::string(expr))});
                            } else if (kind == "property" && parts.size() >= 3) {
                                const auto name = trim(parts[1]);
                                const auto expr = trim(parts[2]);
                                (void)ws->add_property(pool->intern(std::string(name)),
                                                       pool->intern(std::string(expr)));
                            } else if (kind == "coverpoint" && parts.size() >= 3) {
                                const auto var = trim(parts[1]);
                                const auto bins = split_ports(*pool, trim(parts[2]));
                                (void)ws->add_coverpoint(pool->intern(std::string(var)), bins);
                            }
                        }
                    }
                }
            }
            if (line_end == std::string::npos)
                break;
            line_start = line_end + 1;
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->eda_load_sv_total.fetch_add(1, std::memory_order_relaxed);
            m->eda_foundation_parse_total.fetch_add(parsed, std::memory_order_relaxed);
            m->eda_infra_parse_success_total.fetch_add(1, std::memory_order_relaxed);
        }
        auto reason_idx = ev.string_heap_.size();
        ev.string_heap_.emplace_back("");
        return build_hash_eda({{"path", path_ev},
                               {"node-count", make_int(static_cast<std::int64_t>(parsed))},
                               {"status-ok", make_int(1)},
                               {"reason", make_string(reason_idx)}});
    });

    // (eda:parse-verification-result path) — Read a simulator
    // coverage/assertion JSON file from disk and extract a small
    // set of canonical fields into a hash. The Agent loop uses this
    // to ingest simulator feedback without writing a per-tool JSON
    // walker each time.
    //
    // Recognized JSON keys (any subset; missing keys default to 0):
    //   - coverage-percent / coverage_pct  → coverage-pct
    //   - assertions-pass / passed         → assertion-pass
    //   - assertions-fail / failed         → assertion-fail
    //
    // Returned hash:
    //   - path:           the input path string (echoed)
    //   - coverage-pct:   int (0 if absent)
    //   - assertion-pass: int (0 if absent)
    //   - assertion-fail: int (0 if absent)
    //   - success:        1 if the file was read AND any recognized
    //                     key was extracted; 0 otherwise
    //
    // Bumps eda_parse_verification_result_total on success;
    // eda_parse_verification_failure_total on any failure path.
    //
    // Note: the parser walks the JSON text via simple substring +
    // integer regex-free extraction. A real JSON walker could be
    // built on top of (json-parse), but the simulation-result files
    // we care about are flat-key/int-value documents, so the
    // substring scan is enough and avoids the cost of building a
    // general hash for every file read.
    add("eda:parse-verification-result",
        [&ev, build_hash_eda](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_string(a[0])) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
                return build_hash_eda({{"path", make_string(0)},
                                       {"coverage-pct", make_int(0)},
                                       {"assertion-pass", make_int(0)},
                                       {"assertion-fail", make_int(0)},
                                       {"success", make_int(0)}});
            }
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size()) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
                return build_hash_eda({{"path", make_string(0)},
                                       {"coverage-pct", make_int(0)},
                                       {"assertion-pass", make_int(0)},
                                       {"assertion-fail", make_int(0)},
                                       {"success", make_int(0)}});
            }
            const auto& path = ev.string_heap_[idx];
            auto path_ev = make_string(idx);
            struct stat st;
            if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
                return build_hash_eda({{"path", path_ev},
                                       {"coverage-pct", make_int(0)},
                                       {"assertion-pass", make_int(0)},
                                       {"assertion-fail", make_int(0)},
                                       {"success", make_int(0)}});
            }
            std::ifstream f(path);
            if (!f) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
                return build_hash_eda({{"path", path_ev},
                                       {"coverage-pct", make_int(0)},
                                       {"assertion-pass", make_int(0)},
                                       {"assertion-fail", make_int(0)},
                                       {"success", make_int(0)}});
            }
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            // Substring scan for canonical keys. For each recognized key,
            // find the next ':' then parse a contiguous integer. This is
            // intentionally minimal — the schema is flat and we only need
            // integer values. If the file is malformed (no recognisable
            // keys), we still bump _failure_total and report success=0.
            auto extract_int = [&](const std::string& text, std::string_view key) -> std::int64_t {
                auto pos = text.find(std::string(key));
                while (pos != std::string::npos) {
                    auto colon = text.find(':', pos);
                    if (colon == std::string::npos)
                        return 0;
                    auto start = colon + 1;
                    while (start < text.size() &&
                           std::isspace(static_cast<unsigned char>(text[start])))
                        ++start;
                    bool neg = false;
                    if (start < text.size() && text[start] == '-') {
                        neg = true;
                        ++start;
                    }
                    if (start >= text.size() ||
                        !std::isdigit(static_cast<unsigned char>(text[start])))
                        return 0;
                    std::int64_t v = 0;
                    while (start < text.size() &&
                           std::isdigit(static_cast<unsigned char>(text[start]))) {
                        v = v * 10 + (text[start] - '0');
                        ++start;
                    }
                    return neg ? -v : v;
                }
                return 0;
            };
            const auto cov = extract_int(text, "coverage-percent");
            const auto cov2 = extract_int(text, "coverage_pct");
            const auto pass = extract_int(text, "assertions-pass");
            const auto pass2 = extract_int(text, "passed");
            const auto fail = extract_int(text, "assertions-fail");
            const auto fail2 = extract_int(text, "failed");
            const auto coverage_pct = cov != 0 ? cov : cov2;
            const auto assertion_pass = pass != 0 ? pass : pass2;
            const auto assertion_fail = fail != 0 ? fail : fail2;
            const int success =
                (coverage_pct != 0 || assertion_pass != 0 || assertion_fail != 0) ? 1 : 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                if (success) {
                    m->eda_parse_verification_result_total.fetch_add(1, std::memory_order_relaxed);
                    m->eda_infra_feedback_ingest_total.fetch_add(1, std::memory_order_relaxed);
                    m->eda_infra_cosim_invoke_total.fetch_add(1, std::memory_order_relaxed);
                } else {
                    m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
                }
            }
            return build_hash_eda({{"path", path_ev},
                                   {"coverage-pct", make_int(coverage_pct)},
                                   {"assertion-pass", make_int(assertion_pass)},
                                   {"assertion-fail", make_int(assertion_fail)},
                                   {"success", make_int(success)}});
        });

    // Issue #841: (eda:ingest-result path) — structured co-sim result
    // ingest alias over (eda:parse-verification-result). Shares the same
    // parser + metrics wiring so Agent loops can use a semantic name.
    add("eda:ingest-result", [&ev, build_hash_eda](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0])) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
            return build_hash_eda({{"path", make_string(0)},
                                   {"coverage-pct", make_int(0)},
                                   {"assertion-pass", make_int(0)},
                                   {"assertion-fail", make_int(0)},
                                   {"success", make_int(0)}});
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
            return build_hash_eda({{"path", make_string(0)},
                                   {"coverage-pct", make_int(0)},
                                   {"assertion-pass", make_int(0)},
                                   {"assertion-fail", make_int(0)},
                                   {"success", make_int(0)}});
        }
        const auto& path = ev.string_heap_[idx];
        auto path_ev = make_string(idx);
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
            return build_hash_eda({{"path", path_ev},
                                   {"coverage-pct", make_int(0)},
                                   {"assertion-pass", make_int(0)},
                                   {"assertion-fail", make_int(0)},
                                   {"success", make_int(0)}});
        }
        std::ifstream f(path);
        if (!f) {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
            return build_hash_eda({{"path", path_ev},
                                   {"coverage-pct", make_int(0)},
                                   {"assertion-pass", make_int(0)},
                                   {"assertion-fail", make_int(0)},
                                   {"success", make_int(0)}});
        }
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto extract_int = [&](const std::string& blob, std::string_view key) -> std::int64_t {
            auto pos = blob.find(std::string(key));
            while (pos != std::string::npos) {
                auto colon = blob.find(':', pos);
                if (colon == std::string::npos)
                    return 0;
                auto start = colon + 1;
                while (start < blob.size() && std::isspace(static_cast<unsigned char>(blob[start])))
                    ++start;
                if (start >= blob.size() || !std::isdigit(static_cast<unsigned char>(blob[start])))
                    return 0;
                std::int64_t v = 0;
                while (start < blob.size() &&
                       std::isdigit(static_cast<unsigned char>(blob[start]))) {
                    v = v * 10 + (blob[start] - '0');
                    ++start;
                }
                return v;
            }
            return 0;
        };
        const auto cov = extract_int(text, "coverage-percent");
        const auto cov2 = extract_int(text, "coverage_pct");
        const auto pass = extract_int(text, "assertions-pass");
        const auto pass2 = extract_int(text, "passed");
        const auto fail = extract_int(text, "assertions-fail");
        const auto fail2 = extract_int(text, "failed");
        const auto coverage_pct = cov != 0 ? cov : cov2;
        const auto assertion_pass = pass != 0 ? pass : pass2;
        const auto assertion_fail = fail != 0 ? fail : fail2;
        const int success =
            (coverage_pct != 0 || assertion_pass != 0 || assertion_fail != 0) ? 1 : 0;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            if (success) {
                m->eda_parse_verification_result_total.fetch_add(1, std::memory_order_relaxed);
                m->eda_infra_feedback_ingest_total.fetch_add(1, std::memory_order_relaxed);
                m->eda_infra_cosim_invoke_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                m->eda_parse_verification_failure_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return build_hash_eda({{"path", path_ev},
                               {"coverage-pct", make_int(coverage_pct)},
                               {"assertion-pass", make_int(assertion_pass)},
                               {"assertion-fail", make_int(assertion_fail)},
                               {"success", make_int(success)}});
    });

    // Issue #841: (eda:invoke-simulator path) — co-simulation bridge stub.
    // Validates the simulator script/config path exists, bumps cosim metrics,
    // and returns a small status hash for Agent branching.
    add("eda:invoke-simulator", [&ev, build_hash_eda](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0])) {
            return build_hash_eda(
                {{"path", make_string(0)}, {"status-ok", make_int(0)}, {"reason", make_string(0)}});
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size()) {
            return build_hash_eda(
                {{"path", make_string(0)}, {"status-ok", make_int(0)}, {"reason", make_string(0)}});
        }
        const auto& path = ev.string_heap_[idx];
        auto path_ev = make_string(idx);
        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            auto reason_idx = ev.string_heap_.size();
            ev.string_heap_.emplace_back("missing-or-not-regular-file");
            return build_hash_eda({{"path", path_ev},
                                   {"status-ok", make_int(0)},
                                   {"reason", make_string(reason_idx)}});
        }
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->eda_infra_cosim_invoke_total.fetch_add(1, std::memory_order_relaxed);
            m->commercial_simulator_runs_total.fetch_add(1, std::memory_order_relaxed);
        }
        auto reason_idx = ev.string_heap_.size();
        ev.string_heap_.emplace_back("");
        return build_hash_eda(
            {{"path", path_ev}, {"status-ok", make_int(1)}, {"reason", make_string(reason_idx)}});
    });

    // Issue #801: (eda:validate-sv-emit-roundtrip node-id [simulator])
    // — re-emit SV for a verification node, run validate_sv_emit roundtrip
    // stub, record commercial emit fidelity metrics, return status hash.
    add("eda:validate-sv-emit-roundtrip",
        [&ev, build_hash_eda](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_int(a[0]))
                return build_hash_eda({{"node-id", make_int(-1)},
                                       {"emit-ok", make_int(0)},
                                       {"emit-len", make_int(0)},
                                       {"roundtrip-ok", make_int(0)}});
            auto* ws = ev.workspace_flat();
            auto* pool = ev.workspace_pool();
            if (!ws || !pool)
                return build_hash_eda({{"node-id", make_int(as_int(a[0]))},
                                       {"emit-ok", make_int(0)},
                                       {"emit-len", make_int(0)},
                                       {"roundtrip-ok", make_int(0)}});
            const auto nid = static_cast<aura::ast::NodeId>(as_int(a[0]));
            if (!ws->is_live_node(nid))
                return build_hash_eda({{"node-id", make_int(static_cast<std::int64_t>(nid))},
                                       {"emit-ok", make_int(0)},
                                       {"emit-len", make_int(0)},
                                       {"roundtrip-ok", make_int(0)}});
            std::string_view simulator = "vcs";
            if (a.size() >= 2 && is_string(a[1])) {
                const auto sim_idx = as_string_idx(a[1]);
                if (sim_idx < ev.string_heap_.size())
                    simulator = ev.string_heap_[sim_idx];
            }
            const auto reemit = aura::compiler::sv_ir::reemit_sv_node(*ws, *pool, nid, simulator);
            const auto validation = aura::compiler::sv_ir::validate_sv_emit(reemit.sv_text);
            ev.record_sv_commercial_emit_fidelity(validation.ok, true,
                                                  !reemit.commercial_do_stub.empty());
            return build_hash_eda(
                {{"node-id", make_int(static_cast<std::int64_t>(nid))},
                 {"emit-ok", make_int(reemit.sv_text.empty() ? 0 : 1)},
                 {"emit-len", make_int(static_cast<std::int64_t>(reemit.sv_text.size()))},
                 {"roundtrip-ok", make_int(validation.ok ? 1 : 0)}});
        });
#endif // AURA_ENABLE_EDA
}

} // namespace aura::compiler::primitives_detail