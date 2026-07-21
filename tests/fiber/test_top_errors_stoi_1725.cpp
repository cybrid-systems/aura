// @category: unit
// @reason: Issue #1725 — evolve-strategy top-errors std::stoi must not
// silently swallow parse failures (#1724 sibling / #1669 pattern).
//
//   AC1: source cites #1725; top-errors stoi uses std::exception catch
//   AC2: shared agent_evolve_analytics_parse_failures metric present
//   AC3: malformed top-errors count bumps metric; evolve returns
//   AC4: valid unbound-variable:3 (≥2) still appends sys-prompt hint

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string heap_str(CompilerService& cs, const aura::compiler::types::EvalValue& v) {
    if (!is_string(v))
        return {};
    auto& heap = cs.evaluator().string_heap_mut();
    auto idx = as_string_idx(v);
    if (idx >= heap.size())
        return {};
    return heap[idx];
}

// Body of add("evolve-strategy" ... until next top-level add(
std::string evolve_window(const std::string& src) {
    auto needle = std::string("add(\"evolve-strategy\"");
    auto pos = src.find(needle);
    if (pos == std::string::npos)
        return {};
    auto end = src.find("\n    add(\"", pos + 10);
    return src.substr(pos, end == std::string::npos ? 8000 : end - pos);
}

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: top-errors stoi + metric ---");
        std::string agent;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            agent = read_file(p);
            if (!agent.empty())
                break;
        }
        CHECK(!agent.empty(), "read agent");
        CHECK(agent.find("#1725") != std::string::npos, "cites #1725");

        auto win = evolve_window(agent);
        CHECK(!win.empty(), "found evolve-strategy");
        CHECK(win.find("std::stoi") != std::string::npos, "uses stoi for top-errors");
        CHECK(win.find("catch (const std::exception&") != std::string::npos,
              "narrows catch to std::exception");
        CHECK(win.find("agent_evolve_analytics_parse_failures") != std::string::npos,
              "bumps parse-failure metric");

        std::string msrc;
        for (const char* p :
             {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
            msrc = read_file(p);
            if (!msrc.empty())
                break;
        }
        CHECK(!msrc.empty() &&
                  msrc.find("agent_evolve_analytics_parse_failures") != std::string::npos,
              "metric field declared");
    }

    // ── AC3: malformed top-errors count → metric bump ──
    {
        std::println("\n--- AC3: malformed top-errors count bumps metric ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "compiler_metrics wired");
        const auto m0 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);

        auto def =
            cs.eval(R"AURA((define-strategy "s1725bad" "(lambda (x) x)" :max-attempts 3))AURA");
        CHECK(static_cast<bool>(def), "define-strategy s1725bad");

        // Well-formed rates (no stod bump); bad count token → stoi throw.
        auto evo = cs.eval(
            R"AURA((evolve-strategy "s1725bad" "#(analytics total-runs:10 success-rate:0.99 avg-attempts:1.0 total-llm-calls:1 avg-duration-ms:1 top-errors:( unbound-variable:not-a-number type-error:xx) by-task:())"))AURA");
        CHECK(static_cast<bool>(evo), "evolve with malformed top-errors returns");

        const auto m1 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);
        CHECK(m1 > m0, "agent_evolve_analytics_parse_failures bumped on stoi");
    }

    // ── AC4: valid top-errors still drive hint ──
    {
        std::println("\n--- AC4: valid top-errors append unbound-variable hint ---");
        CompilerService cs;
        auto* m = metrics_of(cs);
        const auto m0 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);

        auto def =
            cs.eval(R"AURA((define-strategy "s1725ok" "(lambda (x) x)" :max-attempts 3))AURA");
        CHECK(static_cast<bool>(def), "define-strategy s1725ok");

        // rates that skip max-attempts heuristics; top-errors drives prompt hint.
        auto evo = cs.eval(
            R"AURA((evolve-strategy "s1725ok" "#(analytics total-runs:10 success-rate:0.7 avg-attempts:2.0 total-llm-calls:1 avg-duration-ms:1 top-errors:( unbound-variable:3) by-task:())"))AURA");
        CHECK(static_cast<bool>(evo), "evolve with valid top-errors");

        const auto m1 = m->agent_evolve_analytics_parse_failures.load(std::memory_order_relaxed);
        CHECK(m1 == m0, "valid top-errors does not bump parse failures");

        auto field = cs.eval(R"AURA((strategy-field "s1725ok-v1" "sys-prompt-template"))AURA");
        CHECK(field.has_value() && is_string(*field), "sys-prompt-template is string");
        if (field && is_string(*field)) {
            auto tpl = heap_str(cs, *field);
            CHECK(tpl.find("undefined variables") != std::string::npos,
                  "appended unbound-variable hint");
        }
    }

    std::println("\n=== test_top_errors_stoi_1725: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
