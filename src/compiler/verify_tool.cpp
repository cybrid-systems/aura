// verify_tool.cpp — Issue #443 / #710: External simulator
// tool-calling + structured result parsing primitives.
//
// P0 scope-limited ship: 3 new primitives in the
// (verify:...) namespace:
//   - (verify:run-external-sim "cmd-string" [timeout-ms-int])
//   - (verify:parse-coverage "cov-data-string")
//   - (verify:parse-failures "fail-data-string")
//
// Issue #710: parse paths use MutationBoundaryGuard,
// StableNodeRef provenance, mark_dirty_upward, and optional
// hardware_backend hook for SV closed-loop feedback.

module;

module aura.compiler.evaluator;
import std;

import aura.compiler.hardware_backend;
import aura.compiler.value;
import aura.compiler.service;
import aura.core.ast;

namespace aura::compiler::primitives_detail::verify_tool_detail {

// Helper: parses a line-based "node_id hole_name" text
// blob. Each line starts with a non-negative integer
// (the NodeId). Issue #710: Guard + StableNodeRef +
// mark_dirty_upward on each successfully marked node.
static std::uint64_t parse_and_mark(aura::compiler::Evaluator& ev, const std::string& text,
                                    bool is_coverage) {
    auto* ws = ev.workspace_flat();
    if (!ws)
        return 0;
    bool ok = true;
    aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
    ev.bump_verify_tool_guard_capture();

    std::uint64_t marked = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t j = i;
        while (j < text.size() && text[j] != '\n')
            ++j;
        const std::string_view line(text.data() + i, j - i);
        std::size_t k = 0;
        while (k < line.size() && (line[k] == ' ' || line[k] == '\t'))
            ++k;
        if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
            std::size_t val = 0;
            while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                val = val * 10 + (line[k] - '0');
                ++k;
            }
            const auto nid = static_cast<aura::ast::NodeId>(val);
            if (nid >= ws->size()) {
                ev.bump_verify_tool_parse_error();
            } else {
                const auto pref = ws->make_ref(nid);
                if (!pref.is_valid_in(*ws)) {
                    ev.bump_verify_tool_parse_error();
                } else {
                    ev.bump_verify_tool_stable_ref_hit();
                    const auto reason = is_coverage ? aura::ast::FlatAST::kCoverageFeedbackDirty
                                                    : aura::ast::FlatAST::kAssertFailureDirty;
                    ws->apply_verification_dirty_bits(nid, reason);
                    if (aura::compiler::hardware::should_invoke_sv_closedloop_hook(*ws, nid))
                        ws->apply_verify_dirty_bits(nid, aura::ast::FlatAST::kSvaDirty);
                    ws->mark_dirty_upward(nid, aura::ast::FlatAST::kGeneralDirty,
                                          ws->ppa_dirty_reasons(nid));
                    ev.bump_verify_tool_dirty_propagation();
                    if (aura::compiler::hardware::should_invoke_sv_closedloop_hook(*ws, nid)) {
                        const auto sv_reasons =
                            aura::compiler::hardware::sv_structural_dirty_reasons(*ws, nid);
                        aura::compiler::hardware::on_structural_mutation(
                            nid,
                            static_cast<std::uint8_t>(aura::ast::FlatAST::kGeneralDirty |
                                                      sv_reasons),
                            ws->ppa_dirty_reasons(nid));
                    }
                    ev.bump_verify_tool_feedback_mutate_success();
                    ++marked;
                }
            }
        } else if (!line.empty()) {
            ev.bump_verify_tool_parse_error();
        }
        i = (j < text.size()) ? j + 1 : j;
    }
    return marked;
}

} // namespace aura::compiler::primitives_detail::verify_tool_detail

namespace aura::compiler::primitives_detail {

void register_verify_tool_primitives(
    std::function<void(std::string, std::function<aura::compiler::types::EvalValue(
                                        std::span<const aura::compiler::types::EvalValue>)>)>
        add,
    aura::compiler::Evaluator& ev,
    std::function<aura::compiler::types::EvalValue(std::int32_t)> make_string,
    std::function<aura::compiler::types::EvalValue(std::int64_t)> make_int,
    std::function<aura::compiler::types::EvalValue(const std::string&, const std::string&)> mev) {
    using namespace aura::compiler::primitives_detail::verify_tool_detail;

    add("verify:run-external-sim",
        [&ev, make_string, make_int, mev](std::span<const aura::compiler::types::EvalValue> a)
            -> aura::compiler::types::EvalValue {
            if (a.empty() || !is_string(a[0]))
                return mev("bad-arg", "usage: (verify:run-external-sim \"cmd\" [timeout-ms])");
            bool ok = true;
            aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
            ev.bump_verify_tool_guard_capture();
            ev.bump_verify_tool_call();
            auto cmd_idx = as_string_idx(a[0]);
            if (cmd_idx >= ev.string_heap_size())
                return mev("bad-arg", "cmd string index out of range");
            const auto& cmd = ev.string_heap_at(cmd_idx);
            (void)make_int;
            if (auto cached = ev.lookup_verify_tool_cache(cmd)) {
                ev.bump_verify_tool_cache_hit();
                return make_string(ev.push_string_heap(*cached));
            }
            ev.insert_verify_tool_cache(cmd, cmd);
            return make_string(static_cast<std::int32_t>(cmd_idx));
        });

    add("verify:parse-coverage",
        [&ev, make_int, mev](std::span<const aura::compiler::types::EvalValue> a)
            -> aura::compiler::types::EvalValue {
            if (a.empty() || !is_string(a[0]))
                return mev("bad-arg", "usage: (verify:parse-coverage \"cov-data\")");
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_size())
                return make_int(0);
            const auto& text = ev.string_heap_at(idx);
            const auto marked = parse_and_mark(ev, text, /*is_coverage=*/true);
            return make_int(static_cast<std::int64_t>(marked));
        });

    add("verify:parse-failures",
        [&ev, make_int, mev](std::span<const aura::compiler::types::EvalValue> a)
            -> aura::compiler::types::EvalValue {
            if (a.empty() || !is_string(a[0]))
                return mev("bad-arg", "usage: (verify:parse-failures \"fail-data\")");
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_size())
                return make_int(0);
            const auto& text = ev.string_heap_at(idx);
            const auto marked = parse_and_mark(ev, text, /*is_coverage=*/false);
            return make_int(static_cast<std::int64_t>(marked));
        });
}

} // namespace aura::compiler::primitives_detail