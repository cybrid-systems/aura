// verify_tool.cpp — Issue #443: External simulator
// tool-calling + structured result parsing primitives.
//
// P0 scope-limited ship: 3 new primitives in the
// (verify:...) namespace:
//   - (verify:run-external-sim "cmd-string" [timeout-ms-int])
//     — P0 stub: records the call + returns a parseable
//     placeholder string. The follow-up wires the actual
//     ::system() / popen() execution with strict arg
//     validation + timeout. The P0 cache prevents repeated
//     calls in the same generation from re-running.
//   - (verify:parse-coverage "cov-data-string") — parses
//     line-based "node_id hole_name" format and marks
//     each node dirty with kCoverageFeedbackDirty
//     (from #469).
//   - (verify:parse-failures "fail-data-string") — same
//     format, marks with kAssertFailureDirty.
//
// All 3 bump the verify_tool_*_total_ counters on
// Evaluator (readable via (query:verify-tool-stats)).

// verify_tool.cpp — Issue #443: External simulator
// tool-calling + structured result parsing primitives.
//
// aura.compiler.evaluator module partition; registered
// via evaluator_primitives_registry.cpp.
//
// P0 scope-limited ship: 3 new primitives in the
// (verify:...) namespace:
//   - (verify:run-external-sim "cmd-string" [timeout-ms-int])
//     — P0 stub: records the call + returns a parseable
//     placeholder string. The follow-up wires the actual
//     ::system() / popen() execution with strict arg
//     validation + timeout. The P0 cache prevents repeated
//     calls in the same generation from re-running.
//   - (verify:parse-coverage "cov-data-string") — parses
//     line-based "node_id hole_name" format and marks
//     each node dirty with kCoverageFeedbackDirty
//     (from #469).
//   - (verify:parse-failures "fail-data-string") — same
//     format, marks with kAssertFailureDirty.
//
// All 3 bump the verify_tool_*_total_ counters on
// Evaluator (readable via (query:verify-tool-stats)).

module;
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <functional>

module aura.compiler.evaluator;

import aura.compiler.value;
import aura.compiler.service;
import aura.core.ast;

namespace aura::compiler::primitives_detail::verify_tool_detail {

// Helper: parses a line-based "node_id hole_name" text
// blob. Each line starts with a non-negative integer
// (the NodeId). Lines that don't start with an integer
// are skipped. Returns the count of nodes successfully
// marked dirty.
static std::uint64_t parse_and_mark(aura::compiler::Evaluator& ev,
                                    const std::string& text,
                                    bool is_coverage) {
    auto* ws = ev.workspace_flat();
    if (!ws) return 0;
    std::uint64_t marked = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        // Find end of line.
        std::size_t j = i;
        while (j < text.size() && text[j] != '\n') ++j;
        const std::string_view line(text.data() + i, j - i);
        // Skip leading whitespace.
        std::size_t k = 0;
        while (k < line.size() &&
               (line[k] == ' ' || line[k] == '\t')) ++k;
        // Parse the first integer (NodeId).
        if (k < line.size() && line[k] >= '0' && line[k] <= '9') {
            std::size_t val = 0;
            while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
                val = val * 10 + (line[k] - '0');
                ++k;
            }
            const auto nid = static_cast<aura::ast::NodeId>(val);
            if (nid < ws->size()) {
                const auto reason = is_coverage
                    ? aura::ast::FlatAST::kCoverageFeedbackDirty
                    : aura::ast::FlatAST::kAssertFailureDirty;
                ws->apply_verification_dirty_bits(nid, reason);
                ++marked;
            } else {
                ev.bump_verify_tool_parse_error();
            }
        } else if (!line.empty()) {
            // Non-empty line that doesn't start with an
            // integer → parse error.
            ev.bump_verify_tool_parse_error();
        }
        i = (j < text.size()) ? j + 1 : j;
    }
    return marked;
}

} // namespace aura::compiler::primitives_detail::verify_tool_detail

// Main registration function. Called by
// register_evaluate_primitives in
// evaluator_primitives_registry.cpp.
namespace aura::compiler::primitives_detail {

void register_verify_tool_primitives(
    std::function<void(std::string,
                       std::function<aura::compiler::types::EvalValue(
                           std::span<const aura::compiler::types::EvalValue>)>)> add,
    aura::compiler::Evaluator& ev,
    std::function<aura::compiler::types::EvalValue(std::int32_t)> make_string,
    std::function<aura::compiler::types::EvalValue(std::int64_t)> make_int,
    std::function<aura::compiler::types::EvalValue(
        const std::string&, const std::string&)> mev) {
    using namespace aura::compiler::primitives_detail::verify_tool_detail;

    // (verify:run-external-sim "cmd-string"
    //   [timeout-ms-int])
    // — call an external simulator. P0 stub: records
    // the call, checks the cache, and returns a
    // placeholder result string. The follow-up wires
    // the actual subprocess execution (popen with
    // strict arg validation + timeout).
    //
    // The placeholder result is the cmd string itself
    // (so the Agent can verify the call happened). The
    // P0 doesn't actually run the tool — the cache
    // prevents repeated calls from doing the wrong
    // thing in tests.
    add("verify:run-external-sim",
        [&ev, make_string, make_int, mev](std::span<const aura::compiler::types::EvalValue> a) -> aura::compiler::types::EvalValue {
            if (a.empty() || !is_string(a[0]))
                return mev("bad-arg",
                    "usage: (verify:run-external-sim \"cmd\" [timeout-ms])");
            ev.bump_verify_tool_call();
            auto cmd_idx = as_string_idx(a[0]);
            if (cmd_idx >= ev.string_heap_size())
                return mev("bad-arg", "cmd string index out of range");
            const auto& cmd = ev.string_heap_at(cmd_idx);
            // Optional 2nd arg: timeout (in ms). P0: the
            // timeout is recorded (bumped) but the
            // actual execution is a no-op.
            if (a.size() >= 2 && is_int(a[1])) {
                // No-op for P0; the follow-up wires
                // the actual timeout. We can record it
                // via a counter if desired, but the
                // cache_hit counter is sufficient for
                // P0 observability.
            }
            // Cache check: if (cmd, gen) is in the
            // cache, return the cached result. The
            // cache bump_verify_tool_cache_hit() side-
            // effect is the only difference between
            // cache hit + miss.
            if (auto cached = ev.lookup_verify_tool_cache(cmd)) {
                ev.bump_verify_tool_cache_hit();
                return make_string(
                    ev.push_string_heap(*cached));
            }
            // Cache miss: P0 returns the cmd string as
            // a placeholder result. The follow-up
            // replaces this with the actual tool
            // output (stdout + stderr captured via
            // popen()).
            ev.insert_verify_tool_cache(cmd, cmd);
            return make_string(static_cast<std::int32_t>(cmd_idx));
        });

    // (verify:parse-coverage "cov-data-string")
    // — parses line-based "node_id hole_name" format
    // and marks each node dirty with
    // kCoverageFeedbackDirty (from #469). Returns
    // the count of nodes successfully marked.
    add("verify:parse-coverage",
        [&ev, make_int, mev](std::span<const aura::compiler::types::EvalValue> a) -> aura::compiler::types::EvalValue {
            if (a.empty() || !is_string(a[0]))
                return mev("bad-arg",
                    "usage: (verify:parse-coverage \"cov-data\")");
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_size())
                return make_int(0);
            const auto& text = ev.string_heap_at(idx);
            const auto marked = parse_and_mark(ev, text, /*is_coverage=*/true);
            return make_int(static_cast<std::int64_t>(marked));
        });

    // (verify:parse-failures "fail-data-string") —
    // same format, marks with kAssertFailureDirty.
    add("verify:parse-failures",
        [&ev, make_int, mev](std::span<const aura::compiler::types::EvalValue> a) -> aura::compiler::types::EvalValue {
            if (a.empty() || !is_string(a[0]))
                return mev("bad-arg",
                    "usage: (verify:parse-failures \"fail-data\")");
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_size())
                return make_int(0);
            const auto& text = ev.string_heap_at(idx);
            const auto marked = parse_and_mark(ev, text, /*is_coverage=*/false);
            return make_int(static_cast<std::int64_t>(marked));
        });
}

} // namespace aura::compiler::primitives_detail