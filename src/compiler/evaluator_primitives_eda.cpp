// evaluator_primitives_eda.cpp — Issue #499: Foundational EDA primitives
// module (parse/query/mutate/waveform/hardware feedback) for Agent-driven
// verification + hardware co-design workflows.

module;

#include "observability_metrics.h"
#include "security_capabilities.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;
import aura.compiler.hardware_backend;
import aura.compiler.sv_ir;

namespace aura::compiler::primitives_detail::eda_detail {

using EvalValue = types::EvalValue;
using namespace types;

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

inline std::vector<aura::ast::SymId>
split_ports(aura::ast::StringPool& pool, std::string_view ports_csv) {
    std::vector<aura::ast::SymId> out;
    std::size_t start = 0;
    while (start <= ports_csv.size()) {
        auto pos = ports_csv.find(',', start);
        const auto slice = trim(ports_csv.substr(start, pos == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : pos - start));
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
            if (validation.ok)
                m->sv_emit_parse_success_total.fetch_add(1, std::memory_order_relaxed);
            else
                m->sv_emit_parse_fail_total.fetch_add(1, std::memory_order_relaxed);
            m->hardware_backend_hook_calls_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace aura::compiler::primitives_detail::eda_detail

namespace aura::compiler::primitives_detail {

void register_eda_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev) {
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
                    } else if (kind == "modport" && parts.size() >= 3 && last_iface != aura::ast::NULL_NODE) {
                        const auto name = trim(parts[1]);
                        const auto ports = split_ports(*pool, trim(parts[2]));
                        const auto mp = ws->add_modport(pool->intern(std::string(name)), ports);
                        ws->insert_child(last_iface, ws->get(last_iface).children.size(), mp);
                        ++parsed;
                    } else if (kind == "constraint" && parts.size() >= 3) {
                        const auto name = trim(parts[1]);
                        const auto expr = trim(parts[2]);
                        (void)ws->add_constraint(pool->intern(std::string(name)),
                                                 std::span<const aura::ast::SymId>{
                                                     pool->intern(std::string(expr))});
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
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->eda_foundation_parse_total.fetch_add(parsed, std::memory_order_relaxed);
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
        ws->mark_dirty_upward(parent, aura::ast::FlatAST::kGeneralDirty, ws->ppa_dirty_reasons(parent));
        maybe_hardware_feedback(ev, parent);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->eda_foundation_mutate_total.fetch_add(1, std::memory_order_relaxed);
            m->sva_structured_mutate_hits_total.fetch_add(1, std::memory_order_relaxed);
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
        }
        return make_bool(true);
    });
}

} // namespace aura::compiler::primitives_detail